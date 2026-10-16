#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run inside the devkitPro image after the Vulkan archives have been built.
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
apps=(nvk_subtile_diag nvk_subtile_nocache nvk_diag_runner)
if [[ "${1:-}" == --list ]]; then
  printf '%s\n' "${apps[@]}"
  exit 0
elif [[ $# != 0 ]]; then
  echo "Usage: $0 [--list]" >&2
  exit 2
fi
BUILD="${CROSS_BUILD:-$SRC/switch/build/cross}"
OUT="$SRC/switch/smoke/out/diagnostics"
mkdir -p "$SRC/switch/build/diagnostics"
STAGING="$(mktemp -d "$SRC/switch/build/diagnostics/stage.XXXXXX")"
trap 'rm -rf "$STAGING"' EXIT
STAGE="$STAGING/switch/nxvk-diagnostics"
TOOLS=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
mkdir -p "$OUT" "$STAGE"
test -s "$BUILD/src/nouveau/vulkan/libnvk.a"
export VERSION="$(cat "$SRC/VERSION")"
# build-nro.sh writes the provenance, because it is what knows which archives
# the .nro links. It also drops the text here for the packaged bundle, and the
# header stays where the verification below and the probes expect it.
export SRC OUTPUT_DIR="$OUT" BUILDINFO_OUT="$OUT/BUILDINFO.txt" \
  BUILD_ID_HEADER="$OUT/nvk_build_id.h"
rm -f "$OUT/BUILDINFO.txt" "$OUT/nvk_build_id.h"
bash "$SRC/switch/smoke/shaders/gen-shaders.sh"
for app in nvk_subtile_diag nvk_subtile_nocache; do
  defs=-DNVK_DIAGNOSTIC
  [[ "$app" != nvk_subtile_nocache ]] || defs+=' -DNVK_DIAG_NO_CACHE'
  APP_SOURCE="$SRC/switch/diagnostics/nvk_subtile_diag.c" OUTPUT_NAME="$app" \
    EXTRA_DEFS="$defs" bash "$SRC/switch/build/build-nro.sh" "$app"
  "$TOOLS-readelf" -h "$OUT/obj/$app.elf" > "$OUT/$app.elf-header.txt"
  grep -Eq 'Machine:.*AArch64' "$OUT/$app.elf-header.txt"
  "$TOOLS-nm" --defined-only "$OUT/obj/$app.elf" > "$OUT/$app.symbols.txt"
  grep -Eq ' [Tt] vk_icdGetInstanceProcAddr$' "$OUT/$app.symbols.txt"
done
TITLE="NXVK diagnostic runner" APP_SOURCE="$SRC/switch/smoke/nvk_runner.c" OUTPUT_NAME=nvk_diag_runner \
  EXTRA_DEFS=-DNVK_DIAGNOSTIC bash "$SRC/switch/build/build-nro.sh" nvk_runner

# Verify that provenance survived compilation/linking in every application.
python3 - "$OUT" <<'PY'
from pathlib import Path
import re, sys
out = Path(sys.argv[1])
build_id = re.search(r'#define NVK_BUILD_ID "([0-9a-f]{64})"', (out/'nvk_build_id.h').read_text()).group(1)
for obj in ('nvk_subtile_diag', 'nvk_subtile_nocache', 'nvk_runner'):
    data = (out/'obj'/f'{obj}.elf').read_bytes()
    assert build_id.encode() in data, obj
    assert b'sdmc:/nxvk_diag/state' in data, obj
    assert b'sdmc:/nxvk_run/state' not in data, obj
print('PASS: all diagnostic ELFs contain the same embedded build ID:', build_id)
PY
for app in "${apps[@]}"; do
  test -s "$OUT/$app.nro"
  cp "$OUT/$app.nro" "$STAGE/"
done
cp "$OUT/BUILDINFO.txt" "$STAGE/"
cp "$SRC/switch/diagnostics/README.md" "$STAGE/README.md"
cp "$SRC/licenses/GPL-2.0-or-later" "$STAGE/LICENSE"
(cd "$STAGE" && sha256sum "${apps[@]/%/.nro}" > SHA256SUMS && sha256sum -c SHA256SUMS)
tar -czf "$SRC/switch/build/nxvk-vulkan-diagnostics.tar.gz" \
  -C "$STAGING" switch
(cd "$SRC/switch/build" && sha256sum nxvk-vulkan-diagnostics.tar.gz > nxvk-vulkan-diagnostics.tar.gz.sha256)
echo 'PASS: packaged two Vulkan diagnostic probes and an isolated runner (not executed on hardware)'
