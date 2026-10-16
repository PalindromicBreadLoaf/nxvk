#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Native fault-injection and build-dependency tests. No GPU/devkit is required.
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Compile production bodies unchanged against minimal fixtures. Full SDK ABI
# and links are checked by the separate cross-build, not these host mocks.
python3 "$SRC/switch/tests/extract-driver-tests.py" "$SRC" "$TMP"

for test in thread_priority wsi_present diagnostics arena_dirty cache_dedup cache_commit; do
  "${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$SRC/switch/tests/stubs" -I"$SRC/src" -I"$SRC/include" -I"$TMP" \
    "$SRC/switch/tests/${test}_test.c" -o "$TMP/$test"
  "$TMP/$test" "$TMP"
done
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter \
  -fsanitize=address,undefined -fno-omit-frame-pointer -DNVK_TEST_LEGACY_ATOMICS \
  -I"$SRC/src" -I"$TMP" "$SRC/switch/tests/arena_dirty_test.c" -o "$TMP/arena_fallback"
"$TMP/arena_fallback"
python3 -m unittest discover -s "$SRC/switch/tests" -p 'test_*.py' -v
