/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NXVK_STATIC_H
#define NXVK_STATIC_H

/* Define VK_NO_PROTOTYPES before your first Vulkan include when using manual
 * dispatch or volk. This header does not change an application's global policy.
 * Before instance creation, opt in to the non-conformant GM20B target with
 * setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1).
 */
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
#ifdef __cplusplus
}
#endif

#endif
