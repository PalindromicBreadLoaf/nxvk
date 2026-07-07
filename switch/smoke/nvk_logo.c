/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * textured quad
 */
#include "nvk_gfx.h"
#include "shaders/tex_vert.h"
#include "shaders/tex_frag.h"

#define DIM 256
#define TEX 64

#define RED   0xFF0000FFu
#define GREEN 0xFF00FF00u
#define BLUE  0xFFFF0000u
#define WHITE 0xFFFFFFFFu

typedef struct { float x, y, u, v; } vtx;

static bool near_u32(uint32_t a, uint32_t b)
{
   for (int i = 0; i < 4; i++) {
      int da = (int)((a >> (i * 8)) & 0xFF) - (int)((b >> (i * 8)) & 0xFF);
      if (da < -16 || da > 16) return false;
   }
   return true;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_logo.log");
   LOG("=== nvk_logo ===");

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, CmdBeginRenderPass);
   LOAD_DEV(&c, CmdEndRenderPass);
   LOAD_DEV(&c, CmdBindPipeline);
   LOAD_DEV(&c, CmdBindVertexBuffers);
   LOAD_DEV(&c, CmdBindDescriptorSets);
   LOAD_DEV(&c, CmdDraw);

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) { LOG("FAIL: pool"); goto done; }

   /* 4-quadrant texture. */
   static uint32_t texels[TEX * TEX];
   for (uint32_t y = 0; y < TEX; y++)
      for (uint32_t x = 0; x < TEX; x++) {
         uint32_t q = (x < TEX / 2 ? 0 : 1) | (y < TEX / 2 ? 0 : 2);
         texels[y * TEX + x] = (q == 0) ? RED : (q == 1) ? GREEN : (q == 2) ? BLUE : WHITE;
      }
   struct nvk_texture tex;
   VkResult r = nvk_make_texture(&c, pool, texels, TEX, TEX, (VkDeviceSize)TEX * TEX * 4,
                                 VK_FORMAT_R8G8B8A8_UNORM, 1, false, &tex);
   LOG("texture -> %d", r);
   if (r != VK_SUCCESS) goto done;

   vtx verts[4] = {
      { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { -1, 1, 0, 1 }, { 1, 1, 1, 1 },
   };
   struct nvk_buffer vb;
   r = nvk_device_buffer(&c, pool, verts, sizeof(verts),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb);
   if (r != VK_SUCCESS) { LOG("FAIL: vertex buffer -> %d", r); goto done; }

   struct nvk_target t;
   r = nvk_color_target(&c, DIM, DIM, VK_FORMAT_R8G8B8A8_UNORM, false, &t);
   if (r != VK_SUCCESS) { LOG("FAIL: target -> %d", r); goto done; }

   VkDescriptorSetLayout set_layout;
   VkDescriptorPool dpool;
   VkDescriptorSet set;
   r = nvk_single_sampler_set(&c, &tex, &set_layout, &dpool, &set);
   if (r != VK_SUCCESS) { LOG("FAIL: descriptor set -> %d", r); goto done; }

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &set_layout,
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
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &vb.buf, &off);
   CmdDraw(cb, 4, 1, 0, 0);
   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(&c, cb, &t);
   r = nvk_end_submit_wait(&c, cb);
   LOG("submit+wait -> %d", r);
   if (r != VK_SUCCESS) goto done;

   uint32_t tl = nvk_target_pixel(&t, DIM / 4, DIM / 4);
   uint32_t tr = nvk_target_pixel(&t, 3 * DIM / 4, DIM / 4);
   uint32_t bl = nvk_target_pixel(&t, DIM / 4, 3 * DIM / 4);
   uint32_t br = nvk_target_pixel(&t, 3 * DIM / 4, 3 * DIM / 4);
   LOG("tl=0x%08x tr=0x%08x bl=0x%08x br=0x%08x", tl, tr, bl, br);
   if (near_u32(tl, RED) && near_u32(tr, GREEN) && near_u32(bl, BLUE) && near_u32(br, WHITE))
      LOG("=== nvk_logo PASSED  ===");
   else
      LOG("=== nvk_logo FAILED (quadrant colours wrong) ===");

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
