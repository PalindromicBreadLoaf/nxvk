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

/* Three different values so a failed readback says *where* the GPU stopped
 * seeing the recorded stream: the last fill, any earlier fill, or the value
 * the CPU wrote through its own mapping before submitting. */
#define PATTERN     0x5CA1AB1Eu  /* the last of the FILLS fills */
#define MIDFILL     0x0BADF00Du  /* every fill before it */
#define PREFILL     0xDEADBEEFu  /* written by the CPU before the submit */

#define MIN_SPEEDUP_PCT 125u

struct phase {
   uint64_t record_ns;
   uint64_t big_record_ns;
   bool     readback_ok;
   uint32_t bad_words;
   uint32_t first_bad_word;
   uint32_t first_bad_value;
   uint32_t n_pattern;
   uint32_t n_midfill;
   uint32_t n_prefill;
   uint32_t n_other;
};

static uint64_t now_ns(void)
{
   return armTicksToNs(armGetSystemTick());
}

static bool run_phase(const char *name, const char *debug, struct phase *out)
{
   bool ok = false;

   if (debug)
      setenv("NVK_DEBUG", debug, 1);
   else
      unsetenv("NVK_DEBUG");

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
      ((uint32_t *)cpu)[w] = PREFILL;

   const uint64_t big_t0 = now_ns();
   BeginCommandBuffer(cb, &cbbi);

   for (uint32_t i = 0; i < FILLS; i++)
      CmdFillBuffer(cb, buf, 0, FILL_BYTES, i + 1 == FILLS ? PATTERN : MIDFILL);
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

   const uint32_t words = FILL_BYTES / 4;
   for (uint32_t w = 0; w < words; w++) {
      const uint32_t got = ((volatile uint32_t *)cpu)[w];
      if (got == PATTERN) {
         out->n_pattern++;
         continue;
      }
      if (out->bad_words == 0) {
         out->first_bad_word = w;
         out->first_bad_value = got;
      }
      out->bad_words++;
      if (got == MIDFILL)
         out->n_midfill++;
      else if (got == PREFILL)
         out->n_prefill++;
      else
         out->n_other++;
   }
   out->readback_ok = out->bad_words == 0;

   LOG("%s [NVK_DEBUG=%s]: empty record %llu ns, %u-fill record %llu ns, readback %s",
       name, debug ? debug : "",
       (unsigned long long)out->record_ns, FILLS,
       (unsigned long long)out->big_record_ns,
       out->readback_ok ? "OK" : "BAD");
   if (!out->readback_ok) {
      LOG("  readback: first wrong word %u = 0x%08x, %u of %u words wrong",
          out->first_bad_word, out->first_bad_value, out->bad_words, words);
      LOG("  readback: %u words have the last fill (0x%08x), %u an earlier fill "
          "(0x%08x), %u the CPU prefill (0x%08x), %u something else",
          out->n_pattern, PATTERN, out->n_midfill, MIDFILL,
          out->n_prefill, PREFILL, out->n_other);
      if (out->n_prefill == words) {
         LOG("  the buffer still holds the CPU prefill, so no fill reached it:");
         LOG("  the recorded stream (or its execution) was not visible to the GPU");
      } else if (out->n_pattern == 0 && out->n_midfill == words) {
         LOG("  every word has an earlier fill's value but none has the last");
         LOG("  fill's, so either the tail of the recorded stream or the last");
         LOG("  fill's parameter update was not visible to the GPU");
      } else if (out->n_pattern > 0) {
         LOG("  partial: %u of %u words hold the last fill's value",
             out->n_pattern, words);
      } else {
         LOG("  the words match none of the three values this test writes:");
         LOG("  the GPU filled with something else, so a parameter it read was");
         LOG("  stale rather than the command stream itself");
      }
   }
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

   struct phase full = {0}, ranged = {0}, full_c = {0}, ranged_c = {0};

   if (!run_phase("full",          "full_cmd_flush",                 &full))     goto fail;
   if (!run_phase("ranged",        NULL,                             &ranged))   goto fail;
   if (!run_phase("full_cached",   "full_cmd_flush,no_cpu_uncached", &full_c))   goto fail;
   if (!run_phase("ranged_cached", "no_cpu_uncached",                &ranged_c)) goto fail;

   if (!full.readback_ok || !ranged.readback_ok ||
       !full_c.readback_ok || !ranged_c.readback_ok) {
      LOG("FAIL: the GPU did not see the whole pushbuf");
      LOG("  phases are full, ranged, full_cached, ranged_cached in order; the");
      LOG("  readback breakdown of the BAD phase says how far the GPU got");
      goto fail;
   }

   if (ranged.record_ns == 0 || ranged_c.record_ns == 0) {
      LOG("FAIL: timer resolution too coarse");
      goto fail;
   }

   const uint64_t pct   = full.record_ns   * 100ull / ranged.record_ns;
   const uint64_t pct_c = full_c.record_ns * 100ull / ranged_c.record_ns;

   LOG("empty record, cmd mem as shipped: %llu -> %llu ns (%llu%% of ranged)",
       (unsigned long long)full.record_ns,
       (unsigned long long)ranged.record_ns,
       (unsigned long long)pct);
   LOG("empty record, cmd mem CPU-cached: %llu -> %llu ns (%llu%% of ranged)",
       (unsigned long long)full_c.record_ns,
       (unsigned long long)ranged_c.record_ns,
       (unsigned long long)pct_c);

   if (pct < MIN_SPEEDUP_PCT) {
      LOG("as shipped the two are level, so no flush is reaching the memory at");
      LOG("  all");
   } else {
      LOG("as shipped the flush is still being paid");
   }

   if (pct_c >= MIN_SPEEDUP_PCT) {
      LOG("CMD FLUSH OK");
      LOG("=== nvk_cmd_flush PASSED ===");
   } else {
      LOG("CMD FLUSH FAIL: with cacheable command memory, recording an empty");
      LOG("  command buffer still pays for the whole 64 KiB mem (%llu%% < %u%%)",
          (unsigned long long)pct_c, MIN_SPEEDUP_PCT);
      LOG("=== nvk_cmd_flush FAILED ===");
   }

   if (g_nvk_log) fclose(g_nvk_log);
   return 0;

fail:
   LOG("=== nvk_cmd_flush FAILED ===");
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
