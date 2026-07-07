/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * multiple draws with a pipeline switch and alpha blending.
 */
#include "nvk_gfx.h"
#include "nvk_math.h"
#include "shaders/mvp_vert.h"
#include "shaders/vcolor_frag.h"

#define DIM 256

struct pc { float mvp[16]; float tint[4]; };
typedef struct { float pos[3]; float col[3]; } vtx;

static void quad(vtx *out, float s, float r, float g, float b)
{
   float p[4][2] = { { -s, -s }, { s, -s }, { s, s }, { -s, s } };
   int tri[6] = { 0, 1, 2, 0, 2, 3 };
   for (int i = 0; i < 6; i++) {
      out[i] = (vtx){ { p[tri[i]][0], p[tri[i]][1], 0 }, { r, g, b } };
   }
}

static bool rgb_near(uint32_t a, uint8_t r, uint8_t g, uint8_t b)
{
   int dr = (int)(a & 0xFF) - r, dg = (int)((a >> 8) & 0xFF) - g, db = (int)((a >> 16) & 0xFF) - b;
   return dr > -24 && dr < 24 && dg > -24 && dg < 24 && db > -24 && db < 24;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_multi.log");
   LOG("=== nvk_multi ===");

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, CmdBeginRenderPass);
   LOAD_DEV(&c, CmdEndRenderPass);
   LOAD_DEV(&c, CmdBindPipeline);
   LOAD_DEV(&c, CmdBindVertexBuffers);
   LOAD_DEV(&c, CmdPushConstants);
   LOAD_DEV(&c, CmdDraw);

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) { LOG("FAIL: pool"); goto done; }

   vtx bg[6], fg[6];
   quad(bg, 1.0f, 0.0f, 0.0f, 1.0f);
   quad(fg, 0.5f, 1.0f, 0.0f, 0.0f);
   struct nvk_buffer vbg, vfg;
   VkResult r = nvk_device_buffer(&c, pool, bg, sizeof(bg), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vbg);
   if (r != VK_SUCCESS) { LOG("FAIL: vbg -> %d", r); goto done; }
   r = nvk_device_buffer(&c, pool, fg, sizeof(fg), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vfg);
   if (r != VK_SUCCESS) { LOG("FAIL: vfg -> %d", r); goto done; }

   struct nvk_target t;
   r = nvk_color_target(&c, DIM, DIM, VK_FORMAT_R8G8B8A8_UNORM, false, &t);
   if (r != VK_SUCCESS) { LOG("FAIL: target -> %d", r); goto done; }

   VkPushConstantRange pcr = {
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct pc),
   };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
   };
   VkPipelineLayout layout;
   if (CreatePipelineLayout(c.dev, &plci, NULL, &layout) != VK_SUCCESS) { LOG("FAIL: layout"); goto done; }

   VkShaderModule vs = nvk_load_shader(&c, mvp_vert_spv, sizeof(mvp_vert_spv));
   VkShaderModule fs = nvk_load_shader(&c, vcolor_frag_spv, sizeof(vcolor_frag_spv));
   VkVertexInputBindingDescription vbind = { 0, sizeof(vtx), VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription vattr[2] = {
      { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
      { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vtx, col) },
   };
   struct nvk_pipe_desc base = {
      .vs = vs, .fs = fs, .vbind = &vbind, .n_vbind = 1, .vattr = vattr, .n_vattr = 2,
      .layout = layout, .rp = t.rp, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkPipeline pipe_opaque, pipe_blend;
   r = nvk_graphics_pipeline(&c, &base, &pipe_opaque);
   if (r != VK_SUCCESS) { LOG("FAIL: opaque pipe -> %d", r); goto done; }
   base.blend = true;
   r = nvk_graphics_pipeline(&c, &base, &pipe_blend);
   if (r != VK_SUCCESS) { LOG("FAIL: blend pipe -> %d", r); goto done; }

   mat4 id = mat4_identity();
   struct pc p_bg = { { 0 }, { 1, 1, 1, 1.0f } };
   struct pc p_fg = { { 0 }, { 1, 1, 1, 0.5f } };
   memcpy(p_bg.mvp, id.m, sizeof(id.m));
   memcpy(p_fg.mvp, id.m, sizeof(id.m));

   VkCommandBuffer cb = nvk_begin_cb(&c, pool);
   VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = t.rp, .framebuffer = t.fb, .renderArea = { { 0, 0 }, { DIM, DIM } },
      .clearValueCount = 1, .pClearValues = &clear,
   };
   CmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   nvk_set_full_viewport(&c, cb, DIM, DIM);
   VkDeviceSize off = 0;

   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_opaque);
   CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(p_bg), &p_bg);
   CmdBindVertexBuffers(cb, 0, 1, &vbg.buf, &off);
   CmdDraw(cb, 6, 1, 0, 0);

   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_blend);
   CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(p_fg), &p_fg);
   CmdBindVertexBuffers(cb, 0, 1, &vfg.buf, &off);
   CmdDraw(cb, 6, 1, 0, 0);

   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(&c, cb, &t);
   r = nvk_end_submit_wait(&c, cb);
   LOG("submit+wait -> %d", r);
   if (r != VK_SUCCESS) goto done;

   uint32_t center = nvk_target_pixel(&t, DIM / 2, DIM / 2);
   uint32_t border = nvk_target_pixel(&t, DIM / 2, 12);
   LOG("center=0x%08x border=0x%08x", center, border);
   bool blended = rgb_near(center, 128, 0, 128);
   bool bg_blue = rgb_near(border, 0, 0, 255);
   if (blended && bg_blue)
      LOG("=== nvk_multi PASSED ===");
   else
      LOG("=== nvk_multi FAILED (blended=%d bg_blue=%d) ===", blended, bg_blue);

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
