/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-test declarations only. Production builds include the real libnx SDK. */
#ifndef TEST_SWITCH_KERNEL_SVC_H
#define TEST_SWITCH_KERNEL_SVC_H
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint32_t Handle;
typedef uint32_t Result;
#define CUR_PROCESS_HANDLE 0xFFFF8001u
#define InfoType_PriorityMask 1u
Result svcGetInfo(u64 *out, u32 id0, Handle handle, u64 id1);
Result svcGetThreadPriority(s32 *priority, Handle handle);
Result svcSetThreadPriority(Handle handle, u32 priority);
#endif
