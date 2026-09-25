/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DISK_CACHE_HORIZON_H
#define DISK_CACHE_HORIZON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound on one stored blob. */
#define DISK_CACHE_HORIZON_MAX_BLOB (256 * 1024)

struct disk_cache;

/* Backs `cache` with an append-only store on the SD card. */
void disk_cache_horizon_init(struct disk_cache *cache, uint64_t max_size,
                             const void *driver_keys, size_t driver_keys_size);

void disk_cache_horizon_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* DISK_CACHE_HORIZON_H */
