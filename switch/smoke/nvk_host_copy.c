/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * VK_EXT_host_image_copy
 */
#include "nvk_gfx.h"

#define MAX_LEVELS 4
#define MAX_LAYERS 3
#define MAX_SUBS   (MAX_LEVELS * MAX_LAYERS)

enum { TAG_A = 1, TAG_B, TAG_C, TAG_D };

struct hic_case {
   const char   *name;
   VkFormat      fmt;
   uint32_t      texel_B;
   uint32_t      w, h;
   uint32_t      levels, layers;
   VkImageTiling tiling;
};

static const struct hic_case cases[] = {
   { "rgba8_256x256",  VK_FORMAT_R8G8B8A8_UNORM,      4, 256, 256, 1, 1, VK_IMAGE_TILING_OPTIMAL },
   { "r8_129x67",      VK_FORMAT_R8_UNORM,            1, 129,  67, 1, 1, VK_IMAGE_TILING_OPTIMAL },
   { "rg16_64x64",     VK_FORMAT_R16G16_UINT,         4,  64,  64, 1, 1, VK_IMAGE_TILING_OPTIMAL },
   { "rgba16f_mips",   VK_FORMAT_R16G16B16A16_SFLOAT, 8, 128, 128, MAX_LEVELS, 1, VK_IMAGE_TILING_OPTIMAL },
   { "rgba8_3layers",  VK_FORMAT_R8G8B8A8_UNORM,      4,  64,  64, 1, MAX_LAYERS, VK_IMAGE_TILING_OPTIMAL },
   { "rgba8_8x512",    VK_FORMAT_R8G8B8A8_UNORM,      4,   8, 512, 1, 1, VK_IMAGE_TILING_OPTIMAL },
   { "rgba8_linear64", VK_FORMAT_R8G8B8A8_UNORM,      4,  64,  64, 1, 1, VK_IMAGE_TILING_LINEAR  },
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

enum verdict { V_PASS, V_FAIL, V_SKIP };

struct sub {
   uint32_t     level, layer, w, h;
   VkDeviceSize off, size;
};

static void fill_pattern(uint8_t *p, size_t n, uint32_t tag,
                         uint32_t level, uint32_t layer)
{
   uint32_t s = tag * 0x9E3779B9u ^ (level << 16) ^ (layer << 8) ^ 0xA5A5u;
   for (size_t i = 0; i < n; i++) {
      s = s * 1664525u + 1013904223u;
      p[i] = (uint8_t)(s >> 24);
   }
}

static uint32_t count_diff(const uint8_t *got, const uint8_t *want, size_t n,
                           size_t *first_out)
{
   uint32_t bad = 0;
   for (size_t i = 0; i < n; i++) {
      if (got[i] != want[i]) {
         if (!bad) *first_out = i;
         bad++;
      }
   }
   return bad;
}

static VkResult gpu_readback(struct nvk_ctx *c, VkCommandPool pool, VkImage img,
                             const struct hic_case *cs,
                             const struct sub *subs, uint32_t nsub,
                             struct nvk_buffer *down)
{
   LOAD_DEV(c, CmdCopyImageToBuffer);
   LOAD_DEV(c, CmdPipelineBarrier);

   VkCommandBuffer cb = nvk_begin_cb(c, pool);
   if (cb == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;

   nvk_image_barrier(c, cb, img, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                     cs->levels, cs->layers,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

   for (uint32_t i = 0; i < nsub; i++) {
      VkBufferImageCopy r = {
         .bufferOffset = subs[i].off,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, subs[i].level,
                               subs[i].layer, 1 },
         .imageExtent = { subs[i].w, subs[i].h, 1 },
      };
      CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           down->buf, 1, &r);
   }

   nvk_image_barrier(c, cb, img, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                     cs->levels, cs->layers,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_HOST_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);

   VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);

   return nvk_end_submit_wait(c, cb);
}

static VkResult make_host_image(struct nvk_ctx *c, const struct hic_case *cs,
                                VkImage *img_out, VkDeviceMemory *mem_out)
{
   LOAD_DEV(c, CreateImage);
   LOAD_DEV(c, GetImageMemoryRequirements);
   LOAD_DEV(c, AllocateMemory);
   LOAD_DEV(c, BindImageMemory);

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = cs->fmt,
      .extent = { cs->w, cs->h, 1 },
      .mipLevels = cs->levels,
      .arrayLayers = cs->layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = cs->tiling,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_HOST_TRANSFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = CreateImage(c->dev, &ici, NULL, img_out);
   if (r != VK_SUCCESS) return r;

   VkMemoryRequirements mr;
   GetImageMemoryRequirements(c->dev, *img_out, &mr);
   uint32_t mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (mt == UINT32_MAX)
      mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (mt == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   r = AllocateMemory(c->dev, &mai, NULL, mem_out);
   if (r != VK_SUCCESS) return r;
   return BindImageMemory(c->dev, *img_out, *mem_out, 0);
}

static enum verdict run_case(struct nvk_ctx *c, VkCommandPool pool,
                             const struct hic_case *cs)
{
   LOAD_INST(c, GetPhysicalDeviceImageFormatProperties2);
   LOAD_DEV(c, DestroyImage);
   LOAD_DEV(c, FreeMemory);
   LOAD_DEV(c, CmdCopyBufferToImage);
   LOAD_DEV(c, CopyMemoryToImageEXT);
   LOAD_DEV(c, CopyImageToMemoryEXT);
   LOAD_DEV(c, CopyImageToImageEXT);
   LOAD_DEV(c, TransitionImageLayoutEXT);
   LOAD_DEV(c, GetImageSubresourceLayout2KHR);

   VkImage img = VK_NULL_HANDLE, img2 = VK_NULL_HANDLE;
   VkDeviceMemory mem = VK_NULL_HANDLE, mem2 = VK_NULL_HANDLE;
   struct nvk_buffer up = {0}, down = {0};
   uint8_t *host = NULL, *expect = NULL, *raw = NULL;
   enum verdict v = V_FAIL;
   unsigned failed = 0;

   const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_HOST_TRANSFER_BIT;

   VkPhysicalDeviceImageFormatInfo2 fi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = cs->fmt, .type = VK_IMAGE_TYPE_2D,
      .tiling = cs->tiling, .usage = usage,
   };
   VkImageFormatProperties2 fp = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };
   if (!GetPhysicalDeviceImageFormatProperties2) {
      LOG("  FAIL %-15s no vkGetPhysicalDeviceImageFormatProperties2", cs->name);
      return V_FAIL;
   }
   if (GetPhysicalDeviceImageFormatProperties2(c->phys, &fi, &fp) != VK_SUCCESS ||
       fp.imageFormatProperties.maxMipLevels < cs->levels ||
       fp.imageFormatProperties.maxArrayLayers < cs->layers) {
      LOG("  skip %-15s host-transfer unsupported for this format/tiling", cs->name);
      return V_SKIP;
   }

   struct sub subs[MAX_SUBS];
   uint32_t nsub = 0;
   VkDeviceSize total = 0;
   for (uint32_t l = 0; l < cs->levels; l++) {
      const uint32_t lw = (cs->w >> l) ? (cs->w >> l) : 1u;
      const uint32_t lh = (cs->h >> l) ? (cs->h >> l) : 1u;
      for (uint32_t a = 0; a < cs->layers; a++) {
         subs[nsub] = (struct sub){
            .level = l, .layer = a, .w = lw, .h = lh,
            .off = total, .size = (VkDeviceSize)lw * lh * cs->texel_B,
         };
         total += subs[nsub].size;
         nsub++;
      }
   }

   VkResult r = make_host_image(c, cs, &img, &mem);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s image create/bind -> %d", cs->name, r); goto out; }

   r = nvk_host_buffer(c, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s upload buffer -> %d", cs->name, r); goto out; }
   r = nvk_host_buffer(c, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &down);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s readback buffer -> %d", cs->name, r); goto out; }

   host = malloc(total);
   expect = malloc(total);
   if (!host || !expect) { LOG("  FAIL %-15s out of heap", cs->name); goto out; }

   VkHostImageLayoutTransitionInfo tr = {
      .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
      .image = img,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, cs->levels, 0, cs->layers },
   };
   r = TransitionImageLayoutEXT(c->dev, 1, &tr);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s vkTransitionImageLayoutEXT -> %d", cs->name, r); goto out; }

   VkMemoryToImageCopy to_img[MAX_SUBS];
   for (uint32_t i = 0; i < nsub; i++) {
      fill_pattern(host + subs[i].off, subs[i].size, TAG_A, subs[i].level, subs[i].layer);
      to_img[i] = (VkMemoryToImageCopy){
         .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
         .pHostPointer = host + subs[i].off,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, subs[i].level, subs[i].layer, 1 },
         .imageExtent = { subs[i].w, subs[i].h, 1 },
      };
   }
   VkCopyMemoryToImageInfo mti = {
      .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
      .dstImage = img, .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .regionCount = nsub, .pRegions = to_img,
   };
   r = CopyMemoryToImageEXT(c->dev, &mti);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s vkCopyMemoryToImageEXT -> %d", cs->name, r); goto out; }

   r = gpu_readback(c, pool, img, cs, subs, nsub, &down);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s readback submit -> %d", cs->name, r); goto out; }

   for (uint32_t i = 0; i < nsub; i++) {
      size_t first = 0;
      uint32_t bad = count_diff((uint8_t *)down.cpu + subs[i].off,
                                host + subs[i].off, subs[i].size, &first);
      if (bad) {
         failed++;
         LOG("  FAIL %-15s A host->image lvl%u layer%u: %u/%u bytes wrong, first +%u",
             cs->name, subs[i].level, subs[i].layer, bad,
             (unsigned)subs[i].size, (unsigned)first);
      }
   }

   for (uint32_t i = 0; i < nsub; i++)
      fill_pattern((uint8_t *)up.cpu + subs[i].off, subs[i].size, TAG_B,
                   subs[i].level, subs[i].layer);
   memcpy(expect, up.cpu, total);

   {
      VkCommandBuffer cb = nvk_begin_cb(c, pool);
      if (cb == VK_NULL_HANDLE) { LOG("  FAIL %-15s command buffer", cs->name); goto out; }
      nvk_image_barrier(c, cb, img, VK_IMAGE_ASPECT_COLOR_BIT, 0, cs->levels, cs->layers,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      for (uint32_t i = 0; i < nsub; i++) {
         VkBufferImageCopy bic = {
            .bufferOffset = subs[i].off,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, subs[i].level, subs[i].layer, 1 },
            .imageExtent = { subs[i].w, subs[i].h, 1 },
         };
         CmdCopyBufferToImage(cb, up.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
      }
      nvk_image_barrier(c, cb, img, VK_IMAGE_ASPECT_COLOR_BIT, 0, cs->levels, cs->layers,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
      r = nvk_end_submit_wait(c, cb);
      if (r != VK_SUCCESS) { LOG("  FAIL %-15s upload submit -> %d", cs->name, r); goto out; }
   }

   VkImageToMemoryCopy from_img[MAX_SUBS];
   memset(host, 0, total);
   for (uint32_t i = 0; i < nsub; i++) {
      from_img[i] = (VkImageToMemoryCopy){
         .sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY,
         .pHostPointer = host + subs[i].off,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, subs[i].level, subs[i].layer, 1 },
         .imageExtent = { subs[i].w, subs[i].h, 1 },
      };
   }
   VkCopyImageToMemoryInfo itm = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO,
      .srcImage = img, .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .regionCount = nsub, .pRegions = from_img,
   };
   r = CopyImageToMemoryEXT(c->dev, &itm);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s vkCopyImageToMemoryEXT -> %d", cs->name, r); goto out; }

   for (uint32_t i = 0; i < nsub; i++) {
      size_t first = 0;
      uint32_t bad = count_diff(host + subs[i].off, expect + subs[i].off,
                                subs[i].size, &first);
      if (bad) {
         failed++;
         LOG("  FAIL %-15s B image->host lvl%u layer%u: %u/%u bytes wrong, first +%u",
             cs->name, subs[i].level, subs[i].layer, bad,
             (unsigned)subs[i].size, (unsigned)first);
      }
   }

   const uint32_t ox = cs->w / 4, oy = cs->h / 4;
   const uint32_t sw = cs->w / 2 > 1 ? cs->w / 2 - 1 : 1;
   const uint32_t sh = cs->h / 2 > 1 ? cs->h / 2 - 1 : 1;
   const size_t rect_B = (size_t)sw * sh * cs->texel_B;
   uint8_t *rect = malloc(rect_B);
   if (!rect) { LOG("  FAIL %-15s out of heap", cs->name); goto out; }
   fill_pattern(rect, rect_B, TAG_C, 0, 0);

   VkMemoryToImageCopy rect_copy = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
      .pHostPointer = rect,
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageOffset = { (int32_t)ox, (int32_t)oy, 0 },
      .imageExtent = { sw, sh, 1 },
   };
   mti.regionCount = 1;
   mti.pRegions = &rect_copy;
   r = CopyMemoryToImageEXT(c->dev, &mti);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s C sub-rect copy -> %d", cs->name, r); free(rect); goto out; }

   for (uint32_t y = 0; y < sh; y++) {
      memcpy(expect + ((size_t)(oy + y) * cs->w + ox) * cs->texel_B,
             rect + (size_t)y * sw * cs->texel_B,
             (size_t)sw * cs->texel_B);
   }
   free(rect);

   r = gpu_readback(c, pool, img, cs, subs, nsub, &down);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s C readback submit -> %d", cs->name, r); goto out; }
   {
      size_t first = 0;
      uint32_t bad = count_diff((uint8_t *)down.cpu, expect, subs[0].size, &first);
      if (bad) {
         failed++;
         LOG("  FAIL %-15s C sub-rect %ux%u at (%u,%u): %u/%u bytes wrong, first +%u",
             cs->name, sw, sh, ox, oy, bad, (unsigned)subs[0].size, (unsigned)first);
      }
   }

   if (GetImageSubresourceLayout2KHR) {
      VkSubresourceHostMemcpySize hsize = {
         .sType = VK_STRUCTURE_TYPE_SUBRESOURCE_HOST_MEMCPY_SIZE,
      };
      VkSubresourceLayout2 sl = {
         .sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2, .pNext = &hsize,
      };
      VkImageSubresource2 s2 = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 },
      };
      GetImageSubresourceLayout2KHR(c->dev, img, &s2, &sl);

      if (hsize.size == 0) {
         failed++;
         LOG("  FAIL %-15s D memcpy size reported as 0", cs->name);
      } else if ((raw = malloc(hsize.size)) == NULL) {
         LOG("  FAIL %-15s out of heap", cs->name);
         goto out;
      } else {
         VkImageToMemoryCopy save = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY,
            .pHostPointer = raw,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { cs->w, cs->h, 1 },
         };
         itm.flags = VK_HOST_IMAGE_COPY_MEMCPY_BIT;
         itm.regionCount = 1;
         itm.pRegions = &save;
         r = CopyImageToMemoryEXT(c->dev, &itm);

         if (r == VK_SUCCESS) {
            fill_pattern(host, subs[0].size, TAG_D, 0, 0);
            VkMemoryToImageCopy scribble = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
               .pHostPointer = host,
               .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
               .imageExtent = { cs->w, cs->h, 1 },
            };
            mti.flags = 0;
            mti.regionCount = 1;
            mti.pRegions = &scribble;
            r = CopyMemoryToImageEXT(c->dev, &mti);
         }
         if (r == VK_SUCCESS) {
            VkMemoryToImageCopy restore = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
               .pHostPointer = raw,
               .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
               .imageExtent = { cs->w, cs->h, 1 },
            };
            mti.flags = VK_HOST_IMAGE_COPY_MEMCPY_BIT;
            mti.pRegions = &restore;
            r = CopyMemoryToImageEXT(c->dev, &mti);
         }
         itm.flags = 0;
         mti.flags = 0;

         if (r != VK_SUCCESS) {
            failed++;
            LOG("  FAIL %-15s D memcpy round trip -> %d", cs->name, r);
         } else {
            r = gpu_readback(c, pool, img, cs, subs, nsub, &down);
            if (r != VK_SUCCESS) { LOG("  FAIL %-15s D readback submit -> %d", cs->name, r); goto out; }
            size_t first = 0;
            uint32_t bad = count_diff((uint8_t *)down.cpu, expect, subs[0].size, &first);
            if (bad) {
               failed++;
               LOG("  FAIL %-15s D memcpy round trip: %u/%u bytes wrong, first +%u (size=%u)",
                   cs->name, bad, (unsigned)subs[0].size, (unsigned)first,
                   (unsigned)hsize.size);
            }
         }
      }
   }

   r = make_host_image(c, cs, &img2, &mem2);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s second image -> %d", cs->name, r); goto out; }
   tr.image = img2;
   r = TransitionImageLayoutEXT(c->dev, 1, &tr);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s E layout transition -> %d", cs->name, r); goto out; }

   VkImageCopy2 i2i = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .extent = { cs->w, cs->h, 1 },
   };
   VkCopyImageToImageInfo iti = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_IMAGE_INFO,
      .srcImage = img, .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .dstImage = img2, .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .regionCount = 1, .pRegions = &i2i,
   };
   r = CopyImageToImageEXT(c->dev, &iti);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s vkCopyImageToImageEXT -> %d", cs->name, r); goto out; }

   memset(down.cpu, 0, total);
   r = gpu_readback(c, pool, img2, cs, subs, 1, &down);
   if (r != VK_SUCCESS) { LOG("  FAIL %-15s E readback submit -> %d", cs->name, r); goto out; }
   {
      size_t first = 0;
      uint32_t bad = count_diff((uint8_t *)down.cpu, expect, subs[0].size, &first);
      if (bad) {
         failed++;
         LOG("  FAIL %-15s E image->image: %u/%u bytes wrong, first +%u",
             cs->name, bad, (unsigned)subs[0].size, (unsigned)first);
      }
   }

   v = failed ? V_FAIL : V_PASS;
   if (v == V_PASS)
      LOG("  ok   %-15s %ux%u %u level(s) %u layer(s), %u subresources",
          cs->name, cs->w, cs->h, cs->levels, cs->layers, nsub);

out:
   free(raw);
   free(expect);
   free(host);
   nvk_free_buffer(c, &down);
   nvk_free_buffer(c, &up);
   if (img2) DestroyImage(c->dev, img2, NULL);
   if (mem2) FreeMemory(c->dev, mem2, NULL);
   if (img)  DestroyImage(c->dev, img, NULL);
   if (mem)  FreeMemory(c->dev, mem, NULL);
   return v;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_host_copy.log");
   LOG("=== nvk_host_copy ===");

   VkCommandPool pool = VK_NULL_HANDLE;
   struct nvk_ctx c;

   VkPhysicalDeviceHostImageCopyFeatures hic_feat = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES,
      .hostImageCopy = VK_TRUE,
   };
   g_nvk_dev_pnext = &hic_feat;

   const char *dev_exts[] = { VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME };
   if (nvk_bringup(&c, dev_exts, 1) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOG("apiVersion %u.%u -- %s",
       VK_VERSION_MAJOR(c.props.apiVersion), VK_VERSION_MINOR(c.props.apiVersion),
       c.props.apiVersion >= VK_API_VERSION_1_4 ? "Vulkan 1.4" : "below 1.4");
   if (c.props.apiVersion < VK_API_VERSION_1_4)
      LOG("  WARN device does not advertise vk 1.4");

   {
      LOAD_INST(&c, GetPhysicalDeviceProperties2);
      VkPhysicalDeviceHostImageCopyProperties hic_props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES,
      };
      VkPhysicalDeviceProperties2 p2 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hic_props,
      };
      if (GetPhysicalDeviceProperties2) {
         GetPhysicalDeviceProperties2(c.phys, &p2);
         LOG("host image copy: %u src layouts, %u dst layouts, identicalMemoryTypeRequirements=%u",
             hic_props.copySrcLayoutCount, hic_props.copyDstLayoutCount,
             hic_props.identicalMemoryTypeRequirements);
      }
   }

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c.qfi,
   };
   LOAD_DEV(&c, CreateCommandPool);
   if (CreateCommandPool(c.dev, &pci, NULL, &pool) != VK_SUCCESS) { LOG("FAIL: pool"); goto done; }

   unsigned passed = 0, failed = 0, skipped = 0;
   for (unsigned k = 0; k < NCASES; k++) {
      switch (run_case(&c, pool, &cases[k])) {
      case V_PASS: passed++; break;
      case V_SKIP: skipped++; break;
      default:     failed++; break;
      }
   }

   LOG("%u passed, %u failed, %u skipped of %u cases",
       passed, failed, skipped, (unsigned)NCASES);
   if (failed == 0 && c.props.apiVersion >= VK_API_VERSION_1_4) {
      LOG("VERIFY OK");
      LOG("=== nvk_host_copy PASSED ===");
   } else {
      LOG("=== nvk_host_copy FAILED ===");
   }

done:
   if (c.dev) {
      LOAD_DEV(&c, DeviceWaitIdle);
      LOAD_DEV(&c, DestroyCommandPool);
      if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
      if (pool) DestroyCommandPool(c.dev, pool, NULL);
   }
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
