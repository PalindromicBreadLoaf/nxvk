/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host syntax checks only. libnx implements these as inline functions over the
 * ARM system counter register; declarations are enough to parse callers. */
#ifndef TEST_SYNTAX_SWITCH_ARM_COUNTER_H
#define TEST_SYNTAX_SWITCH_ARM_COUNTER_H
#include <stdint.h>

typedef uint64_t u64;

u64 armGetSystemTick(void);
u64 armGetSystemTickFreq(void);
u64 armTicksToNs(u64 ticks);
#endif
