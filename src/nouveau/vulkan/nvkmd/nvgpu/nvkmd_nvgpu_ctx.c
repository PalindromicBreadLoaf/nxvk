/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nvkmd_nvgpu.h"

#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include "vk_log.h"
#include "vk_sync.h"

#include <string.h>

#include <switch/nvidia/gpu.h>
#include <switch/result.h>

/* Maxwell channel GPFIFO host class (0xB06F) syncpoint methods.
 * SYNCPOINTB takes OPERATION at bit 0 and SYNCPT_INDEX at bits 19:8.
 */
#define NVGPU_HOST_SYNCPOINTA           0x0070
#define NVGPU_HOST_SYNCPOINTB_OP_INCR   (1u << 0)
#define NVGPU_HOST_SYNCPOINTB_IDX_SHIFT 8

/* GPU submit wait bound (us) */
#define NVGPU_SUBMIT_TIMEOUT_US 10000000

static uint32_t
gen_fence_cmdlist(uint32_t *cmds, uint32_t syncpt_id)
{
   cmds[0] = 0x20000000u | (2u << 16) | (NVGPU_HOST_SYNCPOINTA >> 2);
   cmds[1] = 0;
   cmds[2] = NVGPU_HOST_SYNCPOINTB_OP_INCR |
             (syncpt_id << NVGPU_HOST_SYNCPOINTB_IDX_SHIFT);
   return 3;
}

static const char *
nvkmd_nvgpu_notif_str(uint32_t info32)
{
   switch (info32) {
   case NvNotificationType_FifoErrorIdleTimeout:       return "fifo idle timeout";
   case NvNotificationType_GrErrorSwNotify:            return "gr exception";
   case NvNotificationType_GrSemaphoreTimeout:         return "gr semaphore timeout";
   case NvNotificationType_GrIllegalNotify:            return "gr illegal notify";
   case NvNotificationType_FifoErrorMmuErrFlt:         return "mmu fault";
   case NvNotificationType_PbdmaError:                 return "pbdma error";
   case NvNotificationType_ResetChannelVerifError:     return "reset channel verify error";
   case NvNotificationType_PbdmaPushbufferCrcMismatch: return "pushbuffer crc mismatch";
   default:                                            return "unknown";
   }
}

/* nvgpu latches the first fault in the channel's error notifier, resets the
 * channel, and then fails every later submit with Timeout. Report the latched
 * fault as soon as it is visible, and say where it was noticed so the batch
 * that raised it can be identified.
 */
static VkResult
nvkmd_nvgpu_report_channel_err(struct nvkmd_nvgpu_exec_ctx *ctx,
                               struct vk_object_base *log_obj,
                               const char *when)
{
   NvNotification notif = {0};

   if (R_FAILED(nvGpuChannelGetErrorNotification(&ctx->channel, &notif)) ||
       notif.info32 == 0)
      return VK_SUCCESS;

   if (ctx->err_reported)
      return VK_ERROR_DEVICE_LOST;
   ctx->err_reported = true;

   NvError err = {0};
   if (R_FAILED(nvGpuChannelGetErrorInfo(&ctx->channel, &err))) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "channel fault %s: notif=%u (%s)", when,
                       notif.info32, nvkmd_nvgpu_notif_str(notif.info32));
   }

   return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                    "channel fault %s: notif=%u (%s) type=%u info=%08x %08x "
                    "%08x %08x %08x %08x %08x %08x", when, notif.info32,
                    nvkmd_nvgpu_notif_str(notif.info32), err.type,
                    err.info[0], err.info[1], err.info[2], err.info[3],
                    err.info[4], err.info[5], err.info[6], err.info[7]);
}

/* Ramp inert fence-only kickoffs to ≥ any expected init IB size,
 * verifying each step so a stall is pinned to the size that caused it.
 */
static VkResult
nvkmd_nvgpu_warmup_channel(struct nvkmd_nvgpu_exec_ctx *ctx,
                           struct vk_object_base *log_obj)
{
   static const uint32_t ramp_dw[] =
      { 32, 128, 512, 1536, 2048, 4096, 8192 };
   const uint32_t max_dw = 8192;
   const uint32_t syncpt = nvGpuChannelGetSyncpointId(&ctx->channel);
   VkResult result;

   struct nvkmd_mem *mem;
   result = nvkmd_dev_alloc_mapped_mem(ctx->base.dev, log_obj, max_dw * 4, 0,
                                       NVKMD_MEM_LOCAL, NVKMD_MEM_MAP_WR, &mem);
   if (result != VK_SUCCESS)
      return result;

   uint32_t fence[3];
   const uint32_t fence_dw = gen_fence_cmdlist(fence, syncpt);

   uint32_t *cmds = mem->map;
   for (uint32_t i = 0; i + fence_dw <= max_dw; i += fence_dw)
      memcpy(&cmds[i], fence, fence_dw * 4);

   /* The map is CPU-cached and the host fetches the pushbuf from memory. */
   nvkmd_mem_sync_map_to_gpu(mem, 0, mem->size_B);

   for (uint32_t r = 0; r < ARRAY_SIZE(ramp_dw); r++) {
      const uint32_t fences = ramp_dw[r] / fence_dw;
      const uint32_t dw = fences * fence_dw;

      Result rc = nvGpuChannelAppendEntry(&ctx->channel, mem->va->addr, dw,
                                          GPFIFO_ENTRY_NOT_MAIN |
                                          GPFIFO_ENTRY_NO_PREFETCH, 0);
      if (R_FAILED(rc)) {
         result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                            "warmup append failed: 0x%x", (unsigned)rc);
         goto out;
      }

      for (uint32_t k = 0; k < fences; k++)
         nvGpuChannelIncrFence(&ctx->channel);

      rc = nvGpuChannelKickoff(&ctx->channel);
      if (R_FAILED(rc)) {
         nvkmd_nvgpu_report_channel_err(ctx, log_obj, "on the warmup submit");
         result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                            "warmup kickoff failed at %u dw: 0x%x", dw,
                            (unsigned)rc);
         goto out;
      }

      NvFence f;
      nvGpuChannelGetFence(&ctx->channel, &f);
      rc = nvFenceWait(&f, NVGPU_SUBMIT_TIMEOUT_US);
      if (R_FAILED(rc)) {
         nvkmd_nvgpu_report_channel_err(ctx, log_obj, "draining the warmup");
         result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                            "warmup drain failed at %u dw (id=%u val=%u): 0x%x",
                            dw, f.id, f.value, (unsigned)rc);
         goto out;
      }

      result = nvkmd_nvgpu_report_channel_err(ctx, log_obj, "during warmup");
      if (result != VK_SUCCESS) {
         result = VK_ERROR_INITIALIZATION_FAILED;
         goto out;
      }
   }

out:
   nvkmd_mem_unref(mem);
   return result;
}

static VkResult
nvkmd_nvgpu_create_exec_ctx(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            enum nvkmd_engines engines,
                            struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_dev);
   VkResult result;

   struct nvkmd_nvgpu_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_nvgpu_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvgpu_exec_ctx_ops;
   ctx->base.dev = &dev->base;

   /* The channel does not need a kernel subchannel bind per requested engine. */
   Result rc = nvGpuChannelCreate(&ctx->channel, &dev->addr_space,
                                  NvChannelPriority_Medium);
   if (R_FAILED(rc)) {
      result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                         "nvGpuChannelCreate() failed: 0x%x", (unsigned)rc);
      goto fail_ctx;
   }

   const uint32_t zcull_size_B = nvGpuGetZcullCtxSize();
   if (zcull_size_B > 0) {
      result = nvkmd_dev_alloc_mem(&dev->base, log_obj, zcull_size_B, 0,
                                   NVKMD_MEM_LOCAL, &ctx->zcull_mem);
      if (result != VK_SUCCESS)
         goto fail_channel;

      rc = nvGpuChannelZcullBind(&ctx->channel, ctx->zcull_mem->va->addr);
      if (R_FAILED(rc)) {
         result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                            "nvGpuChannelZcullBind() failed: 0x%x", (unsigned)rc);
         goto fail_zcull;
      }
   }

   result = nvkmd_dev_alloc_mapped_mem(&dev->base, log_obj,
                                       NVKMD_NVGPU_SMALL_PAGE_SIZE_B, 0,
                                       NVKMD_MEM_LOCAL, NVKMD_MEM_MAP_WR,
                                       &ctx->fence_mem);
   if (result != VK_SUCCESS)
      goto fail_zcull;

   ctx->fence_cmds_dw = gen_fence_cmdlist(ctx->fence_mem->map,
                                          nvGpuChannelGetSyncpointId(&ctx->channel));
   ctx->fence_cmds_addr = ctx->fence_mem->va->addr;

   /* Written once here, refetched from memory on every kickoff. */
   nvkmd_mem_sync_map_to_gpu(ctx->fence_mem, 0, ctx->fence_mem->size_B);

   result = nvkmd_nvgpu_warmup_channel(ctx, log_obj);
   if (result != VK_SUCCESS)
      goto fail_fence;

   *ctx_out = &ctx->base;

   return VK_SUCCESS;

fail_fence:
   nvkmd_mem_unref(ctx->fence_mem);
fail_zcull:
   if (ctx->zcull_mem != NULL)
      nvkmd_mem_unref(ctx->zcull_mem);
fail_channel:
   nvGpuChannelClose(&ctx->channel);
fail_ctx:
   FREE(ctx);
   return result;
}

static VkResult
nvkmd_nvgpu_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   struct nvkmd_nvgpu_exec_ctx *ctx = nvkmd_nvgpu_exec_ctx(_ctx);

   VkResult result =
      nvkmd_nvgpu_report_channel_err(ctx, log_obj, "before this submit");
   if (result != VK_SUCCESS)
      return result;

   /* Skip empty submits. */
   if (!ctx->has_pending)
      return VK_SUCCESS;

   nvGpuChannelIncrFence(&ctx->channel);

   Result rc = nvGpuChannelAppendEntry(&ctx->channel, ctx->fence_cmds_addr,
                                       ctx->fence_cmds_dw,
                                       GPFIFO_ENTRY_NOT_MAIN |
                                       GPFIFO_ENTRY_NO_PREFETCH, 0);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "fence cmdlist append failed: 0x%x", (unsigned)rc);
   }

   rc = nvGpuChannelKickoff(&ctx->channel);
   if (R_FAILED(rc)) {
      nvkmd_nvgpu_report_channel_err(ctx, log_obj, "on this submit");
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvGpuChannelKickoff() failed: 0x%x", (unsigned)rc);
   }

   nvGpuChannelGetFence(&ctx->channel, &ctx->last_fence);
   ctx->has_fence = true;
   ctx->has_pending = false;

   return VK_SUCCESS;
}

static void
nvkmd_nvgpu_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvgpu_exec_ctx *ctx = nvkmd_nvgpu_exec_ctx(_ctx);

   /* Drain outstanding work before tearing the channel/buffers down. */
   if (ctx->has_fence)
      nvFenceWait(&ctx->last_fence, NVGPU_SUBMIT_TIMEOUT_US);

   nvkmd_mem_unref(ctx->fence_mem);
   if (ctx->zcull_mem != NULL)
      nvkmd_mem_unref(ctx->zcull_mem);
   nvGpuChannelClose(&ctx->channel);

   FREE(ctx);
}

static VkResult
nvkmd_nvgpu_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t wait_count,
                          const struct vk_sync_wait *waits)
{
   /* CPU-block on each dependency before dependent work is submitted. */
   struct vk_device *dev = log_obj->device;
   const uint64_t abs_timeout =
      os_time_get_absolute_timeout(NVGPU_SUBMIT_TIMEOUT_US * 1000ull);

   for (uint32_t i = 0; i < wait_count; i++) {
      VkResult result = vk_sync_wait(dev, waits[i].sync, waits[i].wait_value,
                                     VK_SYNC_WAIT_COMPLETE, abs_timeout);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvgpu_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t exec_count,
                          const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_nvgpu_exec_ctx *ctx = nvkmd_nvgpu_exec_ctx(_ctx);

   for (uint32_t i = 0; i < exec_count;) {
      uint32_t run = 1;
      while (execs[i + run - 1].incomplete && i + run < exec_count)
         run++;

      /* Keep room for the trailing fence cmdlist entry. */
      if (unlikely(ctx->channel.num_entries + run >= GPFIFO_QUEUE_SIZE)) {
         VkResult result = nvkmd_nvgpu_exec_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;

         if (unlikely(run >= GPFIFO_QUEUE_SIZE)) {
            return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                             "%u chained pushes exceed the gpfifo queue", run);
         }
      }

      for (uint32_t j = 0; j < run; j++, i++) {
         /* Hardware limits shared by all current GPUs. */
         assert((execs[i].addr % 4) == 0 && (execs[i].size_B % 4) == 0);
         assert(execs[i].size_B < (1u << 23));

         /* A prefetching main entry faults NVK's streams on Tegra. */
         Result rc = nvGpuChannelAppendEntry(&ctx->channel, execs[i].addr,
                                             execs[i].size_B / 4,
                                             GPFIFO_ENTRY_NOT_MAIN |
                                             GPFIFO_ENTRY_NO_PREFETCH, 0);
         if (R_FAILED(rc)) {
            return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                             "nvGpuChannelAppendEntry() failed: 0x%x",
                             (unsigned)rc);
         }

         ctx->has_pending = true;
      }
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvgpu_signal_one(struct vk_device *dev,
                       const struct vk_sync_signal *signal,
                       const NvFence *fence)
{
   struct vk_sync_timeline *timeline = vk_sync_as_timeline(signal->sync);

   if (timeline == NULL) {
      nvkmd_nvgpu_syncobj_set_fence(signal->sync, fence);
      return VK_SUCCESS;
   }

   struct vk_sync_timeline_point *point;
   VkResult result = vk_sync_timeline_alloc_point(dev, timeline,
                                                  signal->signal_value,
                                                  &point);
   if (result != VK_SUCCESS)
      return result;

   nvkmd_nvgpu_syncobj_set_fence(&point->sync, fence);

   return vk_sync_timeline_point_install(dev, point);
}

static VkResult
nvkmd_nvgpu_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t signal_count,
                            const struct vk_sync_signal *signals)
{
   struct nvkmd_nvgpu_exec_ctx *ctx = nvkmd_nvgpu_exec_ctx(_ctx);

   VkResult result = nvkmd_nvgpu_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   /* Route the completion fence into each signalled syncobj so a later CPU
    * wait resolves against it.
    */
   const NvFence *fence = ctx->has_fence ? &ctx->last_fence : NULL;
   for (uint32_t i = 0; i < signal_count; i++) {
      result = nvkmd_nvgpu_signal_one(log_obj->device, &signals[i], fence);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvgpu_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj)
{
   struct nvkmd_nvgpu_exec_ctx *ctx = nvkmd_nvgpu_exec_ctx(_ctx);

   VkResult result = nvkmd_nvgpu_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   if (!ctx->has_fence)
      return VK_SUCCESS;

   Result rc = nvFenceWait(&ctx->last_fence, NVGPU_SUBMIT_TIMEOUT_US);
   if (R_FAILED(rc)) {
      nvkmd_nvgpu_report_channel_err(ctx, log_obj, "waiting on this submit");
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvFenceWait(id=%u val=%u) failed: 0x%x",
                       ctx->last_fence.id, ctx->last_fence.value,
                       (unsigned)rc);
   }

   /* A reset channel releases its waiters, so a successful wait proves
    * nothing. */
   return nvkmd_nvgpu_report_channel_err(ctx, log_obj,
                                         "in the work just waited on");
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
   /* Sparse binding is unused by the milestone ladder. */
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
