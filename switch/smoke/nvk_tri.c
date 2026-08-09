/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * offscreen triangle
 */
#include "nvk_gfx.h"
#include "shaders/tri_vert.h"
#include "shaders/solid_frag.h"

#define DIM       256
#define CLEAR_U32 0xFF000000u

int main(void)
{
   nvk_log_open("sdmc:/nvk_tri.log");
   LOG("=== nvk_tri ===");

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, CmdBeginRenderPass);
   LOAD_DEV(&c, CmdEndRenderPass);
   LOAD_DEV(&c, CmdBindPipeline);
   LOAD_DEV(&c, CmdDraw);

   struct nvk_target t;
   VkResult r = nvk_color_target(&c, DIM, DIM, VK_FORMAT_R8G8B8A8_UNORM, false, &t);
   LOG("color target -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkShaderModule vs = nvk_load_shader(&c, tri_vert_spv, sizeof(tri_vert_spv));
   VkShaderModule fs = nvk_load_shader(&c, solid_frag_spv, sizeof(solid_frag_spv));
   if (!vs || !fs) { LOG("FAIL: shader modules"); goto done; }

   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout layout;
   r = CreatePipelineLayout(c.dev, &plci, NULL, &layout);
   if (r != VK_SUCCESS) { LOG("FAIL vkCreatePipelineLayout -> %d", r); goto done; }

   struct nvk_pipe_desc pd = {
      .vs = vs, .fs = fs, .layout = layout, .rp = t.rp,
      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkPipeline pipe;
   r = nvk_graphics_pipeline(&c, &pd, &pipe);
   LOG("graphics pipeline -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   if (CreateCommandPool(c.dev, &pci, NULL, &pool) != VK_SUCCESS) { LOG("FAIL: pool"); goto done; }

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
   LOG("submit+wait -> %d", r);
   if (r != VK_SUCCESS) goto done;

   uint32_t center = nvk_target_pixel(&t, DIM / 2, DIM / 2);
   uint32_t corner = nvk_target_pixel(&t, 4, 4);
   LOG("center=0x%08x corner=0x%08x (clear=0x%08x)", center, corner, CLEAR_U32);

   bool center_shaded = (center != CLEAR_U32) && ((center & 0x00FFFFFFu) != 0);
   bool corner_clear  = (corner == CLEAR_U32);
   if (center_shaded && corner_clear)
      LOG("=== nvk_tri PASSED ===");
   else
      LOG("=== nvk_tri FAILED (center_shaded=%d corner_clear=%d) ===",
          center_shaded, corner_clear);

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
