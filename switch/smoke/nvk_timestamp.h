/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NVK_TIMESTAMP_H
#define NVK_TIMESTAMP_H
#include <stdint.h>

/* Vulkan timestamps wrap at timestampValidBits, not necessarily at 64 bits. */
static inline uint64_t
nvk_timestamp_delta(uint64_t begin, uint64_t end, uint64_t valid_mask)
{
   return (end - begin) & valid_mask;
}
#endif
