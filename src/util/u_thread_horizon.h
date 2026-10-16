/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef U_THREAD_HORIZON_H
#define U_THREAD_HORIZON_H

#include <stdbool.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
#include <switch/result.h>

/* Best-effort equivalent of a minimum-priority background worker on Horizon.
 * Smaller priority numbers run first. Query the process's allowed priorities
 * instead of assuming that every homebrew loader grants priority 63.
 */
static inline bool
util_horizon_set_current_thread_minimum_priority(void)
{
   u64 allowed = 0;
   if (R_FAILED(svcGetInfo(&allowed, InfoType_PriorityMask,
                          CUR_PROCESS_HANDLE, 0)) || allowed == 0)
      return false;

   const u32 lowest = 63u - (u32)__builtin_clzll(allowed);
   const Handle thread = threadGetCurHandle();
   s32 current;
   if (R_FAILED(svcGetThreadPriority(&current, thread)) ||
       current < 0 || current > 63)
      return false;

   /* Never raise a thread's priority, including under unusual loader masks. */
   if ((u32)current >= lowest)
      return true;

   return R_SUCCEEDED(svcSetThreadPriority(thread, lowest));
}

#endif /* U_THREAD_HORIZON_H */
