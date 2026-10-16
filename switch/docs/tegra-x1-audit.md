# Tegra X1 Vulkan audit — 2026-10-16

Scope: the Vulkan-only release, Cortex-A57 host code, NAK's GM20B selection,
background shader-cache work and presentation error handling. This is a focused
source/build audit, not a complete driver review or a hardware performance claim.

## Changes

### Honor background worker priority on Horizon

`disk_cache_init_queue()` requests `UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY`, but
`util_queue_thread_func()` previously implemented it only on Linux. Horizon's
shader-cache compression/writer workers therefore retained their normal priority.

The Horizon path now queries `InfoType_PriorityMask` and selects the lowest
priority permitted to the process. It uses the worker's real libnx thread handle,
never raises its existing priority, and leaves it unchanged if a query or priority
update fails. This runs once per flagged worker, not for every cache operation.
The application thread, number of workers and affinity policy are unchanged.

This honors the existing scheduling request; it is **not proof of better frame
times**. Horizon scheduling differs from Linux `nice`: a busy high-priority
workload can delay background cache writes. Check both foreground latency and
cache backlog/exit time on the console before adopting it as a performance win.

### Tune the whole Rust target for Cortex-A57

Meson already passed `-Ctarget-cpu=cortex-a57` to the driver, but the custom target
had no CPU default. The separately built `core`, `alloc` and `std` could therefore
use generic CPU tuning. The target now declares `cpu: cortex-a57`; the sysroot
script verifies rustc's resolved target before compiling it.

Make now rebuilds the sysroot when its script or target JSON changes, and
reconfigures the Vulkan build after a sysroot or Rust-wrapper change. This avoids
silently combining an updated target with old generated libraries.

### Propagate present-fence failures

The fallback path in `wsi_switch_swapchain_queue_present()` ignored the result
of `WaitForFences`. After a failed wait (for example device loss), it could still
copy a framebuffer, queue it to the window and report success.

A failed wait now returns its Vulkan result before copying or queueing. Successful
zero-copy syncpoint handoff stays asynchronous; no extra CPU wait was added.

### Use the process's core mask instead of a guess

Horizon grants a process a mask of allowed cores (`InfoType_CoreMask`), not a
count: the system keeps cores for itself, an application normally gets three of
the four Cortex-A57 cores and an applet fewer. Mesa's POSIX CPU detection has no
libnx equivalent, so `util_cpu_caps.nr_cpus` did not describe the hardware, and
everything sized by it did not either — most visibly Zink's threaded context,
which is only enabled when Mesa believes there is more than one CPU.

`util_horizon_get_core_mask()`/`_count()` are now the source for `nr_cpus` on
Horizon, and the physical device logs one line per device with the allowed mask,
the count and what Mesa sized for. A failed query keeps the previously detected
value rather than assuming four. No scheduling policy, priority, affinity or
worker count is changed by this; see
[the CPU budget note](tegra-x1-cpu-budget.md).

### Flush the indirect execution set's written range, not past its end

`nvk_UpdateIndirectExecutionSet{Pipeline,Shader}EXT()` passed
`(max_idx + 1) * stride` as the *length* argument of
`nvkmd_mem_sync_map_to_gpu()`, whose length is measured from `min_idx * stride`.
Whenever `min_idx != 0` that cleaned up to `(min_idx + max_idx + 1)` strides,
i.e. past the end of the set's allocation, and it could walk cache maintenance
over an address the set does not own. This is a divergence from upstream NVK;
it is recorded here so a future rebase keeps the fix. The shipped Horizon
configuration does not exercise it (that allocation is CPU-uncached, so the
sync is a no-op), but CPU-cached descriptor paths and the desktop backend do.

## Deliberately unchanged

- C/C++ remain ARMv8-A + CRC/crypto, tuned for Cortex-A57 at `-O3`; no fast-math
  or LTO was introduced.
- BLAKE3 already selects its NEON implementation for little-endian AArch64.
- NAK already receives the GPU's SM version from nvgpu characteristics; the
  GM20B path is SM 5.3, not a CPU compiler flag.
- GPU cache maintenance, command submission ordering, semaphore waits, register
  limits and shader optimization heuristics were not relaxed without hardware
  evidence.

## Validation and remaining work

Run `bash switch/tests/run-host-tests.sh` for native tests:

- priority masks (all 64 single-bit masks, restricted/non-contiguous masks),
  permission/query errors and no priority promotion, plus core-mask counting
  (non-contiguous masks, unknown mask reported as zero, no cross-talk between
  the two service queries);
- the actual presentation function against mocked WSI/libnx callbacks, covering
  failed waits, CPU-copy suppression, successful fallback and asynchronous
  syncpoint handoff;
- incremental Make dependencies and the Rust CPU default.

The C tests run under AddressSanitizer and UndefinedBehaviorSanitizer. Mocks do
not validate the Horizon scheduler, BufferQueue ABI or actual GPU timing; the
real SDK is checked by cross-compilation and package linking in CI. CI also
rebuilds the existing 22 Vulkan NRO tests and the runner.

For A/B measurements, retain the previous build (`0c389c6`, successful CI run #3)
and each candidate's `BUILDINFO.txt`. Use the same game/scene, firmware, memory
mode and CPU/GPU/memory clocks. Warm each build's shader cache separately, measure
both cold and warm runs, and compare frame-time percentiles as well as FPS,
cache backlog and shutdown time.

A subsequent [review of user-supplied Switch logs](hardware-results-2026-10-16.md)
records 22 passing Vulkan apps and several microbenchmark results. The upload
predates CI run #4 and lacks binary build metadata, so it does **not** establish
hardware validation or a performance gain for the changes above. The sandbox
itself has not executed NROs on a console.
