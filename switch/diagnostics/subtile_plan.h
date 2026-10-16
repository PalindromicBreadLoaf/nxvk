/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NVK_SUBTILE_PLAN_H
#define NVK_SUBTILE_PLAN_H
#include <stdbool.h>

#define DIAG_VARIANTS 3u
#define DIAG_ROUNDS 3u
#define DIAG_PHASES (DIAG_VARIANTS * DIAG_ROUNDS + 1u)
#define DIAG_SAMPLES 15u
#define DIAG_WARMUP_MS 2000.0
#define DIAG_WARMUP_PAIRS 16u

/* Cyclic Latin square: each variant occupies each position once. Finish with
 * the default again to expose drift. Nothing is randomized without a record. */
static inline unsigned
diag_variant(unsigned phase)
{
   if (phase >= DIAG_VARIANTS * DIAG_ROUNDS)
      return 0;
   return (phase / DIAG_VARIANTS + phase % DIAG_VARIANTS) % DIAG_VARIANTS;
}

static inline bool
diag_compute_first(unsigned phase, unsigned sample)
{
   return (phase + sample) % 2 != 0;
}

static inline bool
diag_warmup_needed(double elapsed_ms, unsigned pairs)
{
   return elapsed_ms < DIAG_WARMUP_MS || pairs < DIAG_WARMUP_PAIRS;
}
#endif
