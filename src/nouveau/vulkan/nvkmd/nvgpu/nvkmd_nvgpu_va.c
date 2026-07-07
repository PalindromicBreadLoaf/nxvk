/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvgpu.h"

#include "util/u_memory.h"
#include "vk_log.h"

VkResult
nvkmd_nvgpu_alloc_va(struct nvkmd_dev *_dev,
                     struct vk_object_base *log_obj,
                     enum nvkmd_va_flags flags, uint8_t pte_kind,
                     uint64_t size_B, uint64_t align_B,
                     uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   /* TODO: sub-allocate an address range from the device VA arena. */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
nvkmd_nvgpu_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_nvgpu_va *va = nvkmd_nvgpu_va(_va);

   FREE(va);
}

static VkResult
nvkmd_nvgpu_va_bind_mem(struct nvkmd_va *_va,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        struct nvkmd_mem *_mem,
                        uint64_t mem_offset_B,
                        uint64_t range_B)
{
   /* TODO: FIXED map the nvmap inside the arena at the small page size. */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_va_unbind(struct nvkmd_va *_va,
                      struct vk_object_base *log_obj,
                      uint64_t va_offset_B,
                      uint64_t range_B)
{
   /* TODO: unmap the arena range. */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

const struct nvkmd_va_ops nvkmd_nvgpu_va_ops = {
   .free = nvkmd_nvgpu_va_free,
   .bind_mem = nvkmd_nvgpu_va_bind_mem,
   .unbind = nvkmd_nvgpu_va_unbind,
};
