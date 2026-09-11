/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Does submit cost scale with the descriptor table's size.
 */
#include "nvk_harness.h"

#include <stdlib.h>

#define VIEWS       24576u   /* image views created to grow the table */
#define MIN_VIEWS   4096u    /* below this the growth is too small to judge */
#define SUBMITS     64u      /* timed submits per phase */
#define FILL_BYTES  4096u
#define PATTERN     0x0FEDCBA9u

/* Submit is a handful of microseconds itself. */
#define SLACK_US    100u

static uint64_t now_us(void)
{
   return armTicksToNs(armGetSystemTick()) / 1000ull;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_desc_flush.log");
   LOG("=== nvk_desc_flush ===");
   LOG("params: views=%u submits/phase=%u", VIEWS, SUBMITS);

   VkBuffer buf = VK_NULL_HANDLE;
   VkDeviceMemory bmem = VK_NULL_HANDLE, imem = VK_NULL_HANDLE;
   VkImage img = VK_NULL_HANDLE;
   VkImageView *view = NULL;
   uint32_t views = 0;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   uint64_t a_total_us = 0, a_min_us = UINT64_MAX;
   uint64_t b_total_us = 0, b_min_us = UINT64_MAX;
   uint64_t first_after_write_us = 0;

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_DEV(&c, CreateBuffer);
   LOAD_DEV(&c, GetBufferMemoryRequirements);
   LOAD_DEV(&c, BindBufferMemory);
   LOAD_DEV(&c, CreateImage);
   LOAD_DEV(&c, GetImageMemoryRequirements);
   LOAD_DEV(&c, BindImageMemory);
   LOAD_DEV(&c, CreateImageView);
   LOAD_DEV(&c, AllocateMemory);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, AllocateCommandBuffers);
   LOAD_DEV(&c, BeginCommandBuffer);
   LOAD_DEV(&c, CmdFillBuffer);
   LOAD_DEV(&c, EndCommandBuffer);
   LOAD_DEV(&c, CreateFence);
   LOAD_DEV(&c, ResetFences);
   LOAD_DEV(&c, QueueSubmit);
   LOAD_DEV(&c, WaitForFences);
   if (!CreateBuffer || !CreateImage || !CreateImageView || !AllocateMemory ||
       !CmdFillBuffer || !CreateFence || !ResetFences || !QueueSubmit ||
       !WaitForFences) { LOG("FAIL: missing device entrypoints"); goto done; }

   /* A trivial unit of GPU work, so every submit has a command buffer and
    * therefore flushes the descriptor tables.
    */
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = FILL_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (CreateBuffer(c.dev, &bci, NULL, &buf) != VK_SUCCESS) { LOG("FAIL buffer"); goto done; }

   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(c.dev, buf, &mr);
   uint32_t mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt == UINT32_MAX) { LOG("FAIL: no device-local type"); goto done; }

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   if (AllocateMemory(c.dev, &mai, NULL, &bmem) != VK_SUCCESS) { LOG("FAIL buffer mem"); goto done; }
   if (BindBufferMemory(c.dev, buf, bmem, 0) != VK_SUCCESS) { LOG("FAIL bind buffer"); goto done; }

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { 64, 64, 1 },
      .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (CreateImage(c.dev, &ici, NULL, &img) != VK_SUCCESS) { LOG("FAIL image"); goto done; }

   GetImageMemoryRequirements(c.dev, img, &mr);
   mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mt == UINT32_MAX) { LOG("FAIL: no device-local type"); goto done; }
   mai.allocationSize = mr.size;
   mai.memoryTypeIndex = mt;
   if (AllocateMemory(c.dev, &mai, NULL, &imem) != VK_SUCCESS) { LOG("FAIL image mem"); goto done; }
   if (BindImageMemory(c.dev, img, imem, 0) != VK_SUCCESS) { LOG("FAIL bind image"); goto done; }

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = c.qfi,
   };
   if (CreateCommandPool(c.dev, &pci, NULL, &pool) != VK_SUCCESS) { LOG("FAIL pool"); goto done; }

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   if (AllocateCommandBuffers(c.dev, &cbai, &cb) != VK_SUCCESS) { LOG("FAIL cb"); goto done; }

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   BeginCommandBuffer(cb, &cbbi);
   CmdFillBuffer(cb, buf, 0, FILL_BYTES, PATTERN);
   if (EndCommandBuffer(cb) != VK_SUCCESS) { LOG("FAIL end cb"); goto done; }

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   if (CreateFence(c.dev, &fci, NULL, &fence) != VK_SUCCESS) { LOG("FAIL fence"); goto done; }

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb,
   };

#define SUBMIT_ONCE(out_us) do {                                                 \
      ResetFences(c.dev, 1, &fence);                                             \
      const uint64_t _t0 = now_us();                                             \
      VkResult _r = QueueSubmit(c.queue, 1, &si, fence);                         \
      (out_us) = now_us() - _t0;                                                 \
      if (_r != VK_SUCCESS) { LOG("FAIL vkQueueSubmit -> %d", _r); goto done; }  \
      _r = WaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX);                 \
      if (_r != VK_SUCCESS) { LOG("FAIL vkWaitForFences -> %d", _r); goto done; }\
   } while (0)

   uint64_t us;

   for (uint32_t i = 0; i < 8; i++)
      SUBMIT_ONCE(us);
   for (uint32_t i = 0; i < SUBMITS; i++) {
      SUBMIT_ONCE(us);
      a_total_us += us;
      if (us < a_min_us) a_min_us = us;
   }
   LOG("phase A: %llu us mean, %llu us min",
       (unsigned long long)(a_total_us / SUBMITS), (unsigned long long)a_min_us);

   view = calloc(VIEWS, sizeof(*view));
   if (view == NULL) { LOG("FAIL: out of host memory for the view array"); goto done; }

   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1, .layerCount = 1,
      },
   };
   const uint64_t grow_t0 = now_us();
   for (views = 0; views < VIEWS; views++) {
      if (CreateImageView(c.dev, &vci, NULL, &view[views]) != VK_SUCCESS)
         break;
   }
   LOG("created %u/%u image views in %llu us", views, VIEWS,
       (unsigned long long)(now_us() - grow_t0));
   if (views < MIN_VIEWS) {
      LOG("=== nvk_desc_flush INCONCLUSIVE: only %u views ===", views);
      goto done;
   }

   SUBMIT_ONCE(first_after_write_us);

   for (uint32_t i = 0; i < 8; i++)
      SUBMIT_ONCE(us);
   for (uint32_t i = 0; i < SUBMITS; i++) {
      SUBMIT_ONCE(us);
      b_total_us += us;
      if (us < b_min_us) b_min_us = us;
   }
   LOG("phase B:   %llu us mean, %llu us min",
       (unsigned long long)(b_total_us / SUBMITS), (unsigned long long)b_min_us);
   LOG("first submit after writing %u descriptors: %llu us",
       views, (unsigned long long)first_after_write_us);

#undef SUBMIT_ONCE

   const uint64_t a_mean = a_total_us / SUBMITS;
   const uint64_t b_mean = b_total_us / SUBMITS;

   if (b_mean <= a_mean * 2 + SLACK_US) {
      LOG("FLUSH OK: submit cost is flat across a %ux bigger table (%llu -> %llu us)",
          1u + views / 1024u, (unsigned long long)a_mean, (unsigned long long)b_mean);
      LOG("=== nvk_desc_flush PASSED ===");
   } else {
      LOG("FLUSH FAIL: submit went %llu -> %llu us with no descriptors written",
          (unsigned long long)a_mean, (unsigned long long)b_mean);
      LOG("  the flush still scales with the table, not with what changed");
      LOG("=== nvk_desc_flush FAILED ===");
   }

done:
   if (c.dev) {
      LOAD_DEV(&c, DeviceWaitIdle);
      LOAD_DEV(&c, DestroyFence);
      LOAD_DEV(&c, DestroyCommandPool);
      LOAD_DEV(&c, DestroyImageView);
      LOAD_DEV(&c, DestroyImage);
      LOAD_DEV(&c, DestroyBuffer);
      LOAD_DEV(&c, FreeMemory);
      if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
      if (fence) DestroyFence(c.dev, fence, NULL);
      if (pool) DestroyCommandPool(c.dev, pool, NULL);
      for (uint32_t i = 0; i < views; i++)
         if (view[i]) DestroyImageView(c.dev, view[i], NULL);
      if (img) DestroyImage(c.dev, img, NULL);
      if (imem) FreeMemory(c.dev, imem, NULL);
      if (buf) DestroyBuffer(c.dev, buf, NULL);
      if (bmem) FreeMemory(c.dev, bmem, NULL);
   }
   free(view);
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
