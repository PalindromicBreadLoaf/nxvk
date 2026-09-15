/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nvkmd_nvgpu.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>
#include <stdio.h>

#include <switch/result.h>

static VkResult MUST_CHECK
alloc_va_addr_locked(struct nvkmd_nvgpu_dev *dev,
                     struct vk_object_base *log_obj,
                     enum nvkmd_va_flags flags, bool big_page,
                     uint64_t size_B, uint64_t align_B,
                     uint64_t fixed_addr, uint64_t *addr_out)
{
   if (big_page) {
      assert(!(flags & (NVKMD_VA_ALLOC_FIXED | NVKMD_VA_REPLAY)));

      *addr_out = util_vma_heap_alloc(&dev->big_heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate big-page virtual address range");

      return VK_SUCCESS;
   }

   if (flags & NVKMD_VA_ALLOC_FIXED) {
      assert(flags & NVKMD_VA_REPLAY);

      if (fixed_addr & (align_B - 1)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Unaligned capture address: 0x%" PRIx64, fixed_addr);
      }

      if (!util_vma_heap_alloc_addr(&dev->replay_heap, fixed_addr, size_B)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Replay address collision: 0x%" PRIx64, fixed_addr);
      }

      *addr_out = fixed_addr;
   } else if (flags & NVKMD_VA_REPLAY) {
      *addr_out = util_vma_heap_alloc(&dev->replay_heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address range");
   } else {
      *addr_out = util_vma_heap_alloc(&dev->heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address range");
   }

   return VK_SUCCESS;
}

static VkResult MUST_CHECK
alloc_va_addr(struct nvkmd_nvgpu_dev *dev,
              struct vk_object_base *log_obj,
              enum nvkmd_va_flags flags, bool big_page,
              uint64_t size_B, uint64_t align_B,
              uint64_t fixed_addr, uint64_t *addr_out)
{
   simple_mtx_lock(&dev->heap_mutex);
   VkResult result = alloc_va_addr_locked(dev, log_obj, flags, big_page,
                                          size_B, align_B,
                                          fixed_addr, addr_out);
   simple_mtx_unlock(&dev->heap_mutex);
   return result;
}

static void
free_va_addr(struct nvkmd_nvgpu_dev *dev,
             enum nvkmd_va_flags flags, bool big_page,
             uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&dev->heap_mutex);
   if (big_page)
      util_vma_heap_free(&dev->big_heap, addr, size_B);
   else if (flags & NVKMD_VA_REPLAY)
      util_vma_heap_free(&dev->replay_heap, addr, size_B);
   else
      util_vma_heap_free(&dev->heap, addr, size_B);
   simple_mtx_unlock(&dev->heap_mutex);
}

VkResult
nvkmd_nvgpu_alloc_va_ex(struct nvkmd_dev *_dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_va_flags flags, uint8_t pte_kind,
                        uint64_t size_B, uint64_t align_B,
                        uint64_t fixed_addr, bool big_page,
                        struct nvkmd_va **va_out)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_dev);
   VkResult result;

   assert(!big_page || dev->big_page_size_B > 0);

   struct nvkmd_nvgpu_va *va = CALLOC_STRUCT(nvkmd_nvgpu_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const uint64_t min_align_B =
      big_page ? dev->big_page_size_B : _dev->pdev->bind_align_B;
   size_B = align64(size_B, min_align_B);

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, min_align_B);

   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));

   result = alloc_va_addr(dev, log_obj, flags, big_page, size_B, align_B,
                          fixed_addr, &va->base.addr);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   va->base.ops = &nvkmd_nvgpu_va_ops;
   va->base.dev = &dev->base;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;
   va->big_page = big_page;

   if (big_page && unlikely(_dev->pdev->debug_flags & NVK_DEBUG_VM)) {
      fprintf(stderr, "alloc big-page va [0x%" PRIx64 ", 0x%" PRIx64 ")\n",
              va->base.addr, va->base.addr + size_B);
   }

   *va_out = &va->base;

   return VK_SUCCESS;

fail_alloc:
   FREE(va);

   return result;
}

VkResult
nvkmd_nvgpu_alloc_va(struct nvkmd_dev *dev,
                     struct vk_object_base *log_obj,
                     enum nvkmd_va_flags flags, uint8_t pte_kind,
                     uint64_t size_B, uint64_t align_B,
                     uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   return nvkmd_nvgpu_alloc_va_ex(dev, log_obj, flags, pte_kind,
                                  size_B, align_B, fixed_addr,
                                  false /* big_page */, va_out);
}

static void
nvkmd_nvgpu_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_va->dev);
   struct nvkmd_nvgpu_va *va = nvkmd_nvgpu_va(_va);

   /* The mem path binds one whole-buffer mapping at the VA base. */
   nvioctlNvhostAsGpu_UnmapBuffer(dev->addr_space.fd, va->base.addr);

   free_va_addr(dev, va->base.flags, va->big_page, va->base.addr,
                va->base.size_B);

   FREE(va);
}

static VkResult
nvkmd_nvgpu_va_bind_mem(struct nvkmd_va *_va,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        struct nvkmd_mem *_mem,
                        uint64_t mem_offset_B,
                        uint64_t range_B)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_va->dev);
   struct nvkmd_nvgpu_va *va = nvkmd_nvgpu_va(_va);
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   assert(_mem->dev == _va->dev);

   const uint32_t map_flags = NvMapBufferFlags_FixedOffset |
      ((_mem->flags & NVKMD_MEM_GPU_UNCACHED) ? 0
                                              : NvMapBufferFlags_IsCacheable);

   const uint64_t page_size_B =
      va->big_page ? dev->big_page_size_B : NVKMD_NVGPU_SMALL_PAGE_SIZE_B;

   const iova_t target = va->base.addr + va_offset_B;
   iova_t mapped = 0;
   Result rc = nvioctlNvhostAsGpu_MapBufferEx(
      dev->addr_space.fd, map_flags,
      (uint32_t)va->base.pte_kind, mem->nvmap.handle,
      (uint32_t)page_size_B,
      mem_offset_B /* buffer_offset */, range_B /* mapping_size */,
      target /* input_offset */, &mapped);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "MapBufferEx() failed: 0x%x", (unsigned)rc);
   }

   assert(mapped == target);

   if (unlikely(_va->dev->pdev->debug_flags & NVK_DEBUG_VM)) {
      fprintf(stderr, "  map nvmap<0x%" PRIx32 "> at 0x%" PRIx64
                      " kind=0x%02x page=0x%" PRIx64 " map_flags=0x%03" PRIx32
                      " mem_flags=0x%02x%s\n",
              mem->nvmap.handle, (uint64_t)target,
              (unsigned)va->base.pte_kind, page_size_B,
              map_flags, (unsigned)_mem->flags,
              (map_flags & NvMapBufferFlags_IsCacheable) ? " gpu-cached" : "");
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvgpu_va_unbind(struct nvkmd_va *_va,
                      struct vk_object_base *log_obj,
                      uint64_t va_offset_B,
                      uint64_t range_B)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_va->dev);
   struct nvkmd_nvgpu_va *va = nvkmd_nvgpu_va(_va);

   /* UnmapBuffer keys on the mapping base so range_B is implied by the bind. */
   Result rc = nvioctlNvhostAsGpu_UnmapBuffer(dev->addr_space.fd,
                                              va->base.addr + va_offset_B);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "UnmapBuffer() failed: 0x%x", (unsigned)rc);
   }

   return VK_SUCCESS;
}

const struct nvkmd_va_ops nvkmd_nvgpu_va_ops = {
   .free = nvkmd_nvgpu_va_free,
   .bind_mem = nvkmd_nvgpu_va_bind_mem,
   .unbind = nvkmd_nvgpu_va_unbind,
};
