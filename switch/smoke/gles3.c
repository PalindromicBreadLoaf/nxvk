/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * GLES3 feature probe
 */
#include "gl_egl_harness.h"

#include <math.h>

#define FRAMES     180
#define INSTANCES  4
#define MRT_N      64

/* #pragma mark - instancing */

static const char *INST_VS =
   "#version 300 es\n"
   "layout(location = 0) in vec2 aPos;\n"
   "out vec4 vColor;\n"
   "void main() {\n"
   "   float id = float(gl_InstanceID);\n"
   "   vColor = vec4(id / 3.0, 1.0 - id / 3.0, 0.25, 1.0);\n"
   "   gl_Position = vec4(aPos + vec2((id - 1.5) * 0.45, 0.0), 0.0, 1.0);\n"
   "}\n";

static const char *INST_FS =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec4 vColor;\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = vColor; }\n";

static const float BAR[] = {
   -0.15f, -0.4f,  0.15f, -0.4f, -0.15f, 0.4f,  0.15f, 0.4f,
};

/* #pragma mark - MRT */

static const char *MRT_VS =
   "#version 300 es\n"
   "layout(location = 0) in vec2 aPos;\n"
   "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *MRT_FS =
   "#version 300 es\n"
   "precision mediump float;\n"
   "layout(location = 0) out vec4 outA;\n"
   "layout(location = 1) out vec4 outB;\n"
   "void main() {\n"
   "   outA = vec4(1.0, 0.0, 0.0, 1.0);\n"
   "   outB = vec4(0.0, 0.0, 1.0, 1.0);\n"
   "}\n";

static const float FULL_QUAD[] = {
   -1.0f, -1.0f,  1.0f, -1.0f, -1.0f, 1.0f,  1.0f, 1.0f,
};

/* #pragma mark - transform feedback */

static const char *XFB_VS =
   "#version 300 es\n"
   "layout(location = 0) in vec2 aPos;\n"
   "out vec4 vOut;\n"
   "void main() {\n"
   "   vOut = vec4(aPos * 2.0, 1.0, 7.0);\n"
   "   gl_Position = vec4(aPos, 0.0, 1.0);\n"
   "}\n";

static const char *XFB_FS =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec4 vOut;\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = vOut; }\n";

static const float XFB_TRI[] = {
   -0.5f, -0.5f,  0.5f, -0.5f,  0.0f, 0.5f,
};

static void
xfb_pre_link(GLuint prog, void *data)
{
   const char *varying = "vOut";

   (void)data;
   glTransformFeedbackVaryings(prog, 1, &varying, GL_INTERLEAVED_ATTRIBS);
}

/* #pragma mark - probes */

static bool
probe_instancing(const struct gl_egl *e, GLuint prog, GLuint vbo)
{
   uint8_t *px;
   bool ok = true;

   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         (void *)0);
   glEnableVertexAttribArray(0);
   glUseProgram(prog);

   glViewport(0, 0, e->width, e->height);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, INSTANCES);
   glFinish();
   if (!gl_no_error("glDrawArraysInstanced"))
      return false;

   px = gl_read_rgba(e->width, e->height);
   if (!px)
      return false;

   for (unsigned i = 0; i < INSTANCES; i++) {
      const float dx = ((float)i - 1.5f) * 0.45f;
      const uint8_t want[3] = {
         (uint8_t)(i * 255 / 3),
         (uint8_t)(255 - i * 255 / 3),
         64,
      };
      const uint8_t *got = gl_px_at(px, e->width, e->height,
                                    (dx + 1.0f) * 0.5f, 0.5f);
      char tag[32];

      snprintf(tag, sizeof(tag), "instance %u", i);
      gl_log_px(tag, got);
      if (!gl_px_near(got, want[0], want[1], want[2], 6)) {
         LOG("  expected {%u,%u,%u}", want[0], want[1], want[2]);
         ok = false;
      }
   }

   free(px);
   LOG("instancing: %s", ok ? "PASS" : "FAIL");
   return ok;
}

static bool
probe_mrt(GLuint prog, GLuint vbo)
{
   static const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
   static const uint8_t want[2][3] = {{255, 0, 0}, {0, 0, 255}};
   GLuint fbo = 0, tex[2] = {0, 0};
   bool ok = true;

   glGenTextures(2, tex);
   for (unsigned i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MRT_N, MRT_N, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   }

   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   for (unsigned i = 0; i < 2; i++)
      glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, tex[i], 0);

   GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (status != GL_FRAMEBUFFER_COMPLETE) {
      LOG("  two-attachment FBO incomplete, status = 0x%04x", status);
      ok = false;
      goto out;
   }

   glDrawBuffers(2, bufs);
   glViewport(0, 0, MRT_N, MRT_N);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);

   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         (void *)0);
   glEnableVertexAttribArray(0);
   glUseProgram(prog);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glFinish();
   if (!gl_no_error("MRT draw")) {
      ok = false;
      goto out;
   }

   for (unsigned i = 0; i < 2; i++) {
      uint8_t px[4] = {0, 0, 0, 0};
      char tag[32];

      glReadBuffer(bufs[i]);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(MRT_N / 2, MRT_N / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      if (!gl_no_error("MRT readback")) {
         ok = false;
         break;
      }

      snprintf(tag, sizeof(tag), "attachment %u", i);
      gl_log_px(tag, px);
      if (!gl_px_near(px, want[i][0], want[i][1], want[i][2], 4)) {
         LOG("  expected {%u,%u,%u}", want[i][0], want[i][1], want[i][2]);
         ok = false;
      }
   }

out:
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glDeleteFramebuffers(1, &fbo);
   glDeleteTextures(2, tex);
   LOG("MRT: %s", ok ? "PASS" : "FAIL");
   return ok;
}

static bool
probe_xfb(GLuint prog, GLuint vbo)
{
   const size_t bytes = sizeof(float) * 4 * 3;
   GLuint xfb_buf = 0;
   const float *got;
   bool ok = true;

   glGenBuffers(1, &xfb_buf);
   glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, xfb_buf);
   glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, bytes, NULL, GL_DYNAMIC_READ);
   glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfb_buf);

   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         (void *)0);
   glEnableVertexAttribArray(0);
   glUseProgram(prog);

   glEnable(GL_RASTERIZER_DISCARD);
   glBeginTransformFeedback(GL_TRIANGLES);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glEndTransformFeedback();
   glDisable(GL_RASTERIZER_DISCARD);
   glFinish();
   if (!gl_no_error("transform feedback draw")) {
      ok = false;
      goto out;
   }

   got = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, bytes,
                          GL_MAP_READ_BIT);
   if (!got) {
      LOG("  glMapBufferRange returned NULL (0x%04x)", glGetError());
      ok = false;
      goto out;
   }

   for (unsigned v = 0; v < 3; v++) {
      const float want[4] = {
         XFB_TRI[v * 2] * 2.0f, XFB_TRI[v * 2 + 1] * 2.0f, 1.0f, 7.0f,
      };

      LOG("  vertex %u = {%.3f, %.3f, %.3f, %.3f}", v, got[v * 4],
          got[v * 4 + 1], got[v * 4 + 2], got[v * 4 + 3]);
      for (unsigned c = 0; c < 4; c++) {
         if (fabsf(got[v * 4 + c] - want[c]) > 0.001f) {
            LOG("  expected {%.3f, %.3f, %.3f, %.3f}", want[0], want[1],
                want[2], want[3]);
            ok = false;
            break;
         }
      }
   }
   glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);

out:
   glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
   glDeleteBuffers(1, &xfb_buf);
   LOG("transform feedback: %s", ok ? "PASS" : "FAIL");
   return ok;
}

int main(void)
{
   const struct gl_stage_src inst_stages[] = {
      {GL_VERTEX_SHADER, INST_VS}, {GL_FRAGMENT_SHADER, INST_FS},
   };
   const struct gl_stage_src mrt_stages[] = {
      {GL_VERTEX_SHADER, MRT_VS}, {GL_FRAGMENT_SHADER, MRT_FS},
   };
   const struct gl_stage_src xfb_stages[] = {
      {GL_VERTEX_SHADER, XFB_VS}, {GL_FRAGMENT_SHADER, XFB_FS},
   };
   struct gl_egl e;
   GLuint vao = 0, bar_vbo = 0, quad_vbo = 0, tri_vbo = 0;
   GLuint inst = 0, mrt = 0, xfb = 0;
   bool ok_inst = false, ok_mrt = false, ok_xfb = false;
   int rc = 0, presented = 0;

   nvk_log_open("sdmc:/gles3.log");
   LOG("=== gles3 ===");
   gl_headless_env("sdmc:/gles3_mesa.log");

   if (!gl_egl_up(&e, EGL_OPENGL_ES_API, 3, 0)) {
      rc = 1;
      goto out;
   }

   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);

   glGenBuffers(1, &bar_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, bar_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(BAR), BAR, GL_STATIC_DRAW);
   glGenBuffers(1, &quad_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(FULL_QUAD), FULL_QUAD, GL_STATIC_DRAW);
   glGenBuffers(1, &tri_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, tri_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(XFB_TRI), XFB_TRI, GL_STATIC_DRAW);

   inst = gl_build_program(inst_stages, ARRAY_SIZE(inst_stages), NULL, 0);
   mrt  = gl_build_program(mrt_stages, ARRAY_SIZE(mrt_stages), NULL, 0);
   xfb  = gl_build_program_ex(xfb_stages, ARRAY_SIZE(xfb_stages), NULL, 0,
                              xfb_pre_link, NULL);

   LOG("-- instancing --");
   ok_inst = inst && probe_instancing(&e, inst, bar_vbo);
   LOG("-- multiple render targets --");
   ok_mrt = mrt && probe_mrt(mrt, quad_vbo);
   LOG("-- transform feedback --");
   ok_xfb = xfb && probe_xfb(xfb, tri_vbo);

   if (!ok_inst || !ok_mrt || !ok_xfb)
      rc = 1;

   /* Leave the instanced bars on screen so the run is visible. */
   if (inst) {
      LOG("entering present loop (%d frames)", FRAMES);
      for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         glBindBuffer(GL_ARRAY_BUFFER, bar_vbo);
         glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                               (void *)0);
         glEnableVertexAttribArray(0);
         glUseProgram(inst);
         glViewport(0, 0, e.width, e.height);
         glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, INSTANCES);

         if (!gl_present(&e, frame, FRAMES)) {
            rc = 1;
            break;
         }
         presented++;
      }
      LOG("presented %d/%d frames", presented, FRAMES);
   }

   LOG("instancing=%s MRT=%s transform-feedback=%s",
       ok_inst ? "PASS" : "FAIL", ok_mrt ? "PASS" : "FAIL",
       ok_xfb ? "PASS" : "FAIL");
   if (rc == 0 && presented == FRAMES) {
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED ===");
      rc = 1;
   }

out:
   if (inst)     glDeleteProgram(inst);
   if (mrt)      glDeleteProgram(mrt);
   if (xfb)      glDeleteProgram(xfb);
   if (bar_vbo)  glDeleteBuffers(1, &bar_vbo);
   if (quad_vbo) glDeleteBuffers(1, &quad_vbo);
   if (tri_vbo)  glDeleteBuffers(1, &tri_vbo);
   if (vao)      glDeleteVertexArrays(1, &vao);
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
