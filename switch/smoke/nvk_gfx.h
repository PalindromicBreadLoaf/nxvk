/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared graphics scaffolding for the offscreen rendering validation apps.
 */
#ifndef NVK_GFX_H
#define NVK_GFX_H

#include "nvk_harness.h"

/* buffers */

struct nvk_buffer {
   VkBuffer       buf;
   VkDeviceMemory mem;
   void          *cpu;  /* non-NULL for host-visible buffers */
   VkDeviceSize   size;
};

static VkResult nvk_host_buffer(struct nvk_ctx *c, VkDeviceSize size,
                                VkBufferUsageFlags usage, struct nvk_buffer *out)
{
   LOAD_DEV(c, CreateBuffer);
   LOAD_DEV(c, GetBufferMemoryRequirements);
   LOAD_DEV(c, AllocateMemory);
   LOAD_DEV(c, MapMemory);
   LOAD_DEV(c, BindBufferMemory);

   memset(out, 0, sizeof(*out));
   out->size = size;
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult r = CreateBuffer(c->dev, &bci, NULL, &out->buf);
   if (r != VK_SUCCESS) return r;

   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(c->dev, out->buf, &mr);
   uint32_t mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (mt == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   r = AllocateMemory(c->dev, &mai, NULL, &out->mem);
   if (r != VK_SUCCESS) return r;
   r = MapMemory(c->dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->cpu);
   if (r != VK_SUCCESS) return r;
   return BindBufferMemory(c->dev, out->buf, out->mem, 0);
}

/* command submission */

static VkCommandBuffer nvk_begin_cb(struct nvk_ctx *c, VkCommandPool pool)
{
   LOAD_DEV(c, AllocateCommandBuffers);
   LOAD_DEV(c, BeginCommandBuffer);
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb = VK_NULL_HANDLE;
   if (AllocateCommandBuffers(c->dev, &cbai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   BeginCommandBuffer(cb, &bi);
   return cb;
}

static VkResult nvk_end_submit_wait(struct nvk_ctx *c, VkCommandBuffer cb)
{
   LOAD_DEV(c, EndCommandBuffer);
   LOAD_DEV(c, QueueSubmit);
   LOAD_DEV(c, QueueWaitIdle);
   VkResult r = EndCommandBuffer(cb);
   if (r != VK_SUCCESS) return r;
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb,
   };
   r = QueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE);
   if (r != VK_SUCCESS) return r;
   return QueueWaitIdle(c->queue);
}

static VkResult nvk_device_buffer(struct nvk_ctx *c, VkCommandPool pool,
                                  const void *data, VkDeviceSize size,
                                  VkBufferUsageFlags usage, struct nvk_buffer *out)
{
   LOAD_DEV(c, CreateBuffer);
   LOAD_DEV(c, GetBufferMemoryRequirements);
   LOAD_DEV(c, AllocateMemory);
   LOAD_DEV(c, BindBufferMemory);
   LOAD_DEV(c, CmdCopyBuffer);

   struct nvk_buffer stg;
   VkResult r = nvk_host_buffer(c, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg);
   if (r != VK_SUCCESS) return r;
   memcpy(stg.cpu, data, size);

   memset(out, 0, sizeof(*out));
   out->size = size;
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size, .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   r = CreateBuffer(c->dev, &bci, NULL, &out->buf);
   if (r != VK_SUCCESS) return r;
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(c->dev, out->buf, &mr);
   uint32_t mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt == UINT32_MAX)
      mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits, 0);
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   r = AllocateMemory(c->dev, &mai, NULL, &out->mem);
   if (r != VK_SUCCESS) return r;
   r = BindBufferMemory(c->dev, out->buf, out->mem, 0);
   if (r != VK_SUCCESS) return r;

   VkCommandBuffer cb = nvk_begin_cb(c, pool);
   VkBufferCopy region = { .size = size };
   CmdCopyBuffer(cb, stg.buf, out->buf, 1, &region);
   return nvk_end_submit_wait(c, cb);
}

/* offscreen colour target */

struct nvk_target {
   uint32_t          w, h;
   VkFormat          color_fmt;
   VkImage           color;
   VkDeviceMemory    color_mem;
   VkImageView       color_view;
   VkImage           depth;
   VkDeviceMemory    depth_mem;
   VkImageView       depth_view;
   VkRenderPass      rp;
   VkFramebuffer     fb;
   struct nvk_buffer readback;  /* w*h*4 bytes */
};

static bool nvk_gfx_force_dedicated;

static VkResult nvk_alloc_bind_image(struct nvk_ctx *c, VkImage img,
                                     VkDeviceMemory *mem)
{
   LOAD_DEV(c, GetImageMemoryRequirements);
   LOAD_DEV(c, AllocateMemory);
   LOAD_DEV(c, BindImageMemory);
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(c->dev, img, &mr);
   uint32_t mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt == UINT32_MAX) mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits, 0);
   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img,
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = nvk_gfx_force_dedicated ? &dedicated : NULL,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   VkResult r = AllocateMemory(c->dev, &mai, NULL, mem);
   if (r != VK_SUCCESS) return r;
   return BindImageMemory(c->dev, img, *mem, 0);
}

static VkResult nvk_color_target(struct nvk_ctx *c, uint32_t w, uint32_t h,
                                 VkFormat color_fmt, bool with_depth,
                                 struct nvk_target *t)
{
   LOAD_DEV(c, CreateImage);
   LOAD_DEV(c, CreateImageView);
   LOAD_DEV(c, CreateRenderPass);
   LOAD_DEV(c, CreateFramebuffer);

   memset(t, 0, sizeof(*t));
   t->w = w; t->h = h; t->color_fmt = color_fmt;

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = color_fmt,
      .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = CreateImage(c->dev, &ici, NULL, &t->color);
   if (r != VK_SUCCESS) return r;
   r = nvk_alloc_bind_image(c, t->color, &t->color_mem);
   if (r != VK_SUCCESS) return r;

   VkImageViewCreateInfo cvi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = t->color, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = color_fmt,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   r = CreateImageView(c->dev, &cvi, NULL, &t->color_view);
   if (r != VK_SUCCESS) return r;

   VkAttachmentDescription atts[2];
   VkImageView views[2];
   uint32_t n_att = 1;
   atts[0] = (VkAttachmentDescription){
      .format = color_fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   };
   views[0] = t->color_view;
   VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

   if (with_depth) {
      VkImageCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
         .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      r = CreateImage(c->dev, &dci, NULL, &t->depth);
      if (r != VK_SUCCESS) return r;
      r = nvk_alloc_bind_image(c, t->depth, &t->depth_mem);
      if (r != VK_SUCCESS) return r;
      VkImageViewCreateInfo dvi = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = t->depth, .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_D32_SFLOAT,
         .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
      };
      r = CreateImageView(c->dev, &dvi, NULL, &t->depth_view);
      if (r != VK_SUCCESS) return r;
      atts[1] = (VkAttachmentDescription){
         .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      };
      views[1] = t->depth_view;
      n_att = 2;
   }

   VkSubpassDescription sub = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = with_depth ? &depth_ref : NULL,
   };
   /* Make the colour write finish before the post-pass image to buffer copy. */
   VkSubpassDependency dep = {
      .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   VkRenderPassCreateInfo rpci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = n_att, .pAttachments = atts,
      .subpassCount = 1, .pSubpasses = &sub,
      .dependencyCount = 1, .pDependencies = &dep,
   };
   r = CreateRenderPass(c->dev, &rpci, NULL, &t->rp);
   if (r != VK_SUCCESS) return r;

   VkFramebufferCreateInfo fbci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = t->rp, .attachmentCount = n_att, .pAttachments = views,
      .width = w, .height = h, .layers = 1,
   };
   r = CreateFramebuffer(c->dev, &fbci, NULL, &t->fb);
   if (r != VK_SUCCESS) return r;

   return nvk_host_buffer(c, (VkDeviceSize)w * h * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, &t->readback);
}

/* Record the colour image to readback-buffer copy. */
static void nvk_target_copy_to_host(struct nvk_ctx *c, VkCommandBuffer cb,
                                    struct nvk_target *t)
{
   LOAD_DEV(c, CmdCopyImageToBuffer);
   VkBufferImageCopy region = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { t->w, t->h, 1 },
   };
   CmdCopyImageToBuffer(cb, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        t->readback.buf, 1, &region);
}

/* Read a pixel from the readback buffer after submit. */
static uint32_t nvk_target_pixel(struct nvk_target *t, uint32_t x, uint32_t y)
{
   const uint32_t *px = (const uint32_t *)t->readback.cpu;
   return px[(size_t)y * t->w + x];
}

/* shaders and pipelines */

static VkShaderModule nvk_load_shader(struct nvk_ctx *c, const uint32_t *spv,
                                      size_t bytes)
{
   LOAD_DEV(c, CreateShaderModule);
   VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = bytes, .pCode = spv,
   };
   VkShaderModule m = VK_NULL_HANDLE;
   CreateShaderModule(c->dev, &smci, NULL, &m);
   return m;
}

struct nvk_pipe_desc {
   VkShaderModule                             vs, fs;
   const VkVertexInputBindingDescription     *vbind;
   uint32_t                                    n_vbind;
   const VkVertexInputAttributeDescription   *vattr;
   uint32_t                                    n_vattr;
   VkPipelineLayout                            layout;
   VkRenderPass                                rp;
   VkPrimitiveTopology                         topo;
   VkCullModeFlags                             cull;
   bool                                        depth_test;
   bool                                        blend;
};

static VkResult nvk_graphics_pipeline(struct nvk_ctx *c,
                                      const struct nvk_pipe_desc *d,
                                      VkPipeline *out)
{
   LOAD_DEV(c, CreateGraphicsPipelines);

   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = d->vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = d->fs, .pName = "main" },
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = d->n_vbind, .pVertexBindingDescriptions = d->vbind,
      .vertexAttributeDescriptionCount = d->n_vattr, .pVertexAttributeDescriptions = d->vattr,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = d->topo,
   };
   VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .scissorCount = 1,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = d->cull,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = d->depth_test, .depthWriteEnable = d->depth_test,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
   };
   VkPipelineColorBlendAttachmentState cba = {
      .blendEnable = d->blend,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba,
   };
   VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo dyn = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2, .pDynamicStates = dyn_states,
   };
   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia,
      .pViewportState = &vp, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pDepthStencilState = &ds,
      .pColorBlendState = &cb, .pDynamicState = &dyn,
      .layout = d->layout, .renderPass = d->rp, .subpass = 0,
   };
   return CreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gpci, NULL, out);
}

static void nvk_set_full_viewport(struct nvk_ctx *c, VkCommandBuffer cb,
                                  uint32_t w, uint32_t h)
{
   LOAD_DEV(c, CmdSetViewport);
   LOAD_DEV(c, CmdSetScissor);
   VkViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
   VkRect2D sc = { { 0, 0 }, { w, h } };
   CmdSetViewport(cb, 0, 1, &vp);
   CmdSetScissor(cb, 0, 1, &sc);
}

/* textures and descriptors */

struct nvk_texture {
   VkImage        img;
   VkDeviceMemory mem;
   VkImageView    view;
   VkSampler      sampler;
   uint32_t       w, h, levels, layers;
};

static void nvk_image_barrier(struct nvk_ctx *c, VkCommandBuffer cb, VkImage img,
                              VkImageAspectFlags aspect, uint32_t base_level,
                              uint32_t levels, uint32_t layers,
                              VkImageLayout old_l, VkImageLayout new_l,
                              VkAccessFlags src_a, VkAccessFlags dst_a,
                              VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s)
{
   LOAD_DEV(c, CmdPipelineBarrier);
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_a, .dstAccessMask = dst_a,
      .oldLayout = old_l, .newLayout = new_l,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = { aspect, base_level, levels, 0, layers },
   };
   CmdPipelineBarrier(cb, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Create a sampled 2D (or cube) texture. */
static VkResult nvk_make_texture(struct nvk_ctx *c, VkCommandPool pool,
                                 const void *data, uint32_t w, uint32_t h,
                                 VkDeviceSize base_bytes, VkFormat fmt, uint32_t layers,
                                 bool gen_mips, struct nvk_texture *out)
{
   LOAD_DEV(c, CreateImage);
   LOAD_DEV(c, CreateImageView);
   LOAD_DEV(c, CreateSampler);
   LOAD_DEV(c, CmdCopyBufferToImage);
   LOAD_DEV(c, CmdBlitImage);

   memset(out, 0, sizeof(*out));
   uint32_t levels = 1;
   if (gen_mips) { uint32_t d = (w > h ? w : h); while (d > 1) { d >>= 1; levels++; } }
   out->w = w; out->h = h; out->levels = levels; out->layers = layers;

   bool is_cube = (layers == 6);
   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
      .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
      .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = layers,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = CreateImage(c->dev, &ici, NULL, &out->img);
   if (r != VK_SUCCESS) return r;
   r = nvk_alloc_bind_image(c, out->img, &out->mem);
   if (r != VK_SUCCESS) return r;

   VkDeviceSize face = base_bytes;
   struct nvk_buffer stg;
   r = nvk_host_buffer(c, face * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg);
   if (r != VK_SUCCESS) return r;
   memcpy(stg.cpu, data, face * layers);

   VkCommandBuffer cb = nvk_begin_cb(c, pool);

   nvk_image_barrier(c, cb, out->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, layers,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

   VkBufferImageCopy copies[6];
   for (uint32_t l = 0; l < layers; l++) {
      copies[l] = (VkBufferImageCopy){
         .bufferOffset = face * l,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1 },
         .imageExtent = { w, h, 1 },
      };
   }
   CmdCopyBufferToImage(cb, stg.buf, out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        layers, copies);

   if (gen_mips) {
      int32_t mw = w, mh = h;
      for (uint32_t i = 1; i < levels; i++) {
         nvk_image_barrier(c, cb, out->img, VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, layers,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
         int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
         VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, layers },
            .srcOffsets = { { 0, 0, 0 }, { mw, mh, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, layers },
            .dstOffsets = { { 0, 0, 0 }, { nw, nh, 1 } },
         };
         CmdBlitImage(cb, out->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                      VK_FILTER_LINEAR);
         nvk_image_barrier(c, cb, out->img, VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, layers,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
         mw = nw; mh = nh;
      }
      nvk_image_barrier(c, cb, out->img, VK_IMAGE_ASPECT_COLOR_BIT, levels - 1, 1, layers,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   } else {
      nvk_image_barrier(c, cb, out->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, layers,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   }
   r = nvk_end_submit_wait(c, cb);
   if (r != VK_SUCCESS) return r;

   VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out->img,
      .viewType = is_cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
      .format = fmt,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers },
   };
   r = CreateImageView(c->dev, &vi, NULL, &out->view);
   if (r != VK_SUCCESS) return r;

   VkSamplerCreateInfo si = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .maxLod = (float)levels,
   };
   return CreateSampler(c->dev, &si, NULL, &out->sampler);
}

static VkResult nvk_single_sampler_set(struct nvk_ctx *c, const struct nvk_texture *tex,
                                       VkDescriptorSetLayout *out_layout,
                                       VkDescriptorPool *out_pool,
                                       VkDescriptorSet *out_set)
{
   LOAD_DEV(c, CreateDescriptorSetLayout);
   LOAD_DEV(c, CreateDescriptorPool);
   LOAD_DEV(c, AllocateDescriptorSets);
   LOAD_DEV(c, UpdateDescriptorSets);

   VkDescriptorSetLayoutBinding b = {
      .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo lci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &b,
   };
   VkResult r = CreateDescriptorSetLayout(c->dev, &lci, NULL, out_layout);
   if (r != VK_SUCCESS) return r;

   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
   VkDescriptorPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
   };
   r = CreateDescriptorPool(c->dev, &pci, NULL, out_pool);
   if (r != VK_SUCCESS) return r;

   VkDescriptorSetAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = *out_pool, .descriptorSetCount = 1, .pSetLayouts = out_layout,
   };
   r = AllocateDescriptorSets(c->dev, &ai, out_set);
   if (r != VK_SUCCESS) return r;

   VkDescriptorImageInfo ii = {
      .sampler = tex->sampler, .imageView = tex->view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   VkWriteDescriptorSet w = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = *out_set, .dstBinding = 0, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii,
   };
   UpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
   return VK_SUCCESS;
}

#endif /* NVK_GFX_H */
