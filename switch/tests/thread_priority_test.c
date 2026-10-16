/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production helper with mocked libnx services, not a scheduler. */
#include <assert.h>
#include <stdio.h>
#include "util/u_thread_horizon.h"

static u64 mask;
static s32 current;
static Result info_result, get_result, set_result;
static unsigned get_calls, set_calls;
static u32 requested;
static const Handle worker = 0x1234u;

Handle
threadGetCurHandle(void)
{
   return worker;
}

Result
svcGetInfo(u64 *out, u32 id0, Handle handle, u64 id1)
{
   assert(id0 == InfoType_PriorityMask);
   assert(handle == CUR_PROCESS_HANDLE && id1 == 0);
   *out = mask;
   return info_result;
}

Result
svcGetThreadPriority(s32 *priority, Handle handle)
{
   assert(handle == worker);
   get_calls++;
   *priority = current;
   return get_result;
}

Result
svcSetThreadPriority(Handle handle, u32 priority)
{
   assert(handle == worker);
   assert(priority < 64 && (mask & (UINT64_C(1) << priority)));
   assert(priority > (u32)current);
   requested = priority;
   set_calls++;
   return set_result;
}

static void
reset(u64 allowed, s32 priority)
{
   mask = allowed;
   current = priority;
   info_result = get_result = set_result = 0;
   get_calls = set_calls = 0;
   requested = UINT32_MAX;
}

int
main(void)
{
   /* Every single-bit mask, including bit 63 and the no-change case at 0. */
   for (unsigned bit = 0; bit < 64; bit++) {
      reset(UINT64_C(1) << bit, 0);
      assert(util_horizon_set_current_thread_minimum_priority());
      assert(get_calls == 1 && set_calls == (bit != 0));
      if (bit != 0)
         assert(requested == bit);
   }

   /* Non-contiguous and restricted loader masks: choose the highest bit. */
   reset((UINT64_C(1) << 24) | (UINT64_C(1) << 44) |
         (UINT64_C(1) << 59), 44);
   assert(util_horizon_set_current_thread_minimum_priority());
   assert(set_calls == 1 && requested == 59);

   reset(UINT64_MAX, 44);
   assert(util_horizon_set_current_thread_minimum_priority());
   assert(set_calls == 1 && requested == 63);

   reset(UINT64_C(1) << 44, 44);
   assert(util_horizon_set_current_thread_minimum_priority());
   assert(set_calls == 0);
   reset(UINT64_C(1) << 40, 44);
   assert(util_horizon_set_current_thread_minimum_priority());
   assert(set_calls == 0); /* Never promote a worker. */

   reset(0, 44);
   assert(!util_horizon_set_current_thread_minimum_priority());
   assert(get_calls == 0 && set_calls == 0); /* Never call clz(0). */
   reset(UINT64_MAX, 44);
   info_result = 1;
   assert(!util_horizon_set_current_thread_minimum_priority());
   assert(get_calls == 0 && set_calls == 0);
   reset(UINT64_MAX, 44);
   get_result = 1;
   assert(!util_horizon_set_current_thread_minimum_priority());
   assert(get_calls == 1 && set_calls == 0);
   reset(UINT64_MAX, 44);
   set_result = 1;
   assert(!util_horizon_set_current_thread_minimum_priority());
   assert(set_calls == 1);

   reset(UINT64_MAX, -1);
   assert(!util_horizon_set_current_thread_minimum_priority());
   assert(set_calls == 0);
   reset(UINT64_MAX, 64);
   assert(!util_horizon_set_current_thread_minimum_priority());
   assert(set_calls == 0);

   puts("PASS: Horizon priority selection, permissions, errors and no promotion");
   return 0;
}
