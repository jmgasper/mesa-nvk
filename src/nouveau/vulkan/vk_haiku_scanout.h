/*
 * Drawing straight into the screen on Haiku.
 *
 * The frame buffer the accelerant scans out from is ordinary video memory, and
 * the driver lets another program take hold of it. A renderer that copies its
 * finished frame there never sends it across the bus, which is what presenting
 * through the host costs today.
 *
 * This is a private agreement between NVK and the programs on this machine
 * that present with it, not a registered Vulkan extension. It rides on the
 * pNext chain of vkAllocateMemory, so it needs nothing of the loader: pass a
 * VkImportScanoutMemoryHAIKU and the memory that comes back is the screen,
 * with its shape filled in for you.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VK_HAIKU_SCANOUT_H
#define VK_HAIKU_SCANOUT_H

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private sType, from the range reserved for unregistered extensions. */
#define VK_STRUCTURE_TYPE_IMPORT_SCANOUT_MEMORY_HAIKU \
   ((VkStructureType)1000399000)

typedef struct VkImportScanoutMemoryHAIKU {
   VkStructureType sType;
   void *pNext;
   /* Filled in by the driver: the shape of the frame buffer that came back.
    * The memory is a linear surface of `height` rows of `rowPitch` bytes.
    */
   uint32_t width;
   uint32_t height;
   uint32_t rowPitch;
   VkDeviceSize size;
} VkImportScanoutMemoryHAIKU;

#ifdef __cplusplus
}
#endif

#endif /* VK_HAIKU_SCANOUT_H */
