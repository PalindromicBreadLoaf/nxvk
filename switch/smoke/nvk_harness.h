/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Shared scaffolding for the Switch NVK validation apps.
 */
#ifndef NVK_HARNESS_H
#define NVK_HARNESS_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h> /* struct in_addr, for __nxlink_host */
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

/* logging */

static FILE *g_nvk_log;

static FILE *g_nvk_mesa_log;

static void nvk_logf(const char *fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (g_nvk_log) { fputs(buf, g_nvk_log); fputc('\n', g_nvk_log); fflush(g_nvk_log); }
   printf("%s\n", buf);
   fflush(stdout);
}
#define LOG(...) nvk_logf(__VA_ARGS__)

static void nvk_log_open(const char *path)
{
   g_nvk_log = fopen(path, "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
}

/* driver diagnostics */

/* Every message the driver/runtime sends lands here.
 * Written to nvk_mesa.log and mirrored into the per-app log.
 */
static VKAPI_ATTR VkBool32 VKAPI_CALL
nvk_debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
             VkDebugUtilsMessageTypeFlagsEXT types,
             const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
   (void)types; (void)user;
   const char *lvl =
      (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? "ERR"  :
      (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN" :
      (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? "INFO" : "DBG";
   const char *idname = data->pMessageIdName ? data->pMessageIdName : "?";
   const char *msg    = data->pMessage ? data->pMessage : "";
   if (g_nvk_mesa_log) {
      fprintf(g_nvk_mesa_log, "[%s] %s: %s\n", lvl, idname, msg);
      fflush(g_nvk_mesa_log);
   }
   if (g_nvk_log) {
      fprintf(g_nvk_log, "  [vk/%s] %s: %s\n", lvl, idname, msg);
      fflush(g_nvk_log);
   }
   return VK_FALSE;
}

static const VkDebugUtilsMessengerCreateInfoEXT nvk_debug_ci = {
   .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
   .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
   .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
   .pfnUserCallback = nvk_debug_cb,
};

/* ICD entrypoint resolution */

struct nvk_ctx {
   VkInstance                       instance;
   VkPhysicalDevice                 phys;
   VkPhysicalDeviceProperties       props;
   VkPhysicalDeviceMemoryProperties memp;
   uint32_t                         qfi;   /* graphics+compute+transfer family */
   VkDevice                         dev;
   VkQueue                          queue;
   PFN_vkGetDeviceProcAddr          GetDeviceProcAddr;
   VkDebugUtilsMessengerEXT         debug_messenger;
};

#define LOAD_INST(ctx, fn) \
   PFN_vk##fn fn = (PFN_vk##fn)vk_icdGetInstanceProcAddr((ctx)->instance, "vk" #fn)
#define LOAD_DEV(ctx, fn) \
   PFN_vk##fn fn = (PFN_vk##fn)(ctx)->GetDeviceProcAddr((ctx)->dev, "vk" #fn)

/* helpers  */

static uint32_t nvk_pick_mem_type(const VkPhysicalDeviceMemoryProperties *mp,
                                  uint32_t type_bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) &&
          (mp->memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   return UINT32_MAX;
}

/* Bring the driver up to a usable instance. */
static VkResult nvk_bringup_ex(struct nvk_ctx *c,
                               const char *const *inst_exts, uint32_t n_inst_exts,
                               const char *const *dev_exts, uint32_t n_dev_exts)
{
   memset(c, 0, sizeof(*c));

   /*NVK refuses device creation without this. */
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_SHOW_STATS", "1", 1);

   if (!g_nvk_mesa_log) {
      g_nvk_mesa_log = fopen("sdmc:/nvk_mesa.log", "w");
      if (!g_nvk_mesa_log)
         LOG("bringup: WARN could not open sdmc:/nvk_mesa.log ");
   }

   LOAD_INST(c, CreateInstance);
   if (!CreateInstance) { LOG("bringup: no vkCreateInstance"); return VK_ERROR_INITIALIZATION_FAILED; }

   /* Append EXT_debug_utils so the messenger below can surface driver messages. */
   const char *all_inst_exts[16];
   uint32_t n_all = 0;
   for (uint32_t i = 0; i < n_inst_exts && n_all < 15; i++)
      all_inst_exts[n_all++] = inst_exts[i];
   all_inst_exts[n_all++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_ladder",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = &nvk_debug_ci, /* catches messages raised during instance create */
      .pApplicationInfo = &app,
      .enabledExtensionCount = n_all, .ppEnabledExtensionNames = all_inst_exts,
   };
   VkResult r = CreateInstance(&ici, NULL, &c->instance);
   LOG("bringup: vkCreateInstance -> %d", r);
   if (r != VK_SUCCESS) return r;

   LOAD_INST(c, CreateDebugUtilsMessengerEXT);
   if (CreateDebugUtilsMessengerEXT)
      CreateDebugUtilsMessengerEXT(c->instance, &nvk_debug_ci, NULL,
                                   &c->debug_messenger);

   LOAD_INST(c, EnumeratePhysicalDevices);
   LOAD_INST(c, GetPhysicalDeviceProperties);
   LOAD_INST(c, GetPhysicalDeviceMemoryProperties);
   LOAD_INST(c, GetPhysicalDeviceQueueFamilyProperties);
   LOAD_INST(c, CreateDevice);
   c->GetDeviceProcAddr =
      (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(c->instance, "vkGetDeviceProcAddr");
   if (!EnumeratePhysicalDevices || !CreateDevice || !c->GetDeviceProcAddr) {
      LOG("bringup: missing instance entrypoints");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   uint32_t n = 0;
   r = EnumeratePhysicalDevices(c->instance, &n, NULL);
   if (r != VK_SUCCESS || n == 0) { LOG("bringup: no physical devices (r=%d n=%u)", r, n); return VK_ERROR_DEVICE_LOST; }
   VkPhysicalDevice phys[4];
   if (n > 4) n = 4;
   r = EnumeratePhysicalDevices(c->instance, &n, phys);
   if (r != VK_SUCCESS) return r;
   c->phys = phys[0];

   GetPhysicalDeviceProperties(c->phys, &c->props);
   GetPhysicalDeviceMemoryProperties(c->phys, &c->memp);
   LOG("bringup: device '%s' api=%u.%u.%u vendor=0x%x dev=0x%x",
       c->props.deviceName,
       VK_VERSION_MAJOR(c->props.apiVersion), VK_VERSION_MINOR(c->props.apiVersion),
       VK_VERSION_PATCH(c->props.apiVersion), c->props.vendorID, c->props.deviceID);

   uint32_t nqf = 0;
   GetPhysicalDeviceQueueFamilyProperties(c->phys, &nqf, NULL);
   VkQueueFamilyProperties qf[8];
   if (nqf > 8) nqf = 8;
   GetPhysicalDeviceQueueFamilyProperties(c->phys, &nqf, qf);
   c->qfi = UINT32_MAX;
   for (uint32_t i = 0; i < nqf; i++) {
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { c->qfi = i; break; }
   }
   if (c->qfi == UINT32_MAX) { LOG("bringup: no graphics queue family"); return VK_ERROR_FEATURE_NOT_PRESENT; }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = c->qfi, .queueCount = 1, .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .enabledExtensionCount = n_dev_exts, .ppEnabledExtensionNames = dev_exts,
   };
   r = CreateDevice(c->phys, &dci, NULL, &c->dev);
   LOG("bringup: vkCreateDevice -> %d", r);
   if (r != VK_SUCCESS) return r;

   LOAD_DEV(c, GetDeviceQueue);
   GetDeviceQueue(c->dev, c->qfi, 0, &c->queue);
   return VK_SUCCESS;
}

static VkResult nvk_bringup(struct nvk_ctx *c,
                            const char *const *dev_exts, uint32_t n_dev_exts)
{
   return nvk_bringup_ex(c, NULL, 0, dev_exts, n_dev_exts);
}

static void nvk_teardown(struct nvk_ctx *c)
{
   if (c->dev) {
      LOAD_DEV(c, DeviceWaitIdle);
      LOAD_DEV(c, DestroyDevice);
      if (DeviceWaitIdle) DeviceWaitIdle(c->dev);
      if (DestroyDevice)  DestroyDevice(c->dev, NULL);
   }
   if (c->instance) {
      if (c->debug_messenger) {
         PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vk_icdGetInstanceProcAddr(
               c->instance, "vkDestroyDebugUtilsMessengerEXT");
         if (DestroyDebugUtilsMessengerEXT)
            DestroyDebugUtilsMessengerEXT(c->instance, c->debug_messenger, NULL);
      }
      PFN_vkDestroyInstance DestroyInstance =
         (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(c->instance, "vkDestroyInstance");
      if (DestroyInstance) DestroyInstance(c->instance, NULL);
   }
   LOG("teardown: instance/device destroyed");
   if (g_nvk_mesa_log) { fclose(g_nvk_mesa_log); g_nvk_mesa_log = NULL; }
}

#endif /* NVK_HARNESS_H */
