# Build #9 — 21 of 22 passed; the CPU-cached command-flush phase failed

Reviewed user-uploaded snapshot:
[`nxvk_run` at `b8cfab65255ab4eed61c41d0343cc8a3589adfec`](https://github.com/berlint-hub/nxvk/tree/b8cfab65255ab4eed61c41d0343cc8a3589adfec/nxvk_run),
uploaded 2026-10-16 at 18:59:27 UTC.

**21 PASS, 1 FAIL, 0 GONE, 0 SKIP.** The one failure is the fourth phase of
`nvk_cmd_flush`, a diagnostic configuration that the port does not ship.

## Build attribution and consistency checks

- Embedded metadata identifies source `72bef66bd360a8fb0c5c53817e63c08f2f07c406`
  from `refs/heads/arena/92b2b9eb-nxvk` from
  [CI run #12](https://github.com/berlint-hub/nxvk/actions/runs/38947942804),
  which published the package, the SD bundle and the diagnostics as `*-12`
  artifacts.
- Build ID: `ced4cc3d0557dad9944686eea85898ac16f33c2089d80780aae1ae03526a5366`.
  SHA-256 of the uploaded `BUILDINFO.txt` equals that ID, and `summary.log` and
  `BUILD-ID.txt` carry the same value.
- All 49 uploaded files match their Git blob hashes. They are 22 app logs, 22
  Mesa logs, `chain.log`, `summary.log` and the three metadata files.
- `chain.log` has 22 armed entries, `summary.log` lists the 22 distinct apps,
  and every app log's verdict text matches the summary.
- The reference `SHA256SUMS` lists the expected 23 NRO names (22 apps plus the
  runner) with well-formed hashes. NRO binaries were not uploaded, so their
  on-console bytes have not been independently rehashed here.
- Runtime driver: `NVIDIA Tegra X1 (NVK gm20b)`, Vulkan `1.4.354`, vendor
  `0x10de`.

## Both evidence gaps from build #8 are closed in this data

Build #8 could not prove which support code ran (only `libnvk.a` was recorded)
and captured no driver diagnostics (all 22 Mesa logs were empty). This upload
shows both fixed, on hardware:

- `BUILDINFO.txt` now records **18** `Linked <name> SHA256:` lines, one per
  archive the `.nro` links. The archives that actually changed in this build are
  included: `libnvk.a` `2b7515a7…` (was `9f41ae6c…` in build #8, changed by the
  physical-device CPU-budget line) and `libmesa_util.a` `e73c7283…` (changed by
  the core-mask CPU count).
- **22 of 22** Mesa logs are non-empty. They carry the new driver lines below,
  which is the first time the driver's own view of a build reached the upload.
- The shader store is now measured end to end. Nine apps logged
  `shader cache: N blobs offered, N appended (N KiB), 0 duplicates,
  1 header publications (batched)` (2–6 blobs, 0–4 KiB each). With a 256 KiB
  publication interval and this little data, exactly one publication per app —
  at close — is the designed behaviour of the batched policy, and it confirms
  the build #8 store change on hardware instead of by source inspection.

## What the new CPU instrumentation reports

Every app logged the mask the process was given:

```
MESA: info: nvk: Horizon allows cores 0x7 (3 core(s)); Mesa sized for 3 CPU(s)
```

Three cores, not four, on a full-memory application — as expected from
`InfoType_CoreMask`; the fourth core is not the application's to take. Mesa now
sizes itself from that mask instead of a POSIX guess.

The same apps also report what the main thread did with the time it had
(`cpu: main thread busy X ms of Y ms wall`). These are single observations from
one run, not controlled A/B measurements, and they cover the calling thread
only — the shader-cache workers are separate threads:

| App | busy | wall | busy |
| --- | --- | --- | --- |
| `nvk_ce_copy` | 110.1 ms | 206.1 ms | 53 % |
| `nvk_desc_flush` | 77.1 ms | 160.6 ms | 48 % |
| `nvk_push_desc` | 66.0 ms | 166.8 ms | 40 % |
| `nvk_b2_flush` | 121.0 ms | 330.6 ms | 37 % |
| `nvk_cmd_flush` | 17.1 ms | 81.6 ms | 21 % |
| `nvk_subtile` | 46.1 ms | 210.9 ms | 22 % |
| `nvk_host_copy` | 37.2 ms | 156.0 ms | 24 % |
| `nvk_mem_churn` | 34.7 ms | 226.8 ms | 15 % |
| `nvk_engine_wait` | 23.1 ms | 236.8 ms | 10 % |
| `nvk_zcull` | 44.3 ms | 151.7 ms | 29 % |
| `nvk_smoke` | 6.4 ms | 119.9 ms | 5 % |
| `nvk_cubemap` | 10.3 ms | 113.7 ms | 9 % |
| others (`tri`, `logo`, `scene`, `indexed`, `multi`, `textures`, `sector`) | 4.9–10.3 ms | 70.5–177.5 ms | 4–9 % |
| `nvk_vi_swapchain` | 19.8 ms | 5047.0 ms | <1 % |
| `nvk_present` | 27.5 ms | 6044.1 ms | <1 % |

No app in the ladder came close to saturating even one core; the two presenting
apps spend their time waiting for vsync, and the highest figures belong to the
probes that measure driver bookkeeping directly. This is the answer to "is the
port CPU-bound?" for these workloads: not on any of these apps, and adding
cores would not change them.

## The failure

```
READBACK FAIL (ranged_cached): word 0 = 0xdeadbeef, expected 0x5ca1ab1e
ranged_cached [NVK_DEBUG=no_cpu_uncached]: empty record 747 ns, 6000-fill record 27112240 ns, readback BAD
FAIL: the GPU did not see the whole pushbuf
=== nvk_cmd_flush FAILED ===
```

The other three phases of the same app (`full`, `ranged`, `full_cached`)
read back correctly.

- `no_cpu_uncached` is a **diagnostic** flag: it keeps the driver's internal
  write-only allocations — command buffers, upload memory, descriptor memory —
  CPU-cacheable. The shipped configuration maps those CPU-uncached, where
  `nvkmd_mem_sync_to_gpu()` is a no-op, so no CPU flush is involved at all and
  this failure says nothing about the shipped ladder. The phase exists as the
  safety net for the build #7 dirty-range optimization, whose own safety
  argument is "CPU-cached, non-coherent memory retains range tracking,
  alignment and the exact existing flush path"
  ([tegra-x1-optimization.md](tegra-x1-optimization.md)). That is the path this
  run exercised, and it is where the finding is.
- The app's own buffer mapping is `HOST_VISIBLE | HOST_COHERENT`, and the port
  forces CPU-uncached for coherent memory in `nvkmd_nvgpu_alloc_tiled_mem()`
  regardless of the debug knob. The readback therefore reads physical memory
  directly, and `0xdeadbeef` is a value this test writes: it is both the CPU's
  prefill and the value the 5,999 earlier fills write. **That ambiguity is the
  reason this log cannot yet say whether the GPU executed nothing or executed
  everything except the last fill.**
- The driver code between the two runs differs only by the CPU-budget logging:
  `git diff 12d6eea 72bef66 -- src/` is three files (`nvk_physical_device.c`,
  `u_cpu_detect.c`, `u_thread_horizon.h`), 71 inserted lines, no deletions, and
  **no** change in `nvk_mem_arena.*`, `nvk_cmd_buffer.c`, `nvk_cmd_pool.c`,
  `nvkmd/`, or `cache_ops_aarch64.c`. The same phase passed in build #7
  ([build-7-validation.md](build-7-validation.md) records "all four phases have
  correct readback, including forced CPU-cached full/ranged paths") and in
  build #8, whose log for the phase is `readback OK`.

So the outcome is **data-dependent**, and two explanations remain open:

1. The range-based flush of command memory under-covers the bytes the GPU is
   told to execute, and whether that matters depends on what those bytes held
   from earlier submissions. Builds #7/#8 could have had the correct bytes
   there by reuse. The range comes from a per-allocation high-water mark of CPU
   writes (`nvk_cmd_mem::used_B`, `flush_mem_list()` in `nvk_cmd_buffer.c`);
   the `full_cmd_flush` phases flush whole allocations instead and pass.
2. Something about this run's state shifted memory reuse: the SD shader store
   was already populated when the run started (the first app reports
   `28 entries, 11 KiB`), so shaders were cache hits rather than compilations,
   and `nr_cpus` now comes from the core mask, which changes Mesa's worker
   queue sizes. Neither touches the flush code, but both change what the driver
   writes, and where.

Neither is proven, and the honest summary is: **a diagnostic phase flipped from
PASS to FAIL while the code it exercises did not change.** That is worth one
more run, not a rewrite.

## What this commit does about it

`switch/smoke/nvk_cmd_flush.c` now writes three different values — the CPU
prefill, every earlier fill, and the last fill — and classifies a failed
readback instead of stopping at the first mismatch. The next run's log will
therefore say which of these happened:

| Readback content | Meaning |
| --- | --- |
| all words hold the CPU prefill | no fill reached the buffer: the recorded stream, or its execution, was not visible to the GPU |
| all words hold an earlier fill's value | the GPU ran fills but the last one never arrived: the tail of the stream or the last parameter update |
| words that are none of the three | the GPU filled with something else, so a parameter it read was stale |

`switch/tests/run-host-tests.sh` also host-syntax-checks the twelve ladder apps
that the stub headers cover, so a typo in this instrumentation fails natively
before a console run is spent on it.

## What this run does not prove

- The NRO binaries were not uploaded, so the on-console bytes were not
  rehashed; only the metadata and logs were verified.
- The per-app `cpu:` values are single observations of one thread, not a
  measurement of the port's cost in a real game, and they include no
  GPU-side time.
- The three passing phases of `nvk_cmd_flush` passing again does not prove the
  cached path is correct: it proves the shipped configuration still works.
