/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Many draws with program and state switches, alpha blending and scissoring.
 */
#include "gl_egl_harness.h"

#define FRAMES 180
#define GRID   8

static const char *VS_FLAT =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "uniform vec2 uOrigin;\n"
   "uniform vec2 uScale;\n"
   "void main() { gl_Position = vec4(aPos * uScale + uOrigin, 0.0, 1.0); }\n";

static const char *FS_FLAT =
   "#version 100\n"
   "precision mediump float;\n"
   "uniform vec4 uColor;\n"
   "void main() { gl_FragColor = uColor; }\n";

/* Same result through a varying. */
static const char *VS_VARY =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "uniform vec2 uOrigin;\n"
   "uniform vec2 uScale;\n"
   "varying float vMul;\n"
   "void main() {\n"
   "   vMul = 1.0;\n"
   "   gl_Position = vec4(aPos * uScale + uOrigin, 0.0, 1.0);\n"
   "}\n";

static const char *FS_VARY =
   "#version 100\n"
   "precision mediump float;\n"
   "uniform vec4 uColor;\n"
   "varying float vMul;\n"
   "void main() { gl_FragColor = vec4(uColor.rgb * vMul, uColor.a); }\n";

static const float UNIT_QUAD[] = {
   -1.0f, -1.0f,  1.0f, -1.0f, -1.0f, 1.0f,  1.0f, 1.0f,
};

struct prog {
   GLuint id;
   GLint  origin, scale, color;
};

static bool
prog_up(struct prog *p, const char *vs, const char *fs)
{
   const struct gl_stage_src stages[] = {
      {GL_VERTEX_SHADER,   vs},
      {GL_FRAGMENT_SHADER, fs},
   };
   const char *const attribs[] = {"aPos"};

   p->id = gl_build_program(stages, ARRAY_SIZE(stages), attribs,
                            ARRAY_SIZE(attribs));
   if (!p->id)
      return false;

   p->origin = glGetUniformLocation(p->id, "uOrigin");
   p->scale  = glGetUniformLocation(p->id, "uScale");
   p->color  = glGetUniformLocation(p->id, "uColor");
   return true;
}

static void
cell_color(unsigned i, unsigned j, uint8_t out[3])
{
   out[0] = (uint8_t)(i * 32 + 16);
   out[1] = (uint8_t)(j * 32 + 16);
   out[2] = 128;
}

static void
draw_cell(const struct prog *p, unsigned i, unsigned j,
          const float rgba[4])
{
   const float step = 2.0f / GRID;

   glUseProgram(p->id);
   glUniform2f(p->origin, -1.0f + ((float)i + 0.5f) * step,
               -1.0f + ((float)j + 0.5f) * step);
   glUniform2f(p->scale, step * 0.5f, step * 0.5f);
   glUniform4f(p->color, rgba[0], rgba[1], rgba[2], rgba[3]);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static bool
verify(const struct gl_egl *e)
{
   uint8_t plain[3], blended[3], outside[3];
   uint8_t *px = gl_read_rgba(e->width, e->height);
   bool ok = true;

   if (!px)
      return false;

   cell_color(2, 5, plain);
   cell_color(1, 1, blended);
   cell_color(5, 5, outside);
   /* the half-alpha white quad over cell (1,1) */
   for (unsigned c = 0; c < 3; c++)
      blended[c] = (uint8_t)((255 + blended[c]) / 2);

   const struct {
      float          u, v;
      const uint8_t *want;
      const char    *what;
   } probes[] = {
      {(2 + 0.5f) / GRID, (5 + 0.5f) / GRID, plain,   "plain cell (2,5)"},
      {(1 + 0.5f) / GRID, (1 + 0.5f) / GRID, blended, "blended cell (1,1)"},
      {0.85f,             0.85f,             (const uint8_t[]){255, 0, 0},
       "inside the scissor box"},
      {(5 + 0.5f) / GRID, (5 + 0.5f) / GRID, outside, "outside the scissor box"},
   };

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
   struct gl_egl e;
   struct prog flat = {0, -1, -1, -1}, vary = {0, -1, -1, -1};
   GLuint vbo = 0;
   bool verified = false, ok = false;
   int rc = 0, presented = 0, draws = 0;

   nvk_log_open("sdmc:/gl_multi.log");
   LOG("=== gl_multi ===");
   gl_headless_env("sdmc:/gl_multi_mesa.log");

   if (!gl_egl_up(&e, EGL_OPENGL_ES_API, 2, 0)) {
      rc = 1;
      goto out;
   }

   if (!prog_up(&flat, VS_FLAT, FS_FLAT) ||
       !prog_up(&vary, VS_VARY, FS_VARY)) {
      rc = 1;
      goto out;
   }

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(UNIT_QUAD), UNIT_QUAD, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         (void *)0);
   glEnableVertexAttribArray(0);
   glViewport(0, 0, e.width, e.height);

   LOG("entering present loop (%d frames, %d draws each)", FRAMES,
       GRID * GRID + 1);
   for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_BLEND);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      draws = 0;
      for (unsigned j = 0; j < GRID; j++) {
         for (unsigned i = 0; i < GRID; i++) {
            uint8_t c[3];
            float rgba[4];

            cell_color(i, j, c);
            rgba[0] = c[0] / 255.0f;
            rgba[1] = c[1] / 255.0f;
            rgba[2] = c[2] / 255.0f;
            rgba[3] = 1.0f;

            if ((i + j) & 1)
               glEnable(GL_DEPTH_TEST);
            else
               glDisable(GL_DEPTH_TEST);

            draw_cell((i + j) & 1 ? &vary : &flat, i, j, rgba);
            draws++;
         }
      }

      glDisable(GL_DEPTH_TEST);

      /* half alpha white over one cell */
      const float white_half[4] = {1.0f, 1.0f, 1.0f, 0.5f};
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      draw_cell(&flat, 1, 1, white_half);
      draws++;
      glDisable(GL_BLEND);

      /* scissored clear over the top right quarter */
      glEnable(GL_SCISSOR_TEST);
      glScissor((GLint)(e.width * 3 / 4), (GLint)(e.height * 3 / 4),
                (GLsizei)(e.width / 4), (GLsizei)(e.height / 4));
      glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glDisable(GL_SCISSOR_TEST);

      if (!verified) {
         glFinish();
         if (!gl_no_error("draws")) {
            rc = 1;
            break;
         }
         LOG("%d draws issued this frame", draws);
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
      LOG("VERIFY OK: grid, program switches, blending and scissor all held");
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED: verify=%d presented=%d/%d ===", ok, presented, FRAMES);
      rc = 1;
   }

out:
   if (vbo)     glDeleteBuffers(1, &vbo);
   if (flat.id) glDeleteProgram(flat.id);
   if (vary.id) glDeleteProgram(vary.id);
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
