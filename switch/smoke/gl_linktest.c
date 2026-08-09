/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
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
