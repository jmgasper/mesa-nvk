/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include <stdio.h>
#include <poll.h>
#include <string.h>
#include <inttypes.h>

#include "vk_log.h"

#include "util/u_memory.h"
#include "nv_push_clc36f.h"

#include "class/cl0002.h" // NV01_CONTEXT_DMA
#include "class/clc361.h" // NVC361_NOTIFY_CHANNEL_PENDING
#include "class/clc46f.h" // TURING_CHANNEL_GPFIFO_A
#include "class/cl2080_notification.h" // NV2080_ENGINE_TYPE_GRAPHICS
#include "class/clc36f.h" // VOLTA_CHANNEL_GPFIFO_A
#include "class/cla16f.h" // KeplerBControlGPFifo
#include "class/cl906fsw.h" // NV906F_NOTIFIERS_RC
#include "class/cl0005.h" // NV01_EVENT
#include "ctrl/ctrla06f/ctrla06fgpfifo.h" // NVA06F_CTRL_CMD_BIND
#include "ctrl/ctrlc36f.h" // NVC36F_CTRL_CMD_INTERNAL_GPFIFO_GET_WORK_SUBMIT_TOKEN

#define SUBC_NVC36F 0


#define NV_CHECK(nvRes) {NV_STATUS _nvRes = nvRes; if (_nvRes != NV_OK) {vkRes = vk_error(log_obj, VK_ERROR_UNKNOWN); goto error;}}
#define VK_CHECK(vkResIn) {VkResult _vkRes = vkResIn; if (_vkRes != VK_SUCCESS) {vkRes = vk_error(log_obj, _vkRes); goto error;}}

#define NVRM_CTX_CMDBUF_BYTES   0x80000
#define NVRM_CTX_GPFIFO_ENTRIES 0x8000


/* Completed-fence value: the highest wSeq the GPU has released into ctx->sem. */
static uint64_t
ctx_completed(struct nvkmd_nvrm_exec_ctx *ctx)
{
	return *(volatile uint64_t *)ctx->sem->map;
}

/* True once RM has reported a robust-channel error on this channel. */
static bool
ctx_check_error(struct nvkmd_nvrm_exec_ctx *ctx)
{
	volatile NvNotification *notifiers = ctx->notifier->map;
	return notifiers[NV906F_NOTIFIERS_RC].status != 0;
}

static uint32_t
ctx_slot_dwords(void)
{
	return (NVRM_CTX_CMDBUF_BYTES / 4) / NVRM_CTX_CMDBUF_SLOTS;
}

/* GPU virtual address of the current cmdBuf slot. */
static uint64_t
ctx_slot_gpu_base(struct nvkmd_nvrm_exec_ctx *ctx)
{
	return ctx->cmdBuf->va->addr +
		(uint64_t)ctx->cmdBufCurSlot * ctx_slot_dwords() * 4;
}

/* (Re)initialise the push writer into the current cmdBuf slot. */
static void
ctx_begin_slot(struct nvkmd_nvrm_exec_ctx *ctx)
{
	uint32_t slotDwords = ctx_slot_dwords();
	uint32_t *base = (uint32_t *)ctx->cmdBuf->map + ctx->cmdBufCurSlot * slotDwords;
	nv_push_init(&ctx->push, base, slotDwords, BITFIELD_BIT(SUBC_NV9097));
	ctx->cmdBufSubmittedDw = 0;
}


static void
write_gp_fifo_entry(struct nvkmd_nvrm_exec_ctx *ctx, const struct nvkmd_ctx_exec *exec)
{
	//fprintf(stderr, "write_gp_fifo_entry(%#" PRIx64 ", %#" PRIx32 ")\n", exec->addr, exec->size_B);
	uint32_t next = (uint32_t)((ctx->gpPut + 1) % NVRM_CTX_GPFIFO_ENTRIES);

	/* Back-pressure: never let GPPut catch the host's GPGet. Earlier
	 * submissions have been doorbelled, so the host keeps draining and GPGet
	 * advances; bail out on a dead channel rather than spin forever. */
	KeplerBControlGPFifo *userD = ctx->userD->map;
	while (next == (userD->GPGet % NVRM_CTX_GPFIFO_ENTRIES)) {
		if (ctx_check_error(ctx))
			break;
		struct pollfd pfd = { .fd = ctx->osEvent, .events = POLLIN | POLLPRI };
		poll(&pfd, 1, 100);
	}

	uint32_t *ptr = (uint32_t*)ctx->gpFifo->map + 2*ctx->gpPut;

	ptr[0] = DRF_NUM(A16F, _GP_ENTRY0, _GET, NvU64_LO32(exec->addr) >> 2);
	ptr[1] =
		DRF_NUM(A16F, _GP_ENTRY1, _GET_HI, NvU64_HI32(exec->addr)) |
		DRF_NUM(A16F, _GP_ENTRY1, _LENGTH, (exec->size_B >> 2)) |
		DRF_NUM(A16F, _GP_ENTRY1, _SYNC, (exec->no_prefetch ? NVA16F_GP_ENTRY1_SYNC_WAIT : NVA16F_GP_ENTRY1_SYNC_PROCEED));

	ctx->gpPut = next;
}

/* Turn the dwords written to ctx->push since the last segment into one GP
 * entry. Lets wait() (acquires) and signal()/flush() (releases + fence) land
 * as separate GP entries on either side of the exec entries. */
static void
emit_push_segment(struct nvkmd_nvrm_exec_ctx *ctx)
{
	uint32_t total = nv_push_dw_count(&ctx->push);
	if (total <= ctx->cmdBufSubmittedDw)
		return;

	struct nvkmd_ctx_exec exec = {
		.addr = ctx_slot_gpu_base(ctx) + (uint64_t)ctx->cmdBufSubmittedDw * 4,
		.size_B = (total - ctx->cmdBufSubmittedDw) * 4,
	};
	write_gp_fifo_entry(ctx, &exec);
	ctx->cmdBufSubmittedDw = total;
}

static void
write_semaphore_release(struct nv_push *push, uint64_t adrGpu, uint64_t value, bool waitForIdle)
{
   P_MTHD(push, NVC36F, SEM_ADDR_LO);
   P_NVC36F_SEM_ADDR_LO(push, (uint32_t)adrGpu >> 2);
   P_NVC36F_SEM_ADDR_HI(push, (uint32_t)(adrGpu >> 32));
   P_NVC36F_SEM_PAYLOAD_LO(push, (uint32_t)value);
   P_NVC36F_SEM_PAYLOAD_HI(push, (uint32_t)(value >> 32));
   P_NVC36F_SEM_EXECUTE(push, {
      .operation = OPERATION_RELEASE,
      .release_wfi = waitForIdle ? RELEASE_WFI_EN : RELEASE_WFI_DIS,
      .payload_size = PAYLOAD_SIZE_64BIT,
      .release_timestamp = RELEASE_TIMESTAMP_DIS,
   });
   P_MTHD(push, NVC36F, NON_STALL_INTERRUPT);
   P_NVC36F_NON_STALL_INTERRUPT(push, 0);
}

static void
write_semaphore_acquire(struct nv_push *push, uint64_t adrGpu, uint64_t value)
{
   P_MTHD(push, NVC36F, SEM_ADDR_LO);
   P_NVC36F_SEM_ADDR_LO(push, (uint32_t)adrGpu >> 2);
   P_NVC36F_SEM_ADDR_HI(push, (uint32_t)(adrGpu >> 32));
   P_NVC36F_SEM_PAYLOAD_LO(push, (uint32_t)value);
   P_NVC36F_SEM_PAYLOAD_HI(push, (uint32_t)(value >> 32));
   P_NVC36F_SEM_EXECUTE(push, {
      .operation = OPERATION_ACQ_STRICT_GEQ,
      .acquire_switch_tsg = ACQUIRE_SWITCH_TSG_EN,
      .payload_size = PAYLOAD_SIZE_64BIT,
   });
}


static VkResult
nvkmd_nvrm_create_exec_ctx(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              enum nvkmd_engines engines,
                              struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   VkResult vkRes;

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   struct nvkmd_nvrm_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_nvrm_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvrm_exec_ctx_ops;
   ctx->base.dev = &dev->base;

   ctx->osEvent = -1;

   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj,  0x1000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->notifier));
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x80000, 0x10000, NVKMD_MEM_LOCAL, NVKMD_MEM_MAP_RDWR, &ctx->userD));
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x40000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->gpFifo));
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj, NVRM_CTX_CMDBUF_BYTES, 0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR, &ctx->cmdBuf));
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj,  0x1000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->sem));

   /* The fence value and the RC error notifier are read in steady state
    * (ctx_completed / ctx_check_error); start them at zero rather than
    * trusting freshly allocated GART to be cleared. */
   memset(ctx->sem->map, 0, ctx->sem->size_B);
   memset(ctx->notifier->map, 0, ctx->notifier->size_B);

	NV_CONTEXT_DMA_ALLOCATION_PARAMS ctxDmaParams = {
		.flags =
			DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) |
			DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),

		.hMemory = nvkmd_nvrm_mem(ctx->notifier)->hMemoryPhys,
		.offset = 0,
		.limit = ctx->notifier->size_B - 1,
	};
   NV_CHECK(nvRmApiAlloc(&rm, pdev->hDevice, &ctx->hCtxDma, NV01_CONTEXT_DMA, &ctxDmaParams));

   NvU32 engineType = NV2080_ENGINE_TYPE_GRAPHICS;
	NV_CHANNEL_ALLOC_PARAMS createChannelParams = {
		.hObjectError  = ctx->hCtxDma,
		.gpFifoOffset  = ctx->gpFifo->va->addr,
		.gpFifoEntries = 0x8000,
		.flags         = 0,
		.hVASpace      = pdev->hVaSpace,
		.hUserdMemory  = {nvkmd_nvrm_mem(ctx->userD)->hMemoryPhys},
		.userdOffset   = {0},
		.engineType    = engineType,
	};
   NV_CHECK(nvRmApiAlloc(&rm, pdev->hDevice, &ctx->hChannel, pdev->channelClass, &createChannelParams));

	NVA06F_CTRL_BIND_PARAMS bindParams = {.engineType = engineType};
	NV_CHECK(nvRmApiControl(&rm, ctx->hChannel, NVA06F_CTRL_CMD_BIND, &bindParams, sizeof(bindParams)));

	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS scheduleParams = {.bEnable = NV_TRUE};
	NV_CHECK(nvRmApiControl(&rm, ctx->hChannel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &scheduleParams, sizeof(scheduleParams)));

	NVC36F_CTRL_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX_PARAMS notifParams = {
		.index = NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN
	};
	NV_CHECK(nvRmApiControl(&rm,
		ctx->hChannel,
		NVC36F_CTRL_CMD_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX,
		&notifParams,
		sizeof(notifParams)
	));

	NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS tokenParams = {0};
	NV_CHECK(nvRmApiControl(&rm,
		ctx->hChannel,
		NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
		&tokenParams,
		sizeof(tokenParams)
	));

   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hCopy, pdev->base.dev_info.cls_copy, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hEng2d, pdev->base.dev_info.cls_eng2d, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hEng3d, pdev->base.dev_info.cls_eng3d, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hM2mf, pdev->base.dev_info.cls_m2mf, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hCompute, pdev->base.dev_info.cls_compute, NULL));

   ctx->osEvent = open(rm.nodeName, O_RDWR | O_CLOEXEC);
   if (ctx->osEvent < 0) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error;
   }
   struct NvRmApi rmOsEvent = rm;
   rmOsEvent.fd = ctx->osEvent;
   NV_CHECK(nvRmApiAllocOsEvent(&rmOsEvent, ctx->osEvent));

	NV0005_ALLOC_PARAMETERS eventParams = {
		.hParentClient = pdev->hClient,
		.hSrcResource = pdev->hSubdevice,
		.hClass = NV01_EVENT_OS_EVENT,
		.notifyIndex =
			NV2080_NOTIFIERS_FIFO_EVENT_MTHD |
			NV01_EVENT_NONSTALL_INTR |
			NV01_EVENT_WITHOUT_EVENT_DATA |
			NV01_EVENT_SUBDEVICE_SPECIFIC |
			DRF_NUM(0005, _NOTIFY_INDEX, _SUBDEVICE, 0),
		.data = (NvP64)(uintptr_t)ctx->osEvent,
	};
   NV_CHECK(nvRmApiAlloc(&rm, pdev->hSubdevice, &ctx->hEvent, NV01_EVENT_OS_EVENT, &eventParams));

   ctx_begin_slot(ctx);

   *ctx_out = &ctx->base;
   return VK_SUCCESS;

error:
   nvkmd_ctx_destroy(&ctx->base);
	return vkRes;
}

static void
nvkmd_nvrm_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(ctx->base.dev);
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   nvRmApiFree(&rm, ctx->hEvent);
   if (ctx->osEvent >= 0)
      close(ctx->osEvent);
   nvRmApiFree(&rm, ctx->subchannels.hCopy);
   nvRmApiFree(&rm, ctx->subchannels.hEng2d);
   nvRmApiFree(&rm, ctx->subchannels.hEng3d);
   nvRmApiFree(&rm, ctx->subchannels.hM2mf);
   nvRmApiFree(&rm, ctx->subchannels.hCompute);
   nvRmApiFree(&rm, ctx->hChannel);
   nvRmApiFree(&rm, ctx->hCtxDma);
   if (ctx->sem != NULL)
      nvkmd_mem_unref(ctx->sem);
   if (ctx->cmdBuf != NULL)
      nvkmd_mem_unref(ctx->cmdBuf);
   if (ctx->gpFifo != NULL)
      nvkmd_mem_unref(ctx->gpFifo);
   if (ctx->userD != NULL)
      nvkmd_mem_unref(ctx->userD);
   if (ctx->notifier != NULL)
      nvkmd_mem_unref(ctx->notifier);

   FREE(ctx);
}

static VkResult
nvkmd_nvrm_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t wait_count,
                            const struct vk_sync_wait *waits)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   /* In-channel semaphore acquire per wait. They must precede the exec GP
    * entries, so seal them into their own GP entry now. Safe now that flush()
    * is non-blocking: the host stalls on the acquire without the CPU holding
    * a fence wait, so wait-before-signal can't deadlock. */
   for (uint32_t i = 0; i < wait_count; i++) {
      uint64_t addr = nvkmd_nvrm_sync_gpu_addr(waits[i].sync);
      uint64_t value = nvkmd_nvrm_sync_gpu_wait_value(waits[i].sync, waits[i].wait_value);
      write_semaphore_acquire(&ctx->push, addr, value);
   }
   emit_push_segment(ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(ctx->base.dev);
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NvNotification *notifiers = ctx->notifier->map;
   NvNotification *submitTokenNotifier = &notifiers[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN];
   KeplerBControlGPFifo *userD = ctx->userD->map;
   uint64_t semAdrGpu = ctx->sem->va->addr;

   ctx->wSeq++;

   /* Channel fence: drained-state marker the blocking wait below polls on.
    * emit_push_segment turns it (plus any signal releases already written by
    * exec_ctx_signal) into a GP entry after the exec work. */
   write_semaphore_release(&ctx->push, semAdrGpu, ctx->wSeq, true);
   emit_push_segment(ctx);

   userD->GPPut = ctx->gpPut;

   volatile NvU32 *doorbell = (void*)((NvU8*)pdev->usermodeMap.address + NVC361_NOTIFY_CHANNEL_PENDING);
   *doorbell = submitTokenNotifier->info32;

   /* Non-blocking: the work is submitted; we don't wait for it to retire.
    * Tag this slot with the fence that frees it, advance to the next slot,
    * and only stall if the pipeline is full (that slot's prior fence hasn't
    * completed yet). */
   ctx->cmdBufSlotFence[ctx->cmdBufCurSlot] = ctx->wSeq;
   ctx->cmdBufCurSlot = (ctx->cmdBufCurSlot + 1) % NVRM_CTX_CMDBUF_SLOTS;

   VkResult result = VK_SUCCESS;
   while (ctx_completed(ctx) < ctx->cmdBufSlotFence[ctx->cmdBufCurSlot]) {
      if (ctx_check_error(ctx)) {
         result = vk_error(log_obj, VK_ERROR_DEVICE_LOST);
         break;
      }
      struct pollfd pfd = { .fd = ctx->osEvent, .events = POLLIN | POLLPRI };
      poll(&pfd, 1, 1000);
   }

   ctx_begin_slot(ctx);

   return result;
}

static VkResult
nvkmd_nvrm_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t exec_count,
                            const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   for (uint32_t i = 0; i < exec_count; i++) {
      write_gp_fifo_entry(ctx, &execs[i]);
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                              struct vk_object_base *log_obj,
                              uint32_t signal_count,
                              const struct vk_sync_signal *signals)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   /* Emit an in-channel semaphore release per signal, after the exec work.
    * These stay in ctx->push and are sealed into a GP entry by flush()
    * (alongside the channel fence). */
   for (uint32_t i = 0; i < signal_count; i++) {
      uint64_t addr = nvkmd_nvrm_sync_gpu_addr(signals[i].sync);
      uint64_t value = nvkmd_nvrm_sync_gpu_signal_value(signals[i].sync, signals[i].signal_value);
      write_semaphore_release(&ctx->push, addr, value, true);
   }

   return nvkmd_nvrm_exec_ctx_flush(&ctx->base, log_obj);
}

static VkResult
nvkmd_nvrm_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   VkResult result = nvkmd_nvrm_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   /* The explicit idle point: block until everything submitted has retired. */
   while (ctx_completed(ctx) < ctx->wSeq) {
      if (ctx_check_error(ctx))
         return vk_error(log_obj, VK_ERROR_DEVICE_LOST);
      struct pollfd pfd = { .fd = ctx->osEvent, .events = POLLIN | POLLPRI };
      poll(&pfd, 1, 1000);
   }

   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_nvrm_exec_ctx_ops = {
   .destroy = nvkmd_nvrm_exec_ctx_destroy,
   .wait = nvkmd_nvrm_exec_ctx_wait,
   .exec = nvkmd_nvrm_exec_ctx_exec,
   .signal = nvkmd_nvrm_exec_ctx_signal,
   .flush = nvkmd_nvrm_exec_ctx_flush,
   .sync = nvkmd_nvrm_exec_ctx_sync,
};

static VkResult
nvkmd_nvrm_create_bind_ctx(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);

   struct nvkmd_nvrm_bind_ctx *ctx = CALLOC_STRUCT(nvkmd_nvrm_bind_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvrm_bind_ctx_ops;
   ctx->base.dev = &dev->base;

   // TODO: implement using NV_MEMORY_MAPPER

   *ctx_out = &ctx->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_bind_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   FREE(ctx);
}

static VkResult
nvkmd_nvrm_bind_ctx_wait(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t wait_count,
                            const struct vk_sync_wait *waits)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t bind_count,
                            const struct nvkmd_ctx_bind *binds)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                              struct vk_object_base *log_obj,
                              uint32_t signal_count,
                              const struct vk_sync_signal *signals)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   VkResult result = nvkmd_nvrm_bind_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < signal_count; i++)
      nvkmd_nvrm_sync_cpu_signal(signals[i].sync, signals[i].signal_value);

   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_nvrm_bind_ctx_ops = {
   .destroy = nvkmd_nvrm_bind_ctx_destroy,
   .wait = nvkmd_nvrm_bind_ctx_wait,
   .bind = nvkmd_nvrm_bind_ctx_bind,
   .signal = nvkmd_nvrm_bind_ctx_signal,
   .flush = nvkmd_nvrm_bind_ctx_flush,
};

VkResult
nvkmd_nvrm_create_ctx(struct nvkmd_dev *dev,
                         struct vk_object_base *log_obj,
                         enum nvkmd_engines engines,
                         struct nvkmd_ctx **ctx_out)
{
   if (engines == NVKMD_ENGINE_BIND) {
      return nvkmd_nvrm_create_bind_ctx(dev, log_obj, ctx_out);
   } else {
      assert(!(engines & NVKMD_ENGINE_BIND));
      return nvkmd_nvrm_create_exec_ctx(dev, log_obj, engines, ctx_out);
   }
}
