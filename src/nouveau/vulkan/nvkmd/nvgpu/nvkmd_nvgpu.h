/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NVKMD_NVGPU_H
#define NVKMD_NVGPU_H 1

#include "nvkmd/nvkmd.h"
#include "util/simple_mtx.h"
#include "util/vma.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"

#include <switch/nvidia/address_space.h>
#include <switch/nvidia/fence.h>
#include <switch/nvidia/gpu_channel.h>
#include <switch/nvidia/map.h>

/* 4 KiB granularity. */
#define NVKMD_NVGPU_SMALL_PAGE_SIZE_B ((uint64_t)0x1000)

/* Size of the small-page VA arena reserved at device init. */
#define NVKMD_NVGPU_VA_ARENA_SIZE_B ((uint64_t)8 << 30)

/* Top slice of the arena. */
#define NVKMD_NVGPU_REPLAY_HEAP_SIZE_B ((uint64_t)1 << 30)

/* GPU submit wait bound */
#define NVGPU_SUBMIT_TIMEOUT_US 10000000

/* Bounds on the cache of freed backing stores. */
#define NVKMD_NVGPU_MEM_CACHE_MAX_B ((uint64_t)64 << 20)
#define NVKMD_NVGPU_MEM_CACHE_MAX_ENTRIES 128

struct nvkmd_nvgpu_pdev {
   struct nvkmd_pdev base;

   /* Timeline semaphores are emulated on top of the binary NvFence syncobj. */
   struct vk_sync_timeline_type syncobj_timeline_type;
   const struct vk_sync_type *sync_types[3];
};

NVKMD_DECL_SUBCLASS(pdev, nvgpu);

/* Binary vk_sync backed by an nvgpu channel completion NvFence. */
extern const struct vk_sync_type nvkmd_nvgpu_syncobj_type;

/* Route a channel completion fence into a signalled syncobj. */
void nvkmd_nvgpu_syncobj_set_fence(struct vk_sync *sync,
                                   NvGpuChannel *channel,
                                   const NvFence *fence);

/* Whether a vk_sync can be resolved to the syncpoint threshold that releases
 * it.
 */
enum nvkmd_nvgpu_fence_state {
   NVKMD_NVGPU_FENCE_UNKNOWN,
   NVKMD_NVGPU_FENCE_SIGNALED,
   NVKMD_NVGPU_FENCE_PENDING,
};

enum nvkmd_nvgpu_fence_state
nvkmd_nvgpu_syncobj_get_fence(struct vk_sync *sync, NvFence *fence_out);

VkResult nvkmd_nvgpu_try_create_pdev(struct vk_object_base *log_obj,
                                     enum nvk_debug debug_flags,
                                     struct nvkmd_pdev **pdev_out);

struct nvkmd_nvgpu_dev {
   struct nvkmd_dev base;

   NvAddressSpace addr_space;

   iova_t va_arena_addr;
   uint64_t va_arena_size_B;

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;

   /* LRU cache of freed backing stores */
   simple_mtx_t mem_cache_mutex;
   struct list_head mem_cache;
   uint64_t mem_cache_size_B;
   uint32_t mem_cache_count;
   uint64_t mem_cache_hits;
   uint64_t mem_cache_misses;
};

NVKMD_DECL_SUBCLASS(dev, nvgpu);

VkResult nvkmd_nvgpu_create_dev(struct nvkmd_pdev *pdev,
                                struct vk_object_base *log_obj,
                                struct nvkmd_dev **dev_out);

struct nvkmd_nvgpu_mem {
   struct nvkmd_mem base;

   NvMap nvmap;

   bool published;
};

NVKMD_DECL_SUBCLASS(mem, nvgpu);

VkResult nvkmd_nvgpu_alloc_mem(struct nvkmd_dev *dev,
                               struct vk_object_base *log_obj,
                               uint64_t size_B, uint64_t align_B,
                               enum nvkmd_mem_flags flags,
                               struct nvkmd_mem **mem_out);

VkResult nvkmd_nvgpu_alloc_tiled_mem(struct nvkmd_dev *dev,
                                     struct vk_object_base *log_obj,
                                     uint64_t size_B, uint64_t align_B,
                                     uint8_t pte_kind, uint16_t tile_mode,
                                     enum nvkmd_mem_flags flags,
                                     struct nvkmd_mem **mem_out);

VkResult nvkmd_nvgpu_import_dma_buf(struct nvkmd_dev *dev,
                                    struct vk_object_base *log_obj,
                                    int fd, struct nvkmd_mem **mem_out);

/* Release every backing store the cache is holding. */
void nvkmd_nvgpu_mem_cache_trim(struct nvkmd_nvgpu_dev *dev);

struct nvkmd_nvgpu_va {
   struct nvkmd_va base;
};

NVKMD_DECL_SUBCLASS(va, nvgpu);

VkResult nvkmd_nvgpu_alloc_va(struct nvkmd_dev *dev,
                              struct vk_object_base *log_obj,
                              enum nvkmd_va_flags flags, uint8_t pte_kind,
                              uint64_t size_B, uint64_t align_B,
                              uint64_t fixed_addr, struct nvkmd_va **va_out);

struct nvkmd_nvgpu_exec_ctx {
   struct nvkmd_ctx base;

   NvGpuChannel channel;

   /* Zcull context bound to the channel. */
   struct nvkmd_mem *zcull_mem;

   /* Builtin buffer holding the 3D class bind, the two syncpt increment fence
    * cmdlists and the cache acquire cmdlist. */
   struct nvkmd_mem *fence_mem;
   iova_t bind_cmds_addr;
   uint32_t bind_cmds_dw;
   iova_t fence_cmds_addr;
   uint32_t fence_cmds_dw;
   iova_t fence_cpu_cmds_addr;
   uint32_t fence_cpu_cmds_dw;
   iova_t acquire_cmds_addr;
   uint32_t acquire_cmds_dw;
   /* One no-op dword */
   iova_t acquire_sync_addr;

   /* Ring of syncpt-wait cmdlists. */
   struct nvkmd_mem *wait_mem;
   uint64_t wait_head_B;

   bool has_pending;
   bool has_acquire;
   bool has_fence;
   bool fence_cpu_visible;
   NvFence last_fence;

   /* The latched channel fault has already been logged. */
   bool err_reported;
};

NVKMD_DECL_SUBCLASS(ctx, nvgpu_exec);

struct nvkmd_nvgpu_bind_ctx {
   struct nvkmd_ctx base;
};

NVKMD_DECL_SUBCLASS(ctx, nvgpu_bind);

VkResult nvkmd_nvgpu_create_ctx(struct nvkmd_dev *dev,
                                struct vk_object_base *log_obj,
                                enum nvkmd_engines engines,
                                struct nvkmd_ctx **ctx_out);

#endif /* NVKMD_NVGPU_H */
