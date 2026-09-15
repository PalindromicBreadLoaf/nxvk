/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Verify push descriptor sets
 */
#include "nvk_gfx.h"

#include "shaders/pushdesc_vert.h"
#include "shaders/pushdesc_frag.h"
#include "shaders/pushdesc_comp.h"

#define N_SLOTS   24u
#define MAGIC     0x5A17C0DEu

#define N_DESC    (N_SLOTS + 1u)

#define SLOT_STRIDE 256u             /* >= minStorageBufferOffsetAlignment */
#define PHASES      2u               /* descriptor sets A and B */
#define TOTAL_SLOTS (N_SLOTS * PHASES)

#define DRAWS     64u                /* draws per row, pixels per row */
#define ROWS      3u

#define DISPATCHES (DRAWS * 3u + 1u)
#define RESULT_WORDS 512u
#define FILL_OFF    (DISPATCHES * 4u + 64u)
#define FILL_BYTES  256u

#define CLEAR_PIXEL 0xFF0000FFu
#define PASS_PIXEL  0xFF00FF00u

static uint32_t phase_base(uint32_t phase) { return phase * N_SLOTS; }

int main(void)
{
   nvk_log_open("sdmc:/nvk_push_desc.log");
   LOG("=== nvk_push_desc ===");
   LOG("params: %u descriptors/set (%u B of layout), %u draws/row, %u dispatches",
       N_DESC, N_DESC * 16u, DRAWS, DISPATCHES);

   struct nvk_ctx c;
   struct nvk_target t;
   struct nvk_buffer data = {0}, res = {0};
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, cs = VK_NULL_HANDLE;
   VkPipeline gfx_pipe = VK_NULL_HANDLE, comp_pipe = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   bool have_target = false;
   uint32_t fails = 0;

   memset(&t, 0, sizeof(t));

   const char *dev_exts[] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
   if (nvk_bringup(&c, dev_exts, 1) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   if (c.props.limits.minStorageBufferOffsetAlignment > SLOT_STRIDE) {
      LOG("FAIL: minStorageBufferOffsetAlignment %llu > slot stride %u",
          (unsigned long long)c.props.limits.minStorageBufferOffsetAlignment,
          SLOT_STRIDE);
      goto done;
   }

   LOAD_DEV(&c, CreateDescriptorSetLayout);
   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, CreateComputePipelines);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, CmdPushDescriptorSetKHR);
   LOAD_DEV(&c, CmdPushConstants);
   LOAD_DEV(&c, CmdBindPipeline);
   LOAD_DEV(&c, CmdBeginRenderPass);
   LOAD_DEV(&c, CmdEndRenderPass);
   LOAD_DEV(&c, CmdSetScissor);
   LOAD_DEV(&c, CmdDraw);
   LOAD_DEV(&c, CmdDispatch);
   LOAD_DEV(&c, CmdFillBuffer);
   LOAD_DEV(&c, CmdPipelineBarrier);
   if (!CreateDescriptorSetLayout || !CreatePipelineLayout ||
       !CreateComputePipelines || !CmdPushDescriptorSetKHR || !CmdDispatch ||
       !CmdFillBuffer) {
      LOG("FAIL: missing entrypoints");
      goto done;
   }

   if (nvk_host_buffer(&c, (VkDeviceSize)TOTAL_SLOTS * SLOT_STRIDE,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       &data) != VK_SUCCESS) { LOG("FAIL: data buffer"); goto done; }
   memset(data.cpu, 0xCD, (size_t)TOTAL_SLOTS * SLOT_STRIDE);
   for (uint32_t k = 0; k < TOTAL_SLOTS; k++)
      *(uint32_t *)((char *)data.cpu + (size_t)k * SLOT_STRIDE) = MAGIC ^ k;

   if (nvk_host_buffer(&c, RESULT_WORDS * 4u,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       &res) != VK_SUCCESS) { LOG("FAIL: result buffer"); goto done; }
   memset(res.cpu, 0xFF, RESULT_WORDS * 4u);

   VkDescriptorSetLayoutBinding binds[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = N_SLOTS,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
   };
   VkDescriptorSetLayoutCreateInfo slci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = 2, .pBindings = binds,
   };
   if (CreateDescriptorSetLayout(c.dev, &slci, NULL, &set_layout) != VK_SUCCESS) {
      LOG("FAIL: set layout"); goto done;
   }

   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0, .size = 8,
   };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
   };
   if (CreatePipelineLayout(c.dev, &plci, NULL, &pipe_layout) != VK_SUCCESS) {
      LOG("FAIL: pipeline layout"); goto done;
   }

   if (nvk_color_target(&c, DRAWS, ROWS, VK_FORMAT_R8G8B8A8_UNORM, false,
                        &t) != VK_SUCCESS) { LOG("FAIL: target"); goto done; }
   have_target = true;

   vs = nvk_load_shader(&c, pushdesc_vert_spv, sizeof(pushdesc_vert_spv));
   fs = nvk_load_shader(&c, pushdesc_frag_spv, sizeof(pushdesc_frag_spv));
   cs = nvk_load_shader(&c, pushdesc_comp_spv, sizeof(pushdesc_comp_spv));
   if (!vs || !fs || !cs) { LOG("FAIL: shader modules"); goto done; }

   struct nvk_pipe_desc pd = {
      .vs = vs, .fs = fs, .layout = pipe_layout, .rp = t.rp,
      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   if (nvk_graphics_pipeline(&c, &pd, &gfx_pipe) != VK_SUCCESS) {
      LOG("FAIL: graphics pipeline"); goto done;
   }

   VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs,
                 .pName = "main" },
      .layout = pipe_layout,
   };
   if (CreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cpci, NULL,
                              &comp_pipe) != VK_SUCCESS) {
      LOG("FAIL: compute pipeline"); goto done;
   }

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = c.qfi,
   };
   if (CreateCommandPool(c.dev, &pci, NULL, &pool) != VK_SUCCESS) {
      LOG("FAIL: command pool"); goto done;
   }

   VkDescriptorBufferInfo slot_info[PHASES][N_SLOTS];
   VkDescriptorBufferInfo res_info = { res.buf, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet writes[PHASES][2];
   for (uint32_t p = 0; p < PHASES; p++) {
      for (uint32_t i = 0; i < N_SLOTS; i++) {
         slot_info[p][i] = (VkDescriptorBufferInfo){
            .buffer = data.buf,
            .offset = (VkDeviceSize)(phase_base(p) + i) * SLOT_STRIDE,
            .range = SLOT_STRIDE,
         };
      }
      writes[p][0] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = 0, .descriptorCount = N_SLOTS,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = slot_info[p],
      };
      writes[p][1] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &res_info,
      };
   }

   VkCommandBuffer cb = nvk_begin_cb(&c, pool);
   if (cb == VK_NULL_HANDLE) { LOG("FAIL: command buffer"); goto done; }

#define PUSH_SET(bp, phase) \
   CmdPushDescriptorSetKHR(cb, (bp), pipe_layout, 0, 2, writes[phase])
#define PUSH_CONST(base_, index_) do {                                       \
      const uint32_t _pc[2] = { (base_), (index_) };                         \
      CmdPushConstants(cb, pipe_layout,                                      \
                       VK_SHADER_STAGE_FRAGMENT_BIT |                        \
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, _pc);              \
   } while (0)

   VkMemoryBarrier host_to_shader = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
   };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                      1, &host_to_shader, 0, NULL, 0, NULL);

   /* graphics :) */

   VkClearValue clear = { .color = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } } };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = t.rp, .framebuffer = t.fb,
      .renderArea = { { 0, 0 }, { t.w, t.h } },
      .clearValueCount = 1, .pClearValues = &clear,
   };
   CmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe);
   nvk_set_full_viewport(&c, cb, t.w, t.h);

   for (uint32_t row = 0; row < ROWS; row++) {
      if (row < PHASES) {
         PUSH_CONST(phase_base(row), 0);
         PUSH_SET(VK_PIPELINE_BIND_POINT_GRAPHICS, row);
      }
      for (uint32_t j = 0; j < DRAWS; j++) {
         if (row >= PHASES) {
            const uint32_t p = j & 1u;
            PUSH_CONST(phase_base(p), 0);
            PUSH_SET(VK_PIPELINE_BIND_POINT_GRAPHICS, p);
         }
         VkRect2D sc = { { (int32_t)j, (int32_t)row }, { 1, 1 } };
         CmdSetScissor(cb, 0, 1, &sc);
         CmdDraw(cb, 3, 1, 0, 0);
      }
   }

   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(&c, cb, &t);

   /* compute :( */

   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe);
   uint32_t out_index = 0;

   for (uint32_t run = 0; run < 3; run++) {
      if (run < PHASES)
         PUSH_SET(VK_PIPELINE_BIND_POINT_COMPUTE, run);
      for (uint32_t j = 0; j < DRAWS; j++) {
         const uint32_t p = (run < PHASES) ? run : (j & 1u);
         if (run >= PHASES)
            PUSH_SET(VK_PIPELINE_BIND_POINT_COMPUTE, p);
         PUSH_CONST(phase_base(p), out_index++);
         CmdDispatch(cb, 1, 1, 1);
      }
   }

   CmdFillBuffer(cb, res.buf, FILL_OFF, FILL_BYTES, 0xDEADBEEFu);

   VkMemoryBarrier fill_to_shader = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
   };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                      1, &fill_to_shader, 0, NULL, 0, NULL);

   PUSH_CONST(phase_base(1), out_index++);
   CmdDispatch(cb, 1, 1, 1);

   if (out_index != DISPATCHES) { LOG("BUG: %u dispatches, expected %u",
                                      out_index, DISPATCHES); goto done; }

   VkMemoryBarrier shader_to_host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_PIPELINE_STAGE_HOST_BIT, 0,
                      1, &shader_to_host, 0, NULL, 0, NULL);

#undef PUSH_SET
#undef PUSH_CONST

   VkResult r = nvk_end_submit_wait(&c, cb);
   if (r != VK_SUCCESS) { LOG("FAIL: submit -> %d", r); goto done; }

   /* results :| */

   for (uint32_t row = 0; row < ROWS; row++) {
      uint32_t bad_cols = 0, first_col = 0, first_px = 0;
      for (uint32_t j = 0; j < DRAWS; j++) {
         const uint32_t px = nvk_target_pixel(&t, j, row);
         if (px == PASS_PIXEL) continue;
         if (bad_cols++ == 0) { first_col = j; first_px = px; }
      }
      if (bad_cols == 0) {
         LOG("gfx row %u: %u draws, every descriptor matched", row, DRAWS);
      } else {
         fails++;
         LOG("gfx row %u FAIL: %u/%u draws wrong, first at column %u (0x%08x)",
             row, bad_cols, DRAWS, first_col, first_px);
         if (first_px == CLEAR_PIXEL)
            LOG("   that pixel was never drawn");
         else if ((first_px & 0xFFu) != 0u)
            LOG("   descriptor %u of the set read the wrong slot", (first_px & 0xFFu) - 1u);
      }
   }

   {
      const uint32_t *result = (const uint32_t *)res.cpu;
      uint32_t bad = 0, first_i = 0, first_v = 0;
      for (uint32_t i = 0; i < DISPATCHES; i++) {
         if (result[i] == 0) continue;
         if (bad++ == 0) { first_i = i; first_v = result[i]; }
      }
      if (bad == 0) {
         LOG("compute: %u dispatches, every descriptor matched", DISPATCHES);
      } else {
         fails++;
         LOG("compute FAIL: %u/%u dispatches wrong, first at %u (value %u)",
             bad, DISPATCHES, first_i, first_v);
         if (first_v == 0xFFFFFFFFu)
            LOG("   that dispatch never wrote");
         else
            LOG("   descriptor %u of the set read the wrong slot", first_v - 1u);
         if (first_i == DISPATCHES - 1u)
            LOG("   only the post-vkCmdFillBuffer dispatch");
      }
   }

   if (fails == 0) {
      LOG("=== nvk_push_desc PASSED ===");
   } else {
      LOG("=== nvk_push_desc FAILED: %u of 4 checks ===", fails);
   }

done:
   if (c.dev) {
      LOAD_DEV(&c, DeviceWaitIdle);
      LOAD_DEV(&c, DestroyPipeline);
      LOAD_DEV(&c, DestroyPipelineLayout);
      LOAD_DEV(&c, DestroyDescriptorSetLayout);
      LOAD_DEV(&c, DestroyShaderModule);
      LOAD_DEV(&c, DestroyCommandPool);
      if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
      if (pool) DestroyCommandPool(c.dev, pool, NULL);
      if (gfx_pipe) DestroyPipeline(c.dev, gfx_pipe, NULL);
      if (comp_pipe) DestroyPipeline(c.dev, comp_pipe, NULL);
      if (vs) DestroyShaderModule(c.dev, vs, NULL);
      if (fs) DestroyShaderModule(c.dev, fs, NULL);
      if (cs) DestroyShaderModule(c.dev, cs, NULL);
      if (pipe_layout) DestroyPipelineLayout(c.dev, pipe_layout, NULL);
      if (set_layout) DestroyDescriptorSetLayout(c.dev, set_layout, NULL);
      if (have_target) nvk_free_target(&c, &t);
      nvk_free_buffer(&c, &res);
      nvk_free_buffer(&c, &data);
   }
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
