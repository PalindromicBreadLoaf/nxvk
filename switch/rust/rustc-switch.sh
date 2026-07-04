#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: MIT
#
# rustc wrapper that meson invokes to compile NAK.
export RUSTC_BOOTSTRAP=1
export RUST_TARGET_PATH=/work/switch/rust

final=()
all=("$@")
n=${#all[@]}
i=0
while [ $i -lt $n ]; do
  a="${all[$i]}"
  if [ "$a" = "-C" ] && [ $((i+1)) -lt $n ] && [[ "${all[$((i+1))]}" == linker=* ]]; then
    i=$((i+2)); continue            # drop "-C" "linker=<gcc>"
  fi
  if [[ "$a" == -Clinker=* ]]; then i=$((i+1)); continue; fi   # drop joined "-Clinker=<gcc>"
  final+=("$a"); i=$((i+1))
done

exec rustc -Zunstable-options \
  --sysroot /work/switch/rust/sysroot \
  --target aarch64-switch-horizon \
  -L /opt/devkitpro/devkitA64/aarch64-none-elf/lib \
  -L /opt/devkitpro/libnx/lib \
  "${final[@]}"
