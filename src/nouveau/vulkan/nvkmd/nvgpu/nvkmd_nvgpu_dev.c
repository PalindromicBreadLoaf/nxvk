/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvgpu.h"

#include "util/u_memory.h"
#include "vk_log.h"

#include <switch/nvidia/gpu.h>

VkResult
nvkmd_nvgpu_create_dev(struct nvkmd_pdev *_pdev,
                       struct vk_object_base *log_obj,
                       struct nvkmd_dev **dev_out)
{
   struct nvkmd_nvgpu_pdev *pdev = nvkmd_nvgpu_pdev(_pdev);

   struct nvkmd_nvgpu_dev *dev = CALLOC_STRUCT(nvkmd_nvgpu_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->base.ops = &nvkmd_nvgpu_dev_ops;
   dev->base.pdev = &pdev->base;

   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->heap_mutex, mtx_plain);

   /* TODO: create the GPU address space, reserve the non-fixed small-page VA
    * arena, and point base.va_start/va_end and both heaps inside it.
    */

   *dev_out = &dev->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvgpu_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_dev);

   /* TODO: tear down the VA arena and address space. */
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

const struct nvkmd_dev_ops nvkmd_nvgpu_dev_ops = {
   .destroy = nvkmd_nvgpu_dev_destroy,
   .get_gpu_timestamp = nvkmd_nvgpu_dev_get_gpu_timestamp,
   .get_drm_fd = NULL,
   .alloc_mem = nvkmd_nvgpu_alloc_mem,
   .alloc_tiled_mem = nvkmd_nvgpu_alloc_tiled_mem,
   .import_dma_buf = nvkmd_nvgpu_import_dma_buf,
   .alloc_va = nvkmd_nvgpu_alloc_va,
   .create_ctx = nvkmd_nvgpu_create_ctx,
};
