/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host fault injection for the actual queue_present body. Only the libnx/WSI
 * plumbing is mocked; this does NOT exercise GPU or BufferQueue behavior. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <switch/kernel/svc.h>
#include <switch/result.h>

typedef struct { uint32_t id, value; } NvFence;
typedef struct { NvFence fence; } NvMultiFence;
struct wsi_vi_syncpt { uint32_t id, value; };
struct wsi_image { void *cpu_map; uint32_t row_pitches[1]; };
struct wsi_device {
   PFN_vkWaitForFences WaitForFences;
   struct {
      bool (*get_fence_syncpt)(VkDevice, VkFence, struct wsi_vi_syncpt *);
   } vi;
};
struct wsi_swapchain {
   uint32_t image_count;
   VkDevice device;
   VkFence *fences;
   struct wsi_device *wsi;
};
struct wsi_switch_swapchain {
   struct wsi_swapchain base;
   void *window;
   struct { struct wsi_image base; } images[1];
   bool zero_copy, cpu_present, debug;
   uint8_t *scanout_cpu;
   size_t fb_size;
   int dequeued_slot;
   struct {
      uint64_t fence_blocks, swizzle_ns, syncpt_handoffs, frames;
   } stats;
};

static VkResult wait_result;
static Result queue_result;
static bool handoff_result;
static unsigned waits, queues, swizzles, flushes, syncpt_queries;
static bool queued_with_fence;
static VkFence fence;
static struct wsi_device wsi;
static struct wsi_switch_swapchain chain;
static uint8_t framebuffer[64];

static VKAPI_ATTR VkResult VKAPI_CALL
wait_for_fences(VkDevice device, uint32_t count, const VkFence *fences,
                VkBool32 wait_all, uint64_t timeout)
{
   assert(device == chain.base.device);
   assert(count == 1 && fences[0] == fence && wait_all);
   assert(timeout == UINT64_MAX);
   waits++;
   return wait_result;
}

static bool
get_syncpt(VkDevice device, VkFence f, struct wsi_vi_syncpt *out)
{
   assert(device == chain.base.device && f == fence);
   syncpt_queries++;
   out->id = 7;
   out->value = 99;
   return handoff_result;
}

static void
nvMultiFenceCreate(NvMultiFence *out, const NvFence *in)
{
   out->fence = *in;
}

static Result
nwindowQueueBuffer(void *window, uint32_t index, const NvMultiFence *render)
{
   assert(window == chain.window && index == 0);
   queues++;
   queued_with_fence = render != NULL;
   if (render)
      assert(render->fence.id == 7 && render->fence.value == 99);
   return queue_result;
}

static void
swizzle_to_scanout(struct wsi_switch_swapchain *c, uint32_t index,
                   const uint8_t *src, uint32_t stride)
{
   assert(c == &chain && index == 0);
   (void)src; (void)stride;
   swizzles++;
}

static void
armDCacheFlush(void *fb, size_t size)
{
   assert(fb == framebuffer && size == sizeof(framebuffer));
   flushes++;
}

static uint64_t
os_time_get_nano(void)
{
   static uint64_t clock;
   return ++clock;
}

static VkResult
wsi_switch_window_error(const struct wsi_switch_swapchain *c)
{
   assert(c == &chain);
   return VK_ERROR_SURFACE_LOST_KHR;
}

/* Generated verbatim from wsi_switch.c by run-host-tests.sh. */
#include "wsi_switch_queue_present.inc"

static void
reset(void)
{
   waits = queues = swizzles = flushes = syncpt_queries = 0;
   wait_result = VK_SUCCESS;
   queue_result = 0;
   handoff_result = queued_with_fence = false;
   fence = (VkFence)(uintptr_t)1;
   wsi = (struct wsi_device) {
      .WaitForFences = wait_for_fences,
      .vi.get_fence_syncpt = get_syncpt,
   };
   chain = (struct wsi_switch_swapchain) {
      .base = { .image_count = 1, .fences = &fence, .wsi = &wsi },
      .window = &wsi,
      .zero_copy = true,
      .scanout_cpu = framebuffer,
      .fb_size = sizeof(framebuffer),
      .dequeued_slot = 0,
   };
}

static VkResult
present(void)
{
   return wsi_switch_swapchain_queue_present(&chain.base, 0, 0, NULL);
}

int
main(void)
{
   const VkResult errors[] = {
      VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_TIMEOUT,
   };
   for (unsigned copy = 0; copy < 2; copy++) {
      for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
         reset();
         chain.zero_copy = !copy;
         wait_result = errors[i];
         assert(present() == errors[i]);
         assert(waits == 1 && queues == 0 && swizzles == 0 && flushes == 0);
         assert(chain.dequeued_slot == 0 && chain.stats.frames == 0);
      }
   }

   reset();
   assert(present() == VK_SUCCESS);
   assert(waits == 1 && queues == 1 && !queued_with_fence);
   assert(chain.dequeued_slot == -1 && chain.stats.frames == 1);
   assert(swizzles == 0 && flushes == 0);

   reset();
   handoff_result = true;
   assert(present() == VK_SUCCESS);
   assert(syncpt_queries == 1 && waits == 0 && queues == 1 && queued_with_fence);
   assert(chain.stats.syncpt_handoffs == 1);

   reset();
   chain.cpu_present = true;
   handoff_result = true;
   assert(present() == VK_SUCCESS);
   assert(syncpt_queries == 0 && waits == 1 && queues == 1 && !queued_with_fence);

   reset();
   chain.zero_copy = false;
   chain.debug = true;
   assert(present() == VK_SUCCESS);
   assert(waits == 1 && queues == 1 && swizzles == 1 && flushes == 1);
   assert(chain.stats.swizzle_ns > 0);

   reset();
   fence = VK_NULL_HANDLE;
   assert(present() == VK_SUCCESS);
   assert(syncpt_queries == 0 && waits == 0 && queues == 1);

   reset();
   chain.window = NULL;
   assert(present() == VK_ERROR_SURFACE_LOST_KHR);
   assert(waits == 0 && queues == 0);

   reset();
   queue_result = 1;
   assert(present() == VK_ERROR_SURFACE_LOST_KHR);
   assert(queues == 1 && chain.dequeued_slot == 0 && chain.stats.frames == 0);

   puts("PASS: present errors stop copies/queueing; successful syncpoint handoff stays asynchronous");
   return 0;
}
