/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_NVRM_H
#define NVKMD_NVRM_H 1

#include "nvkmd/nvkmd.h"
#include "util/vma.h"
#include "util/u_dynarray.h"
#include "util/simple_mtx.h"
#include "vk_sync.h"
#include "nv_push.h"

#include <sys/types.h>

#include "nvRmApi.h"

#include "nvtypes.h"
#include "ctrl/ctrl2080/ctrl2080fb.h" // NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT

struct nvrm_ws_bo;
struct nvrm_ws_context;
struct nvrm_ws_device;

struct nvkmd_nvrm_pdev {
   struct nvkmd_pdev base;

   struct vk_sync_type syncobj_sync_type;
   const struct vk_sync_type *sync_types[2];

   char *devName;
   int ctlFd;
   int devFd;
   NvHandle hClient;
   NvHandle hDevice;
   NvHandle hSubdevice;
   NvHandle hUsermode;
   struct NvRmApiMapping usermodeMap;
   NvHandle hVaSpace;
   struct NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_PARAMS semSurfLayout;
   uint32_t channelClass;

   uint32_t numClasses;
   uint32_t *classList;
};

NVKMD_DECL_SUBCLASS(pdev, nvrm);

VkResult nvkmd_nvrm_try_create_pdev(struct _drmDevice *drm_device,
                                       struct vk_object_base *log_obj,
                                       enum nvk_debug debug_flags,
                                       struct nvkmd_pdev **pdev_out);

/* Number of cmdBuf ring slots: how many submissions may be in flight before
 * flush() has to wait for the oldest to retire. */
#define NVRM_CTX_CMDBUF_SLOTS 32

struct nvkmd_nvrm_dev {
   struct nvkmd_dev base;
   struct hash_table *mappings;

   /* Pool of plain semaphore words backing non-shareable vk_syncs. Each page
    * is one GART allocation sliced into fixed-size slots handed out by index;
    * pages grow lazily and are freed only at device teardown. Shareable syncs
    * get their own dedicated mem instead so they can be exported. */
   simple_mtx_t sync_mutex;
   struct util_dynarray sync_pages;  /* struct nvkmd_nvrm_sync_page * */
   /* Idle (fd, hEvent) wake-event pairs kept for reuse across CPU waits, so a
    * wait need not open/alloc/free an OS event every time. Guarded by
    * sync_mutex. */
   struct util_dynarray sync_events;  /* struct nvkmd_nvrm_sync_event */
};

NVKMD_DECL_SUBCLASS(dev, nvrm);

VkResult nvkmd_nvrm_create_dev(struct nvkmd_pdev *pdev,
                                  struct vk_object_base *log_obj,
                                  struct nvkmd_dev **dev_out);


VkResult
nvkmd_nvrm_enum_pdev(struct vk_object_base *log_obj,
                     enum nvk_debug debug_flags,
                     nvkmd_enum_pdev_visitor visitor,
                     void *arg);


struct nvkmd_nvrm_mem {
   struct nvkmd_mem base;
   NvHandle hMemoryPhys;
   bool isSystemMem;

   /* For VK_EXT_external_memory_host imports: the application-owned host
    * pointer backing this memory. When non-NULL, map() returns it directly
    * instead of asking RM for a CPU mapping. */
   void *userptr;
};

NVKMD_DECL_SUBCLASS(mem, nvrm);

VkResult nvkmd_nvrm_alloc_mem(struct nvkmd_dev *dev,
                                 struct vk_object_base *log_obj,
                                 uint64_t size_B, uint64_t align_B,
                                 enum nvkmd_mem_flags flags,
                                 struct nvkmd_mem **mem_out);

VkResult nvkmd_nvrm_alloc_tiled_mem(struct nvkmd_dev *dev,
                                       struct vk_object_base *log_obj,
                                       uint64_t size_B, uint64_t align_B,
                                       uint8_t pte_kind, uint16_t tile_mode,
                                       enum nvkmd_mem_flags flags,
                                       struct nvkmd_mem **mem_out);

VkResult nvkmd_nvrm_import_dma_buf(struct nvkmd_dev *dev,
                                      struct vk_object_base *log_obj,
                                      int fd, struct nvkmd_mem **mem_out);

VkResult nvkmd_nvrm_import_userptr(struct nvkmd_dev *dev,
                                      struct vk_object_base *log_obj,
                                      void *userptr, uint64_t size_B,
                                      enum nvkmd_mem_flags flags,
                                      struct nvkmd_mem **mem_out);

struct nvkmd_nvrm_va {
   struct nvkmd_va base;
   NvHandle hMemoryPhys;
   NvHandle hMemoryVirt;
};

NVKMD_DECL_SUBCLASS(va, nvrm);

VkResult nvkmd_nvrm_alloc_va(struct nvkmd_dev *dev,
                                struct vk_object_base *log_obj,
                                enum nvkmd_va_flags flags, uint8_t pte_kind,
                                uint64_t size_B, uint64_t align_B,
                                uint64_t fixed_addr, struct nvkmd_va **va_out);

struct nvkmd_nvrm_exec_ctx {
   struct nvkmd_ctx base;
   struct nvkmd_mem *notifier;
   struct nvkmd_mem *userD;
   struct nvkmd_mem *gpFifo;
   struct nvkmd_mem *cmdBuf;
   struct nvkmd_mem *sem;
   NvHandle hCtxDma;
   NvHandle hChannel;
   struct {
	   NvHandle hCopy;
	   NvHandle hEng2d;
	   NvHandle hEng3d;
	   NvHandle hM2mf;
	   NvHandle hCompute;
   } subchannels;
   int osEvent;
   NvHandle hEvent;
   uint64_t wSeq;
   uint64_t gpGet;
   uint64_t gpPut;
   struct nv_push push;
   /* Dwords of the current cmdBuf slot already turned into GP entries this
    * submit cycle, so acquire methods (emitted before the execs) and
    * release/fence methods (after) become separate GP entries. */
   uint32_t cmdBufSubmittedDw;
   /* cmdBuf is a ring of NVRM_CTX_CMDBUF_SLOTS fixed-size slots so flush()
    * needn't block: each submit cycle uses one slot, tagged with the fence
    * (wSeq) that retires it, and a slot is reused only once that fence has
    * completed. */
   uint32_t cmdBufCurSlot;
   uint64_t cmdBufSlotFence[NVRM_CTX_CMDBUF_SLOTS];
};

NVKMD_DECL_SUBCLASS(ctx, nvrm_exec);

struct nvkmd_nvrm_bind_ctx {
   struct nvkmd_ctx base;
};

NVKMD_DECL_SUBCLASS(ctx, nvrm_bind);

VkResult nvkmd_nvrm_create_ctx(struct nvkmd_dev *dev,
                                  struct vk_object_base *log_obj,
                                  enum nvkmd_engines engines,
                                  struct nvkmd_ctx **ctx_out);

struct nvkmd_nvrm_sync {
   struct vk_sync base;
   struct nvkmd_nvrm_dev *dev;
   /* The backing slot (nvrm_sync_data: payload word + binary coordination
    * header): `mem` plus a byte `offset`. For pooled (non-shareable) syncs
    * `mem` is a device pool page and `offset` selects the slot; for shareable
    * syncs `mem` is dedicated and offset 0. */
   struct nvkmd_mem *mem;
   uint64_t offset;
   bool dedicated;
};

void
nvkmd_nvrm_sync_finish(struct vk_device *device,
                       struct vk_sync *sync);

/* Free every semaphore-word pool page. Called from device teardown. */
void
nvkmd_nvrm_sync_pool_finish(struct nvkmd_nvrm_dev *dev);

/* CPU-signal an nvkmd_nvrm vk_sync to `value` (timeline) or signaled (binary)
 * and wake any blocked CPU waiters via the FIFO trigger. Used as a bridge by
 * the bind ctx (no GPU release path); a no-op for syncs of another type. */
void
nvkmd_nvrm_sync_cpu_signal(struct vk_sync *sync, uint64_t value);

/* GPU-side accessors used by the submission path to emit in-channel SEM
 * acquire (wait) and release (signal) for a sync. The signal and wait values
 * are split: for a binary sync each hands out a fresh sequence number (see
 * nvkmd_nvrm_sync.c) so the payload is monotonic and a post-submit reset can
 * never lower it beneath an in-flight acquire. */
uint64_t nvkmd_nvrm_sync_gpu_addr(struct vk_sync *sync);
uint64_t nvkmd_nvrm_sync_gpu_signal_value(struct vk_sync *sync, uint64_t value);
uint64_t nvkmd_nvrm_sync_gpu_wait_value(struct vk_sync *sync, uint64_t value);

static inline bool
vk_sync_type_is_nvkmd_nvrm_sync(const struct vk_sync_type *type)
{
   return type->finish == nvkmd_nvrm_sync_finish;
}

static inline struct nvkmd_nvrm_sync *
vk_sync_as_nvkmd_nvrm_sync(struct vk_sync *sync)
{
   if (!vk_sync_type_is_nvkmd_nvrm_sync(sync->type))
      return NULL;

   return container_of(sync, struct nvkmd_nvrm_sync, base);
}

struct vk_sync_type
nvkmd_nvrm_sync_get_type(struct nvkmd_nvrm_pdev *pdev);


static inline void
nvkmd_nvrm_dev_api_ctl(struct nvkmd_nvrm_pdev *pdev, struct NvRmApi *rm)
{
   rm->fd = pdev->ctlFd;
   rm->hClient = pdev->hClient;
   rm->nodeName = pdev->devName;
}

#endif /* NVKMD_DRM_H */
