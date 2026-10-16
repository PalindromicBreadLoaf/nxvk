/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NVK_CLOCK_SNAPSHOT_H
#define NVK_CLOCK_SNAPSHOT_H

/* Read-only observations. Never set a clock, voltage or performance policy. */
struct diag_rates {
   u32 hz[3];
   Result rc[3];
   bool valid[3];
};

struct diag_clocks {
   bool ready, modern;
   Result init_rc, open_rc[3];
   bool opened[3];
   ClkrstSession sessions[3];
};

static const PcvModule diag_modules[3] = {
   PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC,
};
static const PcvModuleId diag_module_ids[3] = {
   PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC,
};

static void
diag_clocks_open(struct diag_clocks *c)
{
   memset(c, 0, sizeof(*c));
   if (!hosversionGet()) {
      LOG("clocks: unknown HOS version; rates unavailable");
      return;
   }
   c->modern = hosversionAtLeast(8, 0, 0);
   c->init_rc = c->modern ? clkrstInitialize() : pcvInitialize();
   c->ready = R_SUCCEEDED(c->init_rc);
   LOG("clocks: read-only %s init rc=0x%08x", c->modern ? "clkrst" : "pcv", c->init_rc);
   if (c->ready && c->modern) {
      for (unsigned i = 0; i < 3; i++) {
         c->open_rc[i] = clkrstOpenSession(&c->sessions[i], diag_module_ids[i], 3);
         c->opened[i] = R_SUCCEEDED(c->open_rc[i]);
         LOG("clocks: module=%u open rc=0x%08x", i, c->open_rc[i]);
      }
   }
}

static struct diag_rates
diag_clocks_read(struct diag_clocks *c)
{
   struct diag_rates r = {0};
   for (unsigned i = 0; i < 3; i++) {
      if (!c->ready) {
         r.rc[i] = c->init_rc;
         continue;
      }
      if (c->modern && !c->opened[i]) {
         r.rc[i] = c->open_rc[i];
         continue;
      }
      r.rc[i] = c->modern ? clkrstGetClockRate(&c->sessions[i], &r.hz[i])
                          : pcvGetClockRate(diag_modules[i], &r.hz[i]);
      r.valid[i] = R_SUCCEEDED(r.rc[i]);
   }
   return r;
}

static void
diag_clocks_log(const char *label, struct diag_rates r)
{
   LOG("clocks %s: CPU=%u valid=%u rc=0x%08x GPU=%u valid=%u rc=0x%08x EMC=%u valid=%u rc=0x%08x (Hz reported by service)",
       label, r.hz[0], r.valid[0], r.rc[0], r.hz[1], r.valid[1], r.rc[1],
       r.hz[2], r.valid[2], r.rc[2]);
}

static void
diag_clocks_close(struct diag_clocks *c)
{
   if (!c->ready)
      return;
   if (c->modern) {
      for (unsigned i = 0; i < 3; i++)
         if (c->opened[i]) clkrstCloseSession(&c->sessions[i]);
      clkrstExit();
   } else {
      pcvExit();
   }
}
#endif
