#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: MIT
#
# Build a prebuilt Rust std SYSROOT for the aarch64-switch-horizon target so that meson finds std.
#
# Run inside the toolchain image with the repo bind-mounted at /work:
#   podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build \
#       bash switch/rust/build-std-sysroot.sh
set -e

export RUST_TARGET_PATH=/work/switch/rust RUSTC_BOOTSTRAP=1
export RUSTFLAGS='-Zunstable-options -L /opt/devkitpro/devkitA64/aarch64-none-elf/lib -L /opt/devkitpro/libnx/lib'
TGT=aarch64-switch-horizon
SYSROOT=/work/switch/rust/sysroot
RUSTC_SYSROOT="$(rustc --print sysroot)"

# build-std (release) on a throwaway lib crate
rm -rf /tmp/stdsr && mkdir -p /tmp/stdsr/src && cd /tmp/stdsr
printf '#![no_main]\n' > src/lib.rs
printf '[package]\nname="stdsr"\nversion="0.0.0"\nedition="2021"\n[lib]\ncrate-type=["rlib"]\n[profile.release]\npanic="abort"\n' > Cargo.toml
cargo +nightly build --release -Zbuild-std=core,alloc,std,panic_abort --target "$TGT"

# Assemble the sysroot
DEPS=/tmp/stdsr/target/$TGT/release/deps
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/lib/rustlib/$TGT/lib"
cp -f "$DEPS"/*.rlib  "$SYSROOT/lib/rustlib/$TGT/lib/" 2>/dev/null || true
cp -f "$DEPS"/*.rmeta "$SYSROOT/lib/rustlib/$TGT/lib/" 2>/dev/null || true
for d in "$RUSTC_SYSROOT/lib/rustlib"/*; do
  b=$(basename "$d")
  [ "$b" = "$TGT" ] && continue
  [ -e "$SYSROOT/lib/rustlib/$b" ] || ln -s "$d" "$SYSROOT/lib/rustlib/$b" 2>/dev/null || true
done
ln -sf "$RUSTC_SYSROOT/lib/rustlib/src" "$SYSROOT/lib/rustlib/src" 2>/dev/null || true
echo "=== sysroot std rlibs ==="; ls "$SYSROOT/lib/rustlib/$TGT/lib/" | grep -E 'libstd|libcore|liballoc' | head

# 3. Validate plain rustc with --sysroot resolves std from the assembled sysroot
cd /tmp && printf 'pub fn n() -> usize { let v = vec![1, 2, 3]; v.len() }\n' > t.rs
echo "=== plain rustc --sysroot test (no build-std) ==="
if rustc -Zunstable-options --target "$TGT" --sysroot "$SYSROOT" \
        --crate-type=rlib -C panic=abort t.rs -o /tmp/t.rlib; then
  echo "SYSROOT TEST: OK"
else
  echo "SYSROOT TEST: FAIL"; exit 1
fi
