/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Verify IMMEDIATE presents before vsync.
 */
#include "nvk_harness.h"

#define FRAMES   240u
#define WARMUP   60u
#define INFLIGHT 3u

/* IMMEDIATE has to clear FIFO by this much to count */
#define IMMEDIATE_GAIN 12u

struct present_fns {
   PFN_vkCreateSwapchainKHR     CreateSwapchainKHR;
   PFN_vkDestroySwapchainKHR    DestroySwapchainKHR;
   PFN_vkGetSwapchainImagesKHR  GetSwapchainImagesKHR;
   PFN_vkAcquireNextImageKHR    AcquireNextImageKHR;
   PFN_vkQueuePresentKHR        QueuePresentKHR;
   PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
   PFN_vkBeginCommandBuffer     BeginCommandBuffer;
   PFN_vkEndCommandBuffer       EndCommandBuffer;
   PFN_vkResetCommandBuffer     ResetCommandBuffer;
   PFN_vkCmdClearColorImage     CmdClearColorImage;
   PFN_vkCmdPipelineBarrier     CmdPipelineBarrier;
   PFN_vkCreateSemaphore        CreateSemaphore;
   PFN_vkDestroySemaphore       DestroySemaphore;
   PFN_vkCreateFence            CreateFence;
   PFN_vkDestroyFence           DestroyFence;
   PFN_vkWaitForFences          WaitForFences;
   PFN_vkResetFences            ResetFences;
   PFN_vkQueueSubmit            QueueSubmit;
   PFN_vkQueueWaitIdle          QueueWaitIdle;
};

static uint64_t now_us(void)
{
   return armTicksToNs(armGetSystemTick()) / 1000ull;
}

static void barrier(const struct present_fns *f, VkCommandBuffer cmd,
                    VkImage image, VkImageLayout from, VkImageLayout to)
{
   VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED
                       ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                       ? VK_ACCESS_TRANSFER_WRITE_BIT : 0,
      .oldLayout = from, .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image, .subresourceRange = range,
   };
   f->CmdPipelineBarrier(cmd,
                         from == VK_IMAGE_LAYOUT_UNDEFINED
                            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                            : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                            ? VK_PIPELINE_STAGE_TRANSFER_BIT
                            : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static uint32_t run_present(struct nvk_ctx *c, const struct present_fns *f,
                            VkSurfaceKHR surface, VkSurfaceFormatKHR sf,
                            VkExtent2D ext, uint32_t min_images,
                            VkPresentModeKHR mode, VkCommandPool pool,
                            const char *name, bool check_held_slot)
{
   VkSwapchainCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface, .minImageCount = min_images,
      .imageFormat = sf.format, .imageColorSpace = sf.colorSpace,
      .imageExtent = ext, .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = mode, .clipped = VK_TRUE,
   };

   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   VkSemaphore acquire_sem[INFLIGHT] = {0}, present_sem[INFLIGHT] = {0};
   VkSemaphore spare_sem = VK_NULL_HANDLE;
   VkFence inflight[INFLIGHT] = {0};
   VkCommandBuffer cmd[INFLIGHT] = {0};
   VkImage images[8];
   uint32_t nimg = 8;
   uint32_t fps10 = 0;
   uint64_t elapsed_us = 0;
   uint32_t timed = 0;

   VkResult r = f->CreateSwapchainKHR(c->dev, &sci, NULL, &swapchain);
   if (r != VK_SUCCESS) { LOG("%s: vkCreateSwapchainKHR -> %d", name, r); goto out; }

   f->GetSwapchainImagesKHR(c->dev, swapchain, &nimg, images);
   LOG("%s: %u images", name, nimg);

   for (uint32_t i = 0; i < INFLIGHT; i++) {
      VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      f->CreateSemaphore(c->dev, &si, NULL, &acquire_sem[i]);
      f->CreateSemaphore(c->dev, &si, NULL, &present_sem[i]);
      VkFenceCreateInfo fi = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         .flags = VK_FENCE_CREATE_SIGNALED_BIT,
      };
      f->CreateFence(c->dev, &fi, NULL, &inflight[i]);
      VkCommandBufferAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      f->AllocateCommandBuffers(c->dev, &ai, &cmd[i]);
   }
   {
      VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      f->CreateSemaphore(c->dev, &si, NULL, &spare_sem);
   }

   uint64_t start_us = 0;
   for (uint32_t frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
      const uint32_t slot = frame % INFLIGHT;

      if (frame == WARMUP)
         start_us = now_us();

      f->WaitForFences(c->dev, 1, &inflight[slot], VK_TRUE, UINT64_MAX);

      uint32_t idx = 0;
      r = f->AcquireNextImageKHR(c->dev, swapchain, UINT64_MAX,
                                 acquire_sem[slot], VK_NULL_HANDLE, &idx);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         LOG("%s: acquire frame %u -> %d", name, frame, r);
         goto out;
      }

      if (check_held_slot && frame == 0) {
         uint32_t held = 0;
         VkResult zero = f->AcquireNextImageKHR(c->dev, swapchain, 0,
                                                spare_sem, VK_NULL_HANDLE, &held);
         VkResult timed_out = f->AcquireNextImageKHR(c->dev, swapchain, 1000000,
                                                     spare_sem, VK_NULL_HANDLE, &held);
         LOG("held slot: acquire(timeout=0) -> %d, acquire(timeout=1ms) -> %d",
             zero, timed_out);
         if (zero != VK_NOT_READY || timed_out != VK_TIMEOUT) {
            LOG("HELD SLOT FAIL: expected %d and %d",
                VK_NOT_READY, VK_TIMEOUT);
         } else {
            LOG("HELD SLOT OK");
         }
      }

      f->ResetFences(c->dev, 1, &inflight[slot]);
      f->ResetCommandBuffer(cmd[slot], 0);

      const float phase = (float)(frame % 60) / 60.0f;
      VkClearColorValue color = {{ phase, 0.15f, 1.0f - phase, 1.0f }};
      VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      f->BeginCommandBuffer(cmd[slot], &bi);
      barrier(f, cmd[slot], images[idx], VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      f->CmdClearColorImage(cmd[slot], images[idx],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
      barrier(f, cmd[slot], images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
      f->EndCommandBuffer(cmd[slot]);

      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &acquire_sem[slot],
         .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1, .pCommandBuffers = &cmd[slot],
         .signalSemaphoreCount = 1, .pSignalSemaphores = &present_sem[slot],
      };
      r = f->QueueSubmit(c->queue, 1, &submit, inflight[slot]);
      if (r != VK_SUCCESS) { LOG("%s: submit frame %u -> %d", name, frame, r); goto out; }

      VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &present_sem[slot],
         .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &idx,
      };
      r = f->QueuePresentKHR(c->queue, &pi);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         LOG("%s: present frame %u -> %d", name, frame, r);
         goto out;
      }

      if (frame >= WARMUP)
         timed++;
   }

   elapsed_us = now_us() - start_us;
   if (timed == FRAMES - WARMUP && elapsed_us > 0)
      fps10 = (uint32_t)(((uint64_t)timed * 10000000ull) / elapsed_us);

   LOG("%s: %u frames in %llu us -> %u.%u fps", name, timed,
       (unsigned long long)elapsed_us, fps10 / 10, fps10 % 10);

out:
   if (f->QueueWaitIdle) f->QueueWaitIdle(c->queue);
   for (uint32_t i = 0; i < INFLIGHT; i++) {
      if (acquire_sem[i]) f->DestroySemaphore(c->dev, acquire_sem[i], NULL);
      if (present_sem[i]) f->DestroySemaphore(c->dev, present_sem[i], NULL);
      if (inflight[i])    f->DestroyFence(c->dev, inflight[i], NULL);
   }
   if (spare_sem) f->DestroySemaphore(c->dev, spare_sem, NULL);
   if (swapchain) f->DestroySwapchainKHR(c->dev, swapchain, NULL);

   return fps10;
}

int main(void)
{
   nvk_log_open("sdmc:/nvk_present.log");
   LOG("=== nvk_present ===");
   LOG("params: frames=%u warmup=%u inflight=%u", FRAMES, WARMUP, INFLIGHT);

   VkCommandPool pool = VK_NULL_HANDLE;
   uint32_t fifo10 = 0, immediate10 = 0;

   const char *inst_exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
   const char *dev_exts[]  = { "VK_KHR_swapchain" };

   struct nvk_ctx c;
   if (nvk_bringup_ex(&c, inst_exts, 2, dev_exts, 1) != VK_SUCCESS) {
      LOG("FAIL: bringup");
      goto done;
   }

   LOAD_INST(&c, CreateViSurfaceNN);
   LOAD_INST(&c, DestroySurfaceKHR);
   LOAD_INST(&c, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   LOAD_INST(&c, GetPhysicalDeviceSurfaceFormatsKHR);
   LOAD_INST(&c, GetPhysicalDeviceSurfacePresentModesKHR);
   LOAD_DEV(&c, CreateCommandPool);
   if (!CreateViSurfaceNN) { LOG("FAIL: no vkCreateViSurfaceNN"); goto done; }

   struct present_fns f = {0};
#define P(fn) f.fn = (PFN_vk##fn)c.GetDeviceProcAddr(c.dev, "vk" #fn)
   P(CreateSwapchainKHR); P(DestroySwapchainKHR); P(GetSwapchainImagesKHR);
   P(AcquireNextImageKHR); P(QueuePresentKHR); P(AllocateCommandBuffers);
   P(BeginCommandBuffer); P(EndCommandBuffer); P(ResetCommandBuffer);
   P(CmdClearColorImage); P(CmdPipelineBarrier); P(CreateSemaphore);
   P(DestroySemaphore); P(CreateFence); P(DestroyFence); P(WaitForFences);
   P(ResetFences); P(QueueSubmit); P(QueueWaitIdle);
#undef P

   VkViSurfaceCreateInfoNN vci = {
      .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
      .window = nwindowGetDefault(),
   };
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   VkResult r = CreateViSurfaceNN(c.instance, &vci, NULL, &surface);
   if (r != VK_SUCCESS) { LOG("FAIL: vkCreateViSurfaceNN -> %d", r); goto done; }

   VkSurfaceCapabilitiesKHR caps;
   GetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, surface, &caps);
   VkExtent2D ext = caps.currentExtent;
   if (ext.width == 0xFFFFFFFFu) { ext.width = 1280; ext.height = 720; }
   LOG("surface %ux%u, image count %u..%u", ext.width, ext.height,
       caps.minImageCount, caps.maxImageCount);

   uint32_t nmodes = 0;
   GetPhysicalDeviceSurfacePresentModesKHR(c.phys, surface, &nmodes, NULL);
   VkPresentModeKHR modes[8];
   if (nmodes > 8) nmodes = 8;
   GetPhysicalDeviceSurfacePresentModesKHR(c.phys, surface, &nmodes, modes);
   bool has_immediate = false;
   for (uint32_t i = 0; i < nmodes; i++)
      if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = true;
   if (!has_immediate) { LOG("FAIL: IMMEDIATE not advertised"); goto done; }

   uint32_t nfmt = 0;
   GetPhysicalDeviceSurfaceFormatsKHR(c.phys, surface, &nfmt, NULL);
   VkSurfaceFormatKHR fmts[16];
   if (nfmt > 16) nfmt = 16;
   GetPhysicalDeviceSurfaceFormatsKHR(c.phys, surface, &nfmt, fmts);
   VkSurfaceFormatKHR sf = fmts[0];
   for (uint32_t i = 0; i < nfmt; i++)
      if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
          fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { sf = fmts[i]; break; }

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c.qfi,
   };
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) {
      LOG("FAIL: command pool");
      goto done;
   }

   fifo10 = run_present(&c, &f, surface, sf, ext, caps.minImageCount,
                        VK_PRESENT_MODE_FIFO_KHR, pool, "fifo", true);
   immediate10 = run_present(&c, &f, surface, sf, ext, caps.minImageCount,
                             VK_PRESENT_MODE_IMMEDIATE_KHR, pool, "immediate",
                             false);

   if (DestroySurfaceKHR) DestroySurfaceKHR(c.instance, surface, NULL);

   if (fifo10 == 0 || immediate10 == 0) {
      LOG("=== nvk_present FAILED ===");
      goto done;
   }

   if (immediate10 * 10 >= fifo10 * IMMEDIATE_GAIN) {
      LOG("IMMEDIATE OK: %u.%u fps against %u.%u fps on fifo",
          immediate10 / 10, immediate10 % 10, fifo10 / 10, fifo10 % 10);
      LOG("=== nvk_present PASSED ===");
   } else {
      LOG("IMMEDIATE FAIL: %u.%u fps against %u.%u fps on fifo",
          immediate10 / 10, immediate10 % 10, fifo10 / 10, fifo10 % 10);
      LOG("=== nvk_present FAILED ===");
   }

done:
   if (c.dev) {
      LOAD_DEV(&c, DeviceWaitIdle);
      LOAD_DEV(&c, DestroyCommandPool);
      if (DeviceWaitIdle) DeviceWaitIdle(c.dev);
      if (pool) DestroyCommandPool(c.dev, pool, NULL);
   }
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
