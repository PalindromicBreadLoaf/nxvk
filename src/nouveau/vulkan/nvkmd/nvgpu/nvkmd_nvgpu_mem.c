/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvgpu.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>

#include <switch/result.h>

VkResult
nvkmd_nvgpu_alloc_mem(struct nvkmd_dev *dev,
                      struct vk_object_base *log_obj,
                      uint64_t size_B, uint64_t align_B,
                      enum nvkmd_mem_flags flags,
                      struct nvkmd_mem **mem_out)
{
   return nvkmd_nvgpu_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                      0 /* pte_kind */, 0 /* tile_mode */,
                                      flags, mem_out);
}

static VkResult
create_mem_or_close_nvmap(struct nvkmd_nvgpu_dev *dev,
                          struct vk_object_base *log_obj,
                          enum nvkmd_mem_flags mem_flags,
                          NvMap *nvmap,
                          enum nvkmd_va_flags va_flags,
                          uint8_t pte_kind, uint64_t va_align_B,
                          struct nvkmd_mem **mem_out)
{
   const uint64_t size_B = nvmap->size;
   VkResult result;

   struct nvkmd_nvgpu_mem *mem = CALLOC_STRUCT(nvkmd_nvgpu_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_nvmap;
   }

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_nvgpu_mem_ops,
                  mem_flags, size_B, dev->base.pdev->bind_align_B);
   mem->nvmap = *nvmap;

   result = nvkmd_dev_alloc_va(&dev->base, log_obj,
                               va_flags, pte_kind,
                               size_B, va_align_B,
                               0 /* fixed_addr */,
                               &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0 /* va_offset_B */,
                              &mem->base, 0 /* mem_offset_B */, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;

   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_mem:
   FREE(mem);
fail_nvmap:
   nvMapClose(nvmap);
   align_free(nvmap->cpu_addr);

   return result;
}

VkResult
nvkmd_nvgpu_alloc_tiled_mem(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            uint64_t size_B, uint64_t align_B,
                            uint8_t pte_kind, uint16_t tile_mode,
                            enum nvkmd_mem_flags flags,
                            struct nvkmd_mem **mem_out)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_dev);

   /* LOCAL/GART/VRAM placement does not change the allocation. */
   assert(util_bitcount(flags & (NVKMD_MEM_LOCAL |
                                 NVKMD_MEM_GART |
                                 NVKMD_MEM_VRAM)) == 1);

   const uint32_t mem_align_B = _dev->pdev->bind_align_B;
   size_B = align64(size_B, mem_align_B);

   if (size_B > UINT32_MAX) {
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "allocation of 0x%" PRIx64 " bytes is too large",
                       size_B);
   }

   assert(util_is_power_of_two_or_zero64(align_B));
   const uint64_t va_align_B = MAX2(mem_align_B, align_B);

   void *cpu_addr = align_malloc(size_B, mem_align_B);
   if (cpu_addr == NULL)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%m");

   /* The GPU is not IO-coherent, so an uncached CPU view sees GPU writes once they are
    * flushed out of the GPU L2 at submit time.
    */
   NvMap nvmap;
   Result rc = nvMapCreate(&nvmap, cpu_addr, size_B, mem_align_B,
                           (NvKind)pte_kind, false /* is_cpu_cacheable */);
   if (R_FAILED(rc)) {
      align_free(cpu_addr);
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "nvMapCreate() failed: 0x%x", (unsigned)rc);
   }

   return create_mem_or_close_nvmap(dev, log_obj, flags, &nvmap,
                                    0 /* va_flags */, pte_kind, va_align_B,
                                    mem_out);
}

VkResult
nvkmd_nvgpu_import_dma_buf(struct nvkmd_dev *_dev,
                           struct vk_object_base *log_obj,
                           int fd, struct nvkmd_mem **mem_out)
{
   /* dma-buf import is unused on the Switch. */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
nvkmd_nvgpu_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   nvkmd_va_free(mem->base.va);

   void *cpu_addr = mem->nvmap.cpu_addr;
   nvMapClose(&mem->nvmap);
   align_free(cpu_addr);

   FREE(mem);
}

static VkResult
nvkmd_nvgpu_mem_map(struct nvkmd_mem *_mem,
                    struct vk_object_base *log_obj,
                    enum nvkmd_mem_map_flags map_flags,
                    void *fixed_addr,
                    void **map_out)
{
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   /* The CPU pointer is fixed at allocation time, so a fixed-address request
    * can only succeed if it happens to match.
    */
   void *map = nvMapGetCpuAddr(&mem->nvmap);
   if ((map_flags & NVKMD_MEM_MAP_FIXED) && fixed_addr != map)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   *map_out = map;

   return VK_SUCCESS;
}

static void
nvkmd_nvgpu_mem_unmap(struct nvkmd_mem *_mem,
                      enum nvkmd_mem_map_flags flags,
                      void *map)
{
   /* The CPU mapping lives for the lifetime of the nvmap. */
}

static VkResult
nvkmd_nvgpu_mem_overmap(struct nvkmd_mem *_mem,
                        struct vk_object_base *log_obj,
                        enum nvkmd_mem_map_flags flags,
                        void *map)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_mem_export_dma_buf(struct nvkmd_mem *_mem,
                               struct vk_object_base *log_obj,
                               int *fd_out)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static uint32_t
nvkmd_nvgpu_mem_log_handle(struct nvkmd_mem *_mem)
{
   return nvkmd_nvgpu_mem(_mem)->nvmap.handle;
}

const struct nvkmd_mem_ops nvkmd_nvgpu_mem_ops = {
   .free = nvkmd_nvgpu_mem_free,
   .map = nvkmd_nvgpu_mem_map,
   .unmap = nvkmd_nvgpu_mem_unmap,
   .overmap = nvkmd_nvgpu_mem_overmap,
   .export_dma_buf = nvkmd_nvgpu_mem_export_dma_buf,
   .log_handle = nvkmd_nvgpu_mem_log_handle,
};
