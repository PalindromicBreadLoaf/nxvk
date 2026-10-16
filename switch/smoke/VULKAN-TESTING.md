# Vulkan-only validation on Nintendo Switch

## What CI has checked

- NVK and its support libraries compile in release mode (`-O3`, Cortex-A57).
- OpenGL, EGL, GLES and Zink are disabled in the driver build.
- The distribution tarball's checksum is correct. An isolated client compiles
  using the extracted Vulkan headers and links through the extracted `nxvk.pc`,
  `libnvk.a` and `libnvk_support.a`, without Mesa build-tree archives.
- All 22 Vulkan validation applications and a Vulkan-only runner link as NROs.

**This is build/link validation, not a GPU test result, Vulkan conformance test
or performance benchmark.** CI cannot execute Horizon NROs on a real GM20B GPU.

## Install the tests

Download `nxvk-vulkan-tests-<run number>` from the successful GitHub Actions run.
Verify the enclosed `nxvk-vulkan-tests.tar.gz.sha256` alongside the tarball:

```sh
sha256sum -c nxvk-vulkan-tests.tar.gz.sha256
```

Extract the tarball to the SD card root. It contains:

```text
switch/nxvk-tests/
  nvk_runner.nro
  nvk_smoke.nro
  nvk_tri.nro
  ...                      22 Vulkan tests in total
  SHA256SUMS
  BUILDINFO.txt
  README.md
```

The included `nvk_smoke.nro` is the isolated package-link test, so this tests the
bundled static libraries as well as the driver. The remaining apps use the same
build's individual archives. No GL applications are included, and this runner
cannot launch GL apps even if old ones remain beside it.

## Run on the console

1. Back up logs from any previous run. If `sdmc:/nxvk_run/state` exists from a
   different build or a mixed Vulkan/GL suite, remove **that state file only**
   before starting a fresh run. Otherwise the runner resumes its old cursor.
2. Start `switch/nxvk-tests/nvk_runner.nro` from a homebrew loader with
   **full application memory/title override**, not Album/applet mode. The loader
   must support chainloading (`envHasNextLoad`).
3. Let it advance through the 22 apps. Most tests are headless and finish quickly;
   the swapchain/present tests display output and some probes take longer.
4. If a test hangs or faults, return to the loader as needed and start the runner
   again. Its saved cursor lets it harvest the interrupted app and continue.
   Do not delete the state file when deliberately resuming this same build.
5. The final screen reports counts; press `+` to leave.

The harness already sets `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1` before creating
an instance. Applications linking this release driver must also opt in: GM20B
is not a conformant NVK target and can otherwise enumerate zero devices.

## Read and share results

The primary result is `sdmc:/nxvk_run/summary.log`:

- `PASS`: the application logged a passing verdict.
- `FAIL`: a test reported a failure.
- `GONE`: no verdict; inspect the app log for an early failure, hang or fault.
- `SKIP`: missing NRO or a loader/chainloading problem; **not** a passing test.

For a complete clean run, expect **22 PASS, 0 FAIL, 0 GONE, 0 SKIP**. The first
nine entries are the milestone ladder documented in `switch/smoke/README.md`.
Preserve the per-app `*.log`, driver `*.mesa.log`, `chain.log` and `BUILDINFO.txt`
with the summary. The Mesa log is the driver's own log file: it carries the
shader-cache `hits/misses` line that `MESA_SHADER_CACHE_SHOW_STATS=1` asks for,
the shader store's open/close summaries and any driver warning or error, so an
empty file means the driver logged nothing rather than that nothing was
captured. Newly built runners also write their embedded `BUILDINFO.txt`
and `BUILD-ID.txt` directly into the result directory, and copy `SHA256SUMS`
when present beside the runner. Resuming a different/unknown build is refused;
back up the earlier results and remove only its `state` file to start afresh.
The copied checksum manifest is a reference, not on-console verification.
Logs live under `sdmc:/nxvk_run/`; standalone apps instead
write their app logs at the SD card root.

To investigate performance, also record the console/firmware, docked or handheld
mode, CPU/GPU/memory clocks, cache state and workload. Keep these identical when
comparing builds. Correctness tests and `-O3` alone do not establish a speedup.

For the focused timing investigation, use the separate
[diagnostic package](../diagnostics/README.md). Its two probes do not change the
membership of this 22-app suite and write to `sdmc:/nxvk_diag/` instead.

## Rebuild locally in the devkitPro container

```sh
make CONTAINER=docker all
docker run --rm -v "$PWD:/work:z" -w /work nxvk \
  bash switch/build/build-vulkan-tests.sh
```

Output: `switch/smoke/out/vulkan/`. The suite script supports `--list` without a
toolchain. To independently validate a downloaded library tarball and its
adjacent checksum file:

```sh
docker run --rm -v "$PWD:/work:z" -w /work nxvk \
  bash switch/build/verify-package.sh switch/build/nxvk-vulkan-tegra-x1.tar.gz
```

The latter emits `switch/build/package-check/nvk_smoke.nro` and link diagnostics.
