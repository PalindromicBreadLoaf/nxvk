# NXVK Vulkan diagnostic benchmark for Nintendo Switch

This is a **focused measurement package**, separate from the 22-app validation
suite. It investigates the large first/default-row timing difference seen in
uploaded `nvk_subtile` logs. It does not change production GPU tuning, remove
synchronization, set clocks/voltages or claim a performance improvement.

## Install and run

1. Download `nxvk-vulkan-diagnostics-<run number>` from the successful CI run.
   Verify `sha256sum -c nxvk-vulkan-diagnostics.tar.gz.sha256` alongside the tarball.
2. Extract the tarball at the SD root. Keep these files together:
   ```text
   switch/nxvk-diagnostics/
     nvk_diag_runner.nro
     nvk_subtile_diag.nro
     nvk_subtile_nocache.nro
     BUILDINFO.txt
     SHA256SUMS
     README.md
     LICENSE
   ```
   `SHA256SUMS` verifies the three NROs on the PC before copying to the SD card.
3. Back up an earlier `sdmc:/nxvk_diag/` result before starting a new run. To
   abandon an old/incompatible run, remove **only** `sdmc:/nxvk_diag/state`.
   Leave it in place to resume an interrupted run of the **same build**.
4. Launch **`nvk_diag_runner.nro`** in full-memory/title-override mode, not Album.
   The loader must support chainloading. The runner starts both probes and
   collects their verdicts. It refuses to resume a state from another build.
5. Wait for completion. Warmup alone takes at least **40 seconds** for a complete
   run; pipeline creation, samples and SD I/O take additional time. Progress is
   displayed between phases. A successful complete run reports **2 PASS**.

The old `sdmc:/nxvk_run/` results and old 22-app runner are not used or deleted.
The diagnostic app/chain shim use their own namespace, `sdmc:/nxvk_diag/`.

## Upload these results

Upload the `.log`, `.csv` and `.txt` files directly under **`sdmc:/nxvk_diag/`**:

- `summary.log`, `chain.log`;
- `nvk_subtile_diag.log`, `.csv`, `.mesa.log`, and `.cache.log` if created;
- `nvk_subtile_nocache.log`, `.csv`, `.mesa.log`, and `.cache.log` if created;
- **`BUILDINFO.txt` and `BUILD-ID.txt`** (written from data embedded in the NROs);
- `SHA256SUMS` (reference copy from the distribution, when present beside the runner);
- `build-mismatch.log`, if present after attempting to mix binaries.

Do **not** upload the `cache/` subdirectory. It is an isolated diagnostic shader
cache, limited to 16 MiB. The probes do not delete it or use a game's cache.
Back up results before rerunning: filenames are reused. On a resumed run, a
failed/missing verdict stays failed/missing; it is not converted into a pass.

Build ID hashes embedded source/toolchain metadata and the linked NVK archive's
hash. Each app logs it, writes its embedded build information and checks it
against a resumed run. The runner's reference manifest is **not** a runtime
checksum verification of arbitrary files copied to the SD card.

## What is measured

Two independent processes run the same workloads:

- `nvk_subtile_diag`: shader cache **requested enabled**, in the isolated directory;
- `nvk_subtile_nocache`: `MESA_SHADER_CACHE_DISABLE=1`, set before instance creation.

A cache request is not proof of cache hits or successful SD writes. The optional
`.cache.log` captures Mesa cache statistics/warnings where available. Cache state
persists across runs; the initial probe is **not** a claim of an empty shader cache.

Each process runs ten phases:

```text
default, T210, generic / T210, generic, default / generic, default, T210 / default
```

The three variants occupy each position once; the final default checks drift.
Each phase uses a fresh instance/device and the legacy probe's shaders, render
target and dispatch sizes. It records a pre-warmup pair, performs at least two
seconds **and** 16 alternating fragment/compute warmup pairs, then measures 15
pairs with alternating workload order. Command buffers and upload staging
resources are released after completed work so long warmups do not accumulate
old allocations. No logging/SD writes occur between warmup and the timed block
or between measured pairs.

Measurements include:

- separate GPU timestamps and CPU wall times, all raw samples in CSV;
- GPU median/min/max, plus repeated readback hashes across every variant;
- read-only CPU/GPU/EMC rate snapshots before/after each pair, with validity bits
  and service result codes; mode/firmware/process-memory information;
- Vulkan driver information and pipeline-cache UUID;
- opt-in **`NVK_DEBUG=subtiling`** messages containing `sm`, requested override
  and the **actual emitted** subtiling word, including cache-loaded shaders.
  A requested word of zero means no override, not an emitted zero value.

Clock-service access may be denied. That is logged as unavailable, not as a
measured zero-Hz clock and not as a correctness failure. Rates are service
snapshots, not proof that clocks remained constant inside a workload. No clock
or power setting is changed by the probe.

GPU timestamp wrap respects `timestampValidBits`. If timestamps are unavailable,
GPU CSV fields are `nan` and the phase is marked `TIMING INCONCLUSIVE`; CPU wall
times are retained separately. GPU intervals exclude diagnostic readback copies;
wall time includes them on the last pair (`readback=1`). Other CSV fields label
cache request, phase, cold/warm sample, order and clock validity/results.

**PASS means all phases completed with matching readbacks.** It does not mean
that every timing is valid, clocks are stable, a variant is faster or a game will
have higher FPS. Compare the emitted settings, warm samples, compute controls,
clock snapshots and repeated defaults before attributing a difference to tuning.

## Recorded hardware result

The [2026-10-16 result](https://github.com/berlint-hub/nxvk/blob/arena/068e6f2f-nxvk/switch/docs/diagnostic-results-2026-10-16.md) identifies build
#5 and records both probes passing. Default and explicit T210 agree after warmup;
the slow initial pair coincides with a lower reported CPU/GPU frequency profile.
This is a result for that uploaded run, not an automatic verdict for future runs.

## Rebuild and validation

```sh
make CONTAINER=docker all
make CONTAINER=docker vulkan-diagnostics
```

Inside the prepared image, `bash switch/diagnostics/build.sh` builds/packages the
probes. `--list` needs no SDK. Output: `switch/build/nxvk-vulkan-diagnostics.tar.gz`
and its adjacent checksum. NRO intermediates are under `switch/smoke/out/diagnostics/`.

Native regression tests (`bash switch/tests/run-host-tests.sh`) cover schedule
balance, warmup gates, timestamp wrap, build identity/mismatch handling and mocked
read-only clock-service errors. Syntax checks use real Vulkan headers with SDK
stand-ins; the CI cross-build separately checks actual libnx ABI/linking. CI
also rebuilds the original 22 Vulkan applications and isolated package consumer.
Neither native tests nor cross-compilation execute this benchmark on a console.
