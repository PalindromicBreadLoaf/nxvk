/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Coarse depth culling (ZCULL)
 */
#include "nvk_gfx.h"
#include "shaders/mvp_vert.h"
#include "shaders/vcolor_frag.h"

#define MAX_DRAWS   600
#define MAX_SCEN    12
#define CELL        16

struct pc { float mvp[16]; float tint[4]; };
typedef struct { float pos[3]; float col[3]; } vtx;

struct rect_draw {
   uint32_t x0, y0, x1, y1;
   float    z;
   float    rgb[3];
};

struct scene {
   struct rect_draw d[MAX_DRAWS];
   uint32_t         n;
   uint32_t         split;
   float            clear_depth;
   VkCompareOp      op;
   bool             depth_write;
};

struct scenario {
   const char       *name;
   VkFormat          depth_fmt;
   VkImageUsageFlags extra_depth_usage;
   bool              dedicated;    /* dedicated alloc, i.e. compressed depth */
   bool              two_pass;     /* depth survives a LOAD/STORE round trip */
   bool              read_depth;   /* verify the depth buffer, not just colour */
   bool              always;       /* DONT_CARE depth + ALWAYS compare */
   VkCompareOp       op;
   uint32_t          w, h;
};

static const struct scenario scenarios[] = {
   { "d32_clear",     VK_FORMAT_D32_SFLOAT,        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
     false, false, true,  false, VK_COMPARE_OP_LESS_OR_EQUAL,    256, 256 },
   { "d32_dedicated", VK_FORMAT_D32_SFLOAT,        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
     true,  false, true,  false, VK_COMPARE_OP_LESS_OR_EQUAL,    256, 256 },
   { "d32_load",      VK_FORMAT_D32_SFLOAT,        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
     false, true,  true,  false, VK_COMPARE_OP_LESS_OR_EQUAL,    256, 256 },
   { "d32_greater",   VK_FORMAT_D32_SFLOAT,        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
     false, false, true,  false, VK_COMPARE_OP_GREATER_OR_EQUAL, 256, 256 },
   { "d32_npot",      VK_FORMAT_D32_SFLOAT,        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
     false, false, true,  false, VK_COMPARE_OP_LESS_OR_EQUAL,     67,  43 },
   { "d32_dontcare",  VK_FORMAT_D32_SFLOAT,        0,
     false, false, false, true,  VK_COMPARE_OP_ALWAYS,           256, 256 },
   { "d32_notzcull",  VK_FORMAT_D32_SFLOAT,        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
     false, false, false, false, VK_COMPARE_OP_LESS_OR_EQUAL,    256, 256 },
   { "d16_clear",     VK_FORMAT_D16_UNORM,         0,
     false, false, false, false, VK_COMPARE_OP_LESS_OR_EQUAL,    256, 256 },
   { "d24s8_clear",   VK_FORMAT_D24_UNORM_S8_UINT, 0,
     false, false, false, false, VK_COMPARE_OP_LESS_OR_EQUAL,    256, 256 },
   { "d32s8_clear",   VK_FORMAT_D32_SFLOAT_S8_UINT, 0,
     false, false, false, false, VK_COMPARE_OP_LESS_OR_EQUAL,    256, 256 },
};
#define N_SCEN (sizeof(scenarios) / sizeof(scenarios[0]))

struct result {
   bool         ran;
   uint32_t    *color;
   float       *depth;
   VkDeviceSize depth_mr_size;
   VkDeviceSize depth_mr_align;
   uint32_t     bad_color;
   uint32_t     bad_depth;
   uint32_t     first_bad_x, first_bad_y;
   uint32_t     got, want;
};

struct phase {
   struct result r[MAX_SCEN];
   const char   *err_where;
   VkResult      err;
};

/* scene construction */

static void push_rect(struct scene *s, uint32_t x0, uint32_t y0,
                      uint32_t x1, uint32_t y1, float z,
                      float r, float g, float b)
{
   if (s->n >= MAX_DRAWS) return;
   s->d[s->n++] = (struct rect_draw){ x0, y0, x1, y1, z, { r, g, b } };
}

static void push_checker(struct scene *s, uint32_t w, uint32_t h, uint32_t parity,
                         float z, float r, float g, float b)
{
   for (uint32_t y = 0; y < h; y += CELL) {
      for (uint32_t x = 0; x < w; x += CELL) {
         if (((x / CELL) + (y / CELL)) % 2 != parity) continue;
         const uint32_t x1 = (x + CELL < w) ? x + CELL : w;
         const uint32_t y1 = (y + CELL < h) ? y + CELL : h;
         push_rect(s, x, y, x1, y1, z, r, g, b);
      }
   }
}

static void build_scene(struct scene *s, const struct scenario *sc)
{
   const uint32_t w = sc->w, h = sc->h;
   memset(s, 0, sizeof(*s));
   s->op = sc->op;

   if (sc->always) {
      s->clear_depth = 0.0f;
      s->depth_write = false;
      push_rect(s, 0, 0, w, h, 0.5f, 1.0f, 0.0f, 0.0f);
      push_checker(s, w, h, 0, 0.5f, 0.0f, 1.0f, 0.0f);
      push_rect(s, 0, 0, w / 2, h, 0.5f, 0.0f, 0.0f, 1.0f);
      return;
   }

   s->depth_write = true;

   const bool greater = sc->op == VK_COMPARE_OP_GREATER_OR_EQUAL;
   const float base = greater ? 0.20f : 0.80f;
   const float near_z = greater ? 0.60f : 0.40f;
   const float far_z = greater ? 0.10f : 0.90f;
   const float mid_z = greater ? 0.40f : 0.60f;
   const float nearest = greater ? 0.90f : 0.10f;
   s->clear_depth = greater ? 0.0f : 1.0f;

   push_rect(s, 0, 0, w, h, base, 0.0f, 0.0f, 1.0f);          /* blue   */
   push_checker(s, w, h, 0, near_z, 0.0f, 1.0f, 0.0f);        /* green  */
   s->split = s->n;                                           /* pass break */
   push_checker(s, w, h, 1, far_z, 1.0f, 0.0f, 0.0f);         /* red, rejected */
   push_rect(s, 0, 0, w, h, mid_z, 1.0f, 1.0f, 1.0f);         /* white  */
   push_rect(s, 0, 0, w / 3 + 1, h / 3 + 1, nearest, 1.0f, 1.0f, 0.0f); /* yellow */

   if (!sc->two_pass) s->split = 0;
}

static bool depth_pass(VkCompareOp op, float src, float dst)
{
   switch (op) {
   case VK_COMPARE_OP_LESS_OR_EQUAL:    return src <= dst;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return src >= dst;
   case VK_COMPARE_OP_ALWAYS:           return true;
   default:                             return false;
   }
}

static uint32_t pack_rgba(const float rgb[3])
{
   const uint32_t r = (uint32_t)(rgb[0] * 255.0f + 0.5f);
   const uint32_t g = (uint32_t)(rgb[1] * 255.0f + 0.5f);
   const uint32_t b = (uint32_t)(rgb[2] * 255.0f + 0.5f);
   return r | (g << 8) | (b << 16) | (0xffu << 24);
}

static bool model_scene(const struct scene *s, uint32_t w, uint32_t h,
                        uint32_t *color_out, float *depth_out)
{
   const float black[3] = { 0.0f, 0.0f, 0.0f };
   const uint32_t npx = w * h;

   float *depth = calloc(npx, sizeof(float));
   if (!depth) return false;

   for (uint32_t i = 0; i < npx; i++) {
      color_out[i] = pack_rgba(black);
      depth[i] = s->clear_depth;
   }

   for (uint32_t i = 0; i < s->n; i++) {
      const struct rect_draw *d = &s->d[i];
      for (uint32_t y = d->y0; y < d->y1; y++) {
         for (uint32_t x = d->x0; x < d->x1; x++) {
            const size_t p = (size_t)y * w + x;
            if (!depth_pass(s->op, d->z, depth[p])) continue;
            color_out[p] = pack_rgba(d->rgb);
            if (s->depth_write) depth[p] = d->z;
         }
      }
   }

   if (depth_out) memcpy(depth_out, depth, (size_t)npx * sizeof(float));
   free(depth);
   return true;
}

static VkResult zcull_pipeline(struct nvk_ctx *c, VkShaderModule vs,
                               VkShaderModule fs, VkPipelineLayout layout,
                               VkRenderPass rp, VkCompareOp op,
                               bool depth_write, VkPipeline *out)
{
   LOAD_DEV(c, CreateGraphicsPipelines);

   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
   };
   const VkVertexInputBindingDescription vbind = {
      0, sizeof(vtx), VK_VERTEX_INPUT_RATE_VERTEX,
   };
   const VkVertexInputAttributeDescription vattr[2] = {
      { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vtx, pos) },
      { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vtx, col) },
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbind,
      .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = vattr,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .scissorCount = 1,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE,
      .depthCompareOp = op,
   };
   VkPipelineColorBlendAttachmentState cba = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba,
   };
   VkDynamicState dyn_states[2] = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
   };
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
      .layout = layout, .renderPass = rp, .subpass = 0,
   };
   return CreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gpci, NULL, out);
}

/* geometry upload */

static uint32_t emit_verts(const struct scene *s, uint32_t w, uint32_t h, vtx *out)
{
   uint32_t n = 0;
   for (uint32_t i = 0; i < s->n; i++) {
      const struct rect_draw *d = &s->d[i];
      const float x0 = (float)d->x0 / (float)w * 2.0f - 1.0f;
      const float x1 = (float)d->x1 / (float)w * 2.0f - 1.0f;
      const float y0 = (float)d->y0 / (float)h * 2.0f - 1.0f;
      const float y1 = (float)d->y1 / (float)h * 2.0f - 1.0f;
      const float corner[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
      const int tri[6] = { 0, 1, 2, 0, 2, 3 };
      for (int v = 0; v < 6; v++) {
         out[n++] = (vtx){
            { corner[tri[v]][0], corner[tri[v]][1], d->z },
            { d->rgb[0], d->rgb[1], d->rgb[2] },
         };
      }
   }
   return n;
}

/* per-scenario render target */

struct zt {
   uint32_t       w, h;
   VkImage        color;
   VkDeviceMemory color_mem;
   VkImageView    color_view;
   VkImage        depth;
   VkDeviceMemory depth_mem;
   VkImageView    depth_view;
   VkRenderPass   rp[2];
   VkFramebuffer  fb;
   struct nvk_buffer color_rb, depth_rb;
   VkDeviceSize   depth_mr_size, depth_mr_align;
};

static VkResult make_rp(struct nvk_ctx *c, const struct scenario *sc,
                        bool load, bool read_depth, VkRenderPass *out)
{
   LOAD_DEV(c, CreateRenderPass);

   const VkAttachmentLoadOp dload =
      load                 ? VK_ATTACHMENT_LOAD_OP_LOAD
      : sc->always         ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                           : VK_ATTACHMENT_LOAD_OP_CLEAR;
   const VkAttachmentStoreOp dstore =
      (read_depth || sc->two_pass) ? VK_ATTACHMENT_STORE_OP_STORE
                                   : VK_ATTACHMENT_STORE_OP_DONT_CARE;

   VkAttachmentDescription atts[2] = {
      { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
      { .format = sc->depth_fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = dload, .storeOp = dstore,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
   };
   VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &color_ref,
      .pDepthStencilAttachment = &depth_ref,
   };
   VkSubpassDependency dep = {
      .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
   };
   VkRenderPassCreateInfo rpci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2, .pAttachments = atts,
      .subpassCount = 1, .pSubpasses = &sub,
      .dependencyCount = 1, .pDependencies = &dep,
   };
   return CreateRenderPass(c->dev, &rpci, NULL, out);
}

static VkResult make_target(struct nvk_ctx *c, const struct scenario *sc,
                            struct zt *t)
{
   LOAD_DEV(c, CreateImage);
   LOAD_DEV(c, CreateImageView);
   LOAD_DEV(c, CreateFramebuffer);
   LOAD_DEV(c, GetImageMemoryRequirements);
   LOAD_DEV(c, AllocateMemory);
   LOAD_DEV(c, BindImageMemory);

   memset(t, 0, sizeof(*t));
   t->w = sc->w; t->h = sc->h;

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { sc->w, sc->h, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = CreateImage(c->dev, &ici, NULL, &t->color);
   if (r != VK_SUCCESS) return r;
   r = nvk_alloc_bind_image(c, t->color, &t->color_mem);
   if (r != VK_SUCCESS) return r;

   VkImageViewCreateInfo cvi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = t->color, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   r = CreateImageView(c->dev, &cvi, NULL, &t->color_view);
   if (r != VK_SUCCESS) return r;

   VkImageCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = sc->depth_fmt,
      .extent = { sc->w, sc->h, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               sc->extra_depth_usage,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   r = CreateImage(c->dev, &dci, NULL, &t->depth);
   if (r != VK_SUCCESS) return r;

   VkMemoryRequirements mr;
   GetImageMemoryRequirements(c->dev, t->depth, &mr);
   t->depth_mr_size = mr.size;
   t->depth_mr_align = mr.alignment;

   uint32_t mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt == UINT32_MAX) mt = nvk_pick_mem_type(&c->memp, mr.memoryTypeBits, 0);
   const VkMemoryDedicatedAllocateInfo ded = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = t->depth,
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = sc->dedicated ? &ded : NULL,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   r = AllocateMemory(c->dev, &mai, NULL, &t->depth_mem);
   if (r != VK_SUCCESS) return r;
   r = BindImageMemory(c->dev, t->depth, t->depth_mem, 0);
   if (r != VK_SUCCESS) return r;

   VkImageViewCreateInfo dvi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = t->depth, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = sc->depth_fmt,
      .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
   };
   r = CreateImageView(c->dev, &dvi, NULL, &t->depth_view);
   if (r != VK_SUCCESS) return r;

   r = make_rp(c, sc, false, sc->read_depth, &t->rp[0]);
   if (r != VK_SUCCESS) return r;
   r = make_rp(c, sc, true, sc->read_depth, &t->rp[1]);
   if (r != VK_SUCCESS) return r;

   VkImageView views[2] = { t->color_view, t->depth_view };
   VkFramebufferCreateInfo fbci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = t->rp[0], .attachmentCount = 2, .pAttachments = views,
      .width = sc->w, .height = sc->h, .layers = 1,
   };
   r = CreateFramebuffer(c->dev, &fbci, NULL, &t->fb);
   if (r != VK_SUCCESS) return r;

   r = nvk_host_buffer(c, (VkDeviceSize)sc->w * sc->h * 4,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, &t->color_rb);
   if (r != VK_SUCCESS) return r;
   if (sc->read_depth) {
      r = nvk_host_buffer(c, (VkDeviceSize)sc->w * sc->h * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, &t->depth_rb);
      if (r != VK_SUCCESS) return r;
   }
   return VK_SUCCESS;
}

static void free_target(struct nvk_ctx *c, struct zt *t)
{
   LOAD_DEV(c, DestroyFramebuffer);
   LOAD_DEV(c, DestroyRenderPass);
   LOAD_DEV(c, DestroyImageView);
   LOAD_DEV(c, DestroyImage);
   LOAD_DEV(c, FreeMemory);
   if (t->fb) DestroyFramebuffer(c->dev, t->fb, NULL);
   for (int i = 0; i < 2; i++)
      if (t->rp[i]) DestroyRenderPass(c->dev, t->rp[i], NULL);
   if (t->color_view) DestroyImageView(c->dev, t->color_view, NULL);
   if (t->depth_view) DestroyImageView(c->dev, t->depth_view, NULL);
   if (t->color) DestroyImage(c->dev, t->color, NULL);
   if (t->depth) DestroyImage(c->dev, t->depth, NULL);
   if (t->color_mem) FreeMemory(c->dev, t->color_mem, NULL);
   if (t->depth_mem) FreeMemory(c->dev, t->depth_mem, NULL);
   nvk_free_buffer(c, &t->color_rb);
   nvk_free_buffer(c, &t->depth_rb);
   memset(t, 0, sizeof(*t));
}

/* one scenario */

static void record_pass(struct nvk_ctx *c, VkCommandBuffer cb, struct zt *t,
                        const struct scene *s, VkPipeline pipe,
                        VkPipelineLayout layout, struct nvk_buffer *vb,
                        uint32_t first, uint32_t count, bool load)
{
   LOAD_DEV(c, CmdBeginRenderPass);
   LOAD_DEV(c, CmdEndRenderPass);
   LOAD_DEV(c, CmdBindPipeline);
   LOAD_DEV(c, CmdBindVertexBuffers);
   LOAD_DEV(c, CmdPushConstants);
   LOAD_DEV(c, CmdDraw);

   VkClearValue clears[2];
   memset(clears, 0, sizeof(clears));
   clears[0].color.float32[3] = 1.0f;
   clears[1].depthStencil.depth = s->clear_depth;

   VkRenderPassBeginInfo rbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = t->rp[load ? 1 : 0], .framebuffer = t->fb,
      .renderArea = { { 0, 0 }, { t->w, t->h } },
      .clearValueCount = 2, .pClearValues = clears,
   };
   CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   nvk_set_full_viewport(c, cb, t->w, t->h);

   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &vb->buf, &off);

   struct pc pcv;
   memset(&pcv, 0, sizeof(pcv));
   pcv.mvp[0] = pcv.mvp[5] = pcv.mvp[10] = pcv.mvp[15] = 1.0f;  /* identity */
   pcv.tint[0] = pcv.tint[1] = pcv.tint[2] = pcv.tint[3] = 1.0f;
   CmdPushConstants(cb, layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(pcv), &pcv);

   for (uint32_t i = 0; i < count; i++)
      CmdDraw(cb, 6, 1, (first + i) * 6, 0);

   CmdEndRenderPass(cb);
}

static VkResult run_scenario(struct nvk_ctx *c, VkCommandPool pool,
                             VkPipelineLayout layout, VkShaderModule vs,
                             VkShaderModule fs, const struct scenario *sc,
                             struct result *out, const char **where)
{
   LOAD_DEV(c, DestroyPipeline);
   LOAD_DEV(c, CmdPipelineBarrier);
   LOAD_DEV(c, CmdCopyImageToBuffer);

   struct zt t;
   struct scene s;
   VkPipeline pipe = VK_NULL_HANDLE;
   struct nvk_buffer vb = {0};
   vtx *verts = NULL;
   VkResult r;

   PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties =
      (PFN_vkGetPhysicalDeviceFormatProperties)vk_icdGetInstanceProcAddr(
         c->instance, "vkGetPhysicalDeviceFormatProperties");
   if (!GetPhysicalDeviceFormatProperties) {
      *where = "vkGetPhysicalDeviceFormatProperties";
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   VkFormatProperties fp;
   GetPhysicalDeviceFormatProperties(c->phys, sc->depth_fmt, &fp);
   if (!(fp.optimalTilingFeatures &
         VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      *where = "format support";
      return VK_SUCCESS;
   }

   build_scene(&s, sc);

   *where = "make_target";
   r = make_target(c, sc, &t);
   if (r != VK_SUCCESS) goto out;

   out->depth_mr_size = t.depth_mr_size;
   out->depth_mr_align = t.depth_mr_align;

   *where = "vertices";
   verts = calloc(s.n * 6, sizeof(vtx));
   if (!verts) { r = VK_ERROR_OUT_OF_HOST_MEMORY; goto out; }
   const uint32_t n_verts = emit_verts(&s, sc->w, sc->h, verts);
   r = nvk_device_buffer(c, pool, verts, (VkDeviceSize)n_verts * sizeof(vtx),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb);
   if (r != VK_SUCCESS) goto out;

   *where = "pipeline";
   r = zcull_pipeline(c, vs, fs, layout, t.rp[0], sc->op, s.depth_write, &pipe);
   if (r != VK_SUCCESS) goto out;

   *where = "record";
   VkCommandBuffer cb = nvk_begin_cb(c, pool);
   if (cb == VK_NULL_HANDLE) { r = VK_ERROR_INITIALIZATION_FAILED; goto out; }

   if (s.split) {
      record_pass(c, cb, &t, &s, pipe, layout, &vb, 0, s.split, false);
      record_pass(c, cb, &t, &s, pipe, layout, &vb, s.split, s.n - s.split, true);
   } else {
      record_pass(c, cb, &t, &s, pipe, layout, &vb, 0, s.n, false);
   }

   /* colour to host */
   VkImageMemoryBarrier cb_bar = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = t.color,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                      1, &cb_bar);
   VkBufferImageCopy creg = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { t.w, t.h, 1 },
   };
   CmdCopyImageToBuffer(cb, t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        t.color_rb.buf, 1, &creg);

   if (sc->read_depth) {
      VkImageMemoryBarrier db_bar = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = t.depth,
         .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
      };
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &db_bar);
      VkBufferImageCopy dreg = {
         .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
         .imageExtent = { t.w, t.h, 1 },
      };
      CmdCopyImageToBuffer(cb, t.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t.depth_rb.buf, 1, &dreg);
   }

   *where = "submit";
   r = nvk_end_submit_wait(c, cb);
   if (r != VK_SUCCESS) goto out;

   *where = "verify";
   const uint32_t npx = sc->w * sc->h;
   uint32_t *want_c = calloc(npx, sizeof(uint32_t));
   float *want_d = sc->read_depth ? calloc(npx, sizeof(float)) : NULL;
   out->color = calloc(npx, sizeof(uint32_t));
   if (!want_c || !out->color || (sc->read_depth && !want_d)) {
      r = VK_ERROR_OUT_OF_HOST_MEMORY;
      free(want_c); free(want_d);
      goto out;
   }
   memcpy(out->color, t.color_rb.cpu, (size_t)npx * 4);
   if (sc->read_depth) {
      out->depth = calloc(npx, sizeof(float));
      if (!out->depth) {
         r = VK_ERROR_OUT_OF_HOST_MEMORY;
         free(want_c); free(want_d);
         goto out;
      }
      memcpy(out->depth, t.depth_rb.cpu, (size_t)npx * 4);
   }

   if (!model_scene(&s, sc->w, sc->h, want_c, want_d)) {
      r = VK_ERROR_OUT_OF_HOST_MEMORY;
      free(want_c); free(want_d);
      goto out;
   }

   for (uint32_t i = 0; i < npx; i++) {
      if (out->color[i] != want_c[i]) {
         if (!out->bad_color) {
            out->first_bad_x = i % sc->w;
            out->first_bad_y = i / sc->w;
            out->got = out->color[i];
            out->want = want_c[i];
         }
         out->bad_color++;
      }
      if (want_d) {
         const float d = out->depth[i];
         const float e = want_d[i];
         const float diff = d > e ? d - e : e - d;
         if (diff > 1.0f / 4096.0f) out->bad_depth++;
      }
   }
   free(want_c);
   free(want_d);
   out->ran = true;
   r = VK_SUCCESS;

out:
   if (pipe) DestroyPipeline(c->dev, pipe, NULL);
   nvk_free_buffer(c, &vb);
   free(verts);
   free_target(c, &t);
   return r;
}

static bool run_phase(const char *name, struct phase *p)
{
   struct nvk_ctx c;
   bool ok = false;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;

   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) {
      p->err_where = "bringup";
      p->err = VK_ERROR_INITIALIZATION_FAILED;
      return false;
   }

   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, DestroyPipelineLayout);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, DestroyCommandPool);
   LOAD_DEV(&c, DestroyShaderModule);
   LOAD_DEV(&c, DeviceWaitIdle);

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = c.qfi,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
   };
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) {
      p->err_where = "command pool";
      goto done;
   }

   VkPushConstantRange pcr = {
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(struct pc),
   };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
   };
   if (CreatePipelineLayout(c.dev, &plci, NULL, &layout) != VK_SUCCESS) {
      p->err_where = "pipeline layout";
      goto done;
   }

   vs = nvk_load_shader(&c, mvp_vert_spv, sizeof(mvp_vert_spv));
   fs = nvk_load_shader(&c, vcolor_frag_spv, sizeof(vcolor_frag_spv));
   if (!vs || !fs) { p->err_where = "shaders"; goto done; }

   for (uint32_t i = 0; i < N_SCEN; i++) {
      const char *where = "?";
      const VkResult r = run_scenario(&c, pool, layout, vs, fs, &scenarios[i],
                                      &p->r[i], &where);
      if (r != VK_SUCCESS) {
         LOG("%s: FAIL %s at %s -> %d", name, scenarios[i].name, where, r);
         p->err_where = scenarios[i].name;
         p->err = r;
         goto done;
      }
      if (!p->r[i].ran) {
         LOG("%s: %-14s SKIP, format unsupported", name, scenarios[i].name);
         continue;
      }
      LOG("%s: %-14s depth mr size=%llu align=%llu, %u bad px, %u bad depth",
          name, scenarios[i].name,
          (unsigned long long)p->r[i].depth_mr_size,
          (unsigned long long)p->r[i].depth_mr_align,
          p->r[i].bad_color, p->r[i].bad_depth);
   }
   ok = true;

done:
   if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
   if (vs) DestroyShaderModule(c.dev, vs, NULL);
   if (fs) DestroyShaderModule(c.dev, fs, NULL);
   if (layout) DestroyPipelineLayout(c.dev, layout, NULL);
   if (pool) DestroyCommandPool(c.dev, pool, NULL);
   nvk_teardown(&c);
   return ok;
}

static void free_phase(struct phase *p)
{
   for (uint32_t i = 0; i < N_SCEN; i++) {
      free(p->r[i].color);
      free(p->r[i].depth);
      p->r[i].color = NULL;
      p->r[i].depth = NULL;
   }
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_zcull.log");
   LOG("=== nvk_zcull ===");
   LOG("params: %u scenarios, %u px checkerboard cells", (unsigned)N_SCEN, CELL);

   static struct phase off, on;
   bool failed = true;

   setenv("NVK_DEBUG", "no_zcull", 1);
   LOG("--- phase off ---");
   if (!run_phase("off", &off)) {
      LOG("ZCULL FAIL: the control phase did not run (%s -> %d)",
          off.err_where ? off.err_where : "?", off.err);
      goto out;
   }

   unsetenv("NVK_DEBUG");
   LOG("--- phase on ---");
   if (!run_phase("on", &on)) {
      LOG("ZCULL FAIL: the zcull phase did not run (%s -> %d)",
          on.err_where ? on.err_where : "?", on.err);
      goto out;
   }

   for (uint32_t i = 0; i < N_SCEN; i++) {
      if (!off.r[i].ran) continue;
      if (off.r[i].bad_color || off.r[i].bad_depth) {
         LOG("ZCULL FAIL: the no_zcull control is already wrong on %s: "
             "%u bad px (first at %u,%u got 0x%08x want 0x%08x), %u bad depth",
             scenarios[i].name, off.r[i].bad_color, off.r[i].first_bad_x,
             off.r[i].first_bad_y, off.r[i].got, off.r[i].want,
             off.r[i].bad_depth);
         goto out;
      }
   }

   uint32_t compared = 0;
   for (uint32_t i = 0; i < N_SCEN; i++) {
      if (!on.r[i].ran || !off.r[i].ran) {
         if (on.r[i].ran != off.r[i].ran) {
            LOG("ZCULL FAIL: %s ran in one phase but not the other",
                scenarios[i].name);
            goto out;
         }
         continue;
      }
      compared++;
      if (on.r[i].bad_color || on.r[i].bad_depth) {
         LOG("ZCULL FAIL: %s renders wrong with zcull on: "
             "%u bad px (first at %u,%u got 0x%08x want 0x%08x), %u bad depth",
             scenarios[i].name, on.r[i].bad_color, on.r[i].first_bad_x,
             on.r[i].first_bad_y, on.r[i].got, on.r[i].want, on.r[i].bad_depth);
         goto out;
      }
      const uint32_t npx = scenarios[i].w * scenarios[i].h;
      if (memcmp(on.r[i].color, off.r[i].color, (size_t)npx * 4) != 0) {
         LOG("ZCULL FAIL: %s colour differs between phases", scenarios[i].name);
         goto out;
      }
      if (on.r[i].depth && off.r[i].depth &&
          memcmp(on.r[i].depth, off.r[i].depth, (size_t)npx * 4) != 0) {
         LOG("ZCULL FAIL: %s depth differs between phases", scenarios[i].name);
         goto out;
      }
   }

   uint32_t grew = 0;
   for (uint32_t i = 0; i < N_SCEN; i++) {
      if (!on.r[i].ran) continue;
      const bool eligible = !(scenarios[i].extra_depth_usage &
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT);
      const bool bigger = on.r[i].depth_mr_size > off.r[i].depth_mr_size;
      if (eligible && bigger) grew++;
      if (!eligible && bigger) {
         LOG("ZCULL FAIL: %s is not zcull-eligible but still grew, "
             "%llu -> %llu", scenarios[i].name,
             (unsigned long long)off.r[i].depth_mr_size,
             (unsigned long long)on.r[i].depth_mr_size);
         goto out;
      }
   }
   if (grew == 0) {
      LOG("ZCULL FAIL: no depth image grew a ZCULL plane.");
      goto out;
   }

   LOG("ZCULL OK: %u of %u scenarios ran and are identical with and without "
       "zcull, %u of them carry a ZCULL plane",
       compared, (unsigned)N_SCEN, grew);
   LOG("=== nvk_zcull PASSED ===");
   failed = false;

out:
   if (failed) LOG("=== nvk_zcull FAILED ===");
   free_phase(&off);
   free_phase(&on);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
