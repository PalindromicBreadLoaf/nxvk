/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Indexed drawing out of a VBO/IBO pair, parameterised by a uniform block.
 */
#include "gl_egl_harness.h"

#define FRAMES 180

static const char *VS_SRC =
   "#version 300 es\n"
   "layout(location = 0) in vec2 aPos;\n"
   "layout(std140) uniform Block {\n"
   "   vec4 uColor;\n"
   "   vec4 uOffset;\n"
   "};\n"
   "out vec4 vColor;\n"
   "void main() {\n"
   "   vColor = uColor;\n"
   "   gl_Position = vec4(aPos + uOffset.xy, 0.0, 1.0);\n"
   "}\n";

static const char *FS_SRC =
   "#version 300 es\n"
   "precision mediump float;\n"
   "in vec4 vColor;\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = vColor; }\n";

static const float QUAD[] = {
   -0.35f, -0.5f,
    0.35f, -0.5f,
    0.35f,  0.5f,
   -0.35f,  0.5f,
};
static const uint16_t INDICES[] = {0, 1, 2, 0, 2, 3};

/* vec4 colour then vec4 offset. */
static const float BLOCK_LEFT[8] = {
   0.875f, 0.125f, 0.125f, 1.0f,
  -0.4f,   0.0f,   0.0f,   0.0f,
};
static const float BLOCK_RIGHT[8] = {
   0.125f, 0.875f, 0.125f, 1.0f,
   0.4f,   0.0f,   0.0f,   0.0f,
};

static const uint8_t LEFTC[3]  = {223, 32, 32};
static const uint8_t RIGHTC[3] = {32, 223, 32};
static const uint8_t BG[3]     = {0, 0, 0};

struct probe {
   float          u, v;
   const uint8_t *want;
   const char    *what;
};

static bool
verify(const struct gl_egl *e)
{
   const struct probe probes[] = {
      {0.25f, 0.50f, LEFTC,  "left quad (block 0)"},
      {0.75f, 0.50f, RIGHTC, "right quad (block 1)"},
      {0.50f, 0.50f, BG,     "gap between the quads"},
      {0.50f, 0.02f, BG,     "below both quads"},
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
   const struct gl_stage_src stages[] = {
      {GL_VERTEX_SHADER,   VS_SRC},
      {GL_FRAGMENT_SHADER, FS_SRC},
   };
   struct gl_egl e;
   GLuint prog = 0, vao = 0, vbo = 0, ibo = 0, ubo[2] = {0, 0};
   bool verified = false, ok = false;
   int rc = 0, presented = 0;

   nvk_log_open("sdmc:/gl_ubo_vbo.log");
   LOG("=== gl_ubo_vbo ===");
   gl_headless_env("sdmc:/gl_ubo_vbo_mesa.log");

   if (!gl_egl_up(&e, EGL_OPENGL_ES_API, 3, 0)) {
      rc = 1;
      goto out;
   }

   prog = gl_build_program(stages, ARRAY_SIZE(stages), NULL, 0);
   if (!prog) {
      rc = 1;
      goto out;
   }

   GLuint block = glGetUniformBlockIndex(prog, "Block");
   if (block == GL_INVALID_INDEX) {
      LOG("FAIL: uniform block 'Block' not found");
      rc = 1;
      goto out;
   }
   GLint block_size = 0;
   glGetActiveUniformBlockiv(prog, block, GL_UNIFORM_BLOCK_DATA_SIZE,
                             &block_size);
   LOG("uniform block index %u, std140 size %d bytes", block, block_size);
   glUniformBlockBinding(prog, block, 0);

   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         (void *)0);
   glEnableVertexAttribArray(0);

   glGenBuffers(1, &ibo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(INDICES), INDICES,
                GL_STATIC_DRAW);

   glGenBuffers(2, ubo);
   glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]);
   glBufferData(GL_UNIFORM_BUFFER, sizeof(BLOCK_LEFT), BLOCK_LEFT,
                GL_STATIC_DRAW);
   glBindBuffer(GL_UNIFORM_BUFFER, ubo[1]);
   glBufferData(GL_UNIFORM_BUFFER, sizeof(BLOCK_RIGHT), BLOCK_RIGHT,
                GL_STATIC_DRAW);
   if (!gl_no_error("buffer setup")) {
      rc = 1;
      goto out;
   }

   glViewport(0, 0, e.width, e.height);
   glUseProgram(prog);

   LOG("entering present loop (%d frames)", FRAMES);
   for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      for (unsigned i = 0; i < 2; i++) {
         glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo[i]);
         glDrawElements(GL_TRIANGLES, ARRAY_SIZE(INDICES), GL_UNSIGNED_SHORT,
                        (void *)0);
      }

      if (!verified) {
         glFinish();
         if (!gl_no_error("glDrawElements")) {
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
      LOG("VERIFY OK");
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED: verify=%d presented=%d/%d ===", ok, presented, FRAMES);
      rc = 1;
   }

out:
   if (ubo[0]) glDeleteBuffers(2, ubo);
   if (ibo)    glDeleteBuffers(1, &ibo);
   if (vbo)    glDeleteBuffers(1, &vbo);
   if (vao)    glDeleteVertexArrays(1, &vao);
   if (prog)   glDeleteProgram(prog);
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
