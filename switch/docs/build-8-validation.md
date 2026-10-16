# Build #8 — full Switch Vulkan suite passed (shader-store change included)

Reviewed user-uploaded snapshot:
[`nxvk_run` at `c6f4bb814f274f97edbc9d7cc2b051eb0423c5db`](https://github.com/berlint-hub/nxvk/tree/c6f4bb814f274f97edbc9d7cc2b051eb0423c5db/nxvk_run),
uploaded 2026-10-16 at 18:00:32 UTC.

**22 PASS, 0 FAIL, 0 GONE, 0 SKIP.** This is the complete Vulkan ladder, not the
focused subtile diagnostics.

## Build attribution and consistency checks

- Embedded metadata identifies source `12d6eeaf94323db1513df3c3dc7f2ad0137ef7ac`
  from `refs/heads/arena/92b2b9eb-nxvk`, clean tree, from
  [CI run #8](https://github.com/berlint-hub/nxvk/actions/runs/38940596315).
  That run published the package, the SD bundle and the diagnostic bundle as
  `*-8` artifacts.
- Build ID: `f89b0333acf460beb9b4d9cd6f0981cfbced1cbe752b6e989c4b1fa365f6852a`.
  SHA-256 of the uploaded `BUILDINFO.txt` equals that ID, and the ID embedded in
  `summary.log` and `BUILD-ID.txt` agree.
- All 49 uploaded files match their Git blob hashes. They are 22 app logs, 22
  Mesa logs, `chain.log`, `summary.log`, the three metadata files.
- `chain.log` contains 22 armed entries, and the summary lists exactly the 22
  distinct Vulkan apps. Every app log has a passing verdict.
- The reference `SHA256SUMS` lists the expected 23 NRO names (22 apps plus
  runner) and well-formed hashes. NRO binaries were not uploaded, so their
  on-console bytes have not been independently rehashed here.
- The 22 Mesa log files are **empty** (see "What this run does not prove").
- Runtime driver: `NVIDIA Tegra X1 (NVK gm20b)`, Vulkan `1.4.354`, vendor
  `0x10de`.

## The change under test is the shader store, not the ladder

The only driver-side source change between build #7 (`46972d4`) and this build is
`src/util/disk_cache_horizon.c`: the store header is published once per
`STORE_COMMIT_INTERVAL_B` of appended data instead of once per blob, with the
`MESA_SHADER_CACHE_COMMIT=per-blob` control.

The [`nvk_subtile` readbacks](https://github.com/berlint-hub/nxvk/blob/c6f4bb814f274f97edbc9d7cc2b051eb0423c5db/nxvk_run/nvk_subtile.log)
are unchanged from the
[documented build #5 diagnostic](diagnostic-results-2026-10-16.md): colour
`0xb1884637f4710a21`, compute `0xb81d92b31a9f5e31`, `9 phases`, every readback
identical. The default row's fragment rate is `3179.6` Miter/s against `15399.4`
for `t210`, a `4.84x` ratio: the same cold-start frequency-profile transition
build #5 measured (`4.875x` GPU-rate ratio), not a subtiling change. The warmed
rows agree to within `0.06%`, as they did before.

So: no functional regression and no driver-visible behaviour change was observed
in these scenarios. This says nothing about whether the store change helps — see
below.

## What this run does not prove

Two gaps in the evidence, both about provenance and instrumentation rather than
about the ladder itself.

### 1. The recorded archive hash did not cover the changed code

The uploaded metadata records
`Linked libnvk.a SHA256: 9f41ae6c2ab821ccd4b5a237cceeab3d31ef3e596eea1782a74be699aa3fe645`,
**byte-identical to build #7**. That is correct and expected: the
`src/nouveau/vulkan` tree is unchanged between the two sources (both are
`dbd5d77f55b87ce96fe928862fc0aa4049963ee0`), and the build only hashed the
driver archive.

The consequence is the problem. Everything this build changed lives in
`src/util/libmesa_util.a`, which is bundled into `libnvk_support.a` and linked
from there; the support archives — shader cache, NAK, NIR, NIL — were not
recorded at all. An upload could therefore not distinguish two builds whose only
difference is support code, and could not be checked against a rebuild of the
archive that actually changed.

Fixed in this branch: the embedded record now carries one hash per linked
archive (driver plus the 16 support archives a Vulkan `.nro` links, plus the GL
set for `gl_*` apps), and CI fails the run if any is missing, if the count
changes, or if a recorded hash does not match the archive on disk.

### 2. No driver log was captured, so there is no measurement of the change

All 22 `.mesa.log` files are empty. The ladder captured only the applications'
own output: Mesa writes its log to stderr by default, which the runner never
harvested, and `MESA_SHADER_CACHE_SHOW_STATS=1` — which the harness already sets
— therefore had nowhere to go.

The consequence: this run contains the shader-cache `hits/misses` line, the
store's open summary and the new close summary (`blobs offered`, `appended`,
`duplicates`, `header publications`) only in theory. The write-cycle reduction
the change is for is **not** measured by this run, and neither is the
`MESA_SHADER_CACHE_COMMIT=per-blob` A/B.

Fixed in this branch: the ladder hands `MESA_LOG_FILE` to Mesa so driver log
output lands in the file the runner harvests as `<app>.mesa.log`, and touches
that file so it is always present even when the driver logs nothing.

## Recorded hardware validation

This snapshot records **22 PASS, 0 FAIL, 0 GONE, 0 SKIP** for the full Vulkan
suite on the user's Switch with the shader-store change linked in, with matching
embedded build metadata. That is a correctness statement about these 22
scenarios, not an FPS, SD-throughput or level-load measurement.

## Next run

Build with the provenance and log-capture fixes above, then:

1. Keep the full 22-app ladder as the regression gate — it is cheap and it is
   the only complete correctness evidence the project has.
2. For the store change, run an app that creates many pipelines twice on each
   policy (cold and warm `sdmc:/switch/.mesa_shader_cache`), once with
   `MESA_SHADER_CACHE_COMMIT` unset and once with `per-blob`, and compare the
   close summary and the startup wall time. The harness has no way to set that
   variable today; run it as a standalone app with the variable set, or add a
   marker-file option if the A/B is worth making routine.
3. Back up `sdmc:/switch/.mesa_shader_cache/` state per run: the store is
   append-only and shared across apps, so an A/B is only meaningful from
   comparable starting caches.
