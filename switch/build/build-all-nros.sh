#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build every app in switch/smoke as a .nro
#
# Run inside the toolchain image with the repo bind-mounted at /work:
#   podman run --rm -v "$PWD:/work:z" -w /work nxvk \
#       bash switch/build/build-all-nros.sh
#
# Output: switch/smoke/out/*.nro
set -uo pipefail

SRC="${SRC:-$(pwd)}"
SMOKE="$SRC/switch/smoke"
VK_BUILD="${VK_BUILD:-$SRC/switch/build/cross}"
GL_BUILD="${GL_BUILD:-$SRC/switch/build/cross-zink}"

fail=0
built=0

build_one() {
   local app="$1" build="$2"
   if CROSS_BUILD="$build" bash "$SRC/switch/build/build-nro.sh" "$app" >/dev/null 2>&1; then
      echo "  ok   $app"
      built=$((built + 1))
   else
      echo "  FAIL $app"
      fail=$((fail + 1))
   fi
}

# nvk_compat.c and nvk_chain.c are linked into apps.
list() {
   local pattern="$1"
   for f in "$SMOKE"/$pattern; do
      [ -f "$f" ] || continue
      local app; app="$(basename "$f" .c)"
      case "$app" in
      nvk_compat|nvk_chain) continue ;;
      esac
      echo "$app"
   done
}

echo "##### runner"
build_one nvk_runner "$VK_BUILD"

echo "##### vulkan tree"
for app in $(list 'nvk_*.c'); do
   [ "$app" = nvk_runner ] && continue
   build_one "$app" "$VK_BUILD"
done

echo "##### zink tree"
for app in $(list 'gl_*.c') $(list 'gles*.c'); do
   build_one "$app" "$GL_BUILD"
done

echo "##### built $built apps (fail=$fail)"
exit $(( fail > 0 ? 1 : 0 ))
