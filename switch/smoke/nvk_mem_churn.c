/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Test backing store recycling.
 */
#include "nvk_harness.h"

#include <malloc.h>

#define WARM_ROUNDS   8      /* let the driver's own allocations settle */
#define ROUNDS        48     /* measured rounds */
#define PER_ROUND     12     /* buffers per round */
#define IMG_PER_ROUND 2      /* images per round */
#define IMG_DIM       256

#define NSIZES        4
static const VkDeviceSize g_sizes[NSIZES] = {
   4u * 1024, 64u * 1024, 256u * 1024, 1024u * 1024,
};

/* A few free chunks of drift are normal. */
#define ORDBLKS_SLACK 8

static uint64_t now_us(void)
{
   return armTicksToNs(armGetSystemTick()) / 1000ull;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_mem_churn.log");
   LOG("=== nvk_mem_churn ===");
   LOG("params: rounds=%u (+%u warm) buffers/round=%u images/round=%u",
       ROUNDS, WARM_ROUNDS, PER_ROUND, IMG_PER_ROUND);

   VkBuffer buf[PER_ROUND];
   VkDeviceMemory bmem[PER_ROUND];
   VkImage img[IMG_PER_ROUND];
   VkDeviceMemory imem[IMG_PER_ROUND];
   void *prev_ptr[PER_ROUND];
   struct mallinfo base_mi, end_mi;
   uint64_t warm_us = 0, last_us = 0;
   uint64_t probes = 0, recycled = 0;
   bool ok = false;

   memset(prev_ptr, 0, sizeof(prev_ptr));
   memset(&base_mi, 0, sizeof(base_mi));
   memset(&end_mi, 0, sizeof(end_mi));

   struct nvk_ctx c;
   if (nvk_bringup(&c, NULL, 0) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_DEV(&c, CreateBuffer);
   LOAD_DEV(&c, DestroyBuffer);
   LOAD_DEV(&c, GetBufferMemoryRequirements);
   LOAD_DEV(&c, BindBufferMemory);
   LOAD_DEV(&c, CreateImage);
   LOAD_DEV(&c, DestroyImage);
   LOAD_DEV(&c, GetImageMemoryRequirements);
   LOAD_DEV(&c, BindImageMemory);
   LOAD_DEV(&c, AllocateMemory);
   LOAD_DEV(&c, FreeMemory);
   LOAD_DEV(&c, MapMemory);
   LOAD_DEV(&c, UnmapMemory);
   if (!CreateBuffer || !DestroyBuffer || !GetBufferMemoryRequirements ||
       !BindBufferMemory || !CreateImage || !DestroyImage ||
       !GetImageMemoryRequirements || !BindImageMemory || !AllocateMemory ||
       !FreeMemory || !MapMemory || !UnmapMemory) {
      LOG("FAIL: missing device entrypoints");
      goto done;
   }

   ok = true;
   for (uint32_t round = 0; ok && round < WARM_ROUNDS + ROUNDS; round++) {
      const bool measured = round >= WARM_ROUNDS;
      const uint64_t t0 = now_us();

      memset(buf, 0, sizeof(buf));
      memset(bmem, 0, sizeof(bmem));
      memset(img, 0, sizeof(img));
      memset(imem, 0, sizeof(imem));

      for (uint32_t i = 0; ok && i < PER_ROUND; i++) {
         VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = g_sizes[i % NSIZES],
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         };
         VkResult r = CreateBuffer(c.dev, &bci, NULL, &buf[i]);
         if (r != VK_SUCCESS) { LOG("FAIL round %u: vkCreateBuffer -> %d", round, r); ok = false; break; }

         VkMemoryRequirements mr;
         GetBufferMemoryRequirements(c.dev, buf[i], &mr);

         uint32_t mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
         if (mt == UINT32_MAX)
            mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
         if (mt == UINT32_MAX) { LOG("FAIL round %u: no host visible type", round); ok = false; break; }

         VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = mt,
         };
         r = AllocateMemory(c.dev, &mai, NULL, &bmem[i]);
         if (r != VK_SUCCESS) { LOG("FAIL round %u: vkAllocateMemory -> %d", round, r); ok = false; break; }

         r = BindBufferMemory(c.dev, buf[i], bmem[i], 0);
         if (r != VK_SUCCESS) { LOG("FAIL round %u: vkBindBufferMemory -> %d", round, r); ok = false; break; }

         void *p = NULL;
         r = MapMemory(c.dev, bmem[i], 0, VK_WHOLE_SIZE, 0, &p);
         if (r != VK_SUCCESS || p == NULL) { LOG("FAIL round %u: vkMapMemory -> %d", round, r); ok = false; break; }
         memset(p, (int)round, 64);

         if (measured && prev_ptr[i] != NULL) {
            probes++;
            recycled += (p == prev_ptr[i]);
         }
         prev_ptr[i] = p;
      }

      for (uint32_t i = 0; ok && i < IMG_PER_ROUND; i++) {
         VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { IMG_DIM, IMG_DIM, 1 },
            .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         };
         VkResult r = CreateImage(c.dev, &ici, NULL, &img[i]);
         if (r != VK_SUCCESS) { LOG("FAIL round %u: vkCreateImage -> %d", round, r); ok = false; break; }

         VkMemoryRequirements mr;
         GetImageMemoryRequirements(c.dev, img[i], &mr);
         uint32_t mt = nvk_pick_mem_type(&c.memp, mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
         if (mt == UINT32_MAX) { LOG("FAIL round %u: no device-local type", round); ok = false; break; }

         VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = mt,
         };
         r = AllocateMemory(c.dev, &mai, NULL, &imem[i]);
         if (r != VK_SUCCESS) { LOG("FAIL round %u: image vkAllocateMemory -> %d", round, r); ok = false; break; }

         r = BindImageMemory(c.dev, img[i], imem[i], 0);
         if (r != VK_SUCCESS) { LOG("FAIL round %u: vkBindImageMemory -> %d", round, r); ok = false; break; }
      }

      for (uint32_t i = IMG_PER_ROUND; i-- > 0; ) {
         if (img[i])  DestroyImage(c.dev, img[i], NULL);
         if (imem[i]) FreeMemory(c.dev, imem[i], NULL);
      }
      for (uint32_t i = PER_ROUND; i-- > 0; ) {
         if (buf[i])  DestroyBuffer(c.dev, buf[i], NULL);
         if (bmem[i]) { UnmapMemory(c.dev, bmem[i]); FreeMemory(c.dev, bmem[i], NULL); }
      }

      const uint64_t round_us = now_us() - t0;

      if (round == WARM_ROUNDS - 1) {
         base_mi = mallinfo();
         warm_us = round_us;
         LOG("baseline after warmup: arena=%u ordblks=%u uordblks=%u fordblks=%u (%llu us)",
             (unsigned)base_mi.arena, (unsigned)base_mi.ordblks,
             (unsigned)base_mi.uordblks, (unsigned)base_mi.fordblks,
             (unsigned long long)round_us);
      } else if (measured && ((round - WARM_ROUNDS) % 16 == 15)) {
         struct mallinfo mi = mallinfo();
         LOG("round %2u: arena=%u ordblks=%u uordblks=%u fordblks=%u (%llu us)",
             round - WARM_ROUNDS, (unsigned)mi.arena, (unsigned)mi.ordblks,
             (unsigned)mi.uordblks, (unsigned)mi.fordblks,
             (unsigned long long)round_us);
      }

      if (round == WARM_ROUNDS + ROUNDS - 1) {
         end_mi = mallinfo();
         last_us = round_us;
      }
   }

   if (!ok) { LOG("=== nvk_mem_churn FAILED ==="); goto done; }

   LOG("churn time: %llu us warm -> %llu us final",
       (unsigned long long)warm_us, (unsigned long long)last_us);
   LOG("recycled maps: %llu/%llu (%llu%%)",
       (unsigned long long)recycled, (unsigned long long)probes,
       probes ? (unsigned long long)(recycled * 100 / probes) : 0ull);

   const bool arena_flat = end_mi.arena <= base_mi.arena;
   const bool freelist_flat = end_mi.ordblks <= base_mi.ordblks + ORDBLKS_SLACK;
   const bool cache_used = probes > 0 && recycled * 10 >= probes * 9;

   LOG("heap: arena %u -> %u (%s), free chunks %u -> %u (%s)",
       (unsigned)base_mi.arena, (unsigned)end_mi.arena,
       arena_flat ? "flat" : "GREW",
       (unsigned)base_mi.ordblks, (unsigned)end_mi.ordblks,
       freelist_flat ? "flat" : "GREW");

   if (!arena_flat || !freelist_flat)
      LOG("HEAP FAIL: %u rounds of churn grew the newlib heap", ROUNDS);
   if (!cache_used)
      LOG("CACHE FAIL: backing stores are not coming back out of the cache");

   if (arena_flat && freelist_flat && cache_used)
      LOG("=== nvk_mem_churn PASSED ===");
   else
      LOG("=== nvk_mem_churn FAILED ===");

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
