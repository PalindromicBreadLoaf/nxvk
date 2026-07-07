/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * mipmaps + sRGB decode + BC1 decompression.
 */
#include "nvk_gfx.h"
#include "shaders/tex_vert.h"
#include "shaders/tex_frag.h"

#define DIM 256
#define TEX 64

typedef struct { float x, y, u, v; } vtx;

static bool rgb_near(uint32_t a, uint8_t r, uint8_t g, uint8_t b, int tol)
{
   int dr = (int)(a & 0xFF) - r, dg = (int)((a >> 8) & 0xFF) - g, db = (int)((a >> 16) & 0xFF) - b;
   return dr > -tol && dr < tol && dg > -tol && dg < tol && db > -tol && db < tol;
}

static void quad(vtx *o, float x0, float x1)
{
   o[0] = (vtx){ x0, -1, 0, 0 }; o[1] = (vtx){ x1, -1, 1, 0 };
   o[2] = (vtx){ x0, 1, 0, 1 };  o[3] = (vtx){ x1, 1, 1, 1 };
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_textures.log");
   LOG("=== nvk_textures ===");

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_INST(&c, GetPhysicalDeviceFormatProperties);
   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, CmdBeginRenderPass);
   LOAD_DEV(&c, CmdEndRenderPass);
   LOAD_DEV(&c, CmdBindPipeline);
   LOAD_DEV(&c, CmdBindVertexBuffers);
   LOAD_DEV(&c, CmdBindDescriptorSets);
   LOAD_DEV(&c, CmdDraw);

   VkFormatProperties fp;
   GetPhysicalDeviceFormatProperties(c.phys, VK_FORMAT_BC1_RGB_UNORM_BLOCK, &fp);
   bool bc1 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
   LOG("BC1_RGB_UNORM_BLOCK sampled support: %s", bc1 ? "yes" : "no");

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) { LOG("FAIL: pool"); goto done; }

   static uint32_t srgb[TEX * TEX];
   for (int i = 0; i < TEX * TEX; i++) srgb[i] = 0xFFBCBCBCu;
   struct nvk_texture texA;
   VkResult r = nvk_make_texture(&c, pool, srgb, TEX, TEX, (VkDeviceSize)TEX * TEX * 4,
                                 VK_FORMAT_R8G8B8A8_SRGB, 1, true, &texA);
   LOG("sRGB+mip texture -> %d (levels=%u)", r, texA.levels);
   if (r != VK_SUCCESS) goto done;

   struct nvk_texture texB;
   if (bc1) {
      static const uint8_t block[8] = { 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
      r = nvk_make_texture(&c, pool, block, 4, 4, 8,
                           VK_FORMAT_BC1_RGB_UNORM_BLOCK, 1, false, &texB);
      LOG("BC1 texture -> %d", r);
      if (r != VK_SUCCESS) bc1 = false;
   }

   struct nvk_target t;
   r = nvk_color_target(&c, DIM, DIM, VK_FORMAT_R8G8B8A8_UNORM, false, &t);
   if (r != VK_SUCCESS) { LOG("FAIL: target -> %d", r); goto done; }

   VkDescriptorSetLayout la, lb = VK_NULL_HANDLE;
   VkDescriptorPool pa, pb;
   VkDescriptorSet sa, sb = VK_NULL_HANDLE;
   r = nvk_single_sampler_set(&c, &texA, &la, &pa, &sa);
   if (r != VK_SUCCESS) { LOG("FAIL: set A -> %d", r); goto done; }
   if (bc1) {
      r = nvk_single_sampler_set(&c, &texB, &lb, &pb, &sb);
      if (r != VK_SUCCESS) bc1 = false;
   }

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &la,
   };
   VkPipelineLayout layout;
   if (CreatePipelineLayout(c.dev, &plci, NULL, &layout) != VK_SUCCESS) { LOG("FAIL: layout"); goto done; }

   VkShaderModule vs = nvk_load_shader(&c, tex_vert_spv, sizeof(tex_vert_spv));
   VkShaderModule fs = nvk_load_shader(&c, tex_frag_spv, sizeof(tex_frag_spv));
   VkVertexInputBindingDescription vbind = { 0, sizeof(vtx), VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription vattr[2] = {
      { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
      { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vtx, u) },
   };
   struct nvk_pipe_desc pd = {
      .vs = vs, .fs = fs, .vbind = &vbind, .n_vbind = 1, .vattr = vattr, .n_vattr = 2,
      .layout = layout, .rp = t.rp, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
   };
   VkPipeline pipe;
   r = nvk_graphics_pipeline(&c, &pd, &pipe);
   LOG("pipeline -> %d", r);
   if (r != VK_SUCCESS) goto done;

   vtx ql[4], qr[4];
   quad(ql, -1.0f, 0.0f);
   quad(qr, 0.0f, 1.0f);
   struct nvk_buffer vl, vr;
   nvk_device_buffer(&c, pool, ql, sizeof(ql), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vl);
   nvk_device_buffer(&c, pool, qr, sizeof(qr), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vr);

   VkCommandBuffer cb = nvk_begin_cb(&c, pool);
   VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = t.rp, .framebuffer = t.fb, .renderArea = { { 0, 0 }, { DIM, DIM } },
      .clearValueCount = 1, .pClearValues = &clear,
   };
   CmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   nvk_set_full_viewport(&c, cb, DIM, DIM);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkDeviceSize off = 0;

   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sa, 0, NULL);
   CmdBindVertexBuffers(cb, 0, 1, &vl.buf, &off);
   CmdDraw(cb, 4, 1, 0, 0);
   if (bc1) {
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sb, 0, NULL);
      CmdBindVertexBuffers(cb, 0, 1, &vr.buf, &off);
      CmdDraw(cb, 4, 1, 0, 0);
   }
   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(&c, cb, &t);
   r = nvk_end_submit_wait(&c, cb);
   LOG("submit+wait -> %d", r);
   if (r != VK_SUCCESS) goto done;

   uint32_t left = nvk_target_pixel(&t, DIM / 4, DIM / 2);
   LOG("left(sRGB 0xBC)=0x%08x expect ~0x__808080", left);
   bool srgb_ok = rgb_near(left, 0x80, 0x80, 0x80, 24);
   bool bc1_ok = true;
   if (bc1) {
      uint32_t right = nvk_target_pixel(&t, 3 * DIM / 4, DIM / 2);
      LOG("right(BC1 red)=0x%08x expect ~red", right);
      bc1_ok = rgb_near(right, 0xFF, 0x00, 0x00, 40);
   }
   if (srgb_ok && bc1_ok)
      LOG("=== nvk_textures PASSED");
   else
      LOG("=== nvk_textures FAILED (sRGB=%d BC1=%d) ===", srgb_ok, bc1_ok);

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
