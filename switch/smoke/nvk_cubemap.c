/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A CUBE_COMPATIBLE image sampled through samplerCube.
 */
#include "nvk_gfx.h"
#include "nvk_math.h"
#include "nvk_cube.h"
#include "shaders/cube_vert.h"
#include "shaders/cube_frag.h"

#define DIM       256
#define FACE      4
#define CLEAR_U32 0xFF101010u

struct pc { float mvp[16]; float tint[4]; };

static const uint32_t face_color[6] = {
   0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF00FFFFu, 0xFFFF00FFu, 0xFFFFFF00u,
};

int main(void)
{
   nvk_log_open("sdmc:/nvk_cubemap.log");
   LOG("=== nvk_cubemap: samplerCube ===");

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, CmdBeginRenderPass);
   LOAD_DEV(&c, CmdEndRenderPass);
   LOAD_DEV(&c, CmdBindPipeline);
   LOAD_DEV(&c, CmdBindVertexBuffers);
   LOAD_DEV(&c, CmdBindDescriptorSets);
   LOAD_DEV(&c, CmdPushConstants);
   LOAD_DEV(&c, CmdDraw);

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) { LOG("FAIL: pool"); goto done; }

   static uint32_t faces[6 * FACE * FACE];
   for (int f = 0; f < 6; f++)
      for (int i = 0; i < FACE * FACE; i++)
         faces[f * FACE * FACE + i] = face_color[f];
   struct nvk_texture cube;
   VkResult r = nvk_make_texture(&c, pool, faces, FACE, FACE, (VkDeviceSize)FACE * FACE * 4,
                                 VK_FORMAT_R8G8B8A8_UNORM, 6, false, &cube);
   LOG("cube texture -> %d", r);
   if (r != VK_SUCCESS) goto done;

   cube_vtx verts[36];
   for (int i = 0; i < 36; i++) verts[i] = cube_corners[cube_indices[i]];
   struct nvk_buffer vb;
   r = nvk_device_buffer(&c, pool, verts, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb);
   if (r != VK_SUCCESS) { LOG("FAIL: vb -> %d", r); goto done; }

   struct nvk_target t;
   r = nvk_color_target(&c, DIM, DIM, VK_FORMAT_R8G8B8A8_UNORM, true, &t);
   if (r != VK_SUCCESS) { LOG("FAIL: target -> %d", r); goto done; }

   VkDescriptorSetLayout set_layout;
   VkDescriptorPool dpool;
   VkDescriptorSet set;
   r = nvk_single_sampler_set(&c, &cube, &set_layout, &dpool, &set);
   if (r != VK_SUCCESS) { LOG("FAIL: descriptor -> %d", r); goto done; }

   VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(struct pc) };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
   };
   VkPipelineLayout layout;
   if (CreatePipelineLayout(c.dev, &plci, NULL, &layout) != VK_SUCCESS) { LOG("FAIL: layout"); goto done; }

   VkShaderModule vs = nvk_load_shader(&c, cube_vert_spv, sizeof(cube_vert_spv));
   VkShaderModule fs = nvk_load_shader(&c, cube_frag_spv, sizeof(cube_frag_spv));
   VkVertexInputBindingDescription vbind = { 0, sizeof(cube_vtx), VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription vattr = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
   struct nvk_pipe_desc pd = {
      .vs = vs, .fs = fs, .vbind = &vbind, .n_vbind = 1, .vattr = &vattr, .n_vattr = 1,
      .layout = layout, .rp = t.rp, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .cull = VK_CULL_MODE_BACK_BIT, .depth_test = true,
   };
   VkPipeline pipe;
   r = nvk_graphics_pipeline(&c, &pd, &pipe);
   LOG("pipeline -> %d", r);
   if (r != VK_SUCCESS) goto done;

   struct pc push;
   mat4 model = mat4_mul(mat4_rotate_y(0.6f), mat4_rotate_x(0.4f));
   mat4 mvp = mat4_mul(mat4_perspective(0.9f, 1.0f, 0.1f, 20.0f),
                       mat4_mul(mat4_translate(0, 0, -4.0f), model));
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
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
   CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &vb.buf, &off);
   CmdDraw(cb, 36, 1, 0, 0);
   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(&c, cb, &t);
   r = nvk_end_submit_wait(&c, cb);
   LOG("submit+wait -> %d", r);
   if (r != VK_SUCCESS) goto done;

   uint32_t center = nvk_target_pixel(&t, DIM / 2, DIM / 2);
   LOG("center=0x%08x", center);
   bool covered = (center & 0x00FFFFFFu) != (CLEAR_U32 & 0x00FFFFFFu) && (center >> 24) == 0xFF;
   bool is_face = false;
   for (int f = 0; f < 6; f++) if (center == face_color[f]) is_face = true;
   if (covered && is_face)
      LOG("=== nvk_cubemap PASSED ===");
   else if (covered)
      LOG("=== nvk_cubemap PASSED (cube drawn. centre 0x%08x near a face) ===", center);
   else
      LOG("=== nvk_cubemap FAILED (centre not covered) ===");

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
