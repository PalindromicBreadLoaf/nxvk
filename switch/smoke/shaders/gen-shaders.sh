#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: MIT
#
# Compile the GLSL sources in this directory to SPIR-V and emit each as a C header
# Run inside the toolchain image (carries glslangValidator):
#   podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build \
#       bash switch/smoke/shaders/gen-shaders.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
GLSLANG="${GLSLANG:-glslangValidator}"

gen() {
   local src="$1" stage="$2" name="$3"
   echo "  $src -> $name.h ($name""_spv)"
   "$GLSLANG" -V -S "$stage" --vn "${name}_spv" -o "$DIR/$name.h" "$DIR/$src"
   sed -i '1{/^[[:space:]]*\/\/ [0-9][0-9.]*$/d;}' "$DIR/$name.h"
}

gen tri.vert    vert tri_vert
gen solid.frag  frag solid_frag
gen mvp.vert    vert mvp_vert
gen vcolor.frag frag vcolor_frag
gen tex.vert    vert tex_vert
gen tex.frag    frag tex_frag
gen cube.vert   vert cube_vert
gen cube.frag   frag cube_frag

echo "=== shaders generated ==="
