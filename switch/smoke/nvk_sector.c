/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Texture-header sector promotion
 */
#include "nvk_gfx.h"
#include "shaders/tex_vert.h"
#include "shaders/tex_frag.h"

#define MAX_LEVELS 8
#define CHURN      8
#define N_GEOMS    (sizeof(geoms) / sizeof(geoms[0]))

struct geom {
   const char *name;
   uint32_t    w, h, levels;
};

static const struct geom geoms[] = {
   { "square_256",  256, 256, 1 },
   { "npot_17x13",   17,  13, 1 },
   { "strip_4x256",   4, 256, 1 },
   { "strip_256x4", 256,   4, 1 },
   { "tiny_3x3",      3,   3, 1 },
   { "mips_128",    128, 128, MAX_LEVELS },
};

struct tex {
   VkImage        img;
   VkDeviceMemory mem;
   VkSampler      samp;
   uint32_t       w, h, levels;
   VkDeviceSize   size, align;
};

struct phase {
   uint32_t levels_read;
   uint32_t levels_bad;
   uint32_t bad_texels;
   uint32_t first_bad_x, first_bad_y, first_bad_level;
   uint32_t got_bad, want_bad;
   VkResult err;
   const char *err_where;
};

typedef struct { float x, y, u, v; } vtx;

static uint32_t level_dim(uint32_t d, uint32_t level)
{
   d >>= level;
   return d ? d : 1;
}

static uint32_t texel(uint32_t level, uint32_t x, uint32_t y)
{
   return ((x * 2654435761u) ^ (y * 2246822519u) ^ (level * 3266489917u)) |
          0x01010101u;
}

static VkResult make_tex(struct nvk_ctx *c, VkCommandPool pool,
                         const struct geom *g, struct tex *t)
{
   LOAD_DEV(c, CreateImage);
   LOAD_DEV(c, CreateSampler);
   LOAD_DEV(c, GetImageMemoryRequirements);
   LOAD_DEV(c, CmdCopyBufferToImage);

   memset(t, 0, sizeof(*t));
   t->w = g->w; t->h = g->h; t->levels = g->levels;

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { g->w, g->h, 1 }, .mipLevels = g->levels, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = CreateImage(c->dev, &ici, NULL, &t->img);
   if (r != VK_SUCCESS) return r;

   VkMemoryRequirements mr;
   GetImageMemoryRequirements(c->dev, t->img, &mr);
   t->size = mr.size;
   t->align = mr.alignment;

   r = nvk_alloc_bind_image(c, t->img, &t->mem);
   if (r != VK_SUCCESS) return r;

   VkDeviceSize bytes = 0;
   for (uint32_t l = 0; l < g->levels; l++)
      bytes += (VkDeviceSize)level_dim(g->w, l) * level_dim(g->h, l) * 4;

   struct nvk_buffer stg;
   r = nvk_host_buffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg);
   if (r != VK_SUCCESS) return r;

   VkBufferImageCopy copies[MAX_LEVELS];
   uint32_t *src = (uint32_t *)stg.cpu;
   VkDeviceSize off = 0;
   for (uint32_t l = 0; l < g->levels; l++) {
      const uint32_t lw = level_dim(g->w, l), lh = level_dim(g->h, l);
      for (uint32_t y = 0; y < lh; y++)
         for (uint32_t x = 0; x < lw; x++)
            src[off / 4 + (size_t)y * lw + x] = texel(l, x, y);
      copies[l] = (VkBufferImageCopy){
         .bufferOffset = off,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 },
         .imageExtent = { lw, lh, 1 },
      };
      off += (VkDeviceSize)lw * lh * 4;
   }

   VkCommandBuffer cb = nvk_begin_cb(c, pool);
   nvk_image_barrier(c, cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, g->levels, 1,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
   CmdCopyBufferToImage(cb, stg.buf, t->img,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, g->levels, copies);
   nvk_image_barrier(c, cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, g->levels, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   r = nvk_end_submit_wait(c, cb);
   nvk_free_buffer(c, &stg);
   if (r != VK_SUCCESS) return r;

   VkSamplerCreateInfo si = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
   };
   return CreateSampler(c->dev, &si, NULL, &t->samp);
}

static void free_tex(struct nvk_ctx *c, struct tex *t)
{
   LOAD_DEV(c, DestroySampler);
   LOAD_DEV(c, DestroyImage);
   LOAD_DEV(c, FreeMemory);
   if (t->samp) DestroySampler(c->dev, t->samp, NULL);
   if (t->img) DestroyImage(c->dev, t->img, NULL);
   if (t->mem) FreeMemory(c->dev, t->mem, NULL);
   memset(t, 0, sizeof(*t));
}

static VkResult read_level(struct nvk_ctx *c, VkCommandPool pool,
                           VkPipeline pipe, VkPipelineLayout layout,
                           VkDescriptorSetLayout set_layout,
                           struct nvk_buffer *quad, const struct tex *t,
                           uint32_t level, const char *name, struct phase *p)
{
   LOAD_DEV(c, CreateImageView);
   LOAD_DEV(c, DestroyImageView);
   LOAD_DEV(c, CreateDescriptorPool);
   LOAD_DEV(c, DestroyDescriptorPool);
   LOAD_DEV(c, AllocateDescriptorSets);
   LOAD_DEV(c, UpdateDescriptorSets);
   LOAD_DEV(c, CmdBeginRenderPass);
   LOAD_DEV(c, CmdEndRenderPass);
   LOAD_DEV(c, CmdBindPipeline);
   LOAD_DEV(c, CmdBindVertexBuffers);
   LOAD_DEV(c, CmdBindDescriptorSets);
   LOAD_DEV(c, CmdDraw);

   const uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);

   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = t->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 },
   };
   struct nvk_target target = {0};
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;

   VkResult r = CreateImageView(c->dev, &vci, NULL, &view);
   if (r != VK_SUCCESS) { p->err_where = "image view"; goto out; }

   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
   };
   r = CreateDescriptorPool(c->dev, &dpci, NULL, &dpool);
   if (r != VK_SUCCESS) { p->err_where = "descriptor pool"; goto out; }

   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool, .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet set;
   r = AllocateDescriptorSets(c->dev, &dsai, &set);
   if (r != VK_SUCCESS) { p->err_where = "descriptor set"; goto out; }

   VkDescriptorImageInfo ii = {
      .sampler = t->samp, .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   VkWriteDescriptorSet w = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &ii,
   };
   UpdateDescriptorSets(c->dev, 1, &w, 0, NULL);

   r = nvk_color_target(c, lw, lh, VK_FORMAT_R8G8B8A8_UNORM, false, &target);
   if (r != VK_SUCCESS) { p->err_where = "colour target"; goto out; }
   memset(target.readback.cpu, 0, (size_t)lw * lh * 4);

   VkCommandBuffer cb = nvk_begin_cb(c, pool);
   VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = target.rp, .framebuffer = target.fb,
      .renderArea = { { 0, 0 }, { lw, lh } },
      .clearValueCount = 1, .pClearValues = &clear,
   };
   CmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   nvk_set_full_viewport(c, cb, lw, lh);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                         &set, 0, NULL);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &quad->buf, &off);
   CmdDraw(cb, 4, 1, 0, 0);
   CmdEndRenderPass(cb);
   nvk_target_copy_to_host(c, cb, &target);
   r = nvk_end_submit_wait(c, cb);
   if (r != VK_SUCCESS) { p->err_where = "sample submit"; goto out; }

   uint32_t bad = 0, bx = 0, by = 0, got = 0, want = 0;
   for (uint32_t y = 0; y < lh; y++) {
      for (uint32_t x = 0; x < lw; x++) {
         const uint32_t g = nvk_target_pixel(&target, x, y);
         const uint32_t e = texel(level, x, y);
         if (g != e) {
            if (!bad) { bx = x; by = y; got = g; want = e; }
            bad++;
         }
      }
   }

   p->levels_read++;
   if (bad) {
      if (!p->bad_texels) {
         p->first_bad_x = bx; p->first_bad_y = by; p->first_bad_level = level;
         p->got_bad = got; p->want_bad = want;
      }
      p->levels_bad++;
      p->bad_texels += bad;
      LOG("  %s level %u (%ux%u): %u/%u texels WRONG, first at (%u,%u) "
          "got 0x%08x want 0x%08x",
          name, level, lw, lh, bad, lw * lh, bx, by, got, want);
   } else {
      LOG("  %s level %u (%ux%u): %u texels ok", name, level, lw, lh, lw * lh);
   }

out:
   nvk_free_target(c, &target);
   if (view) DestroyImageView(c->dev, view, NULL);
   if (dpool) DestroyDescriptorPool(c->dev, dpool, NULL);
   return r;
}

static bool run_phase(const char *name, struct phase *p)
{
   struct nvk_ctx c;
   bool ok = false;

   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) {
      LOG("%s: FAIL bringup", name);
      return false;
   }

   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, DestroyCommandPool);
   LOAD_DEV(&c, CreateDescriptorSetLayout);
   LOAD_DEV(&c, DestroyDescriptorSetLayout);
   LOAD_DEV(&c, CreatePipelineLayout);
   LOAD_DEV(&c, DestroyPipelineLayout);
   LOAD_DEV(&c, DestroyPipeline);
   LOAD_DEV(&c, DestroyShaderModule);
   LOAD_DEV(&c, DeviceWaitIdle);

   VkCommandPool pool = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   struct nvk_target proto = {0};
   struct nvk_buffer quad = {0};
   struct tex t = {0};
   struct tex churn[CHURN] = {{0}};
   uint32_t made = 0;

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = c.qfi,
   };
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) {
      LOG("%s: FAIL command pool", name);
      goto done;
   }

   VkDescriptorSetLayoutBinding b = {
      .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dlci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &b,
   };
   if (CreateDescriptorSetLayout(c.dev, &dlci, NULL, &set_layout) != VK_SUCCESS) {
      LOG("%s: FAIL set layout", name);
      goto done;
   }

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &set_layout,
   };
   if (CreatePipelineLayout(c.dev, &plci, NULL, &layout) != VK_SUCCESS) {
      LOG("%s: FAIL pipeline layout", name);
      goto done;
   }

   VkResult r = nvk_color_target(&c, 16, 16, VK_FORMAT_R8G8B8A8_UNORM, false,
                                 &proto);
   if (r != VK_SUCCESS) { LOG("%s: FAIL prototype target -> %d", name, r); goto done; }

   vs = nvk_load_shader(&c, tex_vert_spv, sizeof(tex_vert_spv));
   fs = nvk_load_shader(&c, tex_frag_spv, sizeof(tex_frag_spv));
   if (!vs || !fs) { LOG("%s: FAIL shader modules", name); goto done; }

   VkVertexInputBindingDescription vbind = { 0, sizeof(vtx),
                                             VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription vattr[2] = {
      { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
      { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vtx, u) },
   };
   struct nvk_pipe_desc pd = {
      .vs = vs, .fs = fs, .vbind = &vbind, .n_vbind = 1,
      .vattr = vattr, .n_vattr = 2, .layout = layout, .rp = proto.rp,
      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
   };
   r = nvk_graphics_pipeline(&c, &pd, &pipe);
   if (r != VK_SUCCESS) { LOG("%s: FAIL pipeline -> %d", name, r); goto done; }

   static const vtx quad_v[4] = {
      { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { -1, 1, 0, 1 }, { 1, 1, 1, 1 },
   };
   r = nvk_host_buffer(&c, sizeof(quad_v), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       &quad);
   if (r != VK_SUCCESS) { LOG("%s: FAIL quad buffer -> %d", name, r); goto done; }
   memcpy(quad.cpu, quad_v, sizeof(quad_v));

   for (uint32_t i = 0; i < N_GEOMS; i++) {
      r = make_tex(&c, pool, &geoms[i], &t);
      if (r != VK_SUCCESS) {
         LOG("%s: FAIL %s upload -> %d", name, geoms[i].name, r);
         p->err = r; p->err_where = "texture upload";
         goto done;
      }
      LOG("%s: %s %ux%u levels=%u size=0x%llx align=0x%llx", name,
          geoms[i].name, t.w, t.h, t.levels,
          (unsigned long long)t.size, (unsigned long long)t.align);

      for (uint32_t l = 0; l < t.levels; l++) {
         r = read_level(&c, pool, pipe, layout, set_layout, &quad, &t, l,
                        geoms[i].name, p);
         if (r != VK_SUCCESS) {
            LOG("%s: FAIL %s level %u %s -> %d", name, geoms[i].name, l,
                p->err_where, r);
            p->err = r;
            goto done;
         }
      }
      free_tex(&c, &t);
   }

   struct geom small = { "churn_64", 64, 64, 1 };
   for (; made < CHURN; made++) {
      r = make_tex(&c, pool, &small, &churn[made]);
      if (r != VK_SUCCESS) {
         LOG("%s: FAIL churn upload -> %d", name, r);
         p->err = r; p->err_where = "churn upload";
         goto done;
      }
   }
   for (uint32_t i = 1; i < made; i += 2)
      free_tex(&c, &churn[i]);
   LOG("%s: churn_64 x%u, freed %u neighbours", name, made, made / 2);
   for (uint32_t i = 0; i < made; i += 2) {
      r = read_level(&c, pool, pipe, layout, set_layout, &quad, &churn[i], 0,
                     "churn_64", p);
      if (r != VK_SUCCESS) {
         LOG("%s: FAIL churn %s -> %d", name, p->err_where, r);
         p->err = r;
         goto done;
      }
   }

   ok = true;

done:
   if (DeviceWaitIdle) DeviceWaitIdle(c.dev);

   for (uint32_t i = 0; i < made; i++)
      free_tex(&c, &churn[i]);
   free_tex(&c, &t);
   nvk_free_buffer(&c, &quad);
   nvk_free_target(&c, &proto);
   if (pipe) DestroyPipeline(c.dev, pipe, NULL);
   if (vs) DestroyShaderModule(c.dev, vs, NULL);
   if (fs) DestroyShaderModule(c.dev, fs, NULL);
   if (layout) DestroyPipelineLayout(c.dev, layout, NULL);
   if (set_layout) DestroyDescriptorSetLayout(c.dev, set_layout, NULL);
   if (pool) DestroyCommandPool(c.dev, pool, NULL);

   nvk_teardown(&c);
   return ok;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_sector.log");
   LOG("=== nvk_sector ===");
   LOG("params: RGBA8 point-sampled 1:1");

   struct phase off = {0}, on = {0};

   setenv("NVK_DEBUG", "no_sector_promotion", 1);
   LOG("--- phase off (NVK_DEBUG=no_sector_promotion) ---");
   if (!run_phase("off", &off)) goto fail;

   unsetenv("NVK_DEBUG");
   LOG("--- phase on ---");
   if (!run_phase("on", &on)) goto fail;

   LOG("off: %u levels read, %u bad, %u bad texels",
       off.levels_read, off.levels_bad, off.bad_texels);
   LOG("on : %u levels read, %u bad, %u bad texels",
       on.levels_read, on.levels_bad, on.bad_texels);

   if (off.bad_texels) {
      LOG("SECTOR FAIL: the promotion-off control is already wrong, first at "
          "level %u (%u,%u) got 0x%08x want 0x%08x",
          off.first_bad_level, off.first_bad_x, off.first_bad_y,
          off.got_bad, off.want_bad);
      LOG("=== nvk_sector FAILED ===");
      goto out;
   }

   if (on.bad_texels) {
      LOG("SECTOR FAIL: promotion changed what the sampler returns, first at "
          "level %u (%u,%u) got 0x%08x want 0x%08x",
          on.first_bad_level, on.first_bad_x, on.first_bad_y,
          on.got_bad, on.want_bad);
      LOG("=== nvk_sector FAILED ===");
      goto out;
   }

   if (on.levels_read != off.levels_read) {
      LOG("SECTOR FAIL: the two phases read a different number of levels, "
          "%u vs %u", on.levels_read, off.levels_read);
      LOG("=== nvk_sector FAILED ===");
      goto out;
   }

   LOG("SECTOR OK: %u levels sample identically with and without promotion",
       on.levels_read);
   LOG("=== nvk_sector PASSED ===");

out:
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;

fail:
   LOG("=== nvk_sector FAILED ===");
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
