# Tegra X1 CPU budget — cores, and what "not CPU-bound" can mean here

Scope: how many Cortex-A57 cores a Horizon application can actually use, what
this port does with that number, and how to tell whether a workload is CPU-bound
at all. This is a measurement-and-plumbing document: it adds no clock, voltage,
affinity or scheduling policy change.

## "Use all four cores" is not a thing on Horizon

Horizon does not give a process a core count. It gives it a **mask of allowed
cores** (`InfoType_CoreMask`, libnx: *"Bitmask of allowed Core IDs"*). The
system keeps cores for itself, so:

- a **homebrew application** loaded with full application memory/title override
  normally sees a mask like `0x7` — three of the four Cortex-A57 cores;
- an **applet** (Album, or an applet-mode launch) sees fewer, typically one;
- nothing in userland can move the mask, and no API grants "the fourth core".

Two consequences that matter more than the core count itself:

1. **The driver does not choose the application's threads.** A Vulkan driver
   runs on the threads that call it, plus whatever workers it creates itself.
   Whether a game compiles pipelines, records command buffers and updates
   descriptors on one thread or four is the *application's* decision. A driver
   can stop getting in the way of that; it cannot decide it.
2. **Being CPU-bound is usually not about the number of cores.** On this port's
   own measurements the startup cost is dominated by a **frequency-profile
   transition** (307 MHz -> 1497 MHz GPU, 1020 MHz -> 2703 MHz CPU, ~4.9x), not
   by core count — see the
   [diagnostic result](diagnostic-results-2026-10-16.md). Chasing cores before
   knowing which of the two is happening would be guessing.

So the honest target is: *never be the bottleneck on the cores the process has,
and never serialize the application's threads*.

## What this port now does with the mask

- `util_horizon_get_core_mask()` / `util_horizon_get_core_count()` read the mask
  (0 means "unknown", which is not the same as "one").
- `util_cpu_detect.c` uses it for `util_cpu_caps.nr_cpus` on Horizon, instead of
  whatever the POSIX `sysconf()`/affinity path returns under libnx. That number
  is what Mesa sizes things by — the one that matters most today is Zink's
  threaded context (`zink_screen.c`: `screen->threaded = nr_cpus > 1 && ...`),
  which stays disabled for the GL stack if Mesa believes there is one CPU.
- `nvk_physical_device.c` logs one line per device, so an upload states what the
  process may use and what Mesa sized for:

  ```text
  nvk: Horizon allows cores 0x7 (3 core(s)); Mesa sized for 3 CPU(s)
  ```

This changes *no* scheduling policy: thread priorities, the low-priority
shader-cache workers and the absence of any affinity forcing are unchanged. It
only stops the driver from being wrong about the hardware it is running on.
Whether the GL threaded context should be *enabled by default* on Switch is a
separate decision that needs the GL suite on hardware, not a Vulkan-only CI run.

## Telling CPU-bound from waiting

The ladder now reports, per application (and, for the diagnostic probes, per
phase):

```text
cpu: main thread busy 812.4 ms of 1834.2 ms wall (44%), allowed cores 0x7
```

Built from `clock_gettime(CLOCK_MONOTONIC)` and `InfoType_ThreadTickCount`
(HOS 13.0.0+, `TickCountInfo_Total`), converted with `armGetSystemTickFreq()`.

| Observation | Reading |
| --- | --- |
| busy% near 100, wall time far above GPU time | the app is genuinely CPU-bound |
| busy% low, GPU time dominates | waiting on the GPU; the CPU is not the limit |
| busy% low, GPU time also low, wall time high | waiting on the SD card or on fences |
| busy% near 100 on a phase that should be GPU-heavy | something is spinning in the driver or the app |

Limits of that number, stated plainly: it counts the **calling thread only**. The
driver's shader-cache workers are separate threads, and so is any application
thread the app creates; their CPU time is not in this figure. There is no
per-core utilisation readout either — Horizon exposes idle/thread tick counts per
*core*, but only for the core the querying thread is on, so it cannot be used to
watch all cores at once.

Use it together with the existing observations: `NVK_DEBUG=vm` (arena and
mem-cache counters), `NVK_DEBUG=wsi` (present path, `swizzle_ns`), the store's
open/close summaries in `<app>.mesa.log`, and the read-only clock samples the
diagnostic probes already take.

## Known CPU costs in this port, ranked

Each is a candidate, none is claimed as a measured win yet:

1. **CPU framebuffer path (per frame, when zero-copy fails).**
   `wsi_switch.c` swizzles into block-linear memory with the CPU and flushes the
   whole framebuffer (~3.7 MB at 720p) every present. If a title hits this path
   it is the largest per-frame CPU cost in the driver. Fix direction: do the
   swizzle with the copy engine instead of the CPU. Measure: `NVK_DEBUG=wsi`
   (`swizzle_ns`, `fence_blocks`) plus the new `cpu:` line, against a zero-copy
   run.
2. **Cache maintenance over whole allocations.**
   `nvkmd_nvgpu_mem.c` cleans+invalidates the entire allocation on `nvMapCreate`
   and on mem-cache recycle (`util_flush_inval_range`, i.e. `dc civac` per
   64-byte line). This is load-time cost, not per-frame, but it scales with
   every allocation. Fix direction: narrow the range, or track what the CPU
   actually wrote (the mechanism the arena already has). Measure: `NVK_DEBUG=vm`
   plus `cpu:` on allocation-heavy apps (`nvk_mem_churn`).
3. **Per-submit overhead.** Every submit appends GPFIFO entries, increments a
   fence and kicks off the channel; a CPU submit is a synchronous sequence of
   service calls. It dominates on workloads with many small submits.
   Measure: `cpu:` on `nvk_cmd_flush`, `nvk_engine_wait`.
4. **Shader compilation on the application's thread.** NAK runs inside
   `vkCreateGraphicsPipelines` on whatever thread the app uses; the pipeline
   cache is thread-safe, so an application *can* compile in parallel, and the
   driver does not serialize it. Nothing to change in the driver first — check
   whether the workload compiles at all in steady state before touching it.
5. **Shader-cache compression.** The store deflates at `Z_BEST_COMPRESSION` with
   `zstd` disabled, on four low-priority workers. Already off the critical path
   and now doing fewer header writes; a lower level is a cheap experiment.
   Measure: `cpu:` on a pipeline-heavy app plus the store's close summary.
6. **Worker count vs core count.** The store always creates four workers. On a
   three-core process that is one more thread than there are cores, at the
   lowest priority the process is allowed. Worth revisiting only after the
   foreground effect of the cache writes is measured.

## Deliberately not done

No clock, voltage or performance-mode setter; no `appletSetCpuBoostMode`; no
forced thread affinity (`svcSetThreadCoreMask`); no extra driver worker threads;
no spinning to "use" idle cores; no priority promotion. The diagnostic probes
keep observing clocks read-only. These are the same boundaries the
[Source audit](tegra-x1-audit.md) and the
[optimization pass](tegra-x1-optimization.md) already set, and they exist because
each of them changes device behaviour that the user did not ask the driver to
change.

## Order of work

1. Read the new `cpu:` lines and the driver's core line from a hardware run of
   the existing 22-app ladder. That answers "is this port CPU-bound, and on
   which apps?" with data instead of hypotheses.
2. If a per-frame CPU cost shows up (item 1 above), that is the one worth GPU
   offloading — it is the only per-frame entry in the list.
3. Revisit items 2-3 (load-time and per-submit costs) with the operation counters
   already in the build.
4. Only then consider whether the GL stack should use its threaded context by
   default on Switch, with the GL suite on hardware.
