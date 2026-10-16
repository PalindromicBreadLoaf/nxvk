#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the Vulkan validation suite and a Vulkan-only chainloading runner.
# No OpenGL/Zink configuration or libraries are used. Run in the toolchain image.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SMOKE="$SRC/switch/smoke"
OUT="${OUTPUT_DIR:-$SMOKE/out/vulkan}"
apps=()
for source in "$SMOKE"/nvk_*.c; do
  app="$(basename "$source" .c)"
  case "$app" in
    nvk_chain|nvk_compat|nvk_runner) continue ;;
  esac
  apps+=("$app")
done

if [[ "${1:-}" == --list ]]; then
  printf '%s\n' "${apps[@]}" nvk_runner
  exit 0
elif [[ $# != 0 ]]; then
  echo "Usage: $0 [--list]" >&2
  exit 2
fi

mkdir -p "$OUT"
export SRC
export OUTPUT_DIR="$(cd "$OUT" && pwd)"
bash "$SMOKE/shaders/gen-shaders.sh"
for app in "${apps[@]}"; do
  APP_SOURCE="$SMOKE/$app.c" OUTPUT_NAME="$app" \
    bash "$SRC/switch/build/build-nro.sh" "$app"
done
# The runner must not probe or launch leftover GL .nros on the user's SD card.
APP_SOURCE="$SMOKE/nvk_runner.c" OUTPUT_NAME=nvk_runner \
  EXTRA_DEFS="${EXTRA_DEFS:-} -DNVK_VULKAN_ONLY" \
  bash "$SRC/switch/build/build-nro.sh" nvk_runner

for app in "${apps[@]}" nvk_runner; do
  test -s "$OUTPUT_DIR/$app.nro"
done
printf 'PASS: linked %d Vulkan tests and a Vulkan-only runner (not executed)\n' "${#apps[@]}"
