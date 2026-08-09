#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This checks to verify that the build environment is working as expected.
# Run inside the image with the repo at /work:
#   podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build \
#       bash switch/docker/verify-toolchain.sh

set -e

GCC=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc

echo "=== [1/2] cross-compile an aarch64-none-elf object ==="
TMP=$(mktemp -d)
printf 'int switch_toolchain_ok(int x){return x*2+1;}\n' > "$TMP/t.c"
"$GCC" -march=armv8-a+crc+crypto -mtune=cortex-a57 -c "$TMP/t.c" -o "$TMP/t.o"
file "$TMP/t.o" 2>/dev/null || true
"$GCC" --version | head -1
echo "cross object OK: $TMP/t.o"
rm -rf "$TMP"

echo ""
echo "=== [2/2] build Rust std + validate the Switch sysroot ==="
bash /work/switch/rust/build-std-sysroot.sh
