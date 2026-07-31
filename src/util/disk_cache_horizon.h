/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

#ifndef DISK_CACHE_HORIZON_H
#define DISK_CACHE_HORIZON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound on one stored blob. disk_cache reads an entry back into a buffer
 * it sizes from this same constant, so anything larger is skipped at write
 * time rather than stored in a form nothing can read.
 */
#define DISK_CACHE_HORIZON_MAX_BLOB (256 * 1024)

struct disk_cache;

/* Backs `cache` with an append-only store on the SD card and installs it as
 * the cache's blob callbacks. Every cache in the process shares one store.
 */
void disk_cache_horizon_init(struct disk_cache *cache, uint64_t max_size);

void disk_cache_horizon_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* DISK_CACHE_HORIZON_H */
