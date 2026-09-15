/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hands control back to the ladder runner when a test app exits.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "nvk_run.h"

static void
chain_note(const char *phase, const char *what, const char *detail)
{
   FILE *f = fopen(NVK_RUN_CHAIN, "a");
   if (f == NULL)
      return;
   fprintf(f, "%-4s %-7s %s\n", phase, what, detail);
   fclose(f);
}

static void
nvk_chain_arm(const char *phase)
{
   if (!envHasNextLoad())
      return;

   FILE *f = fopen(NVK_RUN_STATE, "r");
   if (f == NULL)
      return;

   unsigned idx;
   char runner[FS_MAX_PATH];
   const bool got = fscanf(f, "%u\n", &idx) == 1 &&
                    fgets(runner, sizeof(runner), f) != NULL;
   fclose(f);
   if (!got) {
      chain_note(phase, "NOSTATE", "state file unreadable");
      return;
   }

   runner[strcspn(runner, "\r\n")] = '\0';
   if (runner[0] == '\0') {
      chain_note(phase, "NOPATH", "state file names no runner");
      return;
   }

   char args[FS_MAX_PATH + 4];
   snprintf(args, sizeof(args), "\"%s\"", runner);

   const Result rc = envSetNextLoad(runner, args);
   if (R_SUCCEEDED(rc)) {
      chain_note(phase, "armed", runner);
   } else {
      char detail[FS_MAX_PATH + 32];
      snprintf(detail, sizeof(detail), "%s (rc=0x%x)", runner, rc);
      chain_note(phase, "REFUSED", detail);
   }
}

__attribute__((constructor)) static void
nvk_chain_enter(void)
{
   nvk_chain_arm("in");
}

__attribute__((destructor)) static void
nvk_chain_leave(void)
{
   nvk_chain_arm("out");
}
