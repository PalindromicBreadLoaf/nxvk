/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * glClear and glReadPixels round-trip validation.
 */
#include "gl_harness.h"

#define FB_W 128u
#define FB_H 128u

/* Clear colour whose 8-bit unorm image is exactly {64,128,192,255}. */
static const float CLEAR_RGBA[4] = { 64.0f/255, 128.0f/255, 192.0f/255, 1.0f };
static const uint8_t EXPECT_RGBA[4] = { 64, 128, 192, 255 };

int main(void)
{
   struct gl_headless h;
   static uint8_t pixels[FB_W * FB_H * 4];
   int rc = 0;

   nvk_log_open("sdmc:/gl_smoke.log");
   LOG("=== gl_smoke ===");
   gl_headless_env("sdmc:/gl_smoke_mesa.log");

   if (!gl_headless_up(&h, API_OPENGL_COMPAT, FB_W, FB_H)) { rc = 1; goto out; }

   glViewport(0, 0, FB_W, FB_H);
   glClearColor(CLEAR_RGBA[0], CLEAR_RGBA[1], CLEAR_RGBA[2], CLEAR_RGBA[3]);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glFinish();

   GLenum gerr = glGetError();
   if (gerr != GL_NO_ERROR)
      LOG("WARN: glGetError after clear = 0x%04x", gerr);

   memset(pixels, 0, sizeof(pixels));
   glReadPixels(0, 0, FB_W, FB_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   gerr = glGetError();
   if (gerr != GL_NO_ERROR) {
      LOG("FAIL: glReadPixels glGetError = 0x%04x", gerr);
      rc = 1; goto out;
   }

   unsigned bad = 0, first = 0;
   for (unsigned p = 0; p < FB_W * FB_H; p++) {
      const uint8_t *px = &pixels[p * 4];
      for (int c = 0; c < 4; c++) {
         int d = (int)px[c] - (int)EXPECT_RGBA[c];
         if (d < -1 || d > 1) { if (!bad) first = p; bad++; break; }
      }
   }

   if (bad == 0) {
      LOG("VERIFY OK: all %u px == {%u,%u,%u,%u}", FB_W * FB_H,
          EXPECT_RGBA[0], EXPECT_RGBA[1], EXPECT_RGBA[2], EXPECT_RGBA[3]);
      LOG("=== PASSED ===");
   } else {
      const uint8_t *px = &pixels[first * 4];
      LOG("VERIFY FAIL: %u/%u px wrong. First px[%u]={%u,%u,%u,%u} want {%u,%u,%u,%u}",
          bad, FB_W * FB_H, first, px[0], px[1], px[2], px[3],
          EXPECT_RGBA[0], EXPECT_RGBA[1], EXPECT_RGBA[2], EXPECT_RGBA[3]);
      LOG("=== FAILED at verify ===");
      rc = 1;
   }

out:
   gl_headless_down(&h);
   if (g_nvk_log) fclose(g_nvk_log);
   return rc;
}
