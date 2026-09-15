/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Compressed colour target
 */
#include "nvk_gfx.h"
#include "shaders/tri_vert.h"
#include "shaders/solid_frag.h"

#define DIM       256
#define CLEAR_U32 0xFF000000u
#define PIXELS    ((uint32_t)DIM * DIM)

struct phase {
   bool prefers_dedicated;
   bool render_ok;
   bool transfer_ok;
   uint32_t center;
   uint32_t corner;
   uint32_t bad_texels;
   uint32_t first_bad;
};

static uint32_t texel(uint32_t i)
{
   return (i * 2654435761u) | 0xFF000000u;
}

static bool run_phase(const char *name, struct phase *p)
{
   struct nvk_ctx c;
   bool ok = false;

   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) {
      LOG("%s: FAIL bringup", name);
      return false;
   }

   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, CmdBeginRenderPass);
   LOAD_DEV(&c, CmdEndRenderPass);
   LOAD_DEV(&c, CmdBindPipeline);
   LOAD_DEV(&c, CmdDraw);
   LOAD_DEV(&c, CreateImage);
   LOAD_DEV(&c, GetImageMemoryRequirements2);
   LOAD_DEV(&c, CmdCopyBufferToImage);
   LOAD_DEV(&c, DeviceWaitIdle);

   if (!GetImageMemoryRequirements2 || !CmdCopyBufferToImage) {
      LOG("%s: FAIL missing device entrypoints", name);
      goto done;
   }

   {
      VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = { DIM, DIM, 1 }, .mipLevels = 1, .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      VkImage probe = VK_NULL_HANDLE;
      if (CreateImage(c.dev, &ici, NULL, &probe) == VK_SUCCESS) {
         VkMemoryDedicatedRequirements ded = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
         };
         VkMemoryRequirements2 mr2 = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &ded,
         };
         VkImageMemoryRequirementsInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = probe,
         };
         GetImageMemoryRequirements2(c.dev, &info, &mr2);
         p->prefers_dedicated = ded.prefersDedicatedAllocation;
         LOG("%s: prefersDedicatedAllocation=%d size=0x%llx align=0x%llx",
             name, (int)ded.prefersDedicatedAllocation,
             (unsigned long long)mr2.memoryRequirements.size,
             (unsigned long long)mr2.memoryRequirements.alignment);
         LOAD_DEV(&c, DestroyImage);
         if (DestroyImage) DestroyImage(c.dev, probe, NULL);
      }
   }

   nvk_gfx_force_dedicated = true;

   struct nvk_target t;
   VkResult r = nvk_color_target(&c, DIM, DIM, VK_FORMAT_R8G8B8A8_UNORM, false, &t);
   if (r != VK_SUCCESS) { LOG("%s: FAIL colour target -> %d", name, r); goto done; }

   VkShaderModule vs = nvk_load_shader(&c, tri_vert_spv, sizeof(tri_vert_spv));
   VkShaderModule fs = nvk_load_shader(&c, solid_frag_spv, sizeof(solid_frag_spv));
   if (!vs || !fs) { LOG("%s: FAIL shader modules", name); goto done; }

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout layout;
   if (CreatePipelineLayout(c.dev, &plci, NULL, &layout) != VK_SUCCESS) {
      LOG("%s: FAIL pipeline layout", name); goto done;
   }

   struct nvk_pipe_desc pd = {
      .vs = vs, .fs = fs, .layout = layout, .rp = t.rp,
      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkPipeline pipe;
   r = nvk_graphics_pipeline(&c, &pd, &pipe);
   if (r != VK_SUCCESS) { LOG("%s: FAIL pipeline -> %d", name, r); goto done; }

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   if (CreateCommandPool(c.dev, &pci, NULL, &pool) != VK_SUCCESS) {
      LOG("%s: FAIL pool", name); goto done;
   }

   VkCommandBuffer cb = nvk_begin_cb(&c, pool);
   VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = t.rp, .framebuffer = t.fb,
      .renderArea = { { 0, 0 }, { DIM, DIM } },
      .clearValueCount = 1, .pClearValues = &clear,
   };
   CmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   nvk_set_full_viewport(&c, cb, DIM, DIM);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   CmdDraw(cb, 3, 1, 0, 0);
   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(&c, cb, &t);
   r = nvk_end_submit_wait(&c, cb);
   if (r != VK_SUCCESS) { LOG("%s: FAIL render submit -> %d", name, r); goto done; }

   p->center = nvk_target_pixel(&t, DIM / 2, DIM / 2);
   p->corner = nvk_target_pixel(&t, 4, 4);
   p->render_ok = (p->center != CLEAR_U32) &&
                  ((p->center & 0x00FFFFFFu) != 0) &&
                  (p->corner == CLEAR_U32);
   LOG("%s: render center=0x%08x corner=0x%08x -> %s", name, p->center,
       p->corner, p->render_ok ? "ok" : "WRONG");

   struct nvk_buffer upload;
   r = nvk_host_buffer(&c, (VkDeviceSize)PIXELS * 4,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload);
   if (r != VK_SUCCESS) { LOG("%s: FAIL upload buffer -> %d", name, r); goto done; }

   uint32_t *src = (uint32_t *)upload.cpu;
   for (uint32_t i = 0; i < PIXELS; i++)
      src[i] = texel(i);
   memset(t.readback.cpu, 0, (size_t)PIXELS * 4);

   cb = nvk_begin_cb(&c, pool);
   nvk_image_barrier(&c, cb, t.color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
   VkBufferImageCopy region = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { DIM, DIM, 1 },
   };
   CmdCopyBufferToImage(cb, upload.buf, t.color,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
   nvk_image_barrier(&c, cb, t.color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
   nvk_target_copy_to_host(&c, cb, &t);
   r = nvk_end_submit_wait(&c, cb);
   if (r != VK_SUCCESS) { LOG("%s: FAIL transfer submit -> %d", name, r); goto done; }

   const uint32_t *got = (const uint32_t *)t.readback.cpu;
   for (uint32_t i = 0; i < PIXELS; i++) {
      if (got[i] != texel(i)) {
         if (!p->bad_texels) p->first_bad = i;
         p->bad_texels++;
      }
   }
   p->transfer_ok = p->bad_texels == 0;
   if (p->transfer_ok) {
      LOG("%s: transfer round-trip ok over %u texels", name, PIXELS);
   } else {
      LOG("%s: transfer round-trip WRONG, %u/%u texels, first at %u "
          "(got 0x%08x want 0x%08x)",
          name, p->bad_texels, PIXELS, p->first_bad, got[p->first_bad],
          texel(p->first_bad));
   }

   ok = true;

done:
   nvk_gfx_force_dedicated = false;
   if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
   nvk_teardown(&c);
   return ok;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_compress.log");
   LOG("=== nvk_compress ===");
   LOG("params: %ux%u RGBA8, dedicated allocation", DIM, DIM);

   struct phase plain = {0}, compressed = {0};

   setenv("NVK_DEBUG", "no_compression", 1);
   if (!run_phase("uncompressed", &plain)) goto fail;

   unsetenv("NVK_DEBUG");
   if (!run_phase("compressed", &compressed)) goto fail;

   if (!plain.render_ok || !plain.transfer_ok) {
      LOG("COMPRESS FAIL: the uncompressed control is already wrong");
      LOG("=== nvk_compress FAILED ===");
      goto out;
   }

   if (!compressed.render_ok || !compressed.transfer_ok) {
      LOG("COMPRESS FAIL: compression changed what the surface reads back");
      LOG("  render_ok=%d transfer_ok=%d bad_texels=%u",
          compressed.render_ok, compressed.transfer_ok, compressed.bad_texels);
      LOG("=== nvk_compress FAILED ===");
      goto out;
   }

   if (compressed.center != plain.center || compressed.corner != plain.corner) {
      LOG("COMPRESS FAIL: the two phases disagree, center 0x%08x vs 0x%08x, "
          "corner 0x%08x vs 0x%08x",
          compressed.center, plain.center, compressed.corner, plain.corner);
      LOG("=== nvk_compress FAILED ===");
      goto out;
   }

   LOG("COMPRESS OK: both phases render and round-trip identically");
   LOG("=== nvk_compress PASSED ===");

out:
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;

fail:
   LOG("=== nvk_compress FAILED ===");
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
