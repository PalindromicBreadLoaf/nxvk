/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * WSI present path.
 */
#include <math.h>
#include "nvk_harness.h"

#define FRAMES     300
#define MAX_INFLT  3

int main(void)
{
   nvk_log_open("sdmc:/nvk_vi_swapchain.log");
   LOG("=== nvk_vi_swapchain ===");

   const char *inst_exts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
   const char *dev_exts[]  = { "VK_KHR_swapchain" };

   struct nvk_ctx c;
   if (nvk_bringup_ex(&c, inst_exts, 2, dev_exts, 1) != VK_SUCCESS) { LOG("FAIL: bringup"); goto done; }

   LOAD_INST(&c, CreateViSurfaceNN);
   LOAD_INST(&c, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   LOAD_INST(&c, GetPhysicalDeviceSurfaceFormatsKHR);
   LOAD_INST(&c, GetPhysicalDeviceSurfaceSupportKHR);
   LOAD_DEV(&c, CreateSwapchainKHR);
   LOAD_DEV(&c, GetSwapchainImagesKHR);
   LOAD_DEV(&c, AcquireNextImageKHR);
   LOAD_DEV(&c, QueuePresentKHR);
   LOAD_DEV(&c, CreateCommandPool);
   LOAD_DEV(&c, AllocateCommandBuffers);
   LOAD_DEV(&c, BeginCommandBuffer);
   LOAD_DEV(&c, EndCommandBuffer);
   LOAD_DEV(&c, ResetCommandBuffer);
   LOAD_DEV(&c, CmdClearColorImage);
   LOAD_DEV(&c, CmdPipelineBarrier);
   LOAD_DEV(&c, CreateSemaphore);
   LOAD_DEV(&c, CreateFence);
   LOAD_DEV(&c, WaitForFences);
   LOAD_DEV(&c, ResetFences);
   LOAD_DEV(&c, QueueSubmit);
   if (!CreateViSurfaceNN) { LOG("FAIL: no vkCreateViSurfaceNN (VI WSI not wired?)"); goto done; }

   VkViSurfaceCreateInfoNN vci = {
      .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
      .window = nwindowGetDefault(),
   };
   VkSurfaceKHR surface;
   VkResult r = CreateViSurfaceNN(c.instance, &vci, NULL, &surface);
   LOG("vkCreateViSurfaceNN -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkBool32 present_ok = VK_FALSE;
   GetPhysicalDeviceSurfaceSupportKHR(c.phys, c.qfi, surface, &present_ok);
   LOG("queue family %u present support: %d", c.qfi, present_ok);

   VkSurfaceCapabilitiesKHR caps;
   GetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, surface, &caps);
   VkExtent2D ext = caps.currentExtent;
   if (ext.width == 0xFFFFFFFFu) { ext.width = 1280; ext.height = 720; }

   uint32_t nfmt = 0;
   GetPhysicalDeviceSurfaceFormatsKHR(c.phys, surface, &nfmt, NULL);
   VkSurfaceFormatKHR fmts[16];
   if (nfmt > 16) nfmt = 16;
   GetPhysicalDeviceSurfaceFormatsKHR(c.phys, surface, &nfmt, fmts);
   VkSurfaceFormatKHR sf = fmts[0];
   for (uint32_t i = 0; i < nfmt; i++)
      if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
         sf = fmts[i]; break;
      }
   LOG("surface %ux%u fmt=%d colorspace=%d nfmt=%u", ext.width, ext.height, sf.format, sf.colorSpace, nfmt);

   VkSwapchainCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface, .minImageCount = caps.minImageCount,
      .imageFormat = sf.format, .imageColorSpace = sf.colorSpace,
      .imageExtent = ext, .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
   };
   VkSwapchainKHR swapchain;
   r = CreateSwapchainKHR(c.dev, &sci, NULL, &swapchain);
   LOG("vkCreateSwapchainKHR -> %d", r);
   if (r != VK_SUCCESS) goto done;

   uint32_t nimg = 0;
   GetSwapchainImagesKHR(c.dev, swapchain, &nimg, NULL);
   VkImage images[8];
   if (nimg > 8) nimg = 8;
   GetSwapchainImagesKHR(c.dev, swapchain, &nimg, images);
   LOG("swapchain images: %u", nimg);

   VkCommandPoolCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c.qfi,
   };
   VkCommandPool pool;
   if (CreateCommandPool(c.dev, &cpi, NULL, &pool) != VK_SUCCESS) { LOG("FAIL: pool"); goto done; }

   VkSemaphore acquire_sem[MAX_INFLT], present_sem[MAX_INFLT];
   VkFence     inflight[MAX_INFLT];
   VkCommandBuffer cmd[MAX_INFLT];
   for (int i = 0; i < MAX_INFLT; i++) {
      VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      CreateSemaphore(c.dev, &si, NULL, &acquire_sem[i]);
      CreateSemaphore(c.dev, &si, NULL, &present_sem[i]);
      VkFenceCreateInfo fi = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT,
      };
      CreateFence(c.dev, &fi, NULL, &inflight[i]);
      VkCommandBufferAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
      };
      AllocateCommandBuffers(c.dev, &ai, &cmd[i]);
   }

   LOG("entering present loop (%d frames)", FRAMES);
   int frame = 0, presented = 0;
   for (; frame < FRAMES && appletMainLoop(); frame++) {
      int slot = frame % MAX_INFLT;
      WaitForFences(c.dev, 1, &inflight[slot], VK_TRUE, UINT64_MAX);

      uint32_t idx = 0;
      r = AcquireNextImageKHR(c.dev, swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &idx);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         LOG("acquire frame %d -> %d", frame, r);
         if (r == VK_ERROR_OUT_OF_DATE_KHR) break;
         continue;
      }
      ResetFences(c.dev, 1, &inflight[slot]);
      ResetCommandBuffer(cmd[slot], 0);

      float t = frame / 60.0f;
      VkClearColorValue color = { .float32 = {
         0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.094f), 0.5f + 0.5f * sinf(t + 4.188f), 1.0f,
      } };

      VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      BeginCommandBuffer(cmd[slot], &bi);
      VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      VkImageMemoryBarrier to_dst = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[idx], .subresourceRange = range,
      };
      CmdPipelineBarrier(cmd[slot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_dst);
      CmdClearColorImage(cmd[slot], images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);
      VkImageMemoryBarrier to_present = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[idx], .subresourceRange = range,
      };
      CmdPipelineBarrier(cmd[slot], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_present);
      EndCommandBuffer(cmd[slot]);

      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &acquire_sem[slot], .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1, .pCommandBuffers = &cmd[slot],
         .signalSemaphoreCount = 1, .pSignalSemaphores = &present_sem[slot],
      };
      r = QueueSubmit(c.queue, 1, &submit, inflight[slot]);
      if (r != VK_SUCCESS) { LOG("submit frame %d -> %d", frame, r); break; }

      VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &present_sem[slot],
         .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &idx,
      };
      r = QueuePresentKHR(c.queue, &pi);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         LOG("present frame %d -> %d", frame, r);
         if (r == VK_ERROR_OUT_OF_DATE_KHR) break;
      } else {
         presented++;
      }
   }
   LOG("presented %d / %d frames", presented, frame);
   if (presented > 0)
      LOG("=== nvk_vi_swapchain PASSED ===");
   else
      LOG("=== nvk_vi_swapchain FAILED (no frame presented) ===");

done:
   nvk_teardown(&c);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
