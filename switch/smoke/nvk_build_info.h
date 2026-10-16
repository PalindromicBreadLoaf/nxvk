/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NVK_BUILD_INFO_H
#define NVK_BUILD_INFO_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* build-nro.sh normally supplies these from a generated -include header. */
#ifndef NVK_BUILD_ID
#define NVK_BUILD_ID "unversioned"
#define NVK_BUILD_INFO_TEXT "Build provenance unavailable: manual compiler invocation\n"
#endif

static inline bool
nvk_write_text(const char *dir, const char *name, const char *text)
{
   char path[1024];
   int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (n < 0 || (size_t)n >= sizeof(path))
      return false;
   FILE *f = fopen(path, "wb");
   if (!f)
      return false;
   size_t len = strlen(text);
   bool ok = fwrite(text, 1, len, f) == len;
   return fclose(f) == 0 && ok;
}

static inline bool
nvk_record_build_info(const char *dir)
{
   return nvk_write_text(dir, "BUILDINFO.txt", NVK_BUILD_INFO_TEXT) &&
          nvk_write_text(dir, "BUILD-ID.txt", NVK_BUILD_ID "\n");
}

static inline bool
nvk_build_id_matches(const char *dir)
{
   char path[1024], stored[128];
   int n = snprintf(path, sizeof(path), "%s/BUILD-ID.txt", dir);
   if (n < 0 || (size_t)n >= sizeof(path))
      return false;
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   size_t len = fread(stored, 1, sizeof(stored), f);
   bool ok = !ferror(f) && len == strlen(NVK_BUILD_ID "\n") &&
             memcmp(stored, NVK_BUILD_ID "\n", len) == 0;
   fclose(f);
   return ok;
}

#endif
