# Review of uploaded Switch Vulkan logs — 2026-10-16

This report covers the **initial** upload. The later
[build-identified diagnostic result](diagnostic-results-2026-10-16.md) confirms
build #5's T210 selection and records the frequency-profile transition behind
its large first-sample slowdown.

## Result and provenance

**22 PASS, 0 FAIL, 0 GONE, 0 SKIP** in the user-supplied console logs.
This is a recorded hardware test result, not a result of running NROs in CI or
in the development sandbox. It is not Vulkan conformance certification.

Reviewed snapshot:
[`nxvk_run` at `bfd984f31c1c977feb1574fab655375900c2f73e`](https://github.com/berlint-hub/nxvk/tree/bfd984f31c1c977feb1574fab655375900c2f73e/nxvk_run).
The upload commit is dated **2026-10-16 12:55:24 UTC**.

Checks performed on the downloaded data:

- All 46 files match their Git blob hashes.
- The [summary](https://github.com/berlint-hub/nxvk/blob/bfd984f31c1c977feb1574fab655375900c2f73e/nxvk_run/summary.log)
  contains exactly the 22 distinct apps listed by `build-vulkan-tests.sh --list`
  (excluding the runner), with no missing or duplicate app entries.
- Every summary verdict agrees with the corresponding app log. Each app log
  contains a passing verdict and no failing verdict.
- The 22 Mesa log files are empty: no driver diagnostics were captured there.
  Empty logs alone are not evidence of correctness.
- `chain.log` contains 22 `in armed` entries. The logged NRO directory is
  `sdmc:/switch/nxvk-tests/nxvk-tests`; its nesting did not prevent this run.
- The device is reported as `NVIDIA Tegra X1 (NVK gm20b)`, vendor `0x10de`,
  advertising Vulkan `1.4.354`. Advertising an API version does not imply CTS
  conformance.

### Binary identity is not established

This log snapshot contains neither `BUILDINFO.txt` nor `SHA256SUMS`. The commit
that stores logs is **not** necessarily the commit used to build the NROs.
Do not assign this result to a source revision based on the upload branch.

The upload predates even the start of
[CI run #4](https://github.com/berlint-hub/nxvk/actions/runs/38904197451)
(12:57:21 UTC; completed at approximately 13:08 UTC). It therefore cannot
validate that run's artifacts or the changes in `1a77f04`: the Horizon worker
priority implementation, Cortex-A57 Rust sysroot default and present-wait error
propagation. Those retain their separate native-test/cross-build validation.
The earlier run #3 is a possible source of the NROs, but is not proven by these
logs. Preserve the original bundle's build information to resolve this.

## What the tests recorded

The table summarizes the per-app logs in the immutable snapshot above.

| Area | Recorded result | Interpretation / limit |
| --- | --- | --- |
| Basic rendering | Smoke, triangle, logo, scene/depth, indexed drawing, blending, textures and cubemap all pass | Readback checks for these workloads, not exhaustive rendering coverage |
| VI swapchain | 1280×720, three images, 300/300 frames presented | The tested window/swapchain path completed |
| Present modes | FIFO 60.0 frames/s; immediate 120.0 frames/s over 180 timed frames after 60 warmup frames | Minimal clear/present-loop throughput, **not game FPS or a 120 Hz display claim** |
| Descriptor table | 28 µs mean submit before and after creating 24,576 views; first dirty submit 110 µs | No growing steady-state submit cost in this test; the first dirty submit is not free |
| Command recording | Shipped mapping: full/ranged empty record 1,688/1,770 ns; forced CPU-cached mapping: 5,909/1,888 ns; all readbacks pass | Ranged flush helps the forced cached case; do not claim that speedup for the shipped uncached path |
| Engine dependency | Dependent submit 31 µs, fence wait 126,861 µs; all 1,024 tail words correct | The dependent submit did not CPU-wait for the whole preceding workload in this scenario |
| Allocation churn | 48 rounds after eight warmup rounds; 576/576 recycled mappings; allocator arena stays 14,041,088 bytes and free chunks stay 36 | No measured C-heap growth in this test, not proof that every GPU resource is leak-free |
| Copy-engine boundaries | Seven copies byte-exact, with no guard clobber | Includes whole lines, partial lines and unaligned/sub-line cases |
| B2/fence visibility | All 8,388,608 words of a 32 MiB buffer match after the fence | Large-buffer completion/visibility check passes |
| Host image copy | Seven passed, zero failed, zero skipped | Tested formats, mip levels, layers and linear layout pass |
| Compression / sector promotion | Both compression phases render and round-trip correctly; all 17 sector-test levels match in each phase | Correctness evidence for the tested cases, not a compression speedup measurement |
| ZCULL | 10/10 scenarios agree with ZCULL off/on; nine use a ZCULL plane | No pixel/depth mismatches in those scenarios |
| Push descriptors | Three rows of 64 draws and 193 compute dispatches match | Tested descriptor lifetime/update paths pass |

## Subtiling: useful result, but not a fivefold optimization

Source: [nvk_subtile.log](https://github.com/berlint-hub/nxvk/blob/bfd984f31c1c977feb1574fab655375900c2f73e/nxvk_run/nvk_subtile.log).
All nine configurations produced identical colour and compute readbacks. The
probe reports GPU timestamps and medians of seven timed submissions per row.

| Configuration | Fragment ms | Compute-control ms | Fragment / compute throughput |
| --- | ---: | ---: | ---: |
| default | 18.550 | 30.522 | 1.00599 |
| generic `0x20164010` | 4.775 | 6.270 | 0.80285 |
| T210 `0x087f6080` | 3.830 | 6.269 | 1.00090 |
| T210 MRT `0x20806080` | 3.828 | 6.269 | 1.00114 |

The last column divides the **work-normalized throughput**, not the two times:
the fragment and compute workloads perform different amounts of work.

- T210 versus generic: fragment throughput rises from 12,353.5 to 15,401.7
  million iterations/s (**24.7% higher**, or about **19.8% less elapsed time**).
  Compute-control throughput differs by only about 0.006%. This supports the
  T210 setting for this ALU-heavy fragment microbenchmark, not a universal game
  speedup.
- Comparing the first `default` row directly to T210 is misleading: compute
  throughput also rises by **4.87×**, even though the subtiling knob is applied
  to fragment shading. The `default` and T210 work-normalized ratios are close
  to one another. The data show a major common-mode timing change, not evidence
  of a fivefold gain from replacing the default knob.
- Clock/power-state ramp-up or another first-phase effect is a possible cause,
  **not proven**: no clocks/power profile were uploaded. The probe uses a fixed
  phase order and one untimed warmup submit per workload. Its control workload
  is essential to interpreting these numbers.
- The current [`nvk_fs_subtiling_perf_knob()`](../../src/nouveau/vulkan/nvk_shader.c)
  already selects `0x087f6080` for SM 5.3 unless explicitly overridden/disabled.
  There is no newly discovered generic default to replace. Differences of
  approximately 0.002 ms between T210 variants do not justify selecting a new
  global policy from this run, especially since the test uses one colour target.

## Decision and follow-up

No production GPU tuning or synchronization changes are justified by this log
review. Preserve the passing readback paths, existing T210 default and cache
maintenance rules. This review does not require rebuilding unchanged binaries.

For reproducible comparisons:

1. Supply `BUILDINFO.txt` and preferably `SHA256SUMS` from the exact bundle used
   for these NROs. Keep each build's logs separate.
2. Record firmware, application-memory mode, docked/handheld mode and actual
   CPU/GPU/memory clocks. Use a consistent power profile and warm each build's
   shader cache separately.
3. For subtiling, repeat the default after the other phases and rotate phase
   order across runs; compare compute controls as well as fragment timings.
   Repeated controls and a longer warmup are preferable to assuming the first
   phase ran at the same clocks as later phases.
4. Validate run #4 separately before attributing any hardware result or
   performance change to `1a77f04`. Measure real workload frame-time percentiles,
   cache backlog and exit time when assessing the background-priority change.
