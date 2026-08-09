/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Render to an FBO with a depth attachment.
 */
#include "gl_egl_harness.h"

#define FRAMES 180
#define FBO_N  256

static const char *SOLID_VS =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "uniform float uZ;\n"
   "void main() { gl_Position = vec4(aPos, uZ, 1.0); }\n";

static const char *SOLID_FS =
   "#version 100\n"
   "precision mediump float;\n"
   "uniform vec4 uColor;\n"
   "void main() { gl_FragColor = uColor; }\n";

static const char *TEX_VS =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "attribute vec2 aUV;\n"
   "varying vec2 vUV;\n"
   "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *TEX_FS =
   "#version 100\n"
   "precision mediump float;\n"
   "uniform sampler2D uTex;\n"
   "varying vec2 vUV;\n"
   "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

/* Two quads overlapping in x. The near one is drawn first so the far one has
 * to lose the depth test where they meet.
 */
static const float NEAR_QUAD[] = {
   -0.8f, -0.8f,  0.2f, -0.8f, -0.8f, 0.8f,  0.2f, 0.8f,
};
static const float FAR_QUAD[] = {
   -0.2f, -0.8f,  0.8f, -0.8f, -0.2f, 0.8f,  0.8f, 0.8f,
};
static const float SCREEN_QUAD[] = {
   -1.0f, -1.0f, 0.0f, 0.0f,
    1.0f, -1.0f, 1.0f, 0.0f,
   -1.0f,  1.0f, 0.0f, 1.0f,
    1.0f,  1.0f, 1.0f, 1.0f,
};

static const uint8_t BG[3]    = {16, 16, 96};
static const uint8_t NEARC[3] = {224, 32, 32};
static const uint8_t FARC[3]  = {32, 224, 32};

struct probe {
   float       u, v;
   const uint8_t *want;
   const char *what;
};

static bool
verify(const struct gl_egl *e)
{
   const struct probe probes[] = {
      {0.15f, 0.50f, NEARC, "near quad only"},
      {0.50f, 0.50f, NEARC, "overlap (depth test kept the near quad)"},
      {0.85f, 0.50f, FARC,  "far quad only"},
      {0.50f, 0.02f, BG,    "below both quads"},
   };
   uint8_t *px = gl_read_rgba(e->width, e->height);
   bool ok = true;

   if (!px)
      return false;

   for (unsigned i = 0; i < ARRAY_SIZE(probes); i++) {
      const uint8_t *got = gl_px_at(px, e->width, e->height, probes[i].u,
                                    probes[i].v);

      gl_log_px(probes[i].what, got);
      if (!gl_px_near(got, probes[i].want[0], probes[i].want[1],
                      probes[i].want[2], 6)) {
         LOG("VERIFY FAIL: %s expected {%u,%u,%u}", probes[i].what,
             probes[i].want[0], probes[i].want[1], probes[i].want[2]);
         ok = false;
      }
   }

   free(px);
   return ok;
}

int main(void)
{
   const struct gl_stage_src solid_stages[] = {
      {GL_VERTEX_SHADER,   SOLID_VS},
      {GL_FRAGMENT_SHADER, SOLID_FS},
   };
   const struct gl_stage_src tex_stages[] = {
      {GL_VERTEX_SHADER,   TEX_VS},
      {GL_FRAGMENT_SHADER, TEX_FS},
   };
   const char *const solid_attribs[] = {"aPos"};
   const char *const tex_attribs[] = {"aPos", "aUV"};
   struct gl_egl e;
   GLuint solid = 0, textured = 0;
   GLuint vbo_near = 0, vbo_far = 0, vbo_screen = 0;
   GLuint fbo = 0, color = 0, depth = 0;
   GLint uz = -1, ucolor = -1;
   bool verified = false, ok = false;
   int rc = 0, presented = 0;

   nvk_log_open("sdmc:/gl_fbo.log");
   LOG("=== gl_fbo ===");
   gl_headless_env("sdmc:/gl_fbo_mesa.log");

   if (!gl_egl_up(&e, EGL_OPENGL_ES_API, 2, 0)) {
      rc = 1;
      goto out;
   }

   solid = gl_build_program(solid_stages, ARRAY_SIZE(solid_stages),
                            solid_attribs, ARRAY_SIZE(solid_attribs));
   textured = gl_build_program(tex_stages, ARRAY_SIZE(tex_stages),
                               tex_attribs, ARRAY_SIZE(tex_attribs));
   if (!solid || !textured) {
      rc = 1;
      goto out;
   }
   uz = glGetUniformLocation(solid, "uZ");
   ucolor = glGetUniformLocation(solid, "uColor");

   glGenTextures(1, &color);
   glBindTexture(GL_TEXTURE_2D, color);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FBO_N, FBO_N, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

   glGenRenderbuffers(1, &depth);
   glBindRenderbuffer(GL_RENDERBUFFER, depth);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, FBO_N, FBO_N);

   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          color, 0);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_RENDERBUFFER, depth);

   GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (status != GL_FRAMEBUFFER_COMPLETE) {
      LOG("FAIL: framebuffer incomplete, status = 0x%04x", status);
      rc = 1;
      goto out;
   }
   LOG("FBO %dx%d complete",
       FBO_N, FBO_N);

   glGenBuffers(1, &vbo_near);
   glBindBuffer(GL_ARRAY_BUFFER, vbo_near);
   glBufferData(GL_ARRAY_BUFFER, sizeof(NEAR_QUAD), NEAR_QUAD, GL_STATIC_DRAW);
   glGenBuffers(1, &vbo_far);
   glBindBuffer(GL_ARRAY_BUFFER, vbo_far);
   glBufferData(GL_ARRAY_BUFFER, sizeof(FAR_QUAD), FAR_QUAD, GL_STATIC_DRAW);
   glGenBuffers(1, &vbo_screen);
   glBindBuffer(GL_ARRAY_BUFFER, vbo_screen);
   glBufferData(GL_ARRAY_BUFFER, sizeof(SCREEN_QUAD), SCREEN_QUAD,
                GL_STATIC_DRAW);

   LOG("entering present loop (%d frames)", FRAMES);
   for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
      /* pass 1: depth-tested quads into the FBO */
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glViewport(0, 0, FBO_N, FBO_N);
      glClearColor(BG[0] / 255.0f, BG[1] / 255.0f, BG[2] / 255.0f, 1.0f);
      glClearDepthf(1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);

      glUseProgram(solid);
      glEnableVertexAttribArray(0);
      glDisableVertexAttribArray(1);

      glBindBuffer(GL_ARRAY_BUFFER, vbo_near);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                            (void *)0);
      glUniform1f(uz, 0.0f);
      glUniform4f(ucolor, NEARC[0] / 255.0f, NEARC[1] / 255.0f,
                  NEARC[2] / 255.0f, 1.0f);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      glBindBuffer(GL_ARRAY_BUFFER, vbo_far);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                            (void *)0);
      glUniform1f(uz, 0.5f);
      glUniform4f(ucolor, FARC[0] / 255.0f, FARC[1] / 255.0f,
                  FARC[2] / 255.0f, 1.0f);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      /* pass 2: sample the FBO onto the window */
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDisable(GL_DEPTH_TEST);
      glViewport(0, 0, e.width, e.height);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      glUseProgram(textured);
      glBindBuffer(GL_ARRAY_BUFFER, vbo_screen);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            (void *)0);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            (void *)(2 * sizeof(float)));
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glUniform1i(glGetUniformLocation(textured, "uTex"), 0);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, color);
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
      LOG("VERIFY OK: render-to-texture with a working depth attachment");
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED: verify=%d presented=%d/%d ===", ok, presented, FRAMES);
      rc = 1;
   }

out:
   if (fbo)        glDeleteFramebuffers(1, &fbo);
   if (depth)      glDeleteRenderbuffers(1, &depth);
   if (color)      glDeleteTextures(1, &color);
   if (vbo_near)   glDeleteBuffers(1, &vbo_near);
   if (vbo_far)    glDeleteBuffers(1, &vbo_far);
   if (vbo_screen) glDeleteBuffers(1, &vbo_screen);
   if (solid)      glDeleteProgram(solid);
   if (textured)   glDeleteProgram(textured);
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
