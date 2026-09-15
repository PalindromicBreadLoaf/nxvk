/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared between the ladder runner and the chain shim every app links.
 */
#ifndef NVK_RUN_H
#define NVK_RUN_H

#define NVK_RUN_DIR     "sdmc:/nxvk_run"
#define NVK_RUN_STATE   NVK_RUN_DIR "/state"
#define NVK_RUN_SUMMARY NVK_RUN_DIR "/summary.log"
#define NVK_RUN_CHAIN   NVK_RUN_DIR "/chain.log"

/* Only used when the loader gives no argv[0] to locate ourselves with. */
#define NVK_RUN_SELF_FALLBACK "sdmc:/switch/nvk_runner.nro"

#endif /* NVK_RUN_H */
