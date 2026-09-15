/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Multi-line copy-engine buffer copy
 */
#include "nvk_harness.h"

#define SRC_BYTES  (8u * 1024u * 1024u)
#define DST_BYTES  (24u * 1024u * 1024u)
#define LINE_B     (128u * 1024u)
#define GUARD_B    256u
#define STALE      0xEDu

static const struct {
   const char *name;
   uint32_t src_off;
   uint32_t size;
} cases[] = {
   { "4 MiB, 32 whole lines",   0, 4u * 1024u * 1024u },
   { "exactly one line",        0, LINE_B             },
   { "one line plus one byte",  0, LINE_B + 1         },
   { "one byte short of a line",0, LINE_B - 1         },
   { "3 lines plus remainder",  0, LINE_B * 3u + 777u },
   { "unaligned src and size",  5, LINE_B * 2u + 33u  },
   { "sub-line",                1, 7u                 },
};
#define NCASES (sizeof(cases) / sizeof((cases)[0]))

int main(void)
{
   nvk_log_open("sdmc:/nvk_ce_copy.log");
   LOG("=== nvk_ce_copy ===");
   LOG("params: src=%u MiB dst=%u MiB line=%u KiB cases=%u",
       SRC_BYTES / (1024u * 1024u), DST_BYTES / (1024u * 1024u),
       LINE_B / 1024u, (unsigned)NCASES);

   VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
   VkDeviceMemory smem = VK_NULL_HANDLE, dmem = VK_NULL_HANDLE;
   void *scpu = NULL, *dcpu = NULL;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   uint32_t dst_off[NCASES];

   VkBuffer *bufs[2] = { &src, &dst };
   VkDeviceMemory *mems[2] = { &smem, &dmem };
   void **cpus[2] = { &scpu, &dcpu };
   const VkDeviceSize sizes[2] = { SRC_BYTES, DST_BYTES };

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
   LOAD_DEV(&c, CmdCopyBuffer);
   LOAD_DEV(&c, CmdPipelineBarrier);
   LOAD_DEV(&c, EndCommandBuffer);
   LOAD_DEV(&c, CreateFence);
   LOAD_DEV(&c, QueueSubmit);
   LOAD_DEV(&c, WaitForFences);
   if (!CreateBuffer || !AllocateMemory || !MapMemory || !CmdCopyBuffer ||
       !CmdPipelineBarrier || !CreateFence || !QueueSubmit || !WaitForFences) {
      LOG("FAIL: missing device entrypoints"); goto done;
   }

   for (int b = 0; b < 2; b++) {
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = sizes[b],
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkResult r = CreateBuffer(c.dev, &bci, NULL, bufs[b]);
      if (r != VK_SUCCESS) { LOG("FAIL vkCreateBuffer[%d] -> %d", b, r); goto done; }

      VkMemoryRequirements mr;
      GetBufferMemoryRequirements(c.dev, *bufs[b], &mr);
      uint32_t mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (mt == UINT32_MAX) { LOG("FAIL: no host-visible|coherent memory type"); goto done; }

      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = mr.size, .memoryTypeIndex = mt,
      };
      r = AllocateMemory(c.dev, &mai, NULL, mems[b]);
      if (r != VK_SUCCESS) { LOG("FAIL vkAllocateMemory[%d] -> %d", b, r); goto done; }

      r = MapMemory(c.dev, *mems[b], 0, VK_WHOLE_SIZE, 0, cpus[b]);
      if (r != VK_SUCCESS || !*cpus[b]) { LOG("FAIL vkMapMemory[%d] -> %d", b, r); goto done; }

      r = BindBufferMemory(c.dev, *bufs[b], *mems[b], 0);
      if (r != VK_SUCCESS) { LOG("FAIL vkBindBufferMemory[%d] -> %d", b, r); goto done; }
   }

   uint8_t *s = (uint8_t *)scpu;
   for (uint32_t i = 0; i < SRC_BYTES; i++)
      s[i] = (uint8_t)((i * 37u) ^ (i >> 11) ^ 0xA5u);
   memset(dcpu, STALE, DST_BYTES);

   uint32_t cursor = GUARD_B;
   for (unsigned k = 0; k < NCASES; k++) {
      dst_off[k] = cursor;
      cursor += cases[k].size + GUARD_B;
      if (cases[k].src_off + cases[k].size > SRC_BYTES || cursor > DST_BYTES) {
         LOG("FAIL: case %u (%s) does not fit", k, cases[k].name); goto done;
      }
   }
   LOG("dst high-water: %u of %u bytes", cursor, DST_BYTES);

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
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   BeginCommandBuffer(cb, &cbbi);
   for (unsigned k = 0; k < NCASES; k++) {
      VkBufferCopy region = {
         .srcOffset = cases[k].src_off,
         .dstOffset = dst_off[k],
         .size = cases[k].size,
      };
      CmdCopyBuffer(cb, src, dst, 1, &region);
   }
   VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   if (EndCommandBuffer(cb) != VK_SUCCESS) { LOG("FAIL end"); goto done; }

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   if (CreateFence(c.dev, &fci, NULL, &fence) != VK_SUCCESS) { LOG("FAIL fence"); goto done; }

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb,
   };
   VkResult r = QueueSubmit(c.queue, 1, &si, fence);
   LOG("vkQueueSubmit -> %d", r);
   if (r != VK_SUCCESS) goto done;

   r = WaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX);
   LOG("vkWaitForFences -> %d", r);
   if (r != VK_SUCCESS) goto done;

   const uint8_t *d = (const uint8_t *)dcpu;
   unsigned failed = 0;

   for (unsigned k = 0; k < NCASES; k++) {
      const uint8_t *want = s + cases[k].src_off;
      const uint8_t *got = d + dst_off[k];
      uint32_t bad = 0, first = 0;

      for (uint32_t i = 0; i < cases[k].size; i++)
         if (got[i] != want[i]) { if (!bad) first = i; bad++; }

      uint32_t guard_bad = 0;
      for (uint32_t i = 1; i <= GUARD_B; i++)
         if (d[dst_off[k] - i] != STALE) guard_bad++;
      for (uint32_t i = 0; i < GUARD_B; i++)
         if (d[dst_off[k] + cases[k].size + i] != STALE) guard_bad++;

      if (bad == 0 && guard_bad == 0) {
         LOG("  ok   [%u] %-26s off=%u size=%u", k, cases[k].name,
             dst_off[k], cases[k].size);
      } else {
         failed++;
         LOG("  FAIL [%u] %-26s off=%u size=%u", k, cases[k].name,
             dst_off[k], cases[k].size);
         if (bad)
            LOG("         %u/%u bytes wrong, first at +%u (got 0x%02x want 0x%02x)",
                bad, cases[k].size, first, got[first], want[first]);
         if (guard_bad)
            LOG("         %u/%u guard bytes clobbered -- copy ran outside the region",
                guard_bad, GUARD_B * 2u);
      }
   }

   if (failed == 0) {
      LOG("VERIFY OK: all %u copies byte-exact with no guard clobber", (unsigned)NCASES);
      LOG("=== nvk_ce_copy PASSED ===");
   } else {
      LOG("VERIFY FAIL: %u/%u copies wrong", failed, (unsigned)NCASES);
      LOG("=== nvk_ce_copy FAILED ===");
   }

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
      if (pool)  DestroyCommandPool(c.dev, pool, NULL);
      for (int b = 0; b < 2; b++) {
         if (*bufs[b]) DestroyBuffer(c.dev, *bufs[b], NULL);
         if (*cpus[b]) UnmapMemory(c.dev, *mems[b]);
         if (*mems[b]) FreeMemory(c.dev, *mems[b], NULL);
      }
   }
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
