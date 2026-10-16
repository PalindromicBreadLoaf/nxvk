/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Runs the whole milestone ladder unattended.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "nvk_run.h"
#include "nvk_build_info.h"

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

/* Milestone ladder order */
static const char *const apps[] = {
#ifdef NVK_DIAGNOSTIC
   "nvk_subtile_diag", "nvk_subtile_nocache",
#else
   "nvk_smoke", "nvk_tri", "nvk_logo", "nvk_scene", "nvk_indexed", "nvk_multi",
   "nvk_textures", "nvk_cubemap", "nvk_vi_swapchain", "nvk_present",
   "nvk_compress", "nvk_sector", "nvk_zcull", "nvk_push_desc",
   "nvk_desc_flush", "nvk_cmd_flush", "nvk_ce_copy", "nvk_engine_wait",
   "nvk_b2_flush", "nvk_mem_churn", "nvk_subtile", "nvk_host_copy",
#ifndef NVK_VULKAN_ONLY
   "gl_linktest", "gl_gallium", "gl_caps", "gl_smoke", "gl_tri", "gl_tex",
   "gl_fbo", "gl_ubo_vbo", "gl_egl_tri", "gl_multi", "gles3", "gl_feat",
   "gl_diag",
#endif
#endif /* NVK_DIAGNOSTIC */
};
#define APP_COUNT ((unsigned)(sizeof(apps) / sizeof(apps[0])))

/* state */

static bool
state_read(unsigned *idx_out)
{
   FILE *f = fopen(NVK_RUN_STATE, "r");
   if (f == NULL)
      return false;

   unsigned idx;
   const bool got = fscanf(f, "%u\n", &idx) == 1;
   fclose(f);
   if (!got)
      return false;

   *idx_out = idx;
   return true;
}

static void
state_write(unsigned idx, const char *self)
{
   FILE *f = fopen(NVK_RUN_STATE, "w");
   if (f == NULL)
      return;
   fprintf(f, "%u\n%s\n", idx, self);
   fclose(f);
}

/* harvesting */

static void
append_summary(const char *status, const char *app, const char *detail)
{
   FILE *f = fopen(NVK_RUN_SUMMARY, "a");
   if (f == NULL)
      return;
   fprintf(f, "%-4s %-18s %s\n", status, app, detail);
   fclose(f);
}

static void
copy_file(const char *src, const char *dst)
{
   FILE *in = fopen(src, "r");
   if (in == NULL)
      return;
   FILE *out = fopen(dst, "w");
   if (out == NULL) { fclose(in); return; }

   char buf[4096];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      fwrite(buf, 1, n, out);

   fclose(out);
   fclose(in);
}

static const char *
scan_verdict(const char *path, char *detail, size_t detail_sz)
{
   FILE *f = fopen(path, "r");
   if (f == NULL) {
      snprintf(detail, detail_sz, "never started");
      return "GONE";
   }

   const char *status = NULL;
   char line[512];
   while (fgets(line, sizeof(line), f) != NULL) {
      line[strcspn(line, "\r\n")] = '\0';
      if (strncmp(line, "===", 3) != 0)
         continue;
      if (strstr(line, "FAILED") != NULL)
         status = "FAIL";
      else if (strstr(line, "PASSED") != NULL)
         status = "PASS";
      else
         continue;
      snprintf(detail, detail_sz, "%s", line);
   }
   fclose(f);

   if (status == NULL) {
      snprintf(detail, detail_sz, "hung or faulted");
      return "GONE";
   }
   return status;
}

static void
harvest(const char *app)
{
   char src[FS_MAX_PATH], dst[FS_MAX_PATH], detail[512];

#ifdef NVK_DIAGNOSTIC
   snprintf(src, sizeof(src), NVK_RUN_DIR "/%s.log", app);
#else
   snprintf(src, sizeof(src), "sdmc:/%s.log", app);
#endif
   const char *status = scan_verdict(src, detail, sizeof(detail));
   append_summary(status, app, detail);

   snprintf(dst, sizeof(dst), NVK_RUN_DIR "/%s.log", app);
   if (strcmp(src, dst) != 0)
      copy_file(src, dst);

#ifndef NVK_DIAGNOSTIC
   snprintf(dst, sizeof(dst), NVK_RUN_DIR "/%s.mesa.log", app);
   remove(dst);
   snprintf(src, sizeof(src), "sdmc:/%s_mesa.log", app);
   if (rename(src, dst) != 0)
      rename("sdmc:/nvk_mesa.log", dst);
#endif
}

/* the final screen */

static void
report(void)
{
   FILE *f = fopen(NVK_RUN_SUMMARY, "r");
   if (f == NULL) {
      printf("no summary at " NVK_RUN_SUMMARY "\n");
      return;
   }

   unsigned pass = 0, fail = 0, gone = 0, skip = 0;
   char line[512];
   printf("\n");
   while (fgets(line, sizeof(line), f) != NULL) {
      line[strcspn(line, "\r\n")] = '\0';
      if (strncmp(line, "PASS", 4) == 0) { pass++; continue; }
      if (strncmp(line, "FAIL", 4) == 0) fail++;
      else if (strncmp(line, "GONE", 4) == 0) gone++;
      else if (strncmp(line, "SKIP", 4) == 0) skip++;
      else continue;
      printf("%.79s\n", line);
   }
   fclose(f);

   printf("\n%u passed, %u failed, %u gone, %u skipped, of %u\n",
          pass, fail, gone, skip, APP_COUNT);
   printf("full list: " NVK_RUN_SUMMARY "\n");
   printf("per-app logs and driver logs: " NVK_RUN_DIR "/\n");
   printf("chainload trace: " NVK_RUN_CHAIN "\n");
}

int
main(int argc, char **argv)
{
   consoleInit(NULL);
   padConfigureInput(1, HidNpadStyleSet_NpadStandard);
   PadState pad;
   padInitializeDefault(&pad);

   char self[FS_MAX_PATH];
   snprintf(self, sizeof(self), "%s",
            (argc > 0 && argv[0] != NULL && argv[0][0] != '\0')
               ? argv[0] : NVK_RUN_SELF_FALLBACK);

   char dir[FS_MAX_PATH];
   snprintf(dir, sizeof(dir), "%s", self);
   char *slash = strrchr(dir, '/');
   if (slash != NULL)
      *slash = '\0';
   else
      snprintf(dir, sizeof(dir), "sdmc:/switch");

   mkdir(NVK_RUN_DIR, 0777);

   unsigned idx = 0;
   if (state_read(&idx)) {
      if (!nvk_build_id_matches(NVK_RUN_DIR)) {
         printf("Saved run is from another/unknown build. Back up logs, remove only "
                NVK_RUN_STATE " and restart.\n");
         goto wait_for_exit;
      }
      if (idx >= 1 && idx <= APP_COUNT)
         harvest(apps[idx - 1]);
   } else {
      idx = 0;
      if (!nvk_record_build_info(NVK_RUN_DIR)) {
         printf("Cannot write build identity to " NVK_RUN_DIR "; check SD card.\n");
         goto wait_for_exit;
      }
      /* Reference manifest only; checksums are verified when packaging in CI.
       * Never leave an old manifest behind if the new bundle lacks one. */
      char manifest[FS_MAX_PATH + 64];
      snprintf(manifest, sizeof(manifest), "%s/SHA256SUMS", dir);
      remove(NVK_RUN_DIR "/SHA256SUMS");
      copy_file(manifest, NVK_RUN_DIR "/SHA256SUMS");
      remove(NVK_RUN_CHAIN);
      FILE *f = fopen(NVK_RUN_SUMMARY, "w");
      if (f != NULL) {
         fprintf(f, "nxvk milestone ladder, %u apps, from %s\n\n",
                 APP_COUNT, dir);
         fprintf(f, "build-id: %s\n\n", NVK_BUILD_ID);
         fclose(f);
      }
   }

   while (idx < APP_COUNT) {
      char nro[FS_MAX_PATH + 64];
      snprintf(nro, sizeof(nro), "%s/%s.nro", dir, apps[idx]);

      struct stat st;
      if (stat(nro, &st) != 0) {
         append_summary("SKIP", apps[idx], "no .nro beside the runner");
         idx++;
         continue;
      }

      if (!envHasNextLoad()) {
         printf("this loader cannot chainload\n");
         break;
      }

      printf("[%u/%u] %s\n", idx + 1, APP_COUNT, apps[idx]);
      consoleUpdate(NULL);

      char args[FS_MAX_PATH + 68];
      snprintf(args, sizeof(args), "\"%s\"", nro);
      if (R_FAILED(envSetNextLoad(nro, args))) {
         append_summary("SKIP", apps[idx], "envSetNextLoad refused it");
         idx++;
         continue;
      }

      /* If the next app never starts, do not harvest an earlier PASS. */
      char old_log[FS_MAX_PATH + 64];
#ifdef NVK_DIAGNOSTIC
      snprintf(old_log, sizeof(old_log), NVK_RUN_DIR "/%s.log", apps[idx]);
#else
      snprintf(old_log, sizeof(old_log), "sdmc:/%s.log", apps[idx]);
#endif
      remove(old_log);
#ifdef NVK_DIAGNOSTIC
      const char *extras[] = { "csv", "mesa.log", "cache.log" };
      for (unsigned i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
         snprintf(old_log, sizeof(old_log), NVK_RUN_DIR "/%s.%s", apps[idx], extras[i]);
         remove(old_log);
      }
#endif
      state_write(idx + 1, self);
      consoleExit(NULL);
      return 0;
   }

   remove(NVK_RUN_STATE);
   report();
wait_for_exit:
   printf("\npress + to exit\n");
   consoleUpdate(NULL);

   while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
         break;
      consoleUpdate(NULL);
   }

   consoleExit(NULL);
   return 0;
}
