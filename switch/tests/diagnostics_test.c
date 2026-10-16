/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NVK_BUILD_ID "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define NVK_BUILD_INFO_TEXT "Commit: test\nToolchain: test\n"
#include "../smoke/nvk_build_info.h"
#include "../smoke/nvk_timestamp.h"
#include "../diagnostics/subtile_plan.h"

/* Minimal libnx clock mocks. No clock/voltage setter is declared or linked. */
typedef uint32_t u32;
typedef uint32_t Result;
typedef enum { PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC = 56 } PcvModule;
typedef enum {
   PcvModuleId_CpuBus = 0x40000001,
   PcvModuleId_GPU = 0x40000002,
   PcvModuleId_EMC = 0x40000039,
} PcvModuleId;
typedef struct { PcvModuleId id; } ClkrstSession;
#define R_SUCCEEDED(rc) ((rc) == 0)
#define LOG(...) ((void)0)
static unsigned hos, opens, closes, reads, exits;
static Result init_result, open_result, read_result;
static unsigned hosversionGet(void) { return hos; }
static bool hosversionAtLeast(unsigned a, unsigned b, unsigned c) { return hos >= a; }
static Result clkrstInitialize(void) { return init_result; }
static Result pcvInitialize(void) { return init_result; }
static void clkrstExit(void) { exits++; }
static void pcvExit(void) { exits++; }
static Result clkrstOpenSession(ClkrstSession *s, PcvModuleId id, u32 unk)
{
   assert(unk == 3);
   assert(id == PcvModuleId_CpuBus || id == PcvModuleId_GPU || id == PcvModuleId_EMC);
   opens++;
   s->id = id;
   return id == PcvModuleId_GPU ? open_result : 0;
}
static void clkrstCloseSession(ClkrstSession *s) { closes++; }
static Result clkrstGetClockRate(ClkrstSession *s, u32 *hz)
{
   reads++;
   *hz = 384000000;
   return read_result;
}
static Result pcvGetClockRate(PcvModule module, u32 *hz)
{
   assert(module == PcvModule_CpuBus || module == PcvModule_GPU || module == PcvModule_EMC);
   reads++;
   *hz = 384000000;
   return read_result;
}
#include "../diagnostics/clock_snapshot.h"

static void clock_reset(unsigned version)
{
   hos = version;
   opens = closes = reads = exits = 0;
   init_result = open_result = read_result = 0;
}

int main(int argc, char **argv)
{
   assert(argc == 2);
   unsigned positions[DIAG_VARIANTS][DIAG_VARIANTS] = {{0}};
   for (unsigned p = 0; p < DIAG_PHASES - 1; p++) {
      assert(diag_variant(p) < DIAG_VARIANTS);
      positions[p % DIAG_VARIANTS][diag_variant(p)]++;
      unsigned firsts = 0;
      for (unsigned i = 0; i < DIAG_SAMPLES; i++)
         firsts += diag_compute_first(p, i);
      assert(firsts == 7 || firsts == 8);
   }
   for (unsigned a = 0; a < DIAG_VARIANTS; a++)
      for (unsigned b = 0; b < DIAG_VARIANTS; b++)
         assert(positions[a][b] == 1);
   assert(diag_variant(0) == 0 && diag_variant(DIAG_PHASES - 1) == 0);
   assert(diag_warmup_needed(1999.9, 100));
   assert(diag_warmup_needed(3000.0, DIAG_WARMUP_PAIRS - 1));
   assert(!diag_warmup_needed(2000.0, DIAG_WARMUP_PAIRS));

   for (unsigned bits = 1; bits < 64; bits++) {
      uint64_t mask = (UINT64_C(1) << bits) - 1;
      assert(nvk_timestamp_delta(mask, 0, mask) == 1);
      assert(nvk_timestamp_delta(0, mask, mask) == mask);
      assert(nvk_timestamp_delta(4, 4, mask) == 0);
   }
   assert(nvk_timestamp_delta(UINT64_MAX, 0, UINT64_MAX) == 1);
   assert(nvk_timestamp_delta(42, 43, 0) == 0);

   assert(!nvk_build_id_matches(argv[1]));
   assert(nvk_record_build_info(argv[1]));
   assert(nvk_build_id_matches(argv[1]));
   assert(nvk_write_text(argv[1], "BUILD-ID.txt", NVK_BUILD_ID "\nextra"));
   assert(!nvk_build_id_matches(argv[1]));
   assert(nvk_write_text(argv[1], "BUILD-ID.txt", "other-build\n"));
   assert(!nvk_build_id_matches(argv[1]));
   assert(nvk_write_text(argv[1], "BUILD-ID.txt", ""));
   assert(!nvk_build_id_matches(argv[1]));
   assert(nvk_record_build_info(argv[1]));
   assert(!nvk_record_build_info("/no-such-nxvk-test-directory"));
   char huge[2048];
   memset(huge, 'x', sizeof(huge) - 1); huge[sizeof(huge) - 1] = 0;
   assert(!nvk_record_build_info(huge) && !nvk_build_id_matches(huge));

   struct diag_clocks c;
   struct diag_rates r;
   clock_reset(0);
   diag_clocks_open(&c); r = diag_clocks_read(&c); diag_clocks_close(&c);
   assert(!r.valid[0] && opens == 0 && reads == 0 && exits == 0);
   clock_reset(8);
   init_result = 7;
   diag_clocks_open(&c); r = diag_clocks_read(&c); diag_clocks_close(&c);
   assert(!r.valid[0] && r.rc[0] == 7 && opens == 0 && reads == 0 && exits == 0);
   clock_reset(8);
   diag_clocks_open(&c); r = diag_clocks_read(&c); diag_clocks_close(&c);
   assert(r.valid[0] && r.valid[1] && r.valid[2]);
   assert(opens == 3 && reads == 3 && closes == 3 && exits == 1);
   clock_reset(8);
   open_result = 5;
   diag_clocks_open(&c); r = diag_clocks_read(&c); diag_clocks_close(&c);
   assert(r.valid[0] && !r.valid[1] && r.valid[2] && r.rc[1] == 5);
   assert(reads == 2 && closes == 2 && exits == 1);
   clock_reset(7);
   diag_clocks_open(&c); r = diag_clocks_read(&c); diag_clocks_close(&c);
   assert(r.valid[0] && r.valid[1] && r.valid[2]);
   assert(opens == 0 && reads == 3 && closes == 0 && exits == 1);
   clock_reset(8);
   read_result = 8;
   diag_clocks_open(&c); r = diag_clocks_read(&c); diag_clocks_close(&c);
   assert(!r.valid[0] && !r.valid[1] && !r.valid[2] && r.rc[1] == 8);
   diag_clocks_log("mock", r);
   puts("PASS: diagnostic ordering, warmup gates, timestamp wrap, build identity and read-only clock failure paths");
   return 0;
}
