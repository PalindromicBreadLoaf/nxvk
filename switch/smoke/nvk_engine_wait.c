/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Engine wait.
 */
#include "nvk_harness.h"

#define PATTERN    0x5A5AC3C3u
#define BUF_BYTES  (32u * 1024u * 1024u)   /* 32 MiB per buffer */
#define CHK_BYTES  4096u                   /* tail B copies out of dst */
#define COPIES     16u                     /* chain length */

#define MIN_TOTAL_US 5000u

static uint64_t now_us(void)
{
   return armTicksToNs(armGetSystemTick()) / 1000ull;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_engine_wait.log");
   LOG("=== nvk_engine_wait ===");
   LOG("params: buf=%u MiB copies=%u chk=%u B",
       BUF_BYTES / (1024u * 1024u), COPIES, CHK_BYTES);

   VkBuffer buf[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
   VkDeviceMemory mem[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
   void *cpu[3] = { NULL, NULL, NULL };
   VkCommandPool pool = VK_NULL_HANDLE;
   VkSemaphore sem = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_DEV(&c, CreateBuffer);
   LOAD_DEV(&c, GetBufferMemoryRequirements);
   LOAD_DEV(&c, AllocateMemory);
   LOAD_DEV(&c, MapMemory);
   LOAD_DEV(&c, BindBufferMemory);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, AllocateCommandBuffers);
   LOAD_DEV(&c, BeginCommandBuffer);
   LOAD_DEV(&c, CmdFillBuffer);
   LOAD_DEV(&c, CmdCopyBuffer);
   LOAD_DEV(&c, CmdPipelineBarrier);
   LOAD_DEV(&c, EndCommandBuffer);
   LOAD_DEV(&c, CreateFence);
   LOAD_DEV(&c, CreateSemaphore);
   LOAD_DEV(&c, QueueSubmit);
   LOAD_DEV(&c, WaitForFences);
   if (!CreateBuffer || !AllocateMemory || !MapMemory || !CmdFillBuffer ||
       !CmdCopyBuffer || !CmdPipelineBarrier || !CreateFence ||
       !CreateSemaphore || !QueueSubmit || !WaitForFences) {
      LOG("FAIL: missing device entrypoints"); goto done;
   }

   const VkDeviceSize size[3] = { BUF_BYTES, BUF_BYTES, CHK_BYTES };
   enum { SRC = 0, DST = 1, CHK = 2 };

   for (int b = 0; b < 3; b++) {
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = size[b],
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkResult r = CreateBuffer(c.dev, &bci, NULL, &buf[b]);
      if (r != VK_SUCCESS) { LOG("FAIL vkCreateBuffer[%d] -> %d", b, r); goto done; }

      VkMemoryRequirements mr;
      GetBufferMemoryRequirements(c.dev, buf[b], &mr);
      uint32_t mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (mt == UINT32_MAX) { LOG("FAIL: no host visible|coherent memory type"); goto done; }

      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = mr.size, .memoryTypeIndex = mt,
      };
      r = AllocateMemory(c.dev, &mai, NULL, &mem[b]);
      if (r != VK_SUCCESS) { LOG("FAIL vkAllocateMemory[%d] -> %d", b, r); goto done; }

      r = MapMemory(c.dev, mem[b], 0, VK_WHOLE_SIZE, 0, &cpu[b]);
      if (r != VK_SUCCESS || !cpu[b]) { LOG("FAIL vkMapMemory[%d] -> %d", b, r); goto done; }

      r = BindBufferMemory(c.dev, buf[b], mem[b], 0);
      if (r != VK_SUCCESS) { LOG("FAIL vkBindBufferMemory[%d] -> %d", b, r); goto done; }
   }

   memset(cpu[DST], 0, BUF_BYTES);
   memset(cpu[CHK], 0, CHK_BYTES);

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = c.qfi,
   };
   if (CreateCommandPool(c.dev, &pci, NULL, &pool) != VK_SUCCESS) { LOG("FAIL pool"); goto done; }

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 2,
   };
   VkCommandBuffer cb[2];
   if (AllocateCommandBuffers(c.dev, &cbai, cb) != VK_SUCCESS) { LOG("FAIL cb"); goto done; }

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   VkBufferCopy whole = { .srcOffset = 0, .dstOffset = 0, .size = BUF_BYTES };

   /* Fill src and bounce it against dst */
   BeginCommandBuffer(cb[0], &cbbi);
   CmdFillBuffer(cb[0], buf[SRC], 0, BUF_BYTES, PATTERN);
   for (uint32_t i = 0; i < COPIES; i++) {
      CmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
      if (i & 1) CmdCopyBuffer(cb[0], buf[DST], buf[SRC], 1, &whole);
      else       CmdCopyBuffer(cb[0], buf[SRC], buf[DST], 1, &whole);
   }
   if (COPIES & 1) {
      CmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
      CmdCopyBuffer(cb[0], buf[SRC], buf[DST], 1, &whole);
   }
   if (EndCommandBuffer(cb[0]) != VK_SUCCESS) { LOG("FAIL end A"); goto done; }

   /* Read the tail A wrote last. */
   VkBufferCopy tail = {
      .srcOffset = BUF_BYTES - CHK_BYTES, .dstOffset = 0, .size = CHK_BYTES,
   };
   BeginCommandBuffer(cb[1], &cbbi);
   CmdCopyBuffer(cb[1], buf[DST], buf[CHK], 1, &tail);
   if (EndCommandBuffer(cb[1]) != VK_SUCCESS) { LOG("FAIL end B"); goto done; }

   VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   if (CreateSemaphore(c.dev, &sci, NULL, &sem) != VK_SUCCESS) { LOG("FAIL sem"); goto done; }

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   if (CreateFence(c.dev, &fci, NULL, &fence) != VK_SUCCESS) { LOG("FAIL fence"); goto done; }

   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
   VkSubmitInfo si_a = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb[0],
      .signalSemaphoreCount = 1, .pSignalSemaphores = &sem,
   };
   VkSubmitInfo si_b = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1, .pWaitSemaphores = &sem,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1, .pCommandBuffers = &cb[1],
   };

   const uint64_t t0 = now_us();
   VkResult r = QueueSubmit(c.queue, 1, &si_a, VK_NULL_HANDLE);
   const uint64_t t1 = now_us();
   if (r != VK_SUCCESS) { LOG("FAIL vkQueueSubmit(A) -> %d", r); goto done; }

   r = QueueSubmit(c.queue, 1, &si_b, fence);
   const uint64_t t2 = now_us();
   if (r != VK_SUCCESS) { LOG("FAIL vkQueueSubmit(B) -> %d", r); goto done; }

   r = WaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX);
   const uint64_t t3 = now_us();
   if (r != VK_SUCCESS) { LOG("FAIL vkWaitForFences -> %d", r); goto done; }

   const uint64_t submit_a_us = t1 - t0;
   const uint64_t submit_b_us = t2 - t1;
   const uint64_t fence_us    = t3 - t2;
   const uint64_t total_us    = t3 - t0;

   LOG("submit A: %llu us", (unsigned long long)submit_a_us);
   LOG("submit B: %llu us  <- the dependency", (unsigned long long)submit_b_us);
   LOG("fence   : %llu us", (unsigned long long)fence_us);
   LOG("total   : %llu us", (unsigned long long)total_us);

   const uint32_t *w = (const uint32_t *)cpu[CHK];
   uint32_t bad = 0, first = 0;
   for (uint32_t i = 0; i < CHK_BYTES / 4; i++)
      if (w[i] != PATTERN) { if (!bad) first = i; bad++; }

   if (bad) {
      LOG("ORDER FAIL: %u/%u tail words wrong. word[%u]=0x%08x (expected 0x%08x)",
          bad, CHK_BYTES / 4, first, w[first], PATTERN);
      LOG("  B ran before A finished");
      LOG("=== nvk_engine_wait FAILED ===");
      goto done;
   }
   LOG("ORDER OK: all %u tail words == 0x%08x", CHK_BYTES / 4, PATTERN);

   if (total_us < MIN_TOTAL_US) {
      LOG("INCONCLUSIVE: total %llu us is under the %u us floor.",
          (unsigned long long)total_us, MIN_TOTAL_US);
      LOG("=== nvk_engine_wait INCONCLUSIVE ===");
      goto done;
   }

   if (submit_b_us * 4 < total_us) {
      LOG("LATENCY OK: submit B is %llu%% of the run",
          (unsigned long long)(submit_b_us * 100 / total_us));
      LOG("=== nvk_engine_wait PASSED ===");
   } else {
      LOG("LATENCY FAIL: submit B is %llu%% of the run",
          (unsigned long long)(submit_b_us * 100 / total_us));
      LOG("  the CPU is still blocking on the dependency");
      LOG("=== nvk_engine_wait FAILED ===");
   }

done:
   if (c.dev) {
      LOAD_DEV(&c, DeviceWaitIdle);
      LOAD_DEV(&c, DestroyFence);
      LOAD_DEV(&c, DestroySemaphore);
      LOAD_DEV(&c, DestroyCommandPool);
      LOAD_DEV(&c, UnmapMemory);
      LOAD_DEV(&c, DestroyBuffer);
      LOAD_DEV(&c, FreeMemory);
      if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
      if (fence) DestroyFence(c.dev, fence, NULL);
      if (sem)   DestroySemaphore(c.dev, sem, NULL);
      if (pool)  DestroyCommandPool(c.dev, pool, NULL);
      for (int b = 0; b < 3; b++) {
         if (buf[b]) DestroyBuffer(c.dev, buf[b], NULL);
         if (cpu[b]) UnmapMemory(c.dev, mem[b]);
         if (mem[b]) FreeMemory(c.dev, mem[b], NULL);
      }
   }
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
