#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build an app using only an extracted distribution + SDK, not this checkout's
# headers, helper sources, build-tree archives or shader generators.
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCHIVE="$(realpath "${1:-$SRC/switch/build/nxvk-vulkan-tegra-x1.tar.gz}")"
OUT="${OUTPUT_DIR:-$SRC/switch/build/integration-check}"
DKP="${DEVKITPRO:-/opt/devkitpro}"
TOOLS="$DKP/devkitA64/bin/aarch64-none-elf"
(cd "$(dirname "$ARCHIVE")" && sha256sum -c "$(basename "$ARCHIVE").sha256")
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Deliberate space: prove relocation/quoting in CMake rather than testing only
# the original /opt/devkitpro installation layout.
PREFIX="$TMP/relocated sdk"
BUILD="$TMP/client build"
mkdir -p "$PREFIX" "$OUT"
OUT="$(realpath "$OUT")"
tar -xzf "$ARCHIVE" --no-same-owner -C "$PREFIX"
test ! -e "$PREFIX/lib/libnvk_gl.a"
export PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
pc_prefix="$(pkg-config --variable=prefix nxvk)"
python3 - "$PREFIX" "$pc_prefix" <<'PYPC'
from pathlib import Path
import shlex, sys
expected = Path(sys.argv[1]).resolve()
raw = sys.argv[2]
candidates = [raw]
# pkgconf may shell-escape whitespace even in --variable output. Never eval it.
try:
    words = shlex.split(raw)
    if len(words) == 1:
        candidates.append(words[0])
except ValueError:
    pass
assert any(Path(p).resolve() == expected for p in candidates), (raw, expected)
print('PASS: nxvk.pc relocates without a prefix override')
PYPC
DEVKITPRO="$DKP" bash "$PREFIX/examples/switch-minimal/build.sh" "$PREFIX" "$BUILD"
ELF="$BUILD/nxvk_app_example.elf"
"$TOOLS-readelf" -h "$ELF" > "$OUT/elf-header.txt"
grep -Eq 'Machine:.*AArch64' "$OUT/elf-header.txt"
"$TOOLS-nm" --defined-only "$ELF" > "$OUT/symbols.txt"
grep -Eq ' [Tt] vk_icdGetInstanceProcAddr$' "$OUT/symbols.txt"
MAP="$BUILD/nxvk_app_example.map"
grep -F "$PREFIX/lib/libnvk.a(" "$MAP" > /dev/null
grep -F "$PREFIX/lib/libnvk_support.a(" "$MAP" > /dev/null
if grep -E 'switch/build/cross/|nvk_chain\.c|nvk_harness|libnvk_gl\.a|libEGL\.a' "$MAP"; then
  echo 'ERROR: standalone client linked an unexpected source-tree/test/GL dependency' >&2
  exit 1
fi
python3 - "$ELF" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
assert b'sdmc:/nxvk_run/state' not in data
assert b'sdmc:/nxvk_diag/state' not in data
assert b'sdmc:/nxvk_app_example.log' in data
print('PASS: standalone client has no test-runner state/chainloading integration')
PY
cp "$BUILD/nxvk_app_example.nro" "$OUT/"
cp "$MAP" "$OUT/"
cp "$PREFIX/BUILDINFO.txt" "$OUT/NXVK-BUILDINFO.txt"
(cd "$OUT" && sha256sum nxvk_app_example.nro > nxvk_app_example.nro.sha256)
echo 'PASS: relocated package builds an AArch64 standalone NRO through NXVK::Driver'
echo 'This verifies compilation/linking. It does not execute the app or integrate an emulator.'
