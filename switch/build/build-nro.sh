#!/usr/bin/env bash
# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Link one test app against the built static NVK driver and package
# it as a runnable Switch .nro.
#
# Run inside the toolchain image with the repo bind-mounted at /work:
#   podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build \
#       bash switch/build/build-nro.sh nvk_smoke
#
# Output: switch/smoke/out/<app>.nro
set -euo pipefail

APP="${1:-${APP:-nvk_smoke}}"
TITLE="${TITLE:-NVK ${APP#nvk_}}"
VERSION="${VERSION:-0.1.0}"

SRC="${SRC:-$(pwd)}"
BUILD="${CROSS_BUILD:-$SRC/switch/build/cross}"
SMOKE="$SRC/switch/smoke"
APP_SOURCE="${APP_SOURCE:-$SMOKE/$APP.c}"
OUTPUT_NAME="${OUTPUT_NAME:-$APP}"
OUT="${OUTPUT_DIR:-$SMOKE/out}"
OBJ="$OUT/obj"
mkdir -p "$OBJ"

DKP=/opt/devkitpro
GCC=$DKP/devkitA64/bin/aarch64-none-elf-gcc
GXX=$DKP/devkitA64/bin/aarch64-none-elf-g++
STRIP=$DKP/devkitA64/bin/aarch64-none-elf-strip

[ -f "$BUILD/src/nouveau/vulkan/libnvk.a" ] || {
   echo "ERROR: $BUILD/src/nouveau/vulkan/libnvk.a not found" >&2
   exit 1
}
[ -f "$APP_SOURCE" ] || { echo "ERROR: $APP_SOURCE not found" >&2; exit 1; }

ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
# Per-function/data sections so the final link can drop any dead code.
SECTIONS="-ffunction-sections -fdata-sections"
# Mesa's vendored Vulkan headers live in include/.
INC="-I$SRC/include -I$DKP/libnx/include"
DEFS="-D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -DVK_USE_PLATFORM_VI_NN"
EXTRA_DEFS="${EXTRA_DEFS:-}"

# Gallium/Zink client headers for gl_* apps.
GL_INC=""
GL_DEFS=""
case "$APP" in
gl_*|gles*)
  GL_DEFS="-DHAVE_PTHREAD"
  GL_INC="-I$SRC/src/gallium/include -I$SRC/src/gallium/auxiliary
          -I$SRC/src/gallium/drivers/zink
          -I$SRC/src -I$BUILD/src
          -I$SRC/src/util -I$SRC/src/util/format
          -I$BUILD/src/util -I$BUILD/src/util/format
          -I$SRC/src/mesa -I$SRC/src/mesa/main
          -I$BUILD/src/mesa -I$BUILD/src/mesa/libmesa.a.p
          -I$BUILD/src/mesa/program -I$BUILD/src/mesa/glapi/glapi/gen
          -I$SRC/src/compiler -I$BUILD/src/compiler
          -I$SRC/src/compiler/nir -I$BUILD/src/compiler/nir
          -I$BUILD/src/compiler/glsl -I$BUILD/src/compiler/spirv
          -I$SRC/include -I$BUILD/include"
  ;;
esac

echo "=== compiling $APP.c ==="
$GCC -c "$APP_SOURCE"       -o "$OBJ/$APP.o"        $ARCH $SECTIONS $DEFS $GL_DEFS $EXTRA_DEFS $INC $GL_INC -O2 -Wall -Wno-unused-function
$GCC -c "$SMOKE/nvk_compat.c" -o "$OBJ/nvk_compat.o" $ARCH $SECTIONS $DEFS $INC -O2 -Wall

# Support archives libnvk.a pulls in, in dependency order.
cd "$BUILD"
ARCHIVES="
  src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a
  src/c11/impl/libmesa_util_c11.a
  src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a
  src/compiler/rust/libcompiler_c_helpers.a
  src/nouveau/headers/libnvidia_headers_c.a
  src/nouveau/nil/libnil.a src/nouveau/nil/liblibnil_format_table.a
  src/compiler/nir/libnir.a src/compiler/libcompiler.a
  src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a
  src/vulkan/util/libvulkan_util.a src/compiler/spirv/libvtn.a
  src/util/libxmlconfig.a"
PORTLIBS="$DKP/portlibs/switch/lib/libz.a $DKP/portlibs/switch/lib/libexpat.a"

# Zink/Gallium/GL frontend statics added only for gl_* apps so Vulkan .nros stay lean.
GL_ARCHIVES="
  src/egl/libEGL.a
  src/mesa/libmesa.a
  src/compiler/glsl/libglsl.a src/compiler/glsl/glcpp/libglcpp.a
  src/mesa/glapi/shared-glapi/libglapi.a src/mesa/glapi/glapi/libglapi_bridge.a
  src/gallium/auxiliary/libgallium.a src/gallium/drivers/zink/libzink.a
  src/gallium/winsys/zink/drm/libzinkwinsys.a
  src/gallium/winsys/sw/null/libws_null.a src/gallium/winsys/sw/wrapper/libwsw.a"
GL_WHOLE=""
case "$APP" in
gl_*|gles*)
  [ -f "$BUILD/src/gallium/drivers/zink/libzink.a" ] || {
     echo "ERROR: $BUILD/src/gallium/drivers/zink/libzink.a not found — build the" >&2
     echo "       Zink stack first (CROSS_BUILD=$SRC/switch/build/cross-zink)." >&2
     exit 1
  }
  GL_WHOLE="$GL_ARCHIVES"
  ;;
esac

echo "=== linking ELF ==="
# libnvk.a (and the GL set for gl_* apps) must be whole-archived
$GXX -specs="$DKP/libnx/switch.specs" $ARCH \
  -L$DKP/portlibs/switch/lib -L$DKP/libnx/lib \
  -o "$OBJ/$APP.elf" \
  -Wl,--gc-sections \
  -Wl,-u,vk_icdGetInstanceProcAddr \
  "$OBJ/$APP.o" "$OBJ/nvk_compat.o" \
  -Wl,--whole-archive $GL_WHOLE src/nouveau/vulkan/libnvk.a -Wl,--no-whole-archive \
  -Wl,--start-group \
    $ARCHIVES $PORTLIBS \
    -lnx -lc -lm -lstdc++ -pthread \
  -Wl,--end-group

echo "=== packaging NRO ==="
"$STRIP" "$OBJ/$APP.elf" -o "$OBJ/$APP.stripped.elf"
"$DKP/tools/bin/nacptool" --create "$TITLE" "nxvk" "$VERSION" "$OBJ/$APP.nacp"
"$DKP/tools/bin/elf2nro" "$OBJ/$APP.stripped.elf" "$OUT/$OUTPUT_NAME.nro" \
  --icon="$DKP/libnx/default_icon.jpg" --nacp="$OBJ/$APP.nacp"

echo "=== DONE ==="
echo "Output is located at $OUT/$OUTPUT_NAME.nro"
ls -la "$OUT/$OUTPUT_NAME.nro"
