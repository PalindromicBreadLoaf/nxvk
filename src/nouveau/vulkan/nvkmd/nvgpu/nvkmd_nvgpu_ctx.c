/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvgpu.h"

#include "util/u_memory.h"
#include "vk_log.h"

static VkResult
nvkmd_nvgpu_create_exec_ctx(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            enum nvkmd_engines engines,
                            struct nvkmd_ctx **ctx_out)
{
   /* TODO: create a real NvGpuChannel (+ Zcull + builtin fence cmdbuf). */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
nvkmd_nvgpu_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvgpu_exec_ctx *ctx = nvkmd_nvgpu_exec_ctx(_ctx);

   FREE(ctx);
}

static VkResult
nvkmd_nvgpu_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t wait_count,
                          const struct vk_sync_wait *waits)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t exec_count,
                          const struct nvkmd_ctx_exec *execs)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t signal_count,
                            const struct vk_sync_signal *signals)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

const struct nvkmd_ctx_ops nvkmd_nvgpu_exec_ctx_ops = {
   .destroy = nvkmd_nvgpu_exec_ctx_destroy,
   .wait = nvkmd_nvgpu_exec_ctx_wait,
   .exec = nvkmd_nvgpu_exec_ctx_exec,
   .signal = nvkmd_nvgpu_exec_ctx_signal,
   .flush = nvkmd_nvgpu_exec_ctx_flush,
   .sync = nvkmd_nvgpu_exec_ctx_sync,
};

static VkResult
nvkmd_nvgpu_create_bind_ctx(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            struct nvkmd_ctx **ctx_out)
{
   /* TODO: bind ops go straight through the address space ioctls. */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
nvkmd_nvgpu_bind_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvgpu_bind_ctx *ctx = nvkmd_nvgpu_bind_ctx(_ctx);

   FREE(ctx);
}

static VkResult
nvkmd_nvgpu_bind_ctx_wait(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t wait_count,
                          const struct vk_sync_wait *waits)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t bind_count,
                          const struct nvkmd_ctx_bind *binds)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t signal_count,
                            const struct vk_sync_signal *signals)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
nvkmd_nvgpu_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

const struct nvkmd_ctx_ops nvkmd_nvgpu_bind_ctx_ops = {
   .destroy = nvkmd_nvgpu_bind_ctx_destroy,
   .wait = nvkmd_nvgpu_bind_ctx_wait,
   .bind = nvkmd_nvgpu_bind_ctx_bind,
   .signal = nvkmd_nvgpu_bind_ctx_signal,
   .flush = nvkmd_nvgpu_bind_ctx_flush,
};

VkResult
nvkmd_nvgpu_create_ctx(struct nvkmd_dev *dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_engines engines,
                       struct nvkmd_ctx **ctx_out)
{
   if (engines == NVKMD_ENGINE_BIND) {
      return nvkmd_nvgpu_create_bind_ctx(dev, log_obj, ctx_out);
   } else {
      assert(!(engines & NVKMD_ENGINE_BIND));
      return nvkmd_nvgpu_create_exec_ctx(dev, log_obj, engines, ctx_out);
   }
}
