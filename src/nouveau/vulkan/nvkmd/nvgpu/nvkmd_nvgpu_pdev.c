/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvgpu.h"

#include "util/u_memory.h"
#include "vk_log.h"

VkResult
nvkmd_nvgpu_try_create_pdev(struct vk_object_base *log_obj,
                            enum nvk_debug debug_flags,
                            struct nvkmd_pdev **pdev_out)
{
   struct nvkmd_nvgpu_pdev *pdev = CALLOC_STRUCT(nvkmd_nvgpu_pdev);
   if (pdev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   pdev->base.ops = &nvkmd_nvgpu_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf = false,
      .has_get_vram_used = false,
      .has_alloc_tiled = true,
      .has_map_fixed = true,
      .has_overmap = false,
   };

   pdev->base.bind_align_B = (uint32_t)NVKMD_NVGPU_SMALL_PAGE_SIZE_B;

   /* TODO: open the GPU, populate base.dev_info from the GM20B
    * characteristics, and install the fence-backed sync type.
    */

   *pdev_out = &pdev->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvgpu_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvgpu_pdev *pdev = nvkmd_nvgpu_pdev(_pdev);

   FREE(pdev);
}

static uint64_t
nvkmd_nvgpu_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   /* The Switch has no dedicated VRAM. */
   return 0;
}

const struct nvkmd_pdev_ops nvkmd_nvgpu_pdev_ops = {
   .destroy = nvkmd_nvgpu_pdev_destroy,
   .get_vram_used = nvkmd_nvgpu_pdev_get_vram_used,
   .get_drm_primary_fd = NULL,
   .create_dev = nvkmd_nvgpu_create_dev,
};
