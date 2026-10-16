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
- `test_build_dependencies.py` exercises a copy of the actual Makefile with
  fixture timestamps and `make -n`, including Rust sysroot invalidation.

The headers under `stubs/` are only on this host test's include path. Production
builds continue to use the actual libnx SDK. Compilation/linking of the full
Switch driver remains a separate mandatory CI check. Hardware validation is
covered by [the Vulkan NRO suite](../smoke/VULKAN-TESTING.md).
