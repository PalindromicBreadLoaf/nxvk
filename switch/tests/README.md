# Native regression tests for the Switch port

```sh
bash switch/tests/run-host-tests.sh
```

Requires a native C compiler with ASan/UBSan support, Python 3 and GNU Make;
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
- `cache_dedup_test.c` runs the real Horizon put/get/publish/comparison bodies on a
  temporary FILE with a fixture index, under both header-publication policies. It
  forces CRC collisions and injects I/O failures, verifies replacement/corruption
  handling, counts eliminated duplicate writes and the writes and flushes a
  batched publication removes. No SD timing or actual GPU execution is measured.
- `cache_commit_test.c` runs the real open/append/scan/index bodies over a real
  store and index file and simulates an abrupt exit between runs. It checks that
  the batched policy loses only appends since the last publication, that the
  per-blob policy loses none, and that an index claiming more than the published
  header is rejected and rescanned.
- `extract-driver-tests.py` copies these production bodies verbatim into the
  temporary test build, rather than maintaining a second implementation.
- `diagnostics_test.c` checks phase/workload ordering, minimum warmup duration,
  timestamp wrap at 1–64 bits, embedded identity files and mocked read-only clock
  queries (including denied/partial service access).
- `test_diagnostics.py` checks metadata generation and C-string roundtrips,
  rejects mismatched build metadata, checks that every linked archive is recorded
  (including one that is missing), checks both runner memberships and parses
  the actual diagnostic/legacy probes against real Vulkan headers with SDK
  stand-ins. Dummy SPIR-V words in that syntax test are never linked or run.
- `test_build_dependencies.py` exercises a copy of the actual Makefile with
  fixture timestamps and `make -n`, including Rust sysroot invalidation.

The headers under `stubs/` and `syntax-stubs/` are only on this host test's include path. Production
builds continue to use the actual libnx SDK. Compilation/linking of the full
Switch driver remains a separate mandatory CI check. Hardware validation is
covered by [the Vulkan NRO suite](../smoke/VULKAN-TESTING.md).
