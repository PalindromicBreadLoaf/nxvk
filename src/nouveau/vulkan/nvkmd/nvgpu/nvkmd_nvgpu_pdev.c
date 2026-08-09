/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nvkmd_nvgpu.h"

#include "util/u_memory.h"
#include "vk_log.h"

#include <stdio.h>
#include <string.h>

#include <switch/result.h>
#include <switch/services/nv.h>
#include <switch/nvidia/gpu.h>

static void
nvkmd_nvgpu_get_dev_info(const nvioctl_gpu_characteristics *chars,
                         struct nv_device_info *info)
{
   *info = (struct nv_device_info) {
      .type = NV_DEVICE_TYPE_SOC,

      /* GM20B: family arch 0x120 | impl 0xb => chipset 0x12b. */
      .chipset = (uint16_t)((chars->arch & 0xfff0) | (chars->impl & 0x000f)),

      /* SM 5.3. */
      .sm = (uint8_t)(((chars->sm_arch_sm_version >> 8) & 0xff) * 10 +
                      (chars->sm_arch_sm_version & 0xff)),

      .gpc_count = chars->num_gpc,
      .tpc_count = chars->num_gpc * chars->num_tpc_per_gpc,

      /* Maxwell has one SMM per TPC each running up to 64 warps. */
      .mp_per_tpc = 1,
      .max_warps_per_mp = 64,

      /* Cortex-A57 D-cache line must be non-zero */
      .nc_atom_size_B = 64,

      .cls_copy = chars->dma_copy_class,
      .cls_eng2d = chars->twod_class,
      .cls_eng3d = chars->threed_class,
      .cls_m2mf = chars->inline_to_memory_class,
      .cls_compute = chars->compute_class,

      /* GM20B has 64 kB of shared memory per SMM and no configurable split,
       * but like the rest of Kepler through Pascal a single workgroup can only
       * address 48 kB of it.
       */
      .sm_smem_sizes_kB = { 64 },
      .sm_smem_size_count = 1,
      .max_smem_per_wg_kB = 48,

      .vram_size_B = 0,
      .bar_size_B = 0,
   };

   memcpy(info->chipset_name, &chars->chipname, sizeof(chars->chipname));
   info->chipset_name[sizeof(chars->chipname)] = '\0';

   snprintf(info->device_name, sizeof(info->device_name), "NVIDIA Tegra X1");
}

VkResult
nvkmd_nvgpu_try_create_pdev(struct vk_object_base *log_obj,
                            enum nvk_debug debug_flags,
                            struct nvkmd_pdev **pdev_out)
{
   Result rc = nvInitialize();
   if (R_FAILED(rc))
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "nvInitialize() failed: 0x%x", (unsigned)rc);

   rc = nvGpuInit();
   if (R_FAILED(rc)) {
      nvExit();
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "nvGpuInit() failed: 0x%x", (unsigned)rc);
   }

   rc = nvMapInit();
   if (R_FAILED(rc)) {
      nvGpuExit();
      nvExit();
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "nvMapInit() failed: 0x%x", (unsigned)rc);
   }

   /* Channel submit fences are waited on through nvFenceWait. */
   rc = nvFenceInit();
   if (R_FAILED(rc)) {
      nvMapExit();
      nvGpuExit();
      nvExit();
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "nvFenceInit() failed: 0x%x", (unsigned)rc);
   }

   const nvioctl_gpu_characteristics *chars = nvGpuGetCharacteristics();
   if (chars == NULL) {
      nvFenceExit();
      nvMapExit();
      nvGpuExit();
      nvExit();
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "nvGpuGetCharacteristics() returned NULL");
   }

   struct nvkmd_nvgpu_pdev *pdev = CALLOC_STRUCT(nvkmd_nvgpu_pdev);
   if (pdev == NULL) {
      nvFenceExit();
      nvMapExit();
      nvGpuExit();
      nvExit();
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pdev->base.ops = &nvkmd_nvgpu_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   nvkmd_nvgpu_get_dev_info(chars, &pdev->base.dev_info);

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf = false,
      .has_get_vram_used = false,
      .has_alloc_tiled = true,
      .has_map_fixed = true,
      .has_overmap = false,
      .has_sparse = false,
   };

   pdev->base.bind_align_B = (uint32_t)NVKMD_NVGPU_SMALL_PAGE_SIZE_B;

   /* Binary NvFence syncobj, plus a timeline emulated on top of it. */
   pdev->syncobj_timeline_type =
      vk_sync_timeline_get_type(&nvkmd_nvgpu_syncobj_type);
   pdev->sync_types[0] = &nvkmd_nvgpu_syncobj_type;
   pdev->sync_types[1] = &pdev->syncobj_timeline_type.sync;
   pdev->sync_types[2] = NULL;
   pdev->base.sync_types = pdev->sync_types;

   *pdev_out = &pdev->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvgpu_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvgpu_pdev *pdev = nvkmd_nvgpu_pdev(_pdev);

   nvFenceExit();
   nvMapExit();
   nvGpuExit();
   nvExit();
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
