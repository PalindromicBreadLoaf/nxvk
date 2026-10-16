/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone integration example, not application startup code to copy whole.
 * No ladder harness, chainloading constructor, GPU clock API or shader compiler
 * build tools are involved. All driver calls use the packaged static ICD.
 */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <nxvk/nxvk_static.h>
#include <switch.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These globals belong only to this standalone example. */
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
static FILE *log_file;

static void log_line(const char *format, ...)
{
   char text[768];
   va_list args;
   va_start(args, format);
   vsnprintf(text, sizeof(text), format, args);
   va_end(args);
   puts(text);
   if (log_file) {
      fprintf(log_file, "%s\n", text);
      fflush(log_file);
   }
}

#define INSTANCE_FUNCTIONS(X) \
   X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
   X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
   X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
   X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
   X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
   X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
   X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdFillBuffer) X(CmdPipelineBarrier) \
   X(QueueSubmit) X(QueueWaitIdle)
#define DECLARE(name) PFN_vk##name name = NULL;
#define CHECK(call) do { \
   VkResult result_ = (call); \
   if (result_ != VK_SUCCESS) { log_line("FAIL: %s -> %d", #call, result_); goto cleanup; } \
} while (0)

int main(void)
{
   consoleInit(NULL);
   padConfigureInput(1, HidNpadStyleSet_NpadStandard);
   PadState pad;
   padInitializeDefault(&pad);
   log_file = fopen("sdmc:/nxvk_app_example.log", "w");
   log_line("NXVK standalone app / static ICD / headless fill + readback");

   bool ok = false;
   VkInstance instance = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   void *mapped = NULL;
   INSTANCE_FUNCTIONS(DECLARE)
   DEVICE_FUNCTIONS(DECLARE)

   /* Required explicit opt-in, before any Vulkan instance/device creation. */
   if (setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1) != 0) {
      log_line("FAIL: could not set the GM20B opt-in");
      goto cleanup;
   }
   PFN_vkGetInstanceProcAddr gip = vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)
      gip(VK_NULL_HANDLE, "vkCreateInstance");
   if (!CreateInstance) { log_line("FAIL: missing vkCreateInstance"); goto cleanup; }

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nxvk_app_example", .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
   };
   CHECK(CreateInstance(&ici, NULL, &instance));
#define LOAD_INSTANCE(name) do { \
   name = (PFN_vk##name)gip(instance, "vk" #name); \
   if (!name) { log_line("FAIL: missing vk%s", #name); goto cleanup; } \
} while (0);
   INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE

   uint32_t count = 4;
   VkPhysicalDevice physical[4];
   CHECK(EnumeratePhysicalDevices(instance, &count, physical));
   if (count == 0) { log_line("FAIL: no Vulkan devices"); goto cleanup; }
   VkPhysicalDevice gpu = physical[0];
   VkPhysicalDeviceProperties properties;
   GetPhysicalDeviceProperties(gpu, &properties);
   log_line("Device: %s / Vulkan %u.%u.%u", properties.deviceName,
            VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
            VK_VERSION_PATCH(properties.apiVersion));
   PFN_vkGetPhysicalDeviceProperties2 GetProperties2 =
      (PFN_vkGetPhysicalDeviceProperties2)gip(instance, "vkGetPhysicalDeviceProperties2");
   if (GetProperties2) {
      VkPhysicalDeviceDriverProperties driver = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      VkPhysicalDeviceProperties2 p = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver,
      };
      GetProperties2(gpu, &p);
      log_line("Driver: %s / %s", driver.driverName, driver.driverInfo);
   }

   uint32_t family_count = 0, family = UINT32_MAX;
   GetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, NULL);
   if (!family_count || family_count > 16) { log_line("FAIL: unsupported queue count"); goto cleanup; }
   VkQueueFamilyProperties families[16];
   GetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families);
   for (uint32_t i = 0; i < family_count; i++)
      if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
         family = i; break;
      }
   if (family == UINT32_MAX) { log_line("FAIL: no graphics/transfer queue"); goto cleanup; }
   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
   };
   CHECK(CreateDevice(gpu, &dci, NULL, &device));
#define LOAD_DEVICE(name) do { \
   name = (PFN_vk##name)GetDeviceProcAddr(device, "vk" #name); \
   if (!name) { log_line("FAIL: missing vk%s", #name); goto cleanup; } \
} while (0);
   DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
   VkQueue queue = VK_NULL_HANDLE;
   GetDeviceQueue(device, family, 0, &queue);
   if (!queue) { log_line("FAIL: no device queue"); goto cleanup; }

   const uint32_t bytes = 4096, pattern = 0x4e58564b;
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   CHECK(CreateBuffer(device, &bci, NULL, &buffer));
   VkMemoryRequirements requirements;
   GetBufferMemoryRequirements(device, buffer, &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   GetPhysicalDeviceMemoryProperties(gpu, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
      if ((requirements.memoryTypeBits & (1u << i)) &&
          (memory_properties.memoryTypes[i].propertyFlags & want) == want) {
         memory_type = i; break;
      }
   }
   if (memory_type == UINT32_MAX) { log_line("FAIL: no host-visible coherent memory"); goto cleanup; }
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size, .memoryTypeIndex = memory_type,
   };
   CHECK(AllocateMemory(device, &mai, NULL, &memory));
   CHECK(BindBufferMemory(device, buffer, memory, 0));
   CHECK(MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
   if (!mapped) { log_line("FAIL: null mapping"); goto cleanup; }
   memset(mapped, 0, bytes);

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
   };
   CHECK(CreateCommandPool(device, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   VkCommandBuffer cb = VK_NULL_HANDLE;
   CHECK(AllocateCommandBuffers(device, &cbai, &cb));
   VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(BeginCommandBuffer(cb, &begin));
   CmdFillBuffer(cb, buffer, 0, bytes, pattern);
   VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                      0, 1, &barrier, 0, NULL, 0, NULL);
   CHECK(EndCommandBuffer(cb));
   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb,
   };
   CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
   CHECK(QueueWaitIdle(queue));
   const volatile uint32_t *words = mapped;
   for (uint32_t i = 0; i < bytes / 4; i++) {
      if (words[i] != pattern) {
         log_line("FAIL: word %u = 0x%08x, expected 0x%08x", i, words[i], pattern);
         goto cleanup;
      }
   }
   log_line("Readback: all %u words match", bytes / 4);
   ok = true;

cleanup:
   if (device) {
      if (DeviceWaitIdle) {
         VkResult r = DeviceWaitIdle(device);
         if (r != VK_SUCCESS) { log_line("FAIL: teardown wait -> %d", r); ok = false; }
      }
      if (pool && DestroyCommandPool) DestroyCommandPool(device, pool, NULL);
      if (buffer && DestroyBuffer) DestroyBuffer(device, buffer, NULL);
      if (mapped && UnmapMemory) UnmapMemory(device, memory);
      if (memory && FreeMemory) FreeMemory(device, memory, NULL);
      if (DestroyDevice) DestroyDevice(device, NULL);
   }
   if (instance && DestroyInstance) DestroyInstance(instance, NULL);
   log_line("=== nxvk_app_example %s ===", ok ? "PASSED" : "FAILED");
   log_line("Log: sdmc:/nxvk_app_example.log — press + to exit");
   if (log_file) fclose(log_file);
   consoleUpdate(NULL);
   while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
      consoleUpdate(NULL);
   }
   consoleExit(NULL);
   return ok ? 0 : 1;
}
