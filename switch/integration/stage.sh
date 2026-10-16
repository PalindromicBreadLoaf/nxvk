#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Add integration files to the already staged Vulkan library package.
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PKG="${1:-$SRC/switch/build/pkg}"
for file in lib/libnvk.a lib/libnvk_support.a lib/pkgconfig/nxvk.pc include/vulkan/vulkan.h; do
  test -s "$PKG/$file" || { echo "ERROR: stage the Vulkan libraries/headers first: $PKG/$file" >&2; exit 1; }
done
mkdir -p "$PKG/include/nxvk" "$PKG/lib/cmake/NXVK" "$PKG/examples/switch-minimal"
cp "$SRC/switch/integration/include/nxvk/nxvk_static.h" "$PKG/include/nxvk/"
cp "$SRC/switch/integration/cmake/NXVKConfig.cmake" "$PKG/lib/cmake/NXVK/"
cp "$SRC/switch/integration/README.md" "$PKG/INTEGRATION.md"
for file in CMakeLists.txt switch-toolchain.cmake build.sh main.c README.md; do
  cp "$SRC/switch/integration/example/$file" "$PKG/examples/switch-minimal/"
done
# Explicitly example-only. The imported driver target never pulls these global
# fallbacks into a real application's runtime automatically.
cp "$SRC/switch/smoke/nvk_compat.c" "$PKG/examples/switch-minimal/example_compat.c"
echo 'PASS: staged static ICD header, relocatable CMake target and standalone client sources'
