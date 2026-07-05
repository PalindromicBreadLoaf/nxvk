#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: MIT
#
# Cross-configure Mesa for Horizon (aarch64/GM20B) building only NVK and NAK.
#
# Run inside the toolchain image with the repo bind-mounted at /work:
#   podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build \
#       bash switch/build/configure-mesa.sh
set -euo pipefail

SRC="${SRC:-$(pwd)}"
BUILD="${CROSS_BUILD:-$SRC/switch/build/cross}"
NATIVE_PREFIX="${NATIVE_PREFIX:-$SRC/switch/build/native-tools}"

# find_program('mesa_clc'/'vtn_bindgen', native:true) resolves off PATH.
export PATH="$NATIVE_PREFIX/bin:$PATH"

meson setup "$BUILD" "$SRC" \
  --cross-file "$SRC/switch/crossfiles/switch.cross" \
  --cross-file "$SRC/switch/crossfiles/rust.cross" \
  --native-file "$SRC/switch/crossfiles/native.txt" \
  --buildtype release \
  -Dmesa-clc=system \
  -Dprecomp-compiler=system \
  -Dllvm=disabled \
  -Dvulkan-drivers=nouveau \
  -Dgallium-drivers= \
  -Dplatforms= \
  -Dglx=disabled \
  -Degl=disabled \
  -Dgbm=disabled \
  -Dopengl=false \
  -Dgles1=disabled \
  -Dgles2=disabled \
  -Dvideo-codecs= \
  -Dvulkan-layers= \
  -Dvulkan-beta=false \
  -Dshader-cache=disabled \
  -Dzstd=disabled \
  -Dlibunwind=disabled \
  -Dlmsensors=disabled \
  -Dvalgrind=disabled \
  -Dperfetto=false \
  -Dandroid-libbacktrace=disabled \
  -Dbuild-tests=false

echo "=== configured; building libnvk.a ==="
