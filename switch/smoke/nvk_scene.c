/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * a 3D cube with a ZETA depth buffer.
 */
#include "nvk_gfx.h"
#include "nvk_math.h"
#include "nvk_cube.h"
#include "shaders/mvp_vert.h"
#include "shaders/vcolor_frag.h"

#define DIM       256
#define CLEAR_U32 0xFF101010u

struct pc { float mvp[16]; float tint[4]; };

int main(void)
{
   nvk_log_open("sdmc:/nvk_scene.log");
   LOG("=== nvk_scene ===");

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

   cube_vtx verts[36];
   for (int i = 0; i < 36; i++) verts[i] = cube_corners[cube_indices[i]];
   struct nvk_buffer vb;
   VkResult r = nvk_device_buffer(&c, pool, verts, sizeof(verts),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb);
   if (r != VK_SUCCESS) { LOG("FAIL: vertex buffer -> %d", r); goto done; }

   struct nvk_target t;
   r = nvk_color_target(&c, DIM, DIM, VK_FORMAT_R8G8B8A8_UNORM, true, &t);
   LOG("color+depth target -> %d", r);
   if (r != VK_SUCCESS) goto done;

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
   VkVertexInputBindingDescription vbind = { 0, sizeof(cube_vtx), VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription vattr[2] = {
      { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
      { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(cube_vtx, col) },
   };
   struct nvk_pipe_desc pd = {
      .vs = vs, .fs = fs, .vbind = &vbind, .n_vbind = 1, .vattr = vattr, .n_vattr = 2,
      .layout = layout, .rp = t.rp, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .cull = VK_CULL_MODE_BACK_BIT, .depth_test = true,
   };
   VkPipeline pipe;
   r = nvk_graphics_pipeline(&c, &pd, &pipe);
   LOG("pipeline -> %d", r);
   if (r != VK_SUCCESS) goto done;

   struct pc push;
   mat4 model = mat4_mul(mat4_rotate_y(0.7f), mat4_rotate_x(0.5f));
   mat4 view = mat4_translate(0, 0, -4.5f);
   mat4 proj = mat4_perspective(0.9f, 1.0f, 0.1f, 20.0f);
   mat4 mvp = mat4_mul(proj, mat4_mul(view, model));
   memcpy(push.mvp, mvp.m, sizeof(mvp.m));
   push.tint[0] = push.tint[1] = push.tint[2] = push.tint[3] = 1.0f;

   VkCommandBuffer cb = nvk_begin_cb(&c, pool);
   VkClearValue clears[2] = {
      { .color = { .float32 = { 0.0627f, 0.0627f, 0.0627f, 1.0f } } },
      { .depthStencil = { 1.0f, 0 } },
   };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = t.rp, .framebuffer = t.fb, .renderArea = { { 0, 0 }, { DIM, DIM } },
      .clearValueCount = 2, .pClearValues = clears,
   };
   CmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   nvk_set_full_viewport(&c, cb, DIM, DIM);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &vb.buf, &off);
   CmdDraw(cb, 36, 1, 0, 0);
   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(&c, cb, &t);
   r = nvk_end_submit_wait(&c, cb);
   LOG("submit+wait -> %d", r);
   if (r != VK_SUCCESS) goto done;

   uint32_t center = nvk_target_pixel(&t, DIM / 2, DIM / 2);
   uint32_t corner = nvk_target_pixel(&t, 3, 3);
   LOG("center=0x%08x corner=0x%08x (clear=0x%08x)", center, corner, CLEAR_U32);
   if ((center & 0x00FFFFFFu) != (CLEAR_U32 & 0x00FFFFFFu) && (center >> 24) == 0xFF)
      LOG("=== nvk_scene PASSED ===");
   else
      LOG("=== nvk_scene FAILED (centre not covered by cube) ===");

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
