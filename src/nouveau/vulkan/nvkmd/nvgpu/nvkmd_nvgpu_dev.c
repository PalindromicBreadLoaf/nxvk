/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nvkmd_nvgpu.h"

#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>
#include <stdio.h>

#include <switch/nvidia/gpu.h>
#include <switch/result.h>

VkResult
nvkmd_nvgpu_create_dev(struct nvkmd_pdev *_pdev,
                       struct vk_object_base *log_obj,
                       struct nvkmd_dev **dev_out)
{
   struct nvkmd_nvgpu_pdev *pdev = nvkmd_nvgpu_pdev(_pdev);
   VkResult result;

   struct nvkmd_nvgpu_dev *dev = CALLOC_STRUCT(nvkmd_nvgpu_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->base.ops = &nvkmd_nvgpu_dev_ops;
   dev->base.pdev = &pdev->base;

   list_inithead(&dev->base.mems);
   list_inithead(&dev->mem_cache);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->heap_mutex, mtx_plain);
   simple_mtx_init(&dev->mem_cache_mutex, mtx_plain);

   /* The address space carries the big-page half of the split.
    * every small-page mapping lands in the low half out of the arena reserved below.
    */
   const nvioctl_gpu_characteristics *chars = nvGpuGetCharacteristics();
   Result rc = nvAddressSpaceCreate(&dev->addr_space, chars->big_page_size);
   if (R_FAILED(rc)) {
      result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                         "nvAddressSpaceCreate() failed: 0x%x", (unsigned)rc);
      goto fail_locks;
   }

   /* A FIXED map is only legal inside a region reserved non-fixed first. */
   const uint32_t arena_pages =
      (uint32_t)(NVKMD_NVGPU_VA_ARENA_SIZE_B / NVKMD_NVGPU_SMALL_PAGE_SIZE_B);
   iova_t arena_addr = 0;
   rc = nvioctlNvhostAsGpu_AllocSpace(dev->addr_space.fd, arena_pages,
                                      (uint32_t)NVKMD_NVGPU_SMALL_PAGE_SIZE_B,
                                      0 /* non-fixed, non-sparse */,
                                      NVKMD_NVGPU_SMALL_PAGE_SIZE_B /* align */,
                                      &arena_addr);
   if (R_FAILED(rc)) {
      result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                         "GPU VA arena reservation failed: 0x%x", (unsigned)rc);
      goto fail_as;
   }

   dev->va_arena_addr = arena_addr;
   dev->va_arena_size_B = NVKMD_NVGPU_VA_ARENA_SIZE_B;

   /* NVK's default heap sits outside nvgpu's addressable range, so point the
    * whole usable range and both heaps inside the arena.
    */
   dev->base.va_start = arena_addr;
   dev->base.va_end = arena_addr + NVKMD_NVGPU_VA_ARENA_SIZE_B;

   const uint64_t replay_size_B = NVKMD_NVGPU_REPLAY_HEAP_SIZE_B;
   const uint64_t heap_size_B = NVKMD_NVGPU_VA_ARENA_SIZE_B - replay_size_B;
   util_vma_heap_init(&dev->heap, arena_addr, heap_size_B);
   util_vma_heap_init(&dev->replay_heap, arena_addr + heap_size_B, replay_size_B);

   if (pdev->base.kmd_info.has_compression) {
      const uint64_t big_page_size_B = chars->big_page_size;
      const uint32_t big_pages =
         (uint32_t)(NVKMD_NVGPU_BIG_ARENA_SIZE_B / big_page_size_B);
      iova_t big_arena_addr = 0;

      rc = nvioctlNvhostAsGpu_AllocSpace(dev->addr_space.fd, big_pages,
                                         (uint32_t)big_page_size_B,
                                         0 /* non-fixed, non-sparse */,
                                         big_page_size_B /* align */,
                                         &big_arena_addr);
      if (R_FAILED(rc)) {
         result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                            "big-page VA arena reservation failed: 0x%x",
                            (unsigned)rc);
         goto fail_heaps;
      }

      dev->va_big_arena_addr = big_arena_addr;
      dev->va_big_arena_size_B = NVKMD_NVGPU_BIG_ARENA_SIZE_B;
      dev->big_page_size_B = big_page_size_B;
      util_vma_heap_init(&dev->big_heap, big_arena_addr,
                         NVKMD_NVGPU_BIG_ARENA_SIZE_B);

      if (unlikely(pdev->base.debug_flags & NVK_DEBUG_VM)) {
         fprintf(stderr, "big-page va arena [0x%" PRIx64 ", 0x%" PRIx64 ") "
                         "page=0x%" PRIx64 "\n",
                 (uint64_t)big_arena_addr,
                 (uint64_t)big_arena_addr + NVKMD_NVGPU_BIG_ARENA_SIZE_B,
                 big_page_size_B);
      }
   }

   *dev_out = &dev->base;

   return VK_SUCCESS;

fail_heaps:
   util_vma_heap_finish(&dev->replay_heap);
   util_vma_heap_finish(&dev->heap);
   nvioctlNvhostAsGpu_FreeSpace(dev->addr_space.fd, arena_addr, arena_pages,
                                (uint32_t)NVKMD_NVGPU_SMALL_PAGE_SIZE_B);
fail_as:
   nvAddressSpaceClose(&dev->addr_space);
fail_locks:
   simple_mtx_destroy(&dev->mem_cache_mutex);
   simple_mtx_destroy(&dev->heap_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);
   FREE(dev);
   return result;
}

static void
nvkmd_nvgpu_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_dev);

   if (unlikely(_dev->pdev->debug_flags & NVK_DEBUG_VM)) {
      fprintf(stderr, "mem cache: %" PRIu64 " hits, %" PRIu64 " misses, "
                      "%" PRIu32 " entries / 0x%" PRIx64 " bytes held\n",
              dev->mem_cache_hits, dev->mem_cache_misses,
              dev->mem_cache_count, dev->mem_cache_size_B);
   }

   nvkmd_nvgpu_mem_cache_trim(dev);

   if (dev->va_big_arena_size_B > 0) {
      const uint32_t big_pages =
         (uint32_t)(dev->va_big_arena_size_B / dev->big_page_size_B);
      uint32_t live = 0;

      simple_mtx_lock(&_dev->mems_mutex);
      list_for_each_entry(struct nvkmd_mem, mem, &_dev->mems, link) {
         if (mem->va == NULL ||
             mem->va->addr < dev->va_big_arena_addr ||
             mem->va->addr >= dev->va_big_arena_addr + dev->va_big_arena_size_B)
            continue;

         nvioctlNvhostAsGpu_UnmapBuffer(dev->addr_space.fd, mem->va->addr);
         live++;
      }
      simple_mtx_unlock(&_dev->mems_mutex);

      util_vma_heap_finish(&dev->big_heap);
      const Result rc =
         nvioctlNvhostAsGpu_FreeSpace(dev->addr_space.fd,
                                      dev->va_big_arena_addr, big_pages,
                                      (uint32_t)dev->big_page_size_B);

      if (unlikely(_dev->pdev->debug_flags & NVK_DEBUG_VM)) {
         fprintf(stderr, "big-page arena: unmapped %" PRIu32 " leaked "
                         "mappings, FreeSpace -> 0x%x\n",
                 live, (unsigned)rc);
      }
   }

   util_vma_heap_finish(&dev->replay_heap);
   util_vma_heap_finish(&dev->heap);

   const uint32_t arena_pages =
      (uint32_t)(dev->va_arena_size_B / NVKMD_NVGPU_SMALL_PAGE_SIZE_B);
   nvioctlNvhostAsGpu_FreeSpace(dev->addr_space.fd, dev->va_arena_addr,
                                arena_pages,
                                (uint32_t)NVKMD_NVGPU_SMALL_PAGE_SIZE_B);
   nvAddressSpaceClose(&dev->addr_space);

   simple_mtx_destroy(&dev->mem_cache_mutex);
   simple_mtx_destroy(&dev->heap_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);
   FREE(dev);
}

static uint64_t
nvkmd_nvgpu_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   u64 ts = 0;
   nvGpuGetTimestamp(&ts);
   return ts;
}

static bool
nvkmd_nvgpu_dev_get_sync_syncpt(struct nvkmd_dev *_dev, struct vk_sync *sync,
                                uint32_t *id_out, uint32_t *value_out)
{
   NvFence fence;

   if (nvkmd_nvgpu_syncobj_get_fence(sync, &fence) != NVKMD_NVGPU_FENCE_PENDING)
      return false;

   *id_out = fence.id;
   *value_out = fence.value;

   return true;
}

const struct nvkmd_dev_ops nvkmd_nvgpu_dev_ops = {
   .destroy = nvkmd_nvgpu_dev_destroy,
   .get_gpu_timestamp = nvkmd_nvgpu_dev_get_gpu_timestamp,
   .get_drm_fd = NULL,
   .alloc_mem = nvkmd_nvgpu_alloc_mem,
   .alloc_tiled_mem = nvkmd_nvgpu_alloc_tiled_mem,
   .import_dma_buf = nvkmd_nvgpu_import_dma_buf,
   .alloc_va = nvkmd_nvgpu_alloc_va,
   .create_ctx = nvkmd_nvgpu_create_ctx,
   .get_sync_syncpt = nvkmd_nvgpu_dev_get_sync_syncpt,
};
