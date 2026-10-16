#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Native fault-injection and build-dependency tests. No GPU/devkit is required.
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Compile the production presentation function unchanged against minimal mocks.
# This intentionally covers control flow, not libnx ABI or GPU timing; the full
# cross-build separately checks the real SDK/WSI types and link dependencies.
python3 - "$SRC" "$TMP" <<'PY'
from pathlib import Path
import sys
src, out = map(Path, sys.argv[1:])
code = (src / 'src/vulkan/wsi/wsi_switch.c').read_text()
start = code.index('static VkResult\nwsi_switch_swapchain_queue_present(')
end = code.index('\n}\n', start) + 3
(out / 'wsi_switch_queue_present.inc').write_text(code[start:end])
PY

for test in thread_priority wsi_present; do
  "${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$SRC/switch/tests/stubs" -I"$SRC/src" -I"$SRC/include" -I"$TMP" \
    "$SRC/switch/tests/${test}_test.c" -o "$TMP/$test"
  "$TMP/$test"
done
python3 "$SRC/switch/tests/test_build_dependencies.py" -v
