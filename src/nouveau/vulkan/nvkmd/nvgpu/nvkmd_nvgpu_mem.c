/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvgpu.h"

#include "util/u_memory.h"
#include "vk_log.h"

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

VkResult
nvkmd_nvgpu_alloc_tiled_mem(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            uint64_t size_B, uint64_t align_B,
                            uint8_t pte_kind, uint16_t tile_mode,
                            enum nvkmd_mem_flags flags,
                            struct nvkmd_mem **mem_out)
{
   /* TODO: allocate an nvmap object, wrap it in nvkmd_nvgpu_mem, and bind a
    * VA range from the device arena.
    */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
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

   FREE(mem);
}

static VkResult
nvkmd_nvgpu_mem_map(struct nvkmd_mem *_mem,
                    struct vk_object_base *log_obj,
                    enum nvkmd_mem_map_flags map_flags,
                    void *fixed_addr,
                    void **map_out)
{
   /* TODO: return the nvmap CPU mapping. */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
nvkmd_nvgpu_mem_unmap(struct nvkmd_mem *_mem,
                      enum nvkmd_mem_map_flags flags,
                      void *map)
{
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
