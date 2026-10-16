/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Controlled diagnostic schedule around the legacy probe's exact workloads. */
#ifndef NVK_DIAGNOSTIC
#define NVK_DIAGNOSTIC 1
#endif
#include <math.h>
#include <sys/stat.h>
#include "../smoke/nvk_run.h"
#include "../smoke/nvk_build_info.h"

#ifdef NVK_DIAG_NO_CACHE
#define DIAG_APP "nvk_subtile_nocache"
#define DIAG_CACHE "disabled"
#define DIAG_DISABLE_CACHE "1"
#else
#define DIAG_APP "nvk_subtile_diag"
#define DIAG_CACHE "enabled"
#define DIAG_DISABLE_CACHE "0"
#endif

#define NVK_DIAG_MESA_LOG NVK_RUN_DIR "/" DIAG_APP ".mesa.log"
#define NVK_SUBTILE_DIAGNOSTIC 1
#include "../smoke/nvk_subtile.c"
#include "subtile_plan.h"
#include "clock_snapshot.h"

static const char *const variant_names[] = { "default", "t210", "generic" };
static const char *const variant_knobs[] = { NULL, "0x087f6080", "0x20164010" };

struct diag_sample {
   double frag_gpu, comp_gpu, frag_wall, comp_wall;
   bool compute_first, readback;
   struct diag_rates before, after;
};

struct diag_result {
   bool ok, gpu_valid;
   double frag, comp;
   uint64_t frag_hash, comp_hash;
};

/* Samples contain separate GPU and wall times. Never substitute a wall-clock
 * value into the GPU column when timestamps fail partway through a phase. */
static bool
diag_pair(struct nvk_ctx *c, struct frag_rig *f, struct comp_rig *k,
          VkCommandPool pool, struct timer *tm, bool compute_first, bool readback,
          struct diag_sample *s)
{
   memset(s, 0, sizeof(*s));
   s->compute_first = compute_first;
   s->readback = readback;
   s->frag_gpu = s->comp_gpu = NAN;
   for (unsigned i = 0; i < 2; i++) {
      bool compute = compute_first ? i == 0 : i == 1;
      double t = compute ? comp_run(c, k, pool, tm, readback)
                         : frag_run(c, f, pool, tm, readback);
      if (t < 0.0)
         return false;
      if (compute) {
         s->comp_wall = tm->wall_ms;
         if (tm->pool && !tm->gave_up) s->comp_gpu = t;
      } else {
         s->frag_wall = tm->wall_ms;
         if (tm->pool && !tm->gave_up) s->frag_gpu = t;
      }
   }
   return true;
}

static void
diag_csv(FILE *csv, unsigned phase, const char *kind, unsigned sample,
         const struct diag_sample *s)
{
   fprintf(csv, "%s,%u,%s,%s,%s,%u,%s,%u,%.6f,%.6f,%.6f,%.6f",
           DIAG_APP, phase, variant_names[diag_variant(phase)], DIAG_CACHE,
           kind, sample, s->compute_first ? "compute" : "fragment", s->readback,
           s->frag_gpu, s->comp_gpu, s->frag_wall, s->comp_wall);
   const struct diag_rates *rates[] = { &s->before, &s->after };
   for (unsigned side = 0; side < 2; side++)
      for (unsigned i = 0; i < 3; i++)
         fprintf(csv, ",%u,%u,0x%08x", rates[side]->hz[i],
                 rates[side]->valid[i], rates[side]->rc[i]);
   fputc('\n', csv);
}

static void
diag_device_info(struct nvk_ctx *c)
{
   LOAD_INST(c, GetPhysicalDeviceProperties2);
   if (GetPhysicalDeviceProperties2) {
      VkPhysicalDeviceDriverProperties driver = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      VkPhysicalDeviceProperties2 p = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver,
      };
      GetPhysicalDeviceProperties2(c->phys, &p);
      LOG("driver: name=%s info=%s id=%u", driver.driverName, driver.driverInfo,
          (unsigned)driver.driverID);
   }
   char uuid[VK_UUID_SIZE * 2 + 1];
   for (unsigned i = 0; i < VK_UUID_SIZE; i++)
      snprintf(uuid + 2 * i, 3, "%02x", c->props.pipelineCacheUUID[i]);
   LOG("pipelineCacheUUID=%s timestampPeriod_ns=%.6f", uuid,
       c->props.limits.timestampPeriod);
}

static bool
diag_phase(unsigned phase, struct diag_clocks *clocks, FILE *csv,
           struct diag_result *out)
{
   struct nvk_ctx ctx = {0}, *c = &ctx;
   struct frag_rig f = {0};
   struct comp_rig k = {0};
   struct timer tm = {0};
   VkCommandPool pool = VK_NULL_HANDLE;
   struct diag_sample cold, samples[DIAG_SAMPLES];
   bool ok = false;
   const unsigned v = diag_variant(phase);
   const char *knob = variant_knobs[v];
   memset(out, 0, sizeof(*out));
   out->frag = out->comp = NAN;
   g_wall_fallback = false;

   LOG("PHASE %u/%u requested=%s knob=%s cache=%s", phase + 1, DIAG_PHASES,
       variant_names[v], knob ? knob : "unset (driver choice)", DIAG_CACHE);
   LOG("phase modes: operation=%u performance=%u", (unsigned)appletGetOperationMode(),
       (unsigned)appletGetPerformanceMode());
   consoleUpdate(NULL);
   if ((knob ? setenv("NVK_SUBTILING_KNOB", knob, 1) : unsetenv("NVK_SUBTILING_KNOB")) != 0)
      goto done;
   if (nvk_bringup(c, NULL, 0) != VK_SUCCESS || timer_init(c, &tm) != VK_SUCCESS)
      goto done;
   diag_device_info(c);
   {
      LOAD_DEV(c, CreateCommandPool);
      VkCommandPoolCreateInfo ci = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = c->qfi,
      };
      if (CreateCommandPool(c->dev, &ci, NULL, &pool) != VK_SUCCESS)
         goto done;
   }
   /* Driver's opt-in NVK_SUBTILING trace reports the emitted word here, for
    * both compiled and cache-loaded fragment shaders, before timed work. */
   if (frag_rig_init(c, &f) != VK_SUCCESS || comp_rig_init(c, pool, &k) != VK_SUCCESS)
      goto done;

   struct diag_rates before = diag_clocks_read(clocks);
   if (!diag_pair(c, &f, &k, pool, &tm, diag_compute_first(phase, 0), false, &cold))
      goto done;
   cold.before = before;
   cold.after = diag_clocks_read(clocks);
   LOG("cold probe (not a steady-state result): frag_gpu_ms=%.6f comp_gpu_ms=%.6f",
       cold.frag_gpu, cold.comp_gpu);
   diag_clocks_log("cold-before", cold.before);
   diag_clocks_log("cold-after", cold.after);
   diag_csv(csv, phase, "cold", 0, &cold);

   unsigned warm_pairs = 0;
   double warm_start = now_ms();
   do {
      struct diag_sample unused;
      if (!appletMainLoop() ||
          !diag_pair(c, &f, &k, pool, &tm, diag_compute_first(phase, warm_pairs),
                     false, &unused))
         goto done;
      warm_pairs++;
   } while (diag_warmup_needed(now_ms() - warm_start, warm_pairs));
   double warm_ms = now_ms() - warm_start;
   struct diag_rates warm_rates = diag_clocks_read(clocks);
   /* Do not insert an SD write between warmup and sampling. */

   /* No SD/log/console output in the timed sampling block. Clock reads are
    * outside each GPU timestamp interval, before/after each workload pair. */
   for (unsigned i = 0; i < DIAG_SAMPLES; i++) {
      before = diag_clocks_read(clocks);
      if (!diag_pair(c, &f, &k, pool, &tm, diag_compute_first(phase, i),
                     i == DIAG_SAMPLES - 1, &samples[i]))
         goto done;
      samples[i].before = before;
      samples[i].after = diag_clocks_read(clocks);
   }
   LOG("warmup: %.3f ms, %u fragment/compute pairs (minimum %.0f ms and %u pairs)",
       warm_ms, warm_pairs, DIAG_WARMUP_MS, DIAG_WARMUP_PAIRS);
   diag_clocks_log("post-warmup", warm_rates);
   out->frag_hash = fnv1a(f.t.readback.cpu, (size_t)RT_W * RT_H * 4);
   out->comp_hash = fnv1a(k.rb.cpu, (size_t)COMP_BYTES);
   out->gpu_valid = true;
   double fg[DIAG_SAMPLES], cp[DIAG_SAMPLES];
   for (unsigned i = 0; i < DIAG_SAMPLES; i++) {
      diag_csv(csv, phase, "warm", i, &samples[i]);
      fg[i] = samples[i].frag_gpu;
      cp[i] = samples[i].comp_gpu;
      if (!isfinite(fg[i]) || !isfinite(cp[i])) out->gpu_valid = false;
   }
   if (out->gpu_valid) {
      out->frag = median(fg, DIAG_SAMPLES);
      out->comp = median(cp, DIAG_SAMPLES);
      LOG("warm GPU ms: frag median=%.6f min=%.6f max=%.6f; comp median=%.6f min=%.6f max=%.6f",
          out->frag, fg[0], fg[DIAG_SAMPLES - 1], out->comp, cp[0], cp[DIAG_SAMPLES - 1]);
   } else {
      LOG("TIMING INCONCLUSIVE: missing GPU timestamps; CSV retains separate wall times, GPU fields are nan");
   }
   LOG("READBACK phase=%u colour=0x%016llx compute=0x%016llx", phase,
       (unsigned long long)out->frag_hash, (unsigned long long)out->comp_hash);
   if (fflush(csv) != 0 || ferror(csv))
      goto done;
   ok = out->ok = true;

done:
   if (c->dev) {
      LOAD_DEV(c, DeviceWaitIdle);
      LOAD_DEV(c, DestroyCommandPool);
      LOAD_DEV(c, DestroyQueryPool);
      if (DeviceWaitIdle(c->dev) != VK_SUCCESS) ok = out->ok = false;
      comp_rig_free(c, &k);
      frag_rig_free(c, &f);
      if (tm.pool) DestroyQueryPool(c->dev, tm.pool, NULL);
      if (pool) DestroyCommandPool(c->dev, pool, NULL);
   }
   nvk_teardown(c);
   return ok;
}

int main(void)
{
   consoleInit(NULL);
   mkdir(NVK_RUN_DIR, 0777);
   FILE *state = fopen(NVK_RUN_STATE, "r");
   bool resuming = state != NULL;
   if (state) fclose(state);
   if (resuming && !nvk_build_id_matches(NVK_RUN_DIR)) {
      nvk_write_text(NVK_RUN_DIR, "build-mismatch.log",
                     "Refused mixed-build diagnostic run. App build ID: " NVK_BUILD_ID "\n");
      consoleExit(NULL);
      return 1;
   }
   if (!nvk_record_build_info(NVK_RUN_DIR)) {
      printf("Cannot record diagnostic build identity; check SD card.\n");
      consoleExit(NULL);
      return 1;
   }
   nvk_log_open(NVK_RUN_DIR "/" DIAG_APP ".log");
   if (!g_nvk_log) { consoleExit(NULL); return 1; }
   remove(NVK_DIAG_MESA_LOG);
   remove(NVK_RUN_DIR "/" DIAG_APP ".csv");
   remove(NVK_RUN_DIR "/" DIAG_APP ".cache.log");
   LOG("=== " DIAG_APP " ===");
   LOG("diagnostic schema=1 build-id=" NVK_BUILD_ID);
   LOG("phases=%u samples=%u warmup_ms=%.0f cache=%s; PASS means readbacks, not a speedup",
       DIAG_PHASES, DIAG_SAMPLES, DIAG_WARMUP_MS, DIAG_CACHE);
   LOG("HOS=%u.%u.%u operation_mode=%u performance_mode=%u",
       HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
       HOSVER_MICRO(hosversionGet()), (unsigned)appletGetOperationMode(),
       (unsigned)appletGetPerformanceMode());
   u64 limit = 0;
   Result memory_rc = svcGetInfo(&limit, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
   LOG("process memory limit=%llu rc=0x%08x", (unsigned long long)limit, memory_rc);
   LOG("inherited NVK_DEBUG=%s; probe replaces it with subtiling trace only",
       getenv("NVK_DEBUG") ? getenv("NVK_DEBUG") : "(unset)");
   LOG("inherited NAK_DEBUG=%s; probe clears it", getenv("NAK_DEBUG") ? getenv("NAK_DEBUG") : "(unset)");

   bool ok = false;
   struct diag_clocks clocks = {0};
   FILE *csv = NULL;
   struct diag_result results[DIAG_PHASES] = {0};
   unsigned completed = 0;
   mkdir(NVK_RUN_DIR "/cache", 0777);
   if (setenv("NVK_DEBUG", "subtiling", 1) || unsetenv("NAK_DEBUG") ||
       setenv("MESA_LOG_FILE", NVK_RUN_DIR "/" DIAG_APP ".cache.log", 1) ||
       unsetenv("MESA_LOG_FILE_AUTO") || setenv("MESA_LOG_LEVEL", "info", 1) ||
       setenv("MESA_SHADER_CACHE_DISABLE", DIAG_DISABLE_CACHE, 1) ||
       setenv("MESA_SHADER_CACHE_DIR", NVK_RUN_DIR "/cache", 1) ||
       setenv("MESA_SHADER_CACHE_MAX_SIZE", "16M", 1) ||
       unsetenv("MESA_DISK_CACHE_SINGLE_FILE") || unsetenv("MESA_DISK_CACHE_DATABASE") ||
       unsetenv("MESA_DISK_CACHE_MULTI_FILE"))
      goto done;
   LOG("environment: NVK_DEBUG=subtiling MESA_SHADER_CACHE_DISABLE=" DIAG_DISABLE_CACHE
       " MESA_SHADER_CACHE_DIR=" NVK_RUN_DIR "/cache; clocks are never changed by this probe");
   csv = fopen(NVK_RUN_DIR "/" DIAG_APP ".csv", "w");
   if (!csv) goto done;
   fprintf(csv, "# schema=1 build-id=%s\n", NVK_BUILD_ID);
   fputs("app,phase,variant,cache_requested,kind,sample,first,readback,frag_gpu_ms,comp_gpu_ms,frag_wall_ms,comp_wall_ms", csv);
   const char *sides[] = {"before", "after"};
   const char *modules[] = {"cpu", "gpu", "emc"};
   for (unsigned s = 0; s < 2; s++)
      for (unsigned m = 0; m < 3; m++)
         fprintf(csv, ",%s_%s_hz,%s_%s_valid,%s_%s_rc", sides[s], modules[m],
                 sides[s], modules[m], sides[s], modules[m]);
   fputc('\n', csv);
   diag_clocks_open(&clocks);

   for (unsigned p = 0; p < DIAG_PHASES; p++) {
      if (!appletMainLoop() || !diag_phase(p, &clocks, csv, &results[p]))
         break;
      completed++;
      if (results[p].frag_hash != results[0].frag_hash ||
          results[p].comp_hash != results[0].comp_hash) {
         LOG("READBACK MISMATCH: phase %u differs from phase 0", p);
         goto done;
      }
   }
   ok = completed == DIAG_PHASES;
   LOG("SUMMARY: phase requested frag_gpu_median_ms comp_gpu_median_ms timing");
   for (unsigned p = 0; p < completed; p++)
      LOG("SUMMARY %u %s %.6f %.6f %s", p, variant_names[diag_variant(p)],
          results[p].frag, results[p].comp, results[p].gpu_valid ? "GPU" : "UNAVAILABLE");
   LOG("No automatic speedup verdict: compare warm pairs, clock snapshots, emitted knob traces and repeated defaults.");

done:
   diag_clocks_close(&clocks);
   if (csv && fclose(csv) != 0) ok = false;
   LOG("completed %u/%u phases; build-id=" NVK_BUILD_ID, completed, DIAG_PHASES);
   LOG("=== " DIAG_APP " %s ===", ok ? "PASSED" : "FAILED");
   consoleUpdate(NULL);
   fclose(g_nvk_log);
   consoleExit(NULL);
   return ok ? 0 : 1;
}
