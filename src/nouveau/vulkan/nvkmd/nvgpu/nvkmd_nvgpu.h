/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_NVGPU_H
#define NVKMD_NVGPU_H 1

#include "nvkmd/nvkmd.h"
#include "util/simple_mtx.h"
#include "util/vma.h"
#include "vk_sync.h"

#include <switch/nvidia/address_space.h>
#include <switch/nvidia/fence.h>
#include <switch/nvidia/gpu_channel.h>
#include <switch/nvidia/map.h>

/* 4 KiB granularity. */
#define NVKMD_NVGPU_SMALL_PAGE_SIZE_B ((uint64_t)0x1000)

/* Size of the small-page VA arena reserved at device init. */
#define NVKMD_NVGPU_VA_ARENA_SIZE_B ((uint64_t)8 << 30)

struct nvkmd_nvgpu_pdev {
   struct nvkmd_pdev base;

   struct vk_sync_type syncobj_sync_type;
   const struct vk_sync_type *sync_types[2];
};

NVKMD_DECL_SUBCLASS(pdev, nvgpu);

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
};

NVKMD_DECL_SUBCLASS(dev, nvgpu);

VkResult nvkmd_nvgpu_create_dev(struct nvkmd_pdev *pdev,
                                struct vk_object_base *log_obj,
                                struct nvkmd_dev **dev_out);

struct nvkmd_nvgpu_mem {
   struct nvkmd_mem base;

   NvMap nvmap;
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
