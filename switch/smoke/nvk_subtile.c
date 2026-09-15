/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fragment SPM subtiling sweep
 */
#include <time.h>

#include "nvk_gfx.h"
#include "shaders/fullscreen_vert.h"
#include "shaders/alu_frag.h"
#include "shaders/alu_comp.h"

#define RT_W            1280
#define RT_H            720
#define FRAG_DRAWS      1

#define COMP_DIM        256     /* alu.comp indexes a 256x256 grid */
#define COMP_LOCAL      8
#define COMP_GROUPS     (COMP_DIM / COMP_LOCAL)
#define COMP_DISPATCHES 23
#define COMP_BYTES      ((VkDeviceSize)COMP_DIM * COMP_DIM * 16)

#define ALU_LOOP        64      /* iterations in both shaders */
#define REPS            7       /* timed submits per row */

#define N_PHASES (sizeof(phases) / sizeof(phases[0]))

struct phase_spec {
   const char *name;
   const char *knob;
   const char *note;
};

static const struct phase_spec phases[] = {
   { "default",    NULL,         "the driver's own choice" },
   { "generic",    "0x20164010", "every discrete part, and nouveau's nvc0" },
   { "t210",       "0x087f6080", "deko3d fragment default" },
   { "t210_mrt",   "0x20806080", "deko3d, three or more colour outputs" },
   { "quads_06",   "0x06164010", "generic, max quads 0x20 -> 0x06" },
   { "regfile_80", "0x20164080", "generic, register file 0x10 -> 0x80" },
   { "pxout_60",   "0x20166010", "generic, pixel output buffer 0x40 -> 0x60" },
   { "triram_7f",  "0x207f4010", "generic, triangle RAM 0x16 -> 0x7f" },
   { "quads_08",   "0x08164010", "generic, max quads 0x20 -> 0x08" },
};

struct result {
   bool     ok;
   double   frag_ms, comp_ms;      /* GPU, median of REPS */
   double   frag_wall_ms, comp_wall_ms;
   uint64_t frag_sum, comp_sum;    /* readback checksums */
};

/* timing */

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return (x > y) - (x < y);
}

static double median(double *v, unsigned n)
{
   qsort(v, n, sizeof(*v), cmp_double);
   return v[n / 2];
}

static uint64_t fnv1a(const void *data, size_t bytes)
{
   const uint8_t *p = data;
   uint64_t h = 0xcbf29ce484222325ull;
   for (size_t i = 0; i < bytes; i++) {
      h ^= p[i];
      h *= 0x100000001b3ull;
   }
   return h;
}

/* GPU timestamps */

struct timer {
   VkQueryPool pool;
   double      period_ns;
   uint64_t    mask;
   double      wall_ms;
   bool        gave_up;
};

static bool g_wall_fallback;

static VkResult timer_init(struct nvk_ctx *c, struct timer *t)
{
   LOAD_INST(c, GetPhysicalDeviceQueueFamilyProperties);
   LOAD_DEV(c, CreateQueryPool);

   memset(t, 0, sizeof(*t));
   t->period_ns = c->props.limits.timestampPeriod;

   uint32_t nqf = 0;
   GetPhysicalDeviceQueueFamilyProperties(c->phys, &nqf, NULL);
   VkQueueFamilyProperties qf[8];
   if (nqf > 8) nqf = 8;
   GetPhysicalDeviceQueueFamilyProperties(c->phys, &nqf, qf);

   uint32_t bits = c->qfi < nqf ? qf[c->qfi].timestampValidBits : 0;
   if (bits == 0) {
      LOG("timer: queue family %u has no timestamp bits, using wall clock", c->qfi);
      return VK_SUCCESS;
   }
   t->mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);

   VkQueryPoolCreateInfo qpci = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
   };
   return CreateQueryPool(c->dev, &qpci, NULL, &t->pool);
}

static void timer_begin(struct nvk_ctx *c, struct timer *t, VkCommandBuffer cb)
{
   LOAD_DEV(c, CmdResetQueryPool);
   LOAD_DEV(c, CmdWriteTimestamp);
   if (!t->pool) return;
   CmdResetQueryPool(cb, t->pool, 0, 2);
   CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, t->pool, 0);
}

static void timer_end(struct nvk_ctx *c, struct timer *t, VkCommandBuffer cb)
{
   LOAD_DEV(c, CmdWriteTimestamp);
   if (!t->pool) return;
   CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, t->pool, 1);
}

static double timer_submit(struct nvk_ctx *c, struct timer *t, VkCommandBuffer cb)
{
   LOAD_DEV(c, GetQueryPoolResults);

   double t0 = now_ms();
   VkResult r = nvk_end_submit_wait(c, cb);
   t->wall_ms = now_ms() - t0;
   if (r != VK_SUCCESS) {
      LOG("timer: submit -> %d", r);
      return -1.0;
   }
   if (!t->pool || t->gave_up)
      return t->wall_ms;

   uint64_t ts[2] = { 0, 0 };
   double deadline = now_ms() + 200.0;
   do {
      r = GetQueryPoolResults(c->dev, t->pool, 0, 2, sizeof(ts), ts,
                              sizeof(ts[0]), VK_QUERY_RESULT_64_BIT);
      if (r == VK_SUCCESS)
         break;
      if (r != VK_NOT_READY) {
         LOG("timer: GetQueryPoolResults -> %d, falling back to wall clock", r);
         t->gave_up = g_wall_fallback = true;
         return t->wall_ms;
      }
   } while (now_ms() < deadline);

   if (r != VK_SUCCESS) {
      LOG("timer: timestamps never landed after an idle queue, falling back "
          "to wall clock");
      t->gave_up = g_wall_fallback = true;
      return t->wall_ms;
   }

   uint64_t d = (ts[1] & t->mask) - (ts[0] & t->mask);
   if (d == 0) {
      LOG("timer: timestamp delta is zero, falling back to wall clock");
      t->gave_up = g_wall_fallback = true;
      return t->wall_ms;
   }
   return (double)d * t->period_ns / 1e6;
}

/* the fragment row */

struct frag_rig {
   struct nvk_target t;
   VkShaderModule    vs, fs;
   VkPipelineLayout  layout;
   VkPipeline        pipe;
};

static VkResult frag_rig_init(struct nvk_ctx *c, struct frag_rig *f)
{
   LOAD_DEV(c, CreatePipelineLayout);

   memset(f, 0, sizeof(*f));
   VkResult r = nvk_color_target(c, RT_W, RT_H, VK_FORMAT_R8G8B8A8_UNORM, false,
                                 &f->t);
   if (r != VK_SUCCESS) { LOG("frag: colour target -> %d", r); return r; }

   f->vs = nvk_load_shader(c, fullscreen_vert_spv, sizeof(fullscreen_vert_spv));
   f->fs = nvk_load_shader(c, alu_frag_spv, sizeof(alu_frag_spv));
   if (!f->vs || !f->fs) { LOG("frag: shader modules failed"); return VK_ERROR_UNKNOWN; }

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   r = CreatePipelineLayout(c->dev, &plci, NULL, &f->layout);
   if (r != VK_SUCCESS) return r;

   struct nvk_pipe_desc d = {
      .vs = f->vs, .fs = f->fs, .layout = f->layout, .rp = f->t.rp,
      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   r = nvk_graphics_pipeline(c, &d, &f->pipe);
   if (r != VK_SUCCESS) LOG("frag: pipeline -> %d", r);
   return r;
}

static void frag_rig_free(struct nvk_ctx *c, struct frag_rig *f)
{
   LOAD_DEV(c, DestroyPipeline);
   LOAD_DEV(c, DestroyPipelineLayout);
   LOAD_DEV(c, DestroyShaderModule);
   if (f->pipe) DestroyPipeline(c->dev, f->pipe, NULL);
   if (f->layout) DestroyPipelineLayout(c->dev, f->layout, NULL);
   if (f->vs) DestroyShaderModule(c->dev, f->vs, NULL);
   if (f->fs) DestroyShaderModule(c->dev, f->fs, NULL);
   nvk_free_target(c, &f->t);
}

static double frag_run(struct nvk_ctx *c, struct frag_rig *f, VkCommandPool pool,
                       struct timer *tm, bool readback)
{
   LOAD_DEV(c, CmdBeginRenderPass);
   LOAD_DEV(c, CmdEndRenderPass);
   LOAD_DEV(c, CmdBindPipeline);
   LOAD_DEV(c, CmdDraw);

   LOAD_DEV(c, ResetCommandPool);
   ResetCommandPool(c->dev, pool, 0);

   VkCommandBuffer cb = nvk_begin_cb(c, pool);
   if (!cb) return -1.0;

   timer_begin(c, tm, cb);

   VkClearValue clear;
   memset(&clear, 0, sizeof(clear));
   VkRenderPassBeginInfo rbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = f->t.rp, .framebuffer = f->t.fb,
      .renderArea = { { 0, 0 }, { RT_W, RT_H } },
      .clearValueCount = 1, .pClearValues = &clear,
   };
   CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f->pipe);
   nvk_set_full_viewport(c, cb, RT_W, RT_H);
   for (unsigned i = 0; i < FRAG_DRAWS; i++)
      CmdDraw(cb, 3, 1, 0, 0);
   CmdEndRenderPass(cb);

   timer_end(c, tm, cb);

   if (readback)
      nvk_target_copy_to_host(c, cb, &f->t);

   return timer_submit(c, tm, cb);
}

/* the compute control */

struct comp_rig {
   struct nvk_buffer     ssbo, rb;
   VkShaderModule        cs;
   VkDescriptorSetLayout dsl;
   VkPipelineLayout      layout;
   VkDescriptorPool      dpool;
   VkDescriptorSet       set;
   VkPipeline            pipe;
};

static VkResult comp_rig_init(struct nvk_ctx *c, VkCommandPool pool,
                              struct comp_rig *k)
{
   LOAD_DEV(c, CreateDescriptorSetLayout);
   LOAD_DEV(c, CreatePipelineLayout);
   LOAD_DEV(c, CreateDescriptorPool);
   LOAD_DEV(c, AllocateDescriptorSets);
   LOAD_DEV(c, UpdateDescriptorSets);
   LOAD_DEV(c, CreateComputePipelines);

   memset(k, 0, sizeof(*k));

   void *zeros = calloc(1, COMP_BYTES);
   if (!zeros) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult r = nvk_device_buffer(c, pool, zeros, COMP_BYTES,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &k->ssbo);
   free(zeros);
   if (r != VK_SUCCESS) { LOG("comp: ssbo -> %d", r); return r; }

   r = nvk_host_buffer(c, COMP_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &k->rb);
   if (r != VK_SUCCESS) return r;

   k->cs = nvk_load_shader(c, alu_comp_spv, sizeof(alu_comp_spv));
   if (!k->cs) return VK_ERROR_UNKNOWN;

   VkDescriptorSetLayoutBinding b = {
      .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dlci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &b,
   };
   r = CreateDescriptorSetLayout(c->dev, &dlci, NULL, &k->dsl);
   if (r != VK_SUCCESS) return r;

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &k->dsl,
   };
   r = CreatePipelineLayout(c->dev, &plci, NULL, &k->layout);
   if (r != VK_SUCCESS) return r;

   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
   };
   r = CreateDescriptorPool(c->dev, &dpci, NULL, &k->dpool);
   if (r != VK_SUCCESS) return r;

   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = k->dpool, .descriptorSetCount = 1, .pSetLayouts = &k->dsl,
   };
   r = AllocateDescriptorSets(c->dev, &dsai, &k->set);
   if (r != VK_SUCCESS) return r;

   VkDescriptorBufferInfo dbi = { k->ssbo.buf, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet w = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = k->set, .dstBinding = 0, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi,
   };
   UpdateDescriptorSets(c->dev, 1, &w, 0, NULL);

   VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = k->cs, .pName = "main",
   };
   VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage, .layout = k->layout,
   };
   r = CreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &cpci, NULL, &k->pipe);
   if (r != VK_SUCCESS) LOG("comp: pipeline -> %d", r);
   return r;
}

static void comp_rig_free(struct nvk_ctx *c, struct comp_rig *k)
{
   LOAD_DEV(c, DestroyPipeline);
   LOAD_DEV(c, DestroyPipelineLayout);
   LOAD_DEV(c, DestroyDescriptorPool);
   LOAD_DEV(c, DestroyDescriptorSetLayout);
   LOAD_DEV(c, DestroyShaderModule);
   if (k->pipe) DestroyPipeline(c->dev, k->pipe, NULL);
   if (k->layout) DestroyPipelineLayout(c->dev, k->layout, NULL);
   if (k->dpool) DestroyDescriptorPool(c->dev, k->dpool, NULL);
   if (k->dsl) DestroyDescriptorSetLayout(c->dev, k->dsl, NULL);
   if (k->cs) DestroyShaderModule(c->dev, k->cs, NULL);
   nvk_free_buffer(c, &k->ssbo);
   nvk_free_buffer(c, &k->rb);
}

static double comp_run(struct nvk_ctx *c, struct comp_rig *k, VkCommandPool pool,
                       struct timer *tm, bool readback)
{
   LOAD_DEV(c, CmdBindPipeline);
   LOAD_DEV(c, CmdBindDescriptorSets);
   LOAD_DEV(c, CmdDispatch);
   LOAD_DEV(c, CmdPipelineBarrier);
   LOAD_DEV(c, CmdCopyBuffer);

   LOAD_DEV(c, ResetCommandPool);
   ResetCommandPool(c->dev, pool, 0);

   VkCommandBuffer cb = nvk_begin_cb(c, pool);
   if (!cb) return -1.0;

   timer_begin(c, tm, cb);

   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->layout, 0, 1,
                         &k->set, 0, NULL);
   VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
   };
   for (unsigned i = 0; i < COMP_DISPATCHES; i++) {
      CmdDispatch(cb, COMP_GROUPS, COMP_GROUPS, 1);
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &mb, 0, NULL, 0, NULL);
   }

   timer_end(c, tm, cb);

   if (readback) {
      VkBufferCopy region = { .size = COMP_BYTES };
      CmdCopyBuffer(cb, k->ssbo.buf, k->rb.buf, 1, &region);
   }

   return timer_submit(c, tm, cb);
}

/* one phase */

static bool run_phase(const struct phase_spec *ps, struct result *out)
{
   struct nvk_ctx ctx;
   struct nvk_ctx *c = &ctx;
   struct frag_rig f = {0};
   struct comp_rig k = {0};
   struct timer tm = {0};
   VkCommandPool pool = VK_NULL_HANDLE;
   bool ok = false;

   memset(out, 0, sizeof(*out));

   if (ps->knob)
      setenv("NVK_SUBTILING_KNOB", ps->knob, 1);
   else
      unsetenv("NVK_SUBTILING_KNOB");

   if (nvk_bringup(c, NULL, 0) != VK_SUCCESS) {
      LOG("%s: bringup failed", ps->name);
      goto done;
   }
   if (timer_init(c, &tm) != VK_SUCCESS) {
      LOG("%s: timer init failed", ps->name);
      goto done;
   }

   {
      LOAD_DEV(c, CreateCommandPool);
      VkCommandPoolCreateInfo cpci = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = c->qfi,
      };
      if (CreateCommandPool(c->dev, &cpci, NULL, &pool) != VK_SUCCESS) {
         LOG("%s: command pool failed", ps->name);
         goto done;
      }
   }

   if (frag_rig_init(c, &f) != VK_SUCCESS) goto done;
   if (comp_rig_init(c, pool, &k) != VK_SUCCESS) goto done;

   double samples[REPS];

   if (frag_run(c, &f, pool, &tm, false) < 0.0) goto done;
   for (unsigned i = 0; i < REPS; i++) {
      samples[i] = frag_run(c, &f, pool, &tm, i == REPS - 1);
      if (samples[i] < 0.0) goto done;
      if (i == REPS - 1) out->frag_wall_ms = tm.wall_ms;
   }
   out->frag_ms = median(samples, REPS);
   out->frag_sum = fnv1a(f.t.readback.cpu, (size_t)RT_W * RT_H * 4);

   if (comp_run(c, &k, pool, &tm, false) < 0.0) goto done;
   for (unsigned i = 0; i < REPS; i++) {
      samples[i] = comp_run(c, &k, pool, &tm, i == REPS - 1);
      if (samples[i] < 0.0) goto done;
      if (i == REPS - 1) out->comp_wall_ms = tm.wall_ms;
   }
   out->comp_ms = median(samples, REPS);
   out->comp_sum = fnv1a(k.rb.cpu, (size_t)COMP_BYTES);

   ok = true;
   out->ok = true;

done:
   if (c->dev) {
      LOAD_DEV(c, DeviceWaitIdle);
      LOAD_DEV(c, DestroyCommandPool);
      DeviceWaitIdle(c->dev);
      comp_rig_free(c, &k);
      frag_rig_free(c, &f);
      if (tm.pool) {
         LOAD_DEV(c, DestroyQueryPool);
         DestroyQueryPool(c->dev, tm.pool, NULL);
      }
      if (pool) DestroyCommandPool(c->dev, pool, NULL);
   }
   nvk_teardown(c);
   return ok;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_subtile.log");
   LOG("=== nvk_subtile ===");

   const double frag_iters =
      (double)RT_W * RT_H * ALU_LOOP * FRAG_DRAWS;
   const double comp_iters =
      (double)COMP_DIM * COMP_DIM * ALU_LOOP * COMP_DISPATCHES;

   LOG("params: %ux%u x %u draws, %u dispatches of %ux%u, %u-iteration loop, "
       "%u reps", RT_W, RT_H, FRAG_DRAWS, COMP_DISPATCHES, COMP_DIM, COMP_DIM,
       ALU_LOOP, REPS);

   struct result res[N_PHASES];
   unsigned n_ok = 0;

   memset(res, 0, sizeof(res));
   for (unsigned i = 0; i < N_PHASES; i++) {
      LOG("--- phase %s (%s) ---", phases[i].name,
          phases[i].knob ? phases[i].knob : "driver default");
      if (!run_phase(&phases[i], &res[i])) {
         LOG("%s: phase did not complete", phases[i].name);
         break;
      }
      n_ok++;
   }

   LOG("");
   LOG("timing: %s", g_wall_fallback ? "WALL CLOCK (timestamps unusable)"
                                     : "GPU timestamps");
   LOG("knob        frag Miter/s   comp Miter/s   frag/comp   frag ms   comp ms");
   for (unsigned i = 0; i < N_PHASES; i++) {
      if (!res[i].ok) { LOG("%-10s  did not complete", phases[i].name); continue; }
      double fr = frag_iters / (res[i].frag_ms * 1000.0);
      double cr = comp_iters / (res[i].comp_ms * 1000.0);
      LOG("%-10s  %12.1f   %12.1f   %9.5f   %7.3f   %7.3f",
          phases[i].name, fr, cr, fr / cr, res[i].frag_ms, res[i].comp_ms);
   }

   LOG("");
   for (unsigned i = 0; i < N_PHASES; i++)
      LOG("  %-10s %s", phases[i].name, phases[i].note);

   const struct result *ref = NULL;
   unsigned mismatch = 0;
   for (unsigned i = 0; i < N_PHASES; i++) {
      if (!res[i].ok) continue;
      if (!ref) { ref = &res[i]; continue; }
      if (res[i].frag_sum != ref->frag_sum) {
         LOG("SUBTILE FAIL: %s colour readback 0x%016llx, expected 0x%016llx",
             phases[i].name, (unsigned long long)res[i].frag_sum,
             (unsigned long long)ref->frag_sum);
         mismatch++;
      }
      if (res[i].comp_sum != ref->comp_sum) {
         LOG("SUBTILE FAIL: %s compute readback 0x%016llx, expected 0x%016llx",
             phases[i].name, (unsigned long long)res[i].comp_sum,
             (unsigned long long)ref->comp_sum);
         mismatch++;
      }
   }

   if (n_ok != N_PHASES) {
      LOG("SUBTILE FAIL: %u of %u phases completed", n_ok, (unsigned)N_PHASES);
      LOG("=== nvk_subtile FAILED ===");
   } else if (mismatch) {
      LOG("SUBTILE FAIL: %u readback mismatches",
          mismatch);
      LOG("=== nvk_subtile FAILED ===");
   } else {
      LOG("SUBTILE OK: %u phases, every readback identical (colour "
          "0x%016llx, compute 0x%016llx)", n_ok,
          (unsigned long long)ref->frag_sum, (unsigned long long)ref->comp_sum);
      LOG("=== nvk_subtile PASSED ===");
   }

   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
