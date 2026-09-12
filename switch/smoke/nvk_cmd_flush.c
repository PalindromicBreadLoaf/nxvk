/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Command buffer cost
 */
#include "nvk_harness.h"

#include <stdlib.h>

#define RECORDS     4096u
#define WARMUP      128u
#define FILLS       6000u
#define FILL_BYTES  4096u
#define PATTERN     0x5CA1AB1Eu
#define STALE       0xDEADBEEFu

#define MIN_SPEEDUP_PCT 125u

struct phase {
   uint64_t record_ns;
   uint64_t big_record_ns;
   bool     readback_ok;
};

static uint64_t now_ns(void)
{
   return armTicksToNs(armGetSystemTick());
}

static bool run_phase(const char *name, struct phase *out)
{
   bool ok = false;
   VkBuffer buf = VK_NULL_HANDLE;
   VkDeviceMemory mem = VK_NULL_HANDLE;
   void *cpu = NULL;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup (%s)", name); return false; }

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
   LOAD_DEV(&c, CreateFence);
   LOAD_DEV(&c, QueueSubmit);
   LOAD_DEV(&c, WaitForFences);
   if (!CreateBuffer || !AllocateMemory || !MapMemory || !CmdFillBuffer ||
       !CreateFence || !QueueSubmit || !WaitForFences) {
      LOG("FAIL: missing device entrypoints"); goto done;
   }

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

   for (uint32_t i = 0; i < WARMUP; i++) {
      BeginCommandBuffer(cb, &cbbi);
      if (EndCommandBuffer(cb) != VK_SUCCESS) { LOG("FAIL warmup end"); goto done; }
   }

   const uint64_t t0 = now_ns();
   for (uint32_t i = 0; i < RECORDS; i++) {
      BeginCommandBuffer(cb, &cbbi);
      if (EndCommandBuffer(cb) != VK_SUCCESS) { LOG("FAIL timed end"); goto done; }
   }
   out->record_ns = (now_ns() - t0) / RECORDS;

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
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (mt == UINT32_MAX) { LOG("FAIL: no host-visible|coherent type"); goto done; }

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   if (AllocateMemory(c.dev, &mai, NULL, &mem) != VK_SUCCESS) { LOG("FAIL mem"); goto done; }
   if (MapMemory(c.dev, mem, 0, VK_WHOLE_SIZE, 0, &cpu) != VK_SUCCESS || !cpu) {
      LOG("FAIL map"); goto done;
   }
   if (BindBufferMemory(c.dev, buf, mem, 0) != VK_SUCCESS) { LOG("FAIL bind"); goto done; }

   for (uint32_t w = 0; w < FILL_BYTES / 4; w++)
      ((uint32_t *)cpu)[w] = STALE;

   const uint64_t big_t0 = now_ns();
   BeginCommandBuffer(cb, &cbbi);

   for (uint32_t i = 0; i < FILLS; i++)
      CmdFillBuffer(cb, buf, 0, FILL_BYTES, i + 1 == FILLS ? PATTERN : STALE);
   if (EndCommandBuffer(cb) != VK_SUCCESS) { LOG("FAIL big end"); goto done; }
   out->big_record_ns = now_ns() - big_t0;

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   if (CreateFence(c.dev, &fci, NULL, &fence) != VK_SUCCESS) { LOG("FAIL fence"); goto done; }

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb,
   };
   VkResult r = QueueSubmit(c.queue, 1, &si, fence);
   if (r != VK_SUCCESS) { LOG("FAIL vkQueueSubmit -> %d", r); goto done; }
   r = WaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX);
   if (r != VK_SUCCESS) { LOG("FAIL vkWaitForFences -> %d", r); goto done; }

   out->readback_ok = true;
   for (uint32_t w = 0; w < FILL_BYTES / 4; w++) {
      const uint32_t got = ((volatile uint32_t *)cpu)[w];
      if (got != PATTERN) {
         LOG("READBACK FAIL (%s): word %u = 0x%08x, expected 0x%08x",
             name, w, got, PATTERN);
         out->readback_ok = false;
         break;
      }
   }

   LOG("%s: empty record %llu ns, %u-fill record %llu ns, readback %s",
       name, (unsigned long long)out->record_ns, FILLS,
       (unsigned long long)out->big_record_ns,
       out->readback_ok ? "OK" : "BAD");
   ok = true;

done:
   if (c.dev) {
      LOAD_DEV(&c, DeviceWaitIdle);
      LOAD_DEV(&c, DestroyFence);
      LOAD_DEV(&c, DestroyCommandPool);
      LOAD_DEV(&c, UnmapMemory);
      LOAD_DEV(&c, DestroyBuffer);
      LOAD_DEV(&c, FreeMemory);
      if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
      if (fence) DestroyFence(c.dev, fence, NULL);
      if (pool) DestroyCommandPool(c.dev, pool, NULL);
      if (buf) DestroyBuffer(c.dev, buf, NULL);
      if (cpu) UnmapMemory(c.dev, mem);
      if (mem) FreeMemory(c.dev, mem, NULL);
   }
   nvk_teardown(&c);
   return ok;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_cmd_flush.log");
   LOG("=== nvk_cmd_flush ===");
   LOG("params: records/phase=%u fills=%u", RECORDS, FILLS);

   struct phase full = {0}, ranged = {0};

   setenv("NVK_DEBUG", "full_cmd_flush", 1);
   if (!run_phase("full", &full)) goto fail;

   unsetenv("NVK_DEBUG");
   if (!run_phase("ranged", &ranged)) goto fail;

   if (!full.readback_ok || !ranged.readback_ok) {
      LOG("FAIL: the GPU did not see the whole pushbuf");
      goto fail;
   }

   if (ranged.record_ns == 0) { LOG("FAIL: timer resolution too coarse"); goto fail; }

   const uint64_t pct = full.record_ns * 100ull / ranged.record_ns;
   LOG("empty record: %llu -> %llu ns (%llu%% of ranged)",
       (unsigned long long)full.record_ns,
       (unsigned long long)ranged.record_ns,
       (unsigned long long)pct);

   if (pct >= MIN_SPEEDUP_PCT) {
      LOG("CMD FLUSH OK");
      LOG("=== nvk_cmd_flush PASSED ===");
   } else {
      LOG("CMD FLUSH FAIL: recording an empty command buffer still pays for the");
      LOG("  whole 64 KiB mem (%llu%% < %u%%)",
          (unsigned long long)pct, MIN_SPEEDUP_PCT);
      LOG("=== nvk_cmd_flush FAILED ===");
   }

   if (g_nvk_log) fclose(g_nvk_log);
   return 0;

fail:
   LOG("=== nvk_cmd_flush FAILED ===");
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
