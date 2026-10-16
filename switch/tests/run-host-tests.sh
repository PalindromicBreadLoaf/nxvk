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
# The ladder apps that the stub headers cover, checked for host-side syntax.
# The list is explicit: apps that include generated shader headers or gallium
# headers are only built by the cross build, and extend the stubs when needed.
# -Wno-vla-parameter and -Wno-deprecated-declarations are host libc artifacts
# (glibc declares regmatch_t[] as a VLA and deprecates mallinfo); the Switch
# toolchain has neither warning.
checked=0
for app in nvk_b2_flush nvk_ce_copy nvk_chain nvk_cmd_flush nvk_compat \
           nvk_desc_flush nvk_engine_wait nvk_host_copy nvk_mem_churn \
           nvk_runner nvk_smoke gl_linktest; do
  "${CC:-cc}" -std=gnu11 -fsyntax-only -Wall -Wextra -Werror \
    -Wno-unused-parameter -Wno-vla-parameter -Wno-deprecated-declarations \
    -I"$SRC/switch/tests/syntax-stubs" -I"$SRC/switch/tests/stubs" \
    -I"$SRC/include" -I"$SRC/src" -I"$SRC/src/util" \
    "$SRC/switch/smoke/$app.c"
  checked=$((checked + 1))
done
echo "PASS: host syntax check of $checked smoke apps"

python3 -m unittest discover -s "$SRC/switch/tests" -p 'test_*.py' -v
