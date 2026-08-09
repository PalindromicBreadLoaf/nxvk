/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nvkmd_nvgpu.h"

#include "util/macros.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"

#include <stdint.h>

#include <switch/nvidia/fence.h>
#include <switch/result.h>

/* A binary vk_sync backed by an nvgpu channel completion NvFence.
 *
 * RESET     no fence has been routed in yet.
 * SUBMITTED holds a valid NvFence.
 * SIGNALED  known complete.
 */
enum nvkmd_nvgpu_sync_state {
   NVKMD_NVGPU_SYNC_RESET,
   NVKMD_NVGPU_SYNC_SUBMITTED,
   NVKMD_NVGPU_SYNC_SIGNALED,
};

struct nvkmd_nvgpu_syncobj {
   struct vk_sync base;

   simple_mtx_t mutex;
   enum nvkmd_nvgpu_sync_state state;
   NvFence fence;
};

static struct nvkmd_nvgpu_syncobj *
to_nvgpu_syncobj(struct vk_sync *sync)
{
   assert(sync->type == &nvkmd_nvgpu_syncobj_type);
   return container_of(sync, struct nvkmd_nvgpu_syncobj, base);
}

void
nvkmd_nvgpu_syncobj_set_fence(struct vk_sync *sync, const NvFence *fence)
{
   struct nvkmd_nvgpu_syncobj *syncobj = to_nvgpu_syncobj(sync);

   simple_mtx_lock(&syncobj->mutex);
   if (fence != NULL) {
      syncobj->fence = *fence;
      syncobj->state = NVKMD_NVGPU_SYNC_SUBMITTED;
   } else {
      syncobj->state = NVKMD_NVGPU_SYNC_SIGNALED;
   }
   simple_mtx_unlock(&syncobj->mutex);
}

static VkResult
nvkmd_nvgpu_syncobj_init(struct vk_device *device,
                         struct vk_sync *sync,
                         uint64_t initial_value)
{
   struct nvkmd_nvgpu_syncobj *syncobj = to_nvgpu_syncobj(sync);

   simple_mtx_init(&syncobj->mutex, mtx_plain);
   syncobj->state = initial_value ? NVKMD_NVGPU_SYNC_SIGNALED
                                  : NVKMD_NVGPU_SYNC_RESET;

   return VK_SUCCESS;
}

static void
nvkmd_nvgpu_syncobj_finish(struct vk_device *device,
                           struct vk_sync *sync)
{
   struct nvkmd_nvgpu_syncobj *syncobj = to_nvgpu_syncobj(sync);

   simple_mtx_destroy(&syncobj->mutex);
}

static VkResult
nvkmd_nvgpu_syncobj_reset(struct vk_device *device,
                          struct vk_sync *sync)
{
   struct nvkmd_nvgpu_syncobj *syncobj = to_nvgpu_syncobj(sync);

   simple_mtx_lock(&syncobj->mutex);
   syncobj->state = NVKMD_NVGPU_SYNC_RESET;
   simple_mtx_unlock(&syncobj->mutex);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvgpu_syncobj_wait(struct vk_device *device,
                         struct vk_sync *sync,
                         uint64_t wait_value,
                         enum vk_sync_wait_flags wait_flags,
                         uint64_t abs_timeout_ns)
{
   struct nvkmd_nvgpu_syncobj *syncobj = to_nvgpu_syncobj(sync);

   /* Snapshot the state and fence under the lock so a concurrent submitter
    * routing in a new fence never stalls behind our wait.
    */
   simple_mtx_lock(&syncobj->mutex);
   const enum nvkmd_nvgpu_sync_state state = syncobj->state;
   const NvFence fence = syncobj->fence;
   simple_mtx_unlock(&syncobj->mutex);

   if (state == NVKMD_NVGPU_SYNC_SIGNALED)
      return VK_SUCCESS;

   /* WAIT_PENDING only asks that the work be in flight. */
   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return state == NVKMD_NVGPU_SYNC_RESET ? VK_TIMEOUT : VK_SUCCESS;

   /* Submits are flushed synchronously. */
   if (state == NVKMD_NVGPU_SYNC_RESET)
      return VK_TIMEOUT;
   
   for (;;) {
      const uint64_t now_ns = os_time_get_nano();
      uint64_t rel_us = now_ns < abs_timeout_ns
                        ? (abs_timeout_ns - now_ns) / 1000 : 0;
      if (rel_us > (uint64_t)INT32_MAX)
         rel_us = INT32_MAX;

      NvFence f = fence;
      Result rc = nvFenceWait(&f, (s32)rel_us);
      if (R_SUCCEEDED(rc)) {
         simple_mtx_lock(&syncobj->mutex);
         if (syncobj->state == NVKMD_NVGPU_SYNC_SUBMITTED)
            syncobj->state = NVKMD_NVGPU_SYNC_SIGNALED;
         simple_mtx_unlock(&syncobj->mutex);
         return VK_SUCCESS;
      }

      if (os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;
   }
}

const struct vk_sync_type nvkmd_nvgpu_syncobj_type = {
   .size = sizeof(struct nvkmd_nvgpu_syncobj),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_GPU_MULTI_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = nvkmd_nvgpu_syncobj_init,
   .finish = nvkmd_nvgpu_syncobj_finish,
   .reset = nvkmd_nvgpu_syncobj_reset,
   .wait = nvkmd_nvgpu_syncobj_wait,
};
