/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Texture upload, sampler state and UV interpolation on a fullscreen quad.
 */
#include "gl_egl_harness.h"

#define FRAMES 180
#define TEX_N  64u

static const char *VS_SRC =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "attribute vec2 aUV;\n"
   "varying vec2 vUV;\n"
   "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS_SRC =
   "#version 100\n"
   "precision mediump float;\n"
   "uniform sampler2D uTex;\n"
   "varying vec2 vUV;\n"
   "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

/* A triangle strip covering the viewport, v=0 at the bottom. */
static const float QUAD[] = {
   -1.0f, -1.0f, 0.0f, 0.0f,
    1.0f, -1.0f, 1.0f, 0.0f,
   -1.0f,  1.0f, 0.0f, 1.0f,
    1.0f,  1.0f, 1.0f, 1.0f,
};

/* Quadrant colours, indexed [v > 0.5][u > 0.5]. */
static const uint8_t QUADRANT[2][2][3] = {
   {{224, 32, 32}, {32, 224, 32}},
   {{32, 64, 224}, {232, 232, 232}},
};

static void
fill_texture(uint8_t *px)
{
   for (unsigned y = 0; y < TEX_N; y++) {
      for (unsigned x = 0; x < TEX_N; x++) {
         const uint8_t *c = QUADRANT[y >= TEX_N / 2][x >= TEX_N / 2];
         uint8_t *t = &px[(y * TEX_N + x) * 4];

         t[0] = c[0];
         t[1] = c[1];
         t[2] = c[2];
         t[3] = 255;
      }
   }
}

static bool
verify(const struct gl_egl *e)
{
   uint8_t *px = gl_read_rgba(e->width, e->height);
   bool ok = true;

   if (!px)
      return false;

   for (unsigned v = 0; v < 2; v++) {
      for (unsigned u = 0; u < 2; u++) {
         const uint8_t *c = QUADRANT[v][u];
         const uint8_t *got = gl_px_at(px, e->width, e->height,
                                       u ? 0.75f : 0.25f, v ? 0.75f : 0.25f);
         char tag[32];

         snprintf(tag, sizeof(tag), "quadrant u%u v%u", u, v);
         gl_log_px(tag, got);
         if (!gl_px_near(got, c[0], c[1], c[2], 4)) {
            LOG("VERIFY FAIL: %s expected {%u,%u,%u}", tag, c[0], c[1], c[2]);
            ok = false;
         }
      }
   }

   free(px);
   return ok;
}

int main(void)
{
   const struct gl_stage_src stages[] = {
      {GL_VERTEX_SHADER,   VS_SRC},
      {GL_FRAGMENT_SHADER, FS_SRC},
   };
   const char *const attribs[] = {"aPos", "aUV"};
   struct gl_egl e;
   uint8_t *texels = NULL;
   GLuint prog = 0, vbo = 0, tex = 0;
   bool verified = false, ok = false;
   int rc = 0, presented = 0;

   nvk_log_open("sdmc:/gl_tex.log");
   LOG("=== gl_tex ===");
   gl_headless_env("sdmc:/gl_tex_mesa.log");

   if (!gl_egl_up(&e, EGL_OPENGL_ES_API, 2, 0)) {
      rc = 1;
      goto out;
   }

   prog = gl_build_program(stages, ARRAY_SIZE(stages), attribs,
                           ARRAY_SIZE(attribs));
   if (!prog) {
      rc = 1;
      goto out;
   }

   texels = malloc(TEX_N * TEX_N * 4);
   if (!texels) {
      LOG("FAIL: out of memory for the texture");
      rc = 1;
      goto out;
   }
   fill_texture(texels);

   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_N, TEX_N, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, texels);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (!gl_no_error("glTexImage2D")) {
      rc = 1;
      goto out;
   }
   LOG("uploaded a %ux%u RGBA8 texture", TEX_N, TEX_N);

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                         (void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                         (void *)(2 * sizeof(float)));
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);

   glViewport(0, 0, e.width, e.height);
   glUseProgram(prog);
   glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex);

   LOG("entering present loop (%d frames)", FRAMES);
   for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      if (!verified) {
         glFinish();
         if (!gl_no_error("draw")) {
            rc = 1;
            break;
         }
         ok = verify(&e);
         verified = true;
      }

      if (!gl_present(&e, frame, FRAMES)) {
         rc = 1;
         break;
      }
      presented++;
   }

   LOG("presented %d/%d frames", presented, FRAMES);
   if (rc == 0 && ok && presented == FRAMES) {
      LOG("VERIFY OK: all four texture quadrants sampled correctly");
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED: verify=%d presented=%d/%d ===", ok, presented, FRAMES);
      rc = 1;
   }

out:
   if (tex)  glDeleteTextures(1, &tex);
   if (vbo)  glDeleteBuffers(1, &vbo);
   if (prog) glDeleteProgram(prog);
   free(texels);
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
