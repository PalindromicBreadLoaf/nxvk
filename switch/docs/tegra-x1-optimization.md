# Tegra X1 optimization pass — 2026-10-16

Scope: Vulkan/NVK on Horizon, CPU-side descriptor/upload bookkeeping and the
SD-backed shader cache. No emulator integration, clock/governor changes, new
CUDA dependency or changes to GPU scheduling/register defaults are involved.

## Documentation reviewed

- NVIDIA **CUDA for Tegra**, memory-management and I/O-coherency sections:
  CPU and iGPU share SoC DRAM, but I/O coherency starts with Xavier. Sharing
  physical memory on the older X1 is not permission to omit cache maintenance.
  CUDA allocator-specific policies are not copied into Horizon's NvMap backend.
  [5](https://docs.nvidia.com/cuda/pdf/CUDA-For-Tegra-AppNote.pdf)
- NVIDIA **Maxwell Tuning Guide**, occupancy and memory-throughput sections:
  reducing transfers/redundant memory work is important; register occupancy
  must be balanced against spilling, not maximized blindly. Its GM204 shared
  memory limits must not be assumed to describe every Maxwell part.
  [1](https://docs.nvidia.com/cuda/pdf/Maxwell_Tuning_Guide.pdf)
- NVIDIA's **Tegra X1** HPG 2015 presentation identifies the two-SMM GPU,
  the A57 CPU cluster, memory-bandwidth constraints and compression facilities.
  This is SoC architectural material, not a description of which CPU clusters
  Horizon exposes to applications.
  [2](https://www.highperformancegraphics.org/wp-content/uploads/2015/Hot3D2/NVIDIA_X1_hpg2015_hot3d.pdf)
- The official **deko3d Primer** recommends CPU-uncached / GPU-cached command
  storage, distinguishes CPU and GPU cacheability, and explicitly requires
  maintenance for CPU-cached memory and appropriate GPU cache invalidation.
  [2](https://github.com/devkitPro/deko3d/blob/master/Primer.md)
  The Primer and `source/dk_memblock.cpp`, `dk_queue.cpp`, `dk_cmdbuf.cpp` were
  also inspected at revision `350f2b00a3e76ecd4f00191f8c5d6544ffbcb9db`.

The public NVIDIA HTML guides were read as well as the indexed PDF references.
The architecture guidance informs the changes below; it does not establish a
measured speedup in this port. No claim is made to have reviewed the entire
Tegra X1 technical reference manual.

## 1. Stop tracking cache-flush byte ranges for maps that do not need it

NXVK already maps internal write-only allocations CPU-uncached on Horizon,
while keeping their GPU cache policy separate. `nvkmd_mem_sync_to_gpu()` already
returns without a CPU cache operation for CPU-uncached or coherent allocations.

However, the arena still performed atomic min/max bookkeeping after every
image/sampler descriptor write and mapped shader upload, only for the eventual
flush to do nothing. Each backing allocation now records whether it requires
this range tracking. Uncached/coherent maps skip the range updates and normal
range-drain work. This is a **CPU bookkeeping optimization**, not removal of
previously required physical cache flushes.

Safeguards retained:

- Data writes still precede the existing `arena->map_dirty` publication.
- The fast path requires the GCC acquire/release atomic builtins (checked in
  the target compile commands by CI); other atomic backends retain tracking.
- The publication store and the flush-side atomic exchange remain; arena locks
  and the `mem_count` publication protocol are unchanged.
- CPU-cached, non-coherent memory retains range tracking, alignment and the
  exact existing flush path. GPU-uncached **alone** does not bypass CPU work.
- GPU cache invalidation, queue submission, fence/writeback ordering and WSI
  behavior are unchanged.
- `NVK_DEBUG=full_arena_flush` retains the full-arena diagnostic path.
- `NVK_DEBUG=track_uncached_dirty` restores the old range bookkeeping for
  same-binary A/B checks; set it before instance creation. It does not change
  memory cacheability. `no_cpu_uncached` still exercises the cached path.

The native fixture runs the actual production functions. For 1,024 ascending
32-byte writes in one uncached backing it counts **1,025 -> 0 range CAS calls**,
while retaining all **1,024 publication stores**. Cached, coherent, GPU-uncached,
forced-legacy, full-flush and cross-backing cases are checked separately.
These are operation counts, not an A57 timing or FPS result.

## 2. Avoid appending an already stored identical shader blob

The Horizon shader store previously appended every `blob_put`, even when the
same key and exact payload already existed. That can happen, for example, when
independent compilations enqueue equivalent results. It consumes SD writes and
append-only cache capacity without adding useful data.

The new path searches the index and uses size/CRC as a cheap filter. Before
skipping the append it compares the complete stored payload in bounded 4 KiB
chunks. A matching key or CRC alone is **not** enough. The existing mutex covers
the lookup, comparison and append, so the FILE position and index stay serialized.

- Identical, intact blobs reuse the old record without changing `store.used`.
- Different payloads (including a CRC collision), changed sizes, corrupted
  content/CRC, invalid offsets or comparison I/O failures take the normal
  replacement/append path.
- New keys do not read existing payloads. Capacity rejection stays cheap for
  new keys, and the CRC is not calculated twice for a replacement candidate.
- The format, normal record/header publication sequence, finalization and
  existing failure handling are unchanged. This is not a new durability claim.

The native fixture uses real temporary-file I/O and production function bodies,
with a deliberately constant CRC to force collision cases. An identical 8,193
byte put changes from **three fwrite calls plus two fflush calls** to **zero
fwrite/fflush calls**, with an 8,193-byte comparison read instead. It also tests
the 256 KiB maximum blob, corruption, failed I/O/header publication, size changes
and full-cache behavior. This does not mean that the OS performs no metadata I/O,
or that finalization never writes; it describes the duplicate put itself.

The benefit depends on duplicate traffic and SD behavior. Unique puts add an
index lookup; no claim of a general FPS or shader-compilation speedup is made.

## Deliberately unchanged

The [identified hardware diagnostic](https://github.com/berlint-hub/nxvk/blob/9565f625c80426ed10c231598b8c552e35383ed2/switch/docs/diagnostic-results-2026-10-16.md) already
confirmed that default and explicit T210 select the same word and have the same
warmed performance. There is no reason to replace that default. Existing NVMap
recycling, zero-copy presentation, compression, ZCULL, sector promotion, NAK SM
selection, Cortex-A57 targeting and NEON selection are retained, not presented
as newly implemented optimizations. No fast-math, forced register cap, affinity
change, extra worker threads, GPU clock setter or relaxed GPU barrier is added.

## Validation and adoption

`bash switch/tests/run-host-tests.sh` includes the new production-body fixtures
under ASan/UBSan. Restoring either original hot-path body makes its new regression
assertion fail. This verifies the intended work reduction and control flow, not
Horizon cache coherency, SD throughput, scheduling or GPU execution.

The Vulkan-only CI must still compile/link the real SDK build, validate the
extracted library package, and rebuild all 22 test apps and the focused probes.
Then run the **full 22-app suite** on the console for the new build and keep its
embedded `BUILDINFO.txt` with the results. Build #5 is the prior baseline.
Compare resource-creation/upload costs and real workload frame times under the
same settled conditions; use `track_uncached_dirty` for an in-build arena control.
Do not infer a measured end-to-end improvement from the host operation counts.
