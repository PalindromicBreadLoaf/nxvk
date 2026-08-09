/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Isolates why a readback of the window comes back empty.
 *
 * Nothing here presents.
 */
#include "gl_egl_harness.h"

#define FBO_N 256u

/* Distinct from both the clear colour and zero, so that "nothing happened" and "the
 * draw happened" are never confusable. */
#define CLEAR_R 0.125f  /* 32 */
#define CLEAR_G 0.500f  /* 128 */
#define CLEAR_B 0.750f  /* 191 */

static const uint8_t CLEAR_RGB[3] = {32, 128, 191};
static const uint8_t DRAW_RGB[3]  = {224, 32, 32};

static const char *VS_SRC =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS_SRC =
   "#version 100\n"
   "precision mediump float;\n"
   "void main() { gl_FragColor = vec4(0.878, 0.125, 0.125, 1.0); }\n";

/* Desktop core rejects the ES 1.00 shaders. */
static const char *VS_SRC_CORE =
   "#version 150\n"
   "in vec2 aPos;\n"
   "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS_SRC_CORE =
   "#version 150\n"
   "out vec4 oColour;\n"
   "void main() { oColour = vec4(0.878, 0.125, 0.125, 1.0); }\n";

static const char *VS_TEX_SRC =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "attribute vec2 aUV;\n"
   "varying vec2 vUV;\n"
   "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS_TEX_SRC =
   "#version 100\n"
   "precision mediump float;\n"
   "uniform sampler2D uTex;\n"
   "varying vec2 vUV;\n"
   "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

static const char *VS_TEX_SRC_CORE =
   "#version 150\n"
   "in vec2 aPos;\n"
   "in vec2 aUV;\n"
   "out vec2 vUV;\n"
   "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS_TEX_SRC_CORE =
   "#version 150\n"
   "uniform sampler2D uTex;\n"
   "in vec2 vUV;\n"
   "out vec4 oColour;\n"
   "void main() { oColour = texture(uTex, vUV); }\n";

/* Covers the middle half of the target, so a probe at the centre lands on the
 * draw and a probe near the edge lands on the clear. */
static const float QUAD[] = {
   -0.5f, -0.5f,
    0.5f, -0.5f,
   -0.5f,  0.5f,
    0.5f,  0.5f,
};

/* pos.xy, uv.xy */
static const float QUAD_UV[] = {
   -0.5f, -0.5f, 0.0f, 0.0f,
    0.5f, -0.5f, 1.0f, 0.0f,
   -0.5f,  0.5f, 0.0f, 1.0f,
    0.5f,  0.5f, 1.0f, 1.0f,
};

struct probe_result {
   bool centre_ok;
   bool edge_ok;
   bool all_zero;
   bool no_read;
   uint8_t centre[4];
   uint8_t edge[4];
};

/* An all zero buffer is the signature failure. */
static void
probe_read(struct probe_result *r, unsigned w, unsigned h, bool expect_draw)
{
   uint8_t *px = gl_read_rgba(w, h);

   memset(r, 0, sizeof(*r));
   if (px == NULL) {
      r->no_read = true;
      return;
   }

   memcpy(r->centre, gl_px_at(px, w, h, 0.5f, 0.5f), 4);
   memcpy(r->edge, gl_px_at(px, w, h, 0.05f, 0.05f), 4);

   r->all_zero = gl_count_near(px, w, h, 0, 0, 0, 0) == (size_t)w * h;

   const uint8_t *want_centre = expect_draw ? DRAW_RGB : CLEAR_RGB;
   r->centre_ok = gl_px_near(r->centre, want_centre[0], want_centre[1],
                             want_centre[2], 4);
   r->edge_ok = gl_px_near(r->edge, CLEAR_RGB[0], CLEAR_RGB[1], CLEAR_RGB[2], 4);

   free(px);
}

static void
probe_log(const char *what, const struct probe_result *r, bool expect_draw)
{
   if (r->no_read) {
      LOG("  %-34s NO READBACK", what);
      return;
   }

   LOG("  %-34s centre={%u,%u,%u,%u} edge={%u,%u,%u,%u} %s%s", what,
       r->centre[0], r->centre[1], r->centre[2], r->centre[3],
       r->edge[0], r->edge[1], r->edge[2], r->edge[3],
       (r->centre_ok && r->edge_ok) ? "OK" : "MISMATCH",
       r->all_zero ? " (ENTIRE BUFFER ZERO)" : "");
   if (!r->centre_ok)
      LOG("      centre expected {%u,%u,%u}",
          expect_draw ? DRAW_RGB[0] : CLEAR_RGB[0],
          expect_draw ? DRAW_RGB[1] : CLEAR_RGB[1],
          expect_draw ? DRAW_RGB[2] : CLEAR_RGB[2]);
   if (!r->edge_ok)
      LOG("      edge expected {%u,%u,%u} (the clear)",
          CLEAR_RGB[0], CLEAR_RGB[1], CLEAR_RGB[2]);
}

static void
draw_scene(unsigned w, unsigned h, GLuint prog, GLuint vbo, bool with_draw)
{
   glViewport(0, 0, w, h);
   glClearColor(CLEAR_R, CLEAR_G, CLEAR_B, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   if (with_draw) {
      glBindBuffer(GL_ARRAY_BUFFER, vbo);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                            (void *)0);
      glEnableVertexAttribArray(0);
      glUseProgram(prog);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }

   glFinish();
}

/* Reading the same unchanged buffer again separates "the pixels never arrived"
 * from "the pixels arrived after the first read", which is what a missing
 * cache invalidate on the readback looks like.
 */
#define REREAD_N 6

static bool
probe_reread(unsigned w, unsigned h, unsigned first, GLuint prog, GLuint vbo)
{
   struct probe_result r, prev;
   unsigned turned = 0;
   bool first_ok = false;

   /* The textured probe left a picture of its own on the window, and a re-read
    * can only be judged against a known one. */
   draw_scene(w, h, prog, vbo, true);
   memset(&prev, 0, sizeof(prev));

   for (unsigned i = 0; i < REREAD_N; i++) {
      char label[48];

      gl_mark("re-read %u of the unchanged window", first + i);
      probe_read(&r, w, h, true);

      if (i == 0) {
         probe_log("window, re-read (no redraw)", &r, true);
         first_ok = !r.no_read && r.centre_ok && r.edge_ok;
      } else if (r.no_read != prev.no_read ||
                 memcmp(r.centre, prev.centre, 4) != 0 ||
                 memcmp(r.edge, prev.edge, 4) != 0) {
         snprintf(label, sizeof(label), "window, re-read %u differs", first + i);
         probe_log(label, &r, true);
         if (!turned)
            turned = first + i;
      }

      prev = r;
   }

   if (!first_ok) {
      LOG("  the first re-read is already wrong, so the later ones say nothing "
          "about when a readback turns");
      return false;
   }

   if (!turned) {
      LOG("  %u re-reads of the unchanged window all agree", REREAD_N);
      return true;
   }

   LOG("  >>> readback %u is the first to answer differently, with nothing "
       "drawn since readback %u", turned, first);
   return false;
}

/* Same scene, same expected picture, but the fragment colour arrives through a
 * sampler.
 */
#define TEX_N 4u

static void
probe_textured(bool *tex_ok, unsigned w, unsigned h, bool core)
{
   const struct gl_stage_src stages[] = {
      {GL_VERTEX_SHADER,   core ? VS_TEX_SRC_CORE : VS_TEX_SRC},
      {GL_FRAGMENT_SHADER, core ? FS_TEX_SRC_CORE : FS_TEX_SRC},
   };
   const char *const attribs[] = {"aPos", "aUV"};
   uint8_t texels[TEX_N * TEX_N * 4];
   GLuint prog = 0, vbo = 0, tex = 0;
   struct probe_result r;

   for (unsigned i = 0; i < TEX_N * TEX_N; i++) {
      texels[i * 4 + 0] = DRAW_RGB[0];
      texels[i * 4 + 1] = DRAW_RGB[1];
      texels[i * 4 + 2] = DRAW_RGB[2];
      texels[i * 4 + 3] = 255;
   }

   prog = gl_build_program(stages, ARRAY_SIZE(stages), attribs,
                           ARRAY_SIZE(attribs));
   if (!prog) {
      LOG("  no textured program, skipping the sampler probe");
      return;
   }

   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_N, TEX_N, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, texels);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_UV), QUAD_UV, GL_STATIC_DRAW);

   gl_mark("textured window probe");
   glViewport(0, 0, w, h);
   glClearColor(CLEAR_R, CLEAR_G, CLEAR_B, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   glUseProgram(prog);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                         (void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                         (void *)(2 * sizeof(float)));
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glFinish();

   probe_read(&r, w, h, true);
   probe_log("window, textured draw", &r, true);
   *tex_ok = r.centre_ok && r.edge_ok;

   glDisableVertexAttribArray(1);
   glDeleteBuffers(1, &vbo);
   glDeleteTextures(1, &tex);
   glDeleteProgram(prog);
}

/* Same scene into an ordinary FBO. If this reads back correctly while the window
 * does not, the swapchain image is what cannot be read, not readback at large.
 */
static bool
probe_fbo(bool *fbo_ok, GLuint prog, GLuint vbo)
{
   GLuint fbo = 0, tex = 0, depth = 0;
   struct probe_result r;
   bool made = false;

   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FBO_N, FBO_N, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   glGenRenderbuffers(1, &depth);
   glBindRenderbuffer(GL_RENDERBUFFER, depth);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, FBO_N, FBO_N);

   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          tex, 0);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_RENDERBUFFER, depth);

   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      LOG("  FBO incomplete, skipping the offscreen probe");
      goto out;
   }

   gl_mark("FBO probe");
   draw_scene(FBO_N, FBO_N, prog, vbo, true);
   probe_read(&r, FBO_N, FBO_N, true);
   probe_log("FBO, cleared + drawn", &r, true);
   *fbo_ok = r.centre_ok && r.edge_ok;
   made = true;

out:
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   if (fbo)   glDeleteFramebuffers(1, &fbo);
   if (tex)   glDeleteTextures(1, &tex);
   if (depth) glDeleteRenderbuffers(1, &depth);
   return made;
}

struct phase_result {
   bool ran;
   bool clear_only;
   bool drawn;
   bool textured;
   bool reread;
   bool fbo;
};

static void
run_phase(struct gl_egl *e, const char *name, bool core,
          struct phase_result *out)
{
   const struct gl_stage_src stages[] = {
      {GL_VERTEX_SHADER,   core ? VS_SRC_CORE : VS_SRC},
      {GL_FRAGMENT_SHADER, core ? FS_SRC_CORE : FS_SRC},
   };
   const char *const attribs[] = {"aPos"};
   GLuint prog = 0, vbo = 0, vao = 0;
   struct probe_result r;

   memset(out, 0, sizeof(*out));

   LOG("=== %s ===", name);
   gl_mark("phase %s", name);
   LOG("  GL_VERSION = %s", (const char *)glGetString(GL_VERSION));

   /* A core profile draws from nothing without a bound VAO. */
   if (core) {
      glGenVertexArrays(1, &vao);
      glBindVertexArray(vao);
   }

   prog = gl_build_program(stages, ARRAY_SIZE(stages), attribs,
                           ARRAY_SIZE(attribs));
   if (!prog) {
      LOG("  no program, skipping this phase");
      goto out;
   }

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);

   gl_mark("clear-only probe");
   draw_scene(e->width, e->height, prog, vbo, false);
   probe_read(&r, e->width, e->height, false);
   probe_log("window, clear only (no draw)", &r, false);
   out->clear_only = r.centre_ok && r.edge_ok;

   gl_mark("cleared+drawn probe");
   draw_scene(e->width, e->height, prog, vbo, true);
   probe_read(&r, e->width, e->height, true);
   probe_log("window, cleared + drawn", &r, true);
   out->drawn = r.centre_ok && r.edge_ok;

   /* Readback 3. */
   probe_textured(&out->textured, e->width, e->height, core);

   out->reread = probe_reread(e->width, e->height, 4, prog, vbo);
   probe_fbo(&out->fbo, prog, vbo);

   out->ran = true;

out:
   if (vbo)  glDeleteBuffers(1, &vbo);
   if (prog) glDeleteProgram(prog);
   if (vao)  glDeleteVertexArrays(1, &vao);
}

static const char *
probe_verdict(bool ran, bool ok)
{
   return !ran ? "n/a" : ok ? "PASS" : "FAIL";
}

int main(void)
{
   struct gl_egl e;
   struct phase_result gles = {0}, core = {0};
   int rc = 0;

   nvk_log_open("sdmc:/gl_diag.log");
   LOG("=== gl_diag ===");
   gl_headless_env("sdmc:/gl_diag_mesa.log");

   /* GLES first */
   if (!gl_egl_display_up(&e, EGL_OPENGL_ES_API, true)) {
      rc = 1;
      goto out;
   }
   if (gl_egl_context_up(&e, 3, 0, false) || gl_egl_context_up(&e, 2, 0, false))
      run_phase(&e, "GLES context", false, &gles);
   else
      LOG("FAIL: no GLES context");
   gl_egl_context_down(&e);
   gl_egl_down(&e);

   if (!gl_egl_display_up(&e, EGL_OPENGL_API, true)) {
      LOG("FAIL: no desktop GL display");
      goto report;
   }
   if (gl_egl_context_up(&e, 4, 5, true) || gl_egl_context_up(&e, 3, 3, true))
      run_phase(&e, "GL core context", true, &core);
   else
      LOG("FAIL: no desktop GL core context");

report:
   LOG("");
   LOG("=== result matrix ===");
   LOG("  %-28s %-8s %-8s", "probe", "GLES", "GL core");
   LOG("  %-28s %-8s %-8s", "window, clear only",
       probe_verdict(gles.ran, gles.clear_only), probe_verdict(core.ran, core.clear_only));
   LOG("  %-28s %-8s %-8s", "window, cleared + drawn",
       probe_verdict(gles.ran, gles.drawn), probe_verdict(core.ran, core.drawn));
   LOG("  %-28s %-8s %-8s", "window, textured draw",
       probe_verdict(gles.ran, gles.textured), probe_verdict(core.ran, core.textured));
   LOG("  %-28s %-8s %-8s", "window, re-read (no redraw)",
       probe_verdict(gles.ran, gles.reread), probe_verdict(core.ran, core.reread));
   LOG("  %-28s %-8s %-8s", "FBO, cleared + drawn",
       probe_verdict(gles.ran, gles.fbo), probe_verdict(core.ran, core.fbo));

   if (gles.ran && core.ran) {
      if (!gles.drawn && core.drawn)
         LOG("VERDICT: the context type decides it. GLES reads the window "
             "empty where GL core reads it correctly");
      else if (!gles.drawn && gles.fbo)
         LOG("VERDICT: only the window is unreadable. The FBO round-trips, so "
             "the swapchain image is the problem");
      else if (!gles.clear_only && !gles.drawn)
         LOG("VERDICT: even a bare clear does not read back, so no draw is "
             "involved");
      else if (gles.drawn && core.drawn && !gles.textured && !core.textured)
         LOG("VERDICT: a sampler is what breaks it. The same picture drawn "
             "flat reads back and drawn through a texture does not");
      else if (!gles.reread || !core.reread)
         LOG("VERDICT: the first readbacks of a window are correct and a later "
             "one is not, with nothing drawn between. The readback switches "
             "source, it does not decay");
      else if (gles.drawn && core.drawn)
         LOG("VERDICT: every readback path works here");
   }

   if (!(gles.ran && gles.clear_only && gles.drawn && gles.textured &&
         gles.reread && gles.fbo))
      rc = 1;
   if (!(core.ran && core.clear_only && core.drawn && core.textured &&
         core.reread && core.fbo))
      rc = 1;

   LOG(rc == 0 ? "=== PASSED ===" : "=== FAILED ===");

out:
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
