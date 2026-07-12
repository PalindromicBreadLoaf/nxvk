/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Bring up a Zink pipe_screen over the statically linked NVK ICD.
 */
#include "nvk_harness.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/os_time.h"
#include "util/u_inlines.h"

#include "zink_public.h"

#define FILL_VALUE 0xCAFEBABEu
#define FILL_BYTES 4096u

int main(void)
{
   struct pipe_screen      *pscreen = NULL;
   struct pipe_context     *pctx    = NULL;
   struct pipe_resource    *buf     = NULL;
   struct pipe_fence_handle *fence   = NULL;
   uint32_t out[FILL_BYTES / 4];
   uint32_t value = FILL_VALUE;

   nvk_log_open("sdmc:/gl_gallium.log");
   LOG("=== gl_gallium ===");

   /* Zink creates the VkInstance/VkDevice internally. */
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);

   pscreen = zink_create_screen(NULL, NULL);
   if (!pscreen) { LOG("FAIL: zink_create_screen returned NULL"); goto out; }
   LOG("screen up: vendor='%s' name='%s'",
       pscreen->get_vendor(pscreen), pscreen->get_name(pscreen));

   pctx = pscreen->context_create(pscreen, NULL, 0);
   if (!pctx) { LOG("FAIL: context_create returned NULL"); goto out; }

   buf = pipe_buffer_create(pscreen, PIPE_BIND_SHADER_BUFFER,
                            PIPE_USAGE_DEFAULT, FILL_BYTES);
   if (!buf) { LOG("FAIL: pipe_buffer_create returned NULL"); goto out; }
   LOG("buffer created: %u bytes", FILL_BYTES);

   pctx->clear_buffer(pctx, buf, 0, FILL_BYTES, &value, sizeof(value));

   pctx->flush(pctx, &fence, 0);
   if (fence) {
      pscreen->fence_finish(pscreen, pctx, fence, OS_TIMEOUT_INFINITE);
      pscreen->fence_reference(pscreen, &fence, NULL);
   }
   LOG("clear_buffer submitted + fenced");

   memset(out, 0, sizeof(out));
   pipe_buffer_read(pctx, buf, 0, FILL_BYTES, out);

   uint32_t bad = 0, first = 0;
   for (uint32_t i = 0; i < FILL_BYTES / 4; i++)
      if (out[i] != FILL_VALUE) { if (!bad) first = i; bad++; }

   if (bad == 0) {
      LOG("VERIFY OK: all %u words == 0x%08x", FILL_BYTES / 4, FILL_VALUE);
      LOG("=== PASSED ===");
   } else {
      LOG("VERIFY FAIL: %u/%u words wrong. First word[%u]=0x%08x",
          bad, FILL_BYTES / 4, first, out[first]);
      LOG("=== FAILED at verify ===");
   }

out:
   if (buf)     pipe_resource_reference(&buf, NULL);
   if (pctx)    pctx->destroy(pctx);
   if (pscreen) pscreen->destroy(pscreen);
   if (g_nvk_log) fclose(g_nvk_log);
   return 0;
}
