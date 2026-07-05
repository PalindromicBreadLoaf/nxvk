/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

/** VK_NN_vi_surface over libnx nwindow */

#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"

#include "util/timespec.h"

#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include "vulkan/vulkan_core.h"

#include <switch/display/native_window.h>
#include <switch/result.h>

#include <assert.h>

struct wsi_switch {
   struct wsi_interface base;

   struct wsi_device *wsi;

   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
};

static VkResult
wsi_switch_surface_get_support(VkIcdSurfaceBase *surface,
                               struct wsi_device *wsi_device,
                               uint32_t queueFamilyIndex,
                               VkBool32 *pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR,
};

static VkResult
wsi_switch_surface_get_capabilities(VkIcdSurfaceBase *icd_surface,
                                    struct wsi_device *wsi_device,
                                    VkSurfaceCapabilitiesKHR *caps)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   NWindow *nw = surface->window;

   u32 width = 0, height = 0;
   if (nw == NULL || R_FAILED(nwindowGetDimensions(nw, &width, &height)) ||
       width == 0 || height == 0) {
      width = 1280;
      height = 720;
   }

   caps->currentExtent = (VkExtent2D) { width, height };
   caps->minImageExtent = (VkExtent2D) { 1, 1 };
   caps->maxImageExtent = (VkExtent2D) {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };

   caps->minImageCount = 2;
   caps->maxImageCount = 3;

   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;

   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

   caps->supportedUsageFlags = wsi_caps_get_image_usage();

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (pdevice->supported_extensions.EXT_attachment_feedback_loop_layout)
      caps->supportedUsageFlags |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;

   return VK_SUCCESS;
}

static VkResult
wsi_switch_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     const void *info_next,
                                     VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   const VkSurfacePresentModeEXT *present_mode =
      (const VkSurfacePresentModeEXT *)vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_EXT);

   VkResult result =
      wsi_switch_surface_get_capabilities(surface, wsi_device,
                                          &caps->surfaceCapabilities);

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT: {
         VkSurfacePresentScalingCapabilitiesEXT *scaling =
            (VkSurfacePresentScalingCapabilitiesEXT *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = caps->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent = caps->surfaceCapabilities.maxImageExtent;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT: {
         /* Unsupported, just report the input present mode. */
         VkSurfacePresentModeCompatibilityEXT *compat =
            (VkSurfacePresentModeCompatibilityEXT *)ext;
         if (compat->pPresentModes) {
            if (compat->presentModeCount) {
               assert(present_mode);
               compat->pPresentModes[0] = present_mode->presentMode;
               compat->presentModeCount = 1;
            }
         } else {
            if (!present_mode)
               wsi_common_vk_warn_once("Use of VkSurfacePresentModeCompatibilityEXT "
                                       "without a VkSurfacePresentModeEXT set. This is an "
                                       "application bug.\n");
            compat->presentModeCount = 1;
         }
         break;
      }

      default:
         /* Ignored */
         break;
      }
   }

   return result;
}

static const VkFormat available_surface_formats[] = {
   VK_FORMAT_R8G8B8A8_UNORM,
   VK_FORMAT_R8G8B8A8_SRGB,
};

static VkResult
wsi_switch_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                               struct wsi_device *wsi_device,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = available_surface_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_switch_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                                struct wsi_device *wsi_device,
                                const void *info_next,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = available_surface_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_switch_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     uint32_t *pPresentModeCount,
                                     VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);

   return *pPresentModeCount < ARRAY_SIZE(present_modes) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
wsi_switch_surface_get_present_rectangles(VkIcdSurfaceBase *icd_surface,
                                          struct wsi_device *wsi_device,
                                          uint32_t *pRectCount,
                                          VkRect2D *pRects)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   NWindow *nw = surface->window;

   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   u32 width = 0, height = 0;
   if (nw == NULL || R_FAILED(nwindowGetDimensions(nw, &width, &height)) ||
       width == 0 || height == 0) {
      width = 1280;
      height = 720;
   }

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      *rect = (VkRect2D) {
         .offset = { 0, 0 },
         .extent = { width, height },
      };
   }

   return vk_outarray_status(&out);
}

struct wsi_switch_image {
   struct wsi_image base;
   bool busy;
};

struct wsi_switch_swapchain {
   struct wsi_swapchain base;

   VkExtent2D extent;
   VkFormat vk_format;

   VkIcdSurfaceVi *surface;

   struct wsi_switch_image images[0];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_switch_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static struct wsi_image *
wsi_switch_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain,
                                   uint32_t image_index)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

static VkResult
wsi_switch_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                        const VkAcquireNextImageInfoKHR *info,
                                        uint32_t *image_index)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;
   struct timespec start_time, end_time;
   struct timespec rel_timeout;

   timespec_from_nsec(&rel_timeout, info->timeout);

   clock_gettime(CLOCK_MONOTONIC, &start_time);
   timespec_add(&end_time, &rel_timeout, &start_time);

   while (1) {
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (!chain->images[i].busy) {
            *image_index = i;
            chain->images[i].busy = true;
            return VK_SUCCESS;
         }
      }

      struct timespec current_time;
      clock_gettime(CLOCK_MONOTONIC, &current_time);
      if (timespec_after(&current_time, &end_time))
         return VK_NOT_READY;
   }
}

static VkResult
wsi_switch_swapchain_queue_present(struct wsi_swapchain *wsi_chain,
                                   uint32_t image_index,
                                   uint64_t present_id,
                                   const VkPresentRegionKHR *damage)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   assert(image_index < chain->base.image_count);

   chain->images[image_index].busy = false;

   return VK_SUCCESS;
}

static VkResult
wsi_switch_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                             const VkAllocationCallbacks *pAllocator)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
   }

   wsi_swapchain_finish(&chain->base);

   vk_free(pAllocator, chain);

   return VK_SUCCESS;
}

static VkResult
wsi_switch_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                    VkDevice device,
                                    struct wsi_device *wsi_device,
                                    const VkSwapchainCreateInfoKHR *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    struct wsi_swapchain **swapchain_out)
{
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   int num_images = pCreateInfo->minImageCount;

   struct wsi_switch_swapchain *chain;
   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct wsi_cpu_image_params cpu_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
   };

   result = wsi_swapchain_init(wsi_device, &chain->base, device,
                               pCreateInfo, &cpu_params.base, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   chain->base.destroy = wsi_switch_swapchain_destroy;
   chain->base.get_wsi_image = wsi_switch_swapchain_get_wsi_image;
   chain->base.acquire_next_image = wsi_switch_swapchain_acquire_next_image;
   chain->base.queue_present = wsi_switch_swapchain_queue_present;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   chain->base.image_count = num_images;
   chain->extent = pCreateInfo->imageExtent;
   chain->vk_format = pCreateInfo->imageFormat;
   chain->surface = (VkIcdSurfaceVi *)icd_surface;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[i].base);
      if (result != VK_SUCCESS)
         goto fail;

      chain->images[i].busy = false;
   }

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail:
   wsi_switch_swapchain_destroy(&chain->base, pAllocator);

   return result;
}

VkResult
wsi_switch_init_wsi(struct wsi_device *wsi_device,
                    const VkAllocationCallbacks *alloc,
                    VkPhysicalDevice physical_device)
{
   struct wsi_switch *wsi;
   VkResult result;

   wsi = vk_alloc(alloc, sizeof(*wsi), 8,
                  VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->physical_device = physical_device;
   wsi->alloc = alloc;
   wsi->wsi = wsi_device;

   wsi->base.get_support = wsi_switch_surface_get_support;
   wsi->base.get_capabilities2 = wsi_switch_surface_get_capabilities2;
   wsi->base.get_formats = wsi_switch_surface_get_formats;
   wsi->base.get_formats2 = wsi_switch_surface_get_formats2;
   wsi->base.get_present_modes = wsi_switch_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_switch_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_switch_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = &wsi->base;

   return VK_SUCCESS;

fail:
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = NULL;

   return result;
}

void
wsi_switch_finish_wsi(struct wsi_device *wsi_device,
                      const VkAllocationCallbacks *alloc)
{
   struct wsi_switch *wsi =
      (struct wsi_switch *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI];
   if (!wsi)
      return;

   vk_free(alloc, wsi);
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateViSurfaceNN(VkInstance _instance,
                      const VkViSurfaceCreateInfoNN *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   VkIcdSurfaceVi *surface;

   surface = vk_alloc2(&instance->alloc, pAllocator, sizeof *surface, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_VI;
   surface->window = pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}
