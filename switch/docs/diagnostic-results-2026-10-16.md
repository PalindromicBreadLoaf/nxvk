# Diagnostic result: default T210 selection is correct — 2026-10-16

## Identified hardware run

Reviewed user-uploaded snapshot:
[`nxvk_run` at `c5d984c597df5486384b1edcb434d57883cd6e80`](https://github.com/berlint-hub/nxvk/tree/c5d984c597df5486384b1edcb434d57883cd6e80/nxvk_run),
uploaded at 2026-10-16 15:09:16 UTC. This is the focused diagnostic package,
not another execution of the original 22-app ladder.

- Source: `d5b1f3fc3f620f61582408bb9a8fd1a924dd95fa`, clean tree.
- Build: [Vulkan-only CI #5](https://github.com/berlint-hub/nxvk/actions/runs/38916684823).
- Embedded build ID: `4662fe0a6c3486588f56fb4f9c0ab7422ae595f83b17e3dfdf031c84909a5b79`.
- Runtime driver: `NVK`, `Mesa 26.2.2 (git-d5b1f3fc3f)`, SM 5.3.
- HOS: 22.1.0; operation/performance mode values both 0.
- Reported process memory limit: 3,343,908,864 bytes.
- **2 PASS, 0 FAIL, 0 GONE, 0 SKIP; 20/20 phases completed.**

The 12 uploaded files were checked against their Git blob hashes. SHA-256 of
`BUILDINFO.txt` matches `BUILD-ID.txt`, both app logs, both CSV headers and the
summary. The runtime driver revision agrees with the metadata. The uploaded
`SHA256SUMS` is a reference manifest; NRO binaries were not uploaded for a new
on-console-file checksum verification.

## Data checks

Each app contains 160 CSV pairs: 10 pre-warmup pairs and 150 warm pairs, with
one fragment and one compute measurement per pair. All 320 pairs were checked:

- ten complete phases per app, 15 distinct warm sample indices per phase;
- phase order, alternating fragment/compute order and last-sample readback flags
  agree with the diagnostic protocol;
- all GPU and wall times are finite and positive; no timestamp fallback;
- phase medians recomputed from CSV match the text summaries;
- all warmups meet both the two-second and 16-pair minima;
- all clock queries report success with valid CPU/GPU/EMC values;
- all 20 fragment traces report SM 53 and the expected selected word;
- all 20 colour/compute readback hash pairs are identical:
  `0xb1884637f4710a21` / `0xb81d92b31a9f5e31`.

## The large first-sample difference tracks a frequency-profile transition

Source: [cache-disabled CSV](https://github.com/berlint-hub/nxvk/blob/c5d984c597df5486384b1edcb434d57883cd6e80/nxvk_run/nvk_subtile_nocache.csv),
phase 0, default setting. Times below are reported GPU timestamps using the
driver's advertised timestamp period; they are not game frame times.

| Same default phase | Pre-warmup pair | Warm median |
| --- | ---: | ---: |
| Reported GPU rate | 307.2 MHz | 1,497.6 MHz at every warm sample boundary |
| Reported CPU rate | 1,020 MHz | 2,703 MHz at every warm sample boundary |
| Reported EMC rate | 2,666 MHz | 2,666 MHz |
| Fragment GPU time | 18.549184 ms | 3.829632 ms |
| Compute GPU time | 30.505536 ms | 6.269888 ms |
| Selected subtiling word | `0x087f6080` | `0x087f6080` (same pipeline) |

The GPU-rate ratio is **4.875×**. The cold/warm duration ratios are **4.844×**
(fragment) and **4.865×** (compute): both workloads change almost proportionally
to the reported GPU rate, without changing the subtiling setting.

This is strong evidence that the large startup difference in this run is a
**frequency-profile transition**, not an incorrect default T210 selection.
The transition occurred between the post-cold and post-warmup observations.
The probe only reads rates; it does not set clocks, voltages or performance
policy. These logs do not identify which external policy/service applied the
profile, nor establish the exact instant of transition inside the warmup.

The cache-enabled process already reports the higher profile at its initial
pair, and starts at 3.832512/6.269280 ms. Across both apps, all warm sample
boundary snapshots report CPU/GPU/EMC = **2,703 / 1,497.6 / 2,666 MHz**. These
are elevated profiles, not stock-Switch performance results. Service snapshots
are not continuous hardware frequency measurements or an overclock stability
certification.

Earlier unidentified logs lacked clock observations. Their similar 18.5 ms
first row must not be advertised as a fivefold benefit from forcing T210;
this new run demonstrates a clock-related confound with the same symptom.

## Default equals explicit T210 after warmup

Medians pooled over warm samples of each variant (60 default, 45 T210, 45 generic
per app). Repeated samples are not independent console runs.

| Requested setting | Cache enabled: fragment ms | Cache disabled: fragment ms | Emitted word |
| --- | ---: | ---: | --- |
| Default | 3.829520 | 3.829840 | `0x087f6080` |
| Explicit T210 | 3.829888 | 3.829632 | `0x087f6080` |
| Generic | 4.774080 | 4.774720 | `0x20164010` |

Default versus explicit T210 differs by about **0.01% or less** in these pooled
medians. The [driver trace](https://github.com/berlint-hub/nxvk/blob/c5d984c597df5486384b1edcb434d57883cd6e80/nxvk_run/nvk_subtile_diag.mesa.log)
confirms the default and override construct the same subtiling word. Repeating
the default later in the sequence does not expose a persistent slow default.

T210 versus generic gives approximately **24.7% higher fragment throughput**
(**19.8% lower elapsed time**) in this ALU-heavy microbenchmark, with stable
compute controls near 6.27 ms. This supports the **existing** T210 default; it
is not a new optimization introduced by this diagnostic package or a game-FPS
prediction.

## Shader cache interpretation

The enabled probe's cache log starts at zero entries and then reports six entries
(2 KiB) in the isolated diagnostic store. The disabled process requested
`MESA_SHADER_CACHE_DISABLE=1` before creating any instance. No cache-disabled
`.cache.log` was uploaded; the other app/CSV/driver logs are complete.

Once warmed at the same reported frequencies, the two processes have essentially
the same GPU workload times. This does **not** show that shader caching is useless:
pipeline compilation time and real-game compilation stutter were not benchmarked.
Do not attribute the first cache-disabled slowdown to caching alone when its
clock profile also differs. Hardware caches and ordinary reuse of an already
created pipeline are not what this disk-cache switch disables.

## Decision

- Keep the current T210 default. Do not introduce a redundant forced override,
  loosen synchronization, or automatically raise clocks to hide startup effects.
- Continue comparing warmed, repeated measurements at a consistent recorded
  profile. The diagnostic already excludes the initial pair from warm medians.
- For the next optimization, profile a representative real application at the
  same settled profile: CPU submission/recording, GPU workload time, shader
  compilation stalls and memory traffic. Game performance is not established by
  this microbenchmark.
- Treat GPU milliseconds as driver-reported timings. CPU wall time includes
  submission/wait overhead and, on the last pair, readback copies; it is not an
  interchangeable GPU timer or proof of a timestamp calibration error.
- This result validates these two diagnostics on build #5. It is not a claim that
  the complete 22-app suite or Vulkan CTS was rerun on that build.

This review changes documentation only. No new driver build is needed to retain
this finding, and the agent did not execute the NROs itself.
