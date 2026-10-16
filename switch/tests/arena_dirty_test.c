/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual arena tracking/flush bodies; memory/backend and atomics are observed
 * through native fixtures. This is not a GPU or SDK ABI test. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nouveau/vulkan/nvk_debug.h"
#include "arena_enums.inc"
#ifndef NVK_TEST_LEGACY_ATOMICS
#define USE_GCC_ATOMIC_BUILTINS
#endif
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#define ROUND_DOWN_TO(v,a) ((v) & ~((uint64_t)(a) - 1))
#define ALIGN_POT(v,a) (((uint64_t)(v) + (a) - 1) & ~((uint64_t)(a) - 1))
#define unlikely(x) (x)
typedef int simple_mtx_t;
struct nvkmd_mem { enum nvkmd_mem_flags flags; uint8_t *map, *gpu; };
#include "arena_cacheable.inc"

static unsigned cas_calls, publications, consumes, range_resets, sync_requests, cpu_flushes;
static uint32_t *publication;
static uint32_t observed_cas(uint32_t *v, uint32_t old, uint32_t next)
{
   cas_calls++;
   return __sync_val_compare_and_swap(v, old, next);
}
static uint32_t observed_exchange(uint32_t *v, uint32_t next)
{
   if (v == publication) consumes++; else range_resets++;
   return __atomic_exchange_n(v, next, __ATOMIC_ACQ_REL);
}
#define p_atomic_read(v) __atomic_load_n(v, __ATOMIC_ACQUIRE)
#define p_atomic_cmpxchg(v,o,n) observed_cas(v,o,n)
#define p_atomic_xchg(v,n) observed_exchange(v,n)
#define p_atomic_set(v,n) do { publications++; __atomic_store_n(v,n,__ATOMIC_RELEASE); } while (0)
#include "arena_helpers.inc"

struct nvk_physical_device { enum nvk_debug debug_flags; struct { uint32_t nc_atom_size_B; } info; };
struct nvk_device { struct nvk_physical_device pdev; };
static struct nvk_mem_arena arena;
static struct nvkmd_mem backing[3];
static uint8_t cpu[3][128 * 1024], gpu[3][128 * 1024];
static struct nvk_device device;
static uint64_t last_offset, last_range;
static const struct nvk_physical_device *nvk_device_physical(struct nvk_device *dev) { return &dev->pdev; }
static uint32_t nvk_mem_arena_mem_count(struct nvk_mem_arena *a) { return p_atomic_read(&a->mem_count); }
static bool nvk_mem_arena_is_mapped(const struct nvk_mem_arena *a) { return a->map_flags != 0; }
static uint32_t nvk_contiguous_mem_arena_find_mem_by_offset(struct nvk_mem_arena *a, uint64_t offset)
{
   for (unsigned i = 0; i < a->mem_count; i++)
      if (offset >= nvk_contiguous_mem_arena_mem_offset_B(i) &&
          offset < nvk_contiguous_mem_arena_mem_offset_B(i) + nvk_mem_arena_mem_size_B(i)) return i;
   abort();
}
static uint32_t nvk_mem_arena_find_mem_by_addr(struct nvk_mem_arena *a, uint64_t addr)
{
   for (unsigned i = 0; i < a->mem_count; i++)
      if (addr >= a->mem[i].addr && addr - a->mem[i].addr < nvk_mem_arena_mem_size_B(i)) return i;
   abort();
}
static void nvkmd_mem_sync_map_to_gpu(struct nvkmd_mem *mem, uint64_t offset, uint64_t size)
{
   sync_requests++;
   if (!nvkmd_mem_is_cpu_cacheable(mem)) return;
   assert(offset % 64 == 0 && size % 64 == 0 && offset + size <= 128 * 1024);
   last_offset = offset; last_range = size;
   cpu_flushes++;
   memcpy(mem->gpu + offset, mem->map + offset, size);
}
#include "arena_functions.inc"

static void reset(enum nvkmd_mem_flags flags, enum nvk_debug debug)
{
   memset(&arena, 0, sizeof(arena));
   memset(cpu, 0, sizeof(cpu)); memset(gpu, 0, sizeof(gpu));
   arena.mem_count = 3; arena.map_flags = NVKMD_MEM_MAP_WR;
   device.pdev.debug_flags = debug; device.pdev.info.nc_atom_size_B = 64;
   for (unsigned i = 0; i < 3; i++) {
      backing[i] = (struct nvkmd_mem) { .flags = flags, .map = cpu[i] };
      backing[i].gpu = nvkmd_mem_is_cpu_cacheable(&backing[i]) ? gpu[i] : cpu[i];
      arena.mem[i] = (struct nvk_arena_mem) {
         .mem = &backing[i], .addr = 0x100000 + nvk_contiguous_mem_arena_mem_offset_B(i),
         .dirty_start_B = NVK_ARENA_MEM_CLEAN, .dirty_end_B = 0,
         .track_dirty_range = nvk_arena_mem_should_track_dirty(&backing[i], debug),
      };
   }
   publication = &arena.map_dirty;
   cas_calls = publications = consumes = range_resets = sync_requests = cpu_flushes = 0;
}

static void write_desc(unsigned i)
{
   memset(cpu[0] + i * 32, 0x5a, 32);
   nvk_contiguous_mem_arena_set_map_dirty_offset(&arena, i * 32, 32);
}

int main(void)
{
#ifdef NVK_TEST_LEGACY_ATOMICS
   reset(NVKMD_MEM_CPU_UNCACHED, 0);
   assert(arena.mem[0].track_dirty_range);
   for (unsigned i = 0; i < 1024; i++) write_desc(i);
   assert(cas_calls == 1025 && publications == 1024);
   nvk_mem_arena_flush_map_locked(&device, &arena);
   assert(consumes == 1 && sync_requests == 1 && cpu_flushes == 0);
   puts("PASS: legacy atomic backends retain conservative range tracking");
   return 0;
#endif
   /* Cache policy is CPU-specific: GPU-uncached does NOT skip CPU maintenance. */
   const enum nvkmd_mem_flags flags[] = {
      NVKMD_MEM_LOCAL, NVKMD_MEM_CPU_UNCACHED, NVKMD_MEM_COHERENT,
      NVKMD_MEM_GPU_UNCACHED, NVKMD_MEM_CPU_UNCACHED | NVKMD_MEM_GPU_UNCACHED,
   };
   for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
      reset(flags[i], 0);
      assert(arena.mem[0].track_dirty_range == nvkmd_mem_is_cpu_cacheable(&backing[0]));
      assert(nvk_arena_mem_should_track_dirty(&backing[0], NVK_DEBUG_TRACK_UNCACHED_DIRTY));
   }

   reset(NVKMD_MEM_CPU_UNCACHED, 0);
   for (unsigned i = 0; i < 1024; i++) write_desc(i);
   assert(cas_calls == 0 && publications == 1024 && arena.map_dirty == 1);
   nvk_mem_arena_flush_map_locked(&device, &arena);
   assert(consumes == 1 && arena.map_dirty == 0 && range_resets == 0);
   assert(sync_requests == 0 && cpu_flushes == 0);
   for (unsigned i = 0; i < 32768; i++) assert(backing[0].gpu[i] == 0x5a);

   reset(NVKMD_MEM_CPU_UNCACHED, NVK_DEBUG_TRACK_UNCACHED_DIRTY);
   for (unsigned i = 0; i < 1024; i++) write_desc(i);
   assert(cas_calls == 1025 && publications == 1024);
   nvk_mem_arena_flush_map_locked(&device, &arena);
   assert(consumes == 1 && sync_requests == 1 && cpu_flushes == 0);

   for (unsigned i = 0; i < 2; i++) {
      reset(i ? NVKMD_MEM_GPU_UNCACHED : NVKMD_MEM_LOCAL, 0);
      uint8_t a[16]; memset(a, 0x71, sizeof(a));
      nvk_mem_arena_copy_to_gpu(&arena, arena.mem[0].addr + 17, a, sizeof(a));
      nvk_mem_arena_copy_to_gpu(&arena, arena.mem[0].addr + 129, a, sizeof(a));
      assert(backing[0].gpu[17] == 0 && arena.map_dirty == 1);
      nvk_mem_arena_flush_map_locked(&device, &arena);
      assert(cpu_flushes == 1 && last_offset == 0 && last_range == 192);
      assert(backing[0].gpu[17] == 0x71 && backing[0].gpu[129] == 0x71);
      assert(arena.mem[0].dirty_start_B == NVK_ARENA_MEM_CLEAN && arena.mem[0].dirty_end_B == 0);
      nvk_mem_arena_flush_map_locked(&device, &arena);
      assert(cpu_flushes == 1); /* no second flush without another write */
   }

   reset(NVKMD_MEM_CPU_UNCACHED, NVK_DEBUG_FULL_ARENA_FLUSH | NVK_DEBUG_VM);
   write_desc(0);
   nvk_mem_arena_flush_map_locked(&device, &arena);
   assert(sync_requests == 3 && cpu_flushes == 0);
   assert(arena.flush_count == 1 && arena.flushed_B == 256 * 1024);

   /* Cross-BO copies and writes with both cached and uncached backing. */
   reset(NVKMD_MEM_LOCAL, 0);
   backing[1].flags = NVKMD_MEM_CPU_UNCACHED;
   backing[1].gpu = backing[1].map;
   arena.mem[1].track_dirty_range = nvk_arena_mem_should_track_dirty(&backing[1], 0);
   uint8_t *source = malloc(70000); assert(source);
   for (unsigned i = 0; i < 70000; i++) source[i] = (uint8_t)(i * 37 + 9);
   nvk_mem_arena_copy_to_gpu(&arena, arena.mem[0].addr + 65000, source, 70000);
   assert(publications == 1 && arena.map_dirty == 1);
   nvk_mem_arena_flush_map_locked(&device, &arena);
   assert(cpu_flushes == 2 && sync_requests == 2);
   for (unsigned i = 0; i < 70000; i++) {
      unsigned off = 65000 + i;
      unsigned slot = nvk_contiguous_mem_arena_find_mem_by_offset(&arena, off);
      assert(backing[slot].gpu[off - nvk_contiguous_mem_arena_mem_offset_B(slot)] == source[i]);
   }
   nvk_mem_arena_set_map_dirty(&arena, arena.mem[2].addr + 1, 64);
   nvk_mem_arena_flush_map_locked(&device, &arena);
   assert(cpu_flushes == 3 && last_offset == 0 && last_range == 128);
   free(source);
   puts("PASS: arena publication preserved, 1025 -> 0 range CAS for 1024 uncached writes; cached ranges still flushed");
   return 0;
}
