/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Test linking Zink driver into apps
 */
#include "nvk_harness.h"

int main(void)
{
   nvk_log_open("sdmc:/gl_linktest.log");
   LOG("=== gl_linktest: GL archive set linked ===");
   return 0;
}
