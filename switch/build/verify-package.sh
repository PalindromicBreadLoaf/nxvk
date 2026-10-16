#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compile and link a real Vulkan consumer using ONLY the distributed package
# (plus devkitPro/libnx/portlibs), not the individual Mesa build-tree archives.
# Run inside the toolchain image. This does not execute the NRO on hardware.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCHIVE="${1:-$SRC/switch/build/nxvk-vulkan-tegra-x1.tar.gz}"
OUT="${OUTPUT_DIR:-$SRC/switch/build/package-check}"
DKP="${DEVKITPRO:-/opt/devkitpro}"
TOOLS="$DKP/devkitA64/bin/aarch64-none-elf"

# Resolve relative inputs before changing directories to the isolated client.
ARCHIVE="$(realpath "$ARCHIVE")"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
(
  cd "$(dirname "$ARCHIVE")"
  sha256sum --check "$(basename "$ARCHIVE").sha256"
)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
PREFIX="$TMP/portlib"
CLIENT="$TMP/client"
mkdir -p "$PREFIX" "$CLIENT"
tar -xzf "$ARCHIVE" --no-same-owner -C "$PREFIX"
for file in lib/libnvk.a lib/libnvk_support.a lib/pkgconfig/nxvk.pc \
            include/vulkan/vulkan.h include/vulkan/vulkan_vi.h; do
  test -s "$PREFIX/$file" || { echo "ERROR: package is missing $file" >&2; exit 1; }
done
test ! -e "$PREFIX/lib/libnvk_gl.a"

# Deliberately use an isolated pkg-config search path. The .pc file must resolve
# the extracted headers/libraries, never a host Mesa or an installed old NXVK.
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=
cflags="$(pkg-config --define-variable=prefix="$PREFIX" --cflags nxvk)"
libs="$(pkg-config --define-variable=prefix="$PREFIX" --libs nxvk)"
version="$(pkg-config --modversion nxvk)"
read -r -a PC_CFLAGS <<< "$cflags"
read -r -a PC_LIBS <<< "$libs"
printf 'Package version: %s\n' "$version"
printf 'Package Cflags: %s\nPackage Libs: %s\n' "${PC_CFLAGS[*]}" "${PC_LIBS[*]}"

# Copy just the client sources; no source-tree Vulkan headers or Mesa archives
# appear on the compile/link command lines. The compatibility/chain sources are
# application glue, not replacements for anything in the packaged driver.
for file in nvk_smoke.c nvk_harness.h nvk_compat.c nvk_chain.c nvk_run.h; do
  cp "$SRC/switch/smoke/$file" "$CLIENT/"
done
cd "$CLIENT"
ARCH=(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE)
for unit in nvk_smoke nvk_compat nvk_chain; do
  "$TOOLS-gcc" -c "$unit.c" -o "$unit.o" \
    "${ARCH[@]}" -O3 -DNDEBUG -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -I"$DKP/libnx/include" "${PC_CFLAGS[@]}" -Wall -Wno-unused-function
done

ELF="$OUT/nvk_smoke.elf"
"$TOOLS-g++" -specs="$DKP/libnx/switch.specs" "${ARCH[@]}" \
  -o "$ELF" nvk_smoke.o nvk_compat.o nvk_chain.o \
  "${PC_LIBS[@]}" -L"$DKP/portlibs/switch/lib" -L"$DKP/libnx/lib" \
  -lnx -lc -lm -lstdc++ -pthread -Wl,-Map="$OUT/nvk_smoke.map"

# Assert the final binary is AArch64 and that both merged archives really were
# used. Write nm output first: grep -q on a large pipe can otherwise cause SIGPIPE.
"$TOOLS-readelf" -h "$ELF" > "$OUT/elf-header.txt"
grep -Eq 'Machine:.*AArch64' "$OUT/elf-header.txt"
"$TOOLS-nm" --defined-only "$ELF" > "$OUT/symbols.txt"
grep -Eq ' [Tt] vk_icdGetInstanceProcAddr$' "$OUT/symbols.txt"
grep -F "$PREFIX/lib/libnvk.a(" "$OUT/nvk_smoke.map" > /dev/null
grep -F "$PREFIX/lib/libnvk_support.a(" "$OUT/nvk_smoke.map" > /dev/null
if grep -F 'switch/build/cross/' "$OUT/nvk_smoke.map"; then
  echo 'ERROR: package link unexpectedly used build-tree archives' >&2
  exit 1
fi

"$TOOLS-strip" "$ELF" -o "$OUT/nvk_smoke.stripped.elf"
"$DKP/tools/bin/nacptool" --create "NVK package smoke" "nxvk" "$(cat "$SRC/VERSION")" "$OUT/nvk_smoke.nacp"
"$DKP/tools/bin/elf2nro" "$OUT/nvk_smoke.stripped.elf" "$OUT/nvk_smoke.nro" \
  --icon="$DKP/libnx/default_icon.jpg" --nacp="$OUT/nvk_smoke.nacp"
test -s "$OUT/nvk_smoke.nro"
echo 'PASS: extracted Vulkan-only package compiles and links an AArch64 NRO via nxvk.pc'
echo 'Hardware execution and performance have NOT been tested.'
