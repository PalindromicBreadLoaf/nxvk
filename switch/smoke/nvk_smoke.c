/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * headless fill and readback.
 */
#include "nvk_harness.h"

#define FILL_VALUE 0xCAFEBABEu
#define FILL_BYTES 4096u

int main(void)
{
   nvk_log_open("sdmc:/nvk_smoke.log");
   LOG("=== nvk_smoke ===");

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
   LOAD_DEV(&c, EndCommandBuffer);
   LOAD_DEV(&c, QueueSubmit);
   LOAD_DEV(&c, QueueWaitIdle);
   if (!CreateBuffer || !AllocateMemory || !MapMemory || !CmdFillBuffer ||
       !QueueSubmit || !QueueWaitIdle) { LOG("FAIL: missing device entrypoints"); goto done; }

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = FILL_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buf;
   VkResult r = CreateBuffer(c.dev, &bci, NULL, &buf);
   LOG("vkCreateBuffer -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(c.dev, buf, &mr);
   uint32_t mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   LOG("mem: reqBits=0x%x size=%llu chosen host-visible|coherent type=%d",
       mr.memoryTypeBits, (unsigned long long)mr.size, (int)mt);
   if (mt == UINT32_MAX) { LOG("FAIL: no host-visible|coherent memory type"); goto done; }

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   VkDeviceMemory mem;
   r = AllocateMemory(c.dev, &mai, NULL, &mem);
   LOG("vkAllocateMemory -> %d", r);
   if (r != VK_SUCCESS) goto done;

   void *cpu = NULL;
   r = MapMemory(c.dev, mem, 0, VK_WHOLE_SIZE, 0, &cpu);
   LOG("vkMapMemory -> %d (ptr=%p)", r, cpu);
   if (r != VK_SUCCESS || !cpu) goto done;
   memset(cpu, 0, FILL_BYTES);

   r = BindBufferMemory(c.dev, buf, mem, 0);
   LOG("vkBindBufferMemory -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   r = CreateCommandPool(c.dev, &pci, NULL, &pool);
   if (r != VK_SUCCESS) { LOG("FAIL vkCreateCommandPool -> %d", r); goto done; }

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   r = AllocateCommandBuffers(c.dev, &cbai, &cb);
   if (r != VK_SUCCESS) { LOG("FAIL vkAllocateCommandBuffers -> %d", r); goto done; }

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   BeginCommandBuffer(cb, &cbbi);
   CmdFillBuffer(cb, buf, 0, FILL_BYTES, FILL_VALUE);
   r = EndCommandBuffer(cb);
   if (r != VK_SUCCESS) { LOG("FAIL vkEndCommandBuffer -> %d", r); goto done; }

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb,
   };
   r = QueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE);
   LOG("vkQueueSubmit -> %d", r);
   if (r != VK_SUCCESS) goto done;
   r = QueueWaitIdle(c.queue);
   LOG("vkQueueWaitIdle -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* HOST_COHERENT memory maps uncached. */
   uint32_t *w = (uint32_t *)cpu, bad = 0, first = 0;
   for (uint32_t i = 0; i < FILL_BYTES / 4; i++)
      if (w[i] != FILL_VALUE) { if (!bad) first = i; bad++; }

   if (bad == 0) {
      LOG("VERIFY OK: all %u words == 0x%08x", FILL_BYTES / 4, FILL_VALUE);
      LOG("=== nvk_smoke PASSED ===");
   } else {
      LOG("VERIFY FAIL: %u/%u words wrong; first word[%u]=0x%08x",
          bad, FILL_BYTES / 4, first, w[first]);
      LOG("=== nvk_smoke FAILED at verify ===");
   }

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
