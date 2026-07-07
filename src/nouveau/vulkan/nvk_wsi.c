/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_wsi.h"
#include "nvk_image.h"
#include "nvk_instance.h"
#include "nvkmd/nvkmd.h"
#include "wsi_common.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
}

#ifdef VK_USE_PLATFORM_VI_NN
/* NvKind_Generic_16BX2 is the only pte_kind the display compositor can scan out from directly. */
#define NVK_VI_SCANOUT_PTE_KIND 0xfe

static bool
nvk_wsi_get_vi_scanout_params(VkDevice _device, VkImage _image,
                              VkDeviceMemory _memory,
                              struct wsi_vi_scanout_params *params)
{
   VK_FROM_HANDLE(nvk_image, image, _image);
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);

   if (image->plane_count != 1)
      return false;

   const struct nvk_image_plane *plane = &image->planes[0];
   const struct nil_image *nil = &plane->nil;

   if (nil->pte_kind != NVK_VI_SCANOUT_PTE_KIND)
      return false;

   uint32_t nvmap_id, nvmap_handle;
   if (!nvkmd_mem_get_scanout_ids(mem->mem, &nvmap_id, &nvmap_handle))
      return false;

   params->nvmap_id = nvmap_id;
   params->nvmap_handle = nvmap_handle;
   /* WSI binds every swapchain image to a dedicated allocation at offset 0. */
   params->offset = 0;
   params->row_stride_B = nil->levels[0].row_stride_B;
   params->block_height_log2 = nil->levels[0].tiling.y_log2;
   params->pte_kind = nil->pte_kind;

   return true;
}
#endif

VkResult
nvk_init_wsi(struct nvk_physical_device *pdev)
{
   VkResult result;

   struct wsi_device_options wsi_options = {
      .sw_device = false
   };
   result = wsi_device_init(&pdev->wsi_device,
                            nvk_physical_device_to_handle(pdev),
                            nvk_wsi_proc_addr, &pdev->vk.instance->alloc,
                            nvkmd_pdev_get_drm_primary_fd(pdev->nvkmd),
                            &nvk_physical_device_instance(pdev)->drirc.options,
                            &wsi_options);
   if (result != VK_SUCCESS)
      return result;

   pdev->wsi_device.supports_scanout = false;
   pdev->wsi_device.supports_modifiers =
      pdev->vk.supported_extensions.table.EXT_image_drm_format_modifier;

#ifdef VK_USE_PLATFORM_VI_NN
   pdev->wsi_device.vi.get_scanout_params = nvk_wsi_get_vi_scanout_params;
#endif

   pdev->vk.wsi_device = &pdev->wsi_device;

   return result;
}

void
nvk_finish_wsi(struct nvk_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi_device, &pdev->vk.instance->alloc);
}
