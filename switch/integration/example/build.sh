#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run in a devkitPro environment. No Mesa checkout or Rust toolchain is needed.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -gt 2 ]]; then
  echo "Usage: $0 [NXVK package root] [output directory]" >&2
  exit 2
fi
PREFIX="$(realpath "${1:-$HERE/../..}")"
OUT="${2:-$HERE/out}"
DKP="${DEVKITPRO:-/opt/devkitpro}"
for file in lib/libnvk.a lib/libnvk_support.a lib/cmake/NXVK/NXVKConfig.cmake \
            include/nxvk/nxvk_static.h; do
  test -s "$PREFIX/$file" || { echo "ERROR: incomplete package: $PREFIX/$file" >&2; exit 1; }
done
cmake -S "$HERE" -B "$OUT" \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/switch-toolchain.cmake" \
  -DDEVKITPRO="$DKP" -DNXVK_DIR="$PREFIX/lib/cmake/NXVK" \
  -DCMAKE_BUILD_TYPE=Release -DNXVK_EXAMPLE_ALLOW_LIMITED_COMPAT=ON
cmake --build "$OUT" --parallel "${JOBS:-2}"
test -s "$OUT/nxvk_app_example.nro"
if [[ -f "$PREFIX/BUILDINFO.txt" ]]; then
  cp "$PREFIX/BUILDINFO.txt" "$OUT/NXVK-BUILDINFO.txt"
fi
printf 'Built: %s/nxvk_app_example.nro (not executed on the console)\n' "$OUT"
