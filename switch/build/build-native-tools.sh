#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build mesa_clc + vtn_bindgen NATIVELY so the cross build can consume them via -Dmesa-clc=system.
#
# Run inside the toolchain image with the repo bind-mounted at /work:
#   podman run --rm -v "$PWD:/work:z" -w /work nxvk \
#       bash switch/build/build-native-tools.sh
set -euo pipefail

SRC="${SRC:-$(pwd)}"
BUILD="${NATIVE_BUILD:-$SRC/switch/build/native}"
PREFIX="${NATIVE_PREFIX:-$SRC/switch/build/native-tools}"

# The only thing we want out of it is the host mesa_clc + vtn_bindgen2 binaries.
RECONF=""
[ -f "$BUILD/build.ninja" ] && RECONF="--wipe"

meson setup $RECONF "$BUILD" "$SRC" \
  --native-file "$SRC/switch/crossfiles/native.txt" \
  --prefix "$PREFIX" \
  --buildtype release \
  -Dmesa-clc=enabled \
  -Dinstall-mesa-clc=true \
  -Dprecomp-compiler=auto \
  -Dllvm=enabled \
  -Dshared-llvm=enabled \
  -Dvulkan-drivers= \
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
  -Dbuild-tests=false

ninja -C "$BUILD" \
  src/compiler/clc/mesa_clc \
  src/compiler/spirv/vtn_bindgen2

install -Dm755 "$BUILD/src/compiler/clc/mesa_clc"      "$PREFIX/bin/mesa_clc.real"
install -Dm755 "$BUILD/src/compiler/spirv/vtn_bindgen2" "$PREFIX/bin/vtn_bindgen2"

cat > "$PREFIX/bin/mesa_clc" <<'WRAP'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REAL="$HERE/mesa_clc.real"

find_res_inc() {
  for c in clang clang-15 clang-16 clang-17 clang-18; do
    if command -v "$c" >/dev/null 2>&1; then
      d="$("$c" -print-resource-dir 2>/dev/null)/include"
      [ -f "$d/opencl-c-base.h" ] && { echo "$d"; return 0; }
    fi
  done
  for d in /usr/lib/llvm-*/lib/clang/*/include /usr/lib/clang/*/include; do
    [ -f "$d/opencl-c-base.h" ] && { echo "$d"; return 0; }
  done
  return 1
}

if INC="$(find_res_inc)"; then
  exec "$REAL" "$@" -I"$INC"
fi
exec "$REAL" "$@"
WRAP
chmod 755 "$PREFIX/bin/mesa_clc"

echo "=== native tools installed ==="
ls -l "$PREFIX/bin/mesa_clc" "$PREFIX/bin/mesa_clc.real" "$PREFIX/bin/vtn_bindgen2"
"$PREFIX/bin/mesa_clc" --help >/dev/null 2>&1 && echo "mesa_clc runs: OK" || echo "mesa_clc runs: (no --help, non-fatal)"
