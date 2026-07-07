/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

/** VK_NN_vi_surface over libnx nwindow */

#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"

#include "util/log.h"
#include "util/u_math.h"

#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include "vulkan/vulkan_core.h"

#include <switch/arm/cache.h>
#include <switch/display/native_window.h>
#include <switch/display/types.h>
#include <switch/nvidia/fence.h>
#include <switch/nvidia/graphic_buffer.h>
#include <switch/nvidia/map.h>
#include <switch/result.h>

#include <assert.h>
#include <malloc.h>
#include <string.h>

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

   caps->minImageCount = 3;
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

/* Everything the compositor needs to scan a single slot's buffer out of an nvmap. */
struct wsi_switch_scanout {
   uint32_t nvmap_id;
   uint32_t nvmap_handle;
   uint32_t offset;
   uint32_t pitch_b;  /* block-linear row stride in bytes */
   uint32_t stride_px;
   uint32_t size;
   uint8_t block_height_log2;
   uint8_t kind;
};

struct wsi_switch_image {
   struct wsi_image base;
   bool busy;
   struct wsi_switch_scanout scanout;
};

/* Tegra block-linear GOB is 64 bytes wide by 8 rows.
 * The compositor scans out 16Bx2-ordered block-linear surfaces with a fixed
 * 16-GOB (block_height_log2=4) block height.
 */
#define GOB_SIZE_X_B 64
#define GOB_SIZE_Y   8
#define WSI_SWITCH_BLOCK_HEIGHT_LOG2 4

struct wsi_switch_swapchain {
   struct wsi_swapchain base;

   VkExtent2D extent;
   VkFormat vk_format;

   NWindow *window;

   bool zero_copy;

   uint32_t bpp;
   NvColorFormat color_format;
   uint32_t pixel_format;

   /* CPU-copy fallback only. Unused when zero_copy. */
   NvMap scanout_map;
   uint8_t *scanout_cpu;
   uint32_t fb_size;
   uint32_t pitch_b;          /* block-linear row stride in bytes */
   uint32_t width_aligned_px; /* pitch_b/bpp */
   uint32_t height_aligned;

   struct wsi_switch_image images[0];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_switch_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static bool
switch_format_info(VkFormat vk_format, uint32_t *bpp,
                   NvColorFormat *color_format, uint32_t *pixel_format)
{
   switch (vk_format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      *bpp = 4;
      *color_format = NvColorFormat_A8B8G8R8;
      *pixel_format = PIXEL_FORMAT_RGBA_8888;
      return true;
   default:
      return false;
   }
}

/* Byte offset of pixel column x_b (a byte, 16-aligned) row y within a
 * block-linear 16Bx2 surface. */
static inline uint32_t
block_linear_offset(uint32_t x_b, uint32_t y,
                    uint32_t width_gobs, uint32_t block_h_gobs)
{
   uint32_t gob_x = x_b / GOB_SIZE_X_B;
   uint32_t gob_y = y / GOB_SIZE_Y;
   uint32_t x_in_gob = x_b % GOB_SIZE_X_B;
   uint32_t y_in_gob = y % GOB_SIZE_Y;

   uint32_t block_y = gob_y / block_h_gobs;
   uint32_t gob_in_block = gob_y % block_h_gobs;
   uint32_t block_bytes = block_h_gobs * 512;

   uint32_t off = block_y * width_gobs * block_bytes;
   off += gob_x * block_bytes;
   off += gob_in_block * 512;
   off += (x_in_gob / 32) * 256;
   off += (y_in_gob / 2) * 64;
   off += ((x_in_gob % 32) / 16) * 32;
   off += (y_in_gob % 2) * 16;
   off += x_in_gob % 16;
   return off;
}

static void
swizzle_to_scanout(struct wsi_switch_swapchain *chain, uint32_t slot,
                   const uint8_t *src, uint32_t src_stride)
{
   uint8_t *dst = chain->scanout_cpu + (size_t)slot * chain->fb_size;
   uint32_t width_b = chain->extent.width * chain->bpp;
   uint32_t width_gobs = chain->pitch_b / GOB_SIZE_X_B;
   uint32_t block_h_gobs = 1u << WSI_SWITCH_BLOCK_HEIGHT_LOG2;

   if (src_stride == 0)
      src_stride = width_b;

   for (uint32_t y = 0; y < chain->extent.height; y++) {
      const uint8_t *srow = src + (size_t)y * src_stride;
      for (uint32_t x_b = 0; x_b < width_b; x_b += 16) {
         uint32_t off = block_linear_offset(x_b, y, width_gobs, block_h_gobs);
         memcpy(dst + off, srow + x_b, MIN2(16, width_b - x_b));
      }
   }
}

static void
build_graphic_buffer(NvGraphicBuffer *gb,
                     const struct wsi_switch_swapchain *chain, uint32_t slot)
{
   const struct wsi_switch_scanout *so = &chain->images[slot].scanout;

   memset(gb, 0, sizeof(*gb));

   /* The producer only marshals num_ints words past the NativeHandle header. */
   gb->header.num_ints = (sizeof(NvGraphicBuffer) - sizeof(NativeHandle)) / 4;
   gb->header.num_fds = 0;

   gb->unk0 = -1;
   gb->nvmap_id = so->nvmap_id;
   gb->magic = 0xDAFFCAFF;
   gb->pid = 42;
   gb->usage = 0xb00;
   gb->format = chain->pixel_format;
   gb->ext_format = chain->pixel_format;
   gb->stride = so->stride_px;
   gb->total_size = so->size;
   gb->num_planes = 1;

   NvSurface *p = &gb->planes[0];
   p->width = chain->extent.width;
   p->height = chain->extent.height;
   p->color_format = chain->color_format;
   p->layout = NvLayout_BlockLinear;
   p->pitch = so->pitch_b;
   p->unused = so->nvmap_handle;
   p->offset = so->offset;
   p->kind = so->kind;
   p->block_height_log2 = so->block_height_log2;
   p->scan = NvDisplayScanFormat_Progressive;
   p->size = so->size;
}

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

   /* out_fence=NULL waits on the compositor's release fence inside libnx,
    * so the slot is ready to be rewritten when this returns. */
   s32 slot = -1;
   Result rc = nwindowDequeueBuffer(chain->window, &slot, NULL);
   if (R_FAILED(rc) || slot < 0 || (uint32_t)slot >= chain->base.image_count)
      return VK_ERROR_OUT_OF_DATE_KHR;

   chain->images[slot].busy = true;
   *image_index = (uint32_t)slot;
   return VK_SUCCESS;
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

   struct wsi_image *image = &chain->images[image_index].base;

   /* wsi_common submitted the render/blit fence right before calling.
    * Wait for it so the scanout buffer holds finished pixels. */
   if (chain->base.fences[image_index] != VK_NULL_HANDLE)
      chain->base.wsi->WaitForFences(chain->base.device, 1,
                                     &chain->base.fences[image_index],
                                     true, UINT64_MAX);

   if (!chain->zero_copy) {
      swizzle_to_scanout(chain, image_index, image->cpu_map,
                         image->row_pitches[0]);

      uint8_t *fb = chain->scanout_cpu + (size_t)image_index * chain->fb_size;
      armDCacheFlush(fb, chain->fb_size);
   }

   Result rc = nwindowQueueBuffer(chain->window, image_index, NULL);

   chain->images[image_index].busy = false;

   return R_FAILED(rc) ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
}

static void
wsi_switch_destroy_images(struct wsi_switch_swapchain *chain)
{
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].base.image != VK_NULL_HANDLE) {
         wsi_destroy_image(&chain->base, &chain->images[i].base);
         memset(&chain->images[i], 0, sizeof(chain->images[i]));
      }
   }

   if (chain->scanout_map.has_init) {
      nvMapClose(&chain->scanout_map);
      memset(&chain->scanout_map, 0, sizeof(chain->scanout_map));
   }

   free(chain->scanout_cpu);
   chain->scanout_cpu = NULL;
}

static VkResult
wsi_switch_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                             const VkAllocationCallbacks *pAllocator)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   wsi_switch_destroy_images(chain);

   /* window is NULL once a newer swapchain has taken ownership of it. */
   if (chain->window != NULL)
      nwindowReleaseBuffers(chain->window);

   nvFenceExit();
   nvMapExit();

   wsi_swapchain_finish(&chain->base);

   vk_free(pAllocator, chain);

   return VK_SUCCESS;
}

/* Tegra X1 is UMA with no VRAM Prefer DEVICE_LOCAL when it
 * exists, otherwise take the first type the image allows.
 */
static uint32_t
wsi_switch_select_memory_type(const struct wsi_device *wsi, uint32_t type_bits)
{
   uint32_t first = UINT32_MAX;
   for (uint32_t t = 0; t < wsi->memory_props.memoryTypeCount; t++) {
      if (!(type_bits & (1u << t)))
         continue;
      if (first == UINT32_MAX)
         first = t;
      if (wsi->memory_props.memoryTypes[t].propertyFlags &
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
         return t;
   }
   return first;
}

static VkResult
wsi_switch_create_native_image_mem(const struct wsi_swapchain *wsi_chain,
                                   UNUSED const struct wsi_image_info *info,
                                   struct wsi_image *image)
{
   const struct wsi_device *wsi = wsi_chain->wsi;
   VkResult result;

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(wsi_chain->device, image->image, &reqs);

   uint32_t mem_type = wsi_switch_select_memory_type(wsi, reqs.memoryTypeBits);
   if (mem_type == UINT32_MAX)
      return VK_ERROR_INITIALIZATION_FAILED;

   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
   };
   const VkMemoryAllocateInfo mem_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .allocationSize = reqs.size,
      .memoryTypeIndex = mem_type,
   };

   result = wsi->AllocateMemory(wsi_chain->device, &mem_info,
                                &wsi_chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      return result;

   image->num_planes = 1;
   image->sizes[0] = reqs.size;
   image->offsets[0] = 0;
   image->row_pitches[0] = 0;

   return VK_SUCCESS;
}

static VkResult
wsi_switch_init_zero_copy(struct wsi_switch_swapchain *chain,
                          struct wsi_device *wsi_device, VkDevice device,
                          const VkSwapchainCreateInfoKHR *pCreateInfo)
{
   VkResult result;

   /* Replace the CPU config from wsi_swapchain_init with a block-linear image
    * that is itself the scanout buffer. */
   wsi_destroy_image_info(&chain->base, &chain->base.image_info);
   chain->base.blit.type = WSI_SWAPCHAIN_NO_BLIT;

   result = wsi_configure_image(&chain->base, pCreateInfo, 0,
                                &chain->base.image_info);
   if (result != VK_SUCCESS)
      return result;
   chain->base.image_info.create_mem = wsi_switch_create_native_image_mem;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      struct wsi_switch_image *img = &chain->images[i];

      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &img->base);
      if (result != VK_SUCCESS)
         return result;
      img->busy = false;

      struct wsi_vi_scanout_params sp;
      if (!wsi_device->vi.get_scanout_params(device, img->base.image,
                                             img->base.memory, &sp))
         return VK_ERROR_INITIALIZATION_FAILED;

      img->scanout = (struct wsi_switch_scanout) {
         .nvmap_id = sp.nvmap_id,
         .nvmap_handle = sp.nvmap_handle,
         .offset = sp.offset,
         .pitch_b = sp.row_stride_B,
         .stride_px = sp.row_stride_B / chain->bpp,
         .size = img->base.sizes[0],
         .block_height_log2 = sp.block_height_log2,
         .kind = sp.pte_kind,
      };

      NvGraphicBuffer gb;
      build_graphic_buffer(&gb, chain, i);
      if (R_FAILED(nwindowConfigureBuffer(chain->window, i, &gb)))
         return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

static VkResult
wsi_switch_init_cpu_copy(struct wsi_switch_swapchain *chain,
                         const VkSwapchainCreateInfoKHR *pCreateInfo)
{
   VkResult result;

   /* Linear render images blitted into a shared block-linear scanout nvmap. */
   wsi_destroy_image_info(&chain->base, &chain->base.image_info);
   chain->base.blit.type = WSI_SWAPCHAIN_BUFFER_BLIT;

   struct wsi_cpu_image_params cpu_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
   };
   result = wsi_configure_cpu_image(&chain->base, pCreateInfo, &cpu_params,
                                    &chain->base.image_info);
   if (result != VK_SUCCESS)
      return result;

   uint32_t block_h_px = GOB_SIZE_Y << WSI_SWITCH_BLOCK_HEIGHT_LOG2;
   chain->pitch_b = align(chain->extent.width * chain->bpp, GOB_SIZE_X_B);
   chain->width_aligned_px = chain->pitch_b / chain->bpp;
   chain->height_aligned = align(chain->extent.height, block_h_px);
   chain->fb_size = chain->pitch_b * chain->height_aligned;

   uint32_t total = align(chain->base.image_count * chain->fb_size, 0x20000);
   chain->scanout_cpu = memalign(0x20000, total);
   if (chain->scanout_cpu == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   memset(chain->scanout_cpu, 0, total);

   if (R_FAILED(nvMapCreate(&chain->scanout_map, chain->scanout_cpu, total,
                            0x20000, NvKind_Pitch, true)))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      chain->images[i].scanout = (struct wsi_switch_scanout) {
         .nvmap_id = chain->scanout_map.id,
         .nvmap_handle = chain->scanout_map.handle,
         .offset = i * chain->fb_size,
         .pitch_b = chain->pitch_b,
         .stride_px = chain->width_aligned_px,
         .size = chain->fb_size,
         .block_height_log2 = WSI_SWITCH_BLOCK_HEIGHT_LOG2,
         .kind = NvKind_Generic_16BX2,
      };

      NvGraphicBuffer gb;
      build_graphic_buffer(&gb, chain, i);
      if (R_FAILED(nwindowConfigureBuffer(chain->window, i, &gb)))
         return VK_ERROR_INITIALIZATION_FAILED;
   }

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[i].base);
      if (result != VK_SUCCESS)
         return result;
      chain->images[i].busy = false;
   }

   return VK_SUCCESS;
}

static VkResult
wsi_switch_init_images(struct wsi_switch_swapchain *chain,
                       struct wsi_device *wsi_device, VkDevice device,
                       const VkSwapchainCreateInfoKHR *pCreateInfo)
{
   if (wsi_device->vi.get_scanout_params != NULL) {
      VkResult result =
         wsi_switch_init_zero_copy(chain, wsi_device, device, pCreateInfo);
      if (result == VK_SUCCESS) {
         chain->zero_copy = true;
         mesa_logi("nvk wsi: zero-copy ENABLED");
         return VK_SUCCESS;
      }

      wsi_switch_destroy_images(chain);
      nwindowReleaseBuffers(chain->window);
      mesa_logi("nvk wsi: zero-copy FAILED");
   }

   chain->zero_copy = false;
   return wsi_switch_init_cpu_copy(chain, pCreateInfo);
}

/* On recreate the app hands us the old swapchain and destroys it after we
 * return. Both share this surface's NWindow, so take ownership away from the
 * old one, then reset the queue and pick up the possibly-changed dimensions. */
static void
wsi_switch_adopt_window(struct wsi_switch_swapchain *chain,
                        VkSwapchainKHR old_handle)
{
   if (old_handle == VK_NULL_HANDLE)
      return;

   struct wsi_switch_swapchain *old =
      wsi_switch_swapchain_from_handle(old_handle);
   old->window = NULL;

   nwindowReleaseBuffers(chain->window);
   nwindowSetDimensions(chain->window, chain->extent.width,
                        chain->extent.height);
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

   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   NWindow *window = surface->window;
   if (window == NULL)
      return VK_ERROR_SURFACE_LOST_KHR;

   uint32_t bpp;
   NvColorFormat color_format;
   uint32_t pixel_format;
   if (!switch_format_info(pCreateInfo->imageFormat, &bpp, &color_format,
                           &pixel_format))
      return VK_ERROR_INITIALIZATION_FAILED;

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
   chain->window = window;
   chain->bpp = bpp;
   chain->color_format = color_format;
   chain->pixel_format = pixel_format;

   wsi_switch_adopt_window(chain, pCreateInfo->oldSwapchain);

   nvMapInit();
   nvFenceInit();

   result = wsi_switch_init_images(chain, wsi_device, device, pCreateInfo);
   if (result != VK_SUCCESS)
      goto fail;

   uint32_t swap_interval =
      chain->base.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0 : 1;
   nwindowSetSwapInterval(chain->window, swap_interval);

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
