# Native regression tests for the Switch port

```sh
bash switch/tests/run-host-tests.sh
```

Requires native C and C++ compilers with ASan/UBSan support, Python 3 and GNU Make;
no Switch SDK or GPU is needed. The script builds in a temporary directory and
removes it when finished. GitHub's Vulkan-only workflow runs these tests before
building the toolchain/driver.

- `thread_priority_test.c` includes the real Horizon priority helper and provides
  mocked libnx query/update services. It checks permitted masks, no promotion,
  redundant updates and error paths.
- `wsi_present_test.c` compiles the **unchanged function body** extracted from
  `wsi_switch.c`, using minimal WSI/libnx types and callbacks to inject failures.
  It tests the function's control flow, not the layout/ABI of the mocked types.
- `arena_dirty_test.c` exercises production arena functions with counted atomic
  operations and mocked memory synchronization: uncached/coherent fast paths,
  forced legacy/full flush, CPU-cached and GPU-uncached-only paths, alignment,
  cross-backing copies and preserved publication.
- `cache_dedup_test.c` runs the real Horizon put/get/commit/comparison bodies on a
  temporary FILE with a fixture index. It forces CRC collisions and injects I/O
  failures, verifies replacement/corruption handling and counts eliminated
  duplicate writes. No SD timing or actual GPU execution is measured.
- `extract-driver-tests.py` copies these production bodies verbatim into the
  temporary test build, rather than maintaining a second implementation.
- `diagnostics_test.c` checks phase/workload ordering, minimum warmup duration,
  timestamp wrap at 1–64 bits, embedded identity files and mocked read-only clock
  queries (including denied/partial service access).
- `test_diagnostics.py` checks metadata generation and C-string roundtrips,
  rejects mismatched build metadata, checks both runner memberships and parses
  the actual diagnostic/legacy probes against real Vulkan headers with SDK
  stand-ins. Dummy SPIR-V words in that syntax test are never linked or run.
- `test_integration.py` stages the actual distribution extras and Makefile .pc
  recipe, checks relocation (including spaces), rejects incomplete packages,
  checks the standalone source/public header syntax and ensures no runner glue
  is packaged. Host CMake/pkg-config checks skip if those tools are absent;
  actual SDK CMake linking/relocation is always a separate mandatory CI step.
- `test_build_dependencies.py` exercises a copy of the actual Makefile with
  fixture timestamps and `make -n`, including Rust sysroot invalidation.

The headers under `stubs/` and `syntax-stubs/` are only on this host test's include path. Production
builds continue to use the actual libnx SDK. Compilation/linking of the full
Switch driver remains a separate mandatory CI check. Hardware validation is
covered by [the Vulkan NRO suite](../smoke/VULKAN-TESTING.md).
