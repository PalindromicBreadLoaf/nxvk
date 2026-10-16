# Build #10 — the full ladder passed again, including the flush phase

Reviewed user-uploaded snapshot:
[`nxvk_run` at `a3511c81f0a594344e992102c14c2ca15daab969`](https://github.com/berlint-hub/nxvk/tree/a3511c81f0a594344e992102c14c2ca15daab969/nxvk_run),
uploaded 2026-10-16 at 19:43:05 UTC.

**22 PASS, 0 FAIL, 0 GONE, 0 SKIP**, including the `ranged_cached` phase that
failed in build #9.

## Build attribution and consistency checks

- Embedded metadata identifies source `74823f7846b5883f92a8b8667c0b208289536826`
  from `refs/heads/arena/92b2b9eb-nxvk` from
  [CI run #13](https://github.com/berlint-hub/nxvk/actions/runs/38953266689),
  which published the `*-13` artifacts.
- Build ID: `266d7c4e8ec3c65631765f3f01e0824812b36216b63f42c83f2b74bc26733de2`.
  SHA-256 of the uploaded `BUILDINFO.txt` equals that ID, and `summary.log` and
  `BUILD-ID.txt` carry the same value.
- All 49 uploaded files match their Git blob hashes; `chain.log` has 22 armed
  entries; `BUILDINFO.txt` records 18 `Linked <name> SHA256:` lines; all 22
  Mesa logs are non-empty; `SHA256SUMS` lists the expected 23 NRO names. NRO
  binaries were not uploaded, so their on-console bytes were not rehashed.
- Runtime driver: `NVIDIA Tegra X1 (NVK gm20b)`, Vulkan `1.4.354`.

## The flush phase is intermittent, not broken

| Run | Source | `nvk_cmd_flush` |
| --- | --- | --- |
| build #7 | `46972d4` | all four phases read back correctly |
| build #8 | `12d6eea` | all four phases read back correctly |
| build #9 | `72bef66` | `ranged_cached` readback BAD, all other phases and apps fine |
| build #10 | `74823f7` | all four phases read back correctly |

Four observations, one failure, and the driver code between builds #8 and #10
differs only by the CPU-budget logging (`nvk_physical_device.c`,
`u_cpu_detect.c`, `u_thread_horizon.h`; no flush, arena, command-buffer, nvkmd
or cache-ops file changed). The failing run showed no other symptom: the phase's
numbers were ordinary (`empty record` 747 ns against 735 ns here, the 6,000-fill
record 27.1 ms against 26.6 ms), and every other app passed.

What that means:

- In the configuration the port ships, internal write-only memory is CPU-uncached
  and `nvkmd_mem_sync_to_gpu()` is already a no-op, so **no CPU flush is
  involved and this flake cannot affect the shipped ladder**. The phase exists
  as the safety net for the build #7 dirty-range optimization, whose safety
  argument is that CPU-cached, non-coherent memory keeps the upstream tracking
  and flush path
  ([tegra-x1-optimization.md](tegra-x1-optimization.md)).
- A one-in-four flake in a diagnostic configuration is worth pinning down, not a
  blocker. The two hypotheses from the build #9 review still stand: either the
  range-based flush of command memory under-covers what the GPU executes and
  whether it matters depends on which bytes the memory held, or the difference
  is elsewhere in the same diagnostic mode.

## The probe can no longer pass by luck

`switch/smoke/nvk_cmd_flush.c` now primes the CPU-cached phases: before the real
recording it submits a same-shaped stream whose every fill writes `0x11111111`,
then prefills the buffer through its own CPU mapping, then records and submits
the real stream (earlier fills `0x0badf00d`, last fill `0x5ca1ab1e`). If a flush
misses part of the real stream, the GPU executes the prime stream's bytes and
the readback reports a value this test writes *only* into the prime stream, so
the failure is named instead of being hidden by stale bytes that happen to
contain the right commands — which is what a rare pass can look like.

A failed readback is now classified per word: last fill, earlier fill, CPU
prefill (`0xdeadbeef`), prime stream, or something else, with a hint line naming
the most likely cause. The prime work is outside the timed regions, and the two
"as shipped" phases (`full`, `ranged`) are untouched, so `empty record` and the
6,000-fill record stay comparable with builds #7-#10. The cost is two extra
submits in a diagnostic app (~30 ms each).

## Per-app CPU budget is stable across runs

The main-thread busy time from the `cpu:` line repeats from build #9 within
about one percentage point for every app (busy ms as reported):

| App | #9 | #10 | | App | #9 | #10 |
| --- | --- | --- | --- | --- | --- | --- |
| `nvk_b2_flush` | 121.0 | 124.4 | | `nvk_push_desc` | 66.0 | 67.8 |
| `nvk_ce_copy` | 110.1 | 110.2 | | `nvk_zcull` | 44.3 | 44.1 |
| `nvk_desc_flush` | 77.1 | 77.1 | | `nvk_subtile` | 46.1 | 46.1 |
| `nvk_host_copy` | 37.2 | 37.2 | | `nvk_engine_wait` | 23.1 | 23.4 |
| `nvk_mem_churn` | 34.7 | 34.9 | | `nvk_cmd_flush` | 17.1 | 16.8 |

The one exception is `nvk_sector` (6.4 ms -> 14.3 ms), which walks promotion
levels and is the noisiest probe in the ladder. No app exceeded 54 % of its own
wall time, and none of them is close to saturating one core, let alone the three
the process is given (`0x7`).

## What this run does not prove

- NRO binaries were not uploaded, so only metadata and logs were verified.
- The `cpu:` figures are single observations of one thread per app; they include
  no GPU-side time and exclude the shader-cache workers.
- The four-value classification path did not run here — nothing failed. It is
  there for the next occurrence.
