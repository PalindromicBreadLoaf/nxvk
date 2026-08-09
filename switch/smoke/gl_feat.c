/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Prove the shader stages behind Zink's GL version gates actually
 * render.
 */
#include "gl_egl_harness.h"

#define FRAMES 180

static const struct {
   EGLint major, minor;
} CORE_VERSIONS[] = {
   {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2},
};

/* #pragma mark - geometry shader */

static const char *GS_VS =
   "#version 150\n"
   "in vec2 aPos;\n"
   "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *GS_GS =
   "#version 150\n"
   "layout(points) in;\n"
   "layout(triangle_strip, max_vertices = 4) out;\n"
   "void main() {\n"
   "   vec4 c = gl_in[0].gl_Position;\n"
   "   gl_Position = c + vec4(-0.4, -0.4, 0.0, 0.0); EmitVertex();\n"
   "   gl_Position = c + vec4( 0.4, -0.4, 0.0, 0.0); EmitVertex();\n"
   "   gl_Position = c + vec4(-0.4,  0.4, 0.0, 0.0); EmitVertex();\n"
   "   gl_Position = c + vec4( 0.4,  0.4, 0.0, 0.0); EmitVertex();\n"
   "   EndPrimitive();\n"
   "}\n";

static const char *GS_FS =
   "#version 150\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = vec4(1.0, 0.5, 0.0, 1.0); }\n";

static const float POINT[] = {0.0f, 0.0f};

/* #pragma mark - tessellation */

static const char *TESS_VS =
   "#version 400\n"
   "in vec2 aPos;\n"
   "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *TESS_TCS =
   "#version 400\n"
   "layout(vertices = 3) out;\n"
   "void main() {\n"
   "   gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
   "   if (gl_InvocationID == 0) {\n"
   "      gl_TessLevelInner[0] = 3.0;\n"
   "      gl_TessLevelOuter[0] = 3.0;\n"
   "      gl_TessLevelOuter[1] = 3.0;\n"
   "      gl_TessLevelOuter[2] = 3.0;\n"
   "   }\n"
   "}\n";

/* The barycentric passthrough is what proves the evaluation shader ran with
 * real tessellation coordinates rather than the patch being drawn.
 */
static const char *TESS_TES =
   "#version 400\n"
   "layout(triangles, equal_spacing, ccw) in;\n"
   "out vec3 vTess;\n"
   "void main() {\n"
   "   vTess = gl_TessCoord;\n"
   "   gl_Position = gl_TessCoord.x * gl_in[0].gl_Position +\n"
   "                 gl_TessCoord.y * gl_in[1].gl_Position +\n"
   "                 gl_TessCoord.z * gl_in[2].gl_Position;\n"
   "}\n";

static const char *TESS_FS =
   "#version 400\n"
   "in vec3 vTess;\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = vec4(vTess, 1.0); }\n";

static const float PATCH[] = {
    0.0f,  0.8f,
   -0.8f, -0.8f,
    0.8f, -0.8f,
};

/* #pragma mark - probes */

static void
draw_gs(const struct gl_egl *e, GLuint prog, GLuint vbo)
{
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         (void *)0);
   glEnableVertexAttribArray(0);
   glUseProgram(prog);
   glViewport(0, 0, e->width, e->height);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glDrawArrays(GL_POINTS, 0, 1);
}

static bool
probe_gs(const struct gl_egl *e, GLuint prog, GLuint vbo)
{
   /* The emitted quad covers NDC +-0.4, i.e. 0.3..0.7 of the window. */
   const struct {
      float u, v;
      uint8_t want[3];
      const char *what;
   } probes[] = {
      {0.50f, 0.50f, {255, 128, 0}, "centre of the emitted quad"},
      {0.65f, 0.65f, {255, 128, 0}, "inside the emitted quad"},
      {0.85f, 0.50f, {0, 0, 0},     "outside the emitted quad"},
      {0.50f, 0.05f, {0, 0, 0},     "below the emitted quad"},
   };
   uint8_t *px;
   bool ok = true;

   draw_gs(e, prog, vbo);
   glFinish();
   if (!gl_no_error("geometry shader draw"))
      return false;

   px = gl_read_rgba(e->width, e->height);
   if (!px)
      return false;

   for (unsigned i = 0; i < ARRAY_SIZE(probes); i++) {
      const uint8_t *got = gl_px_at(px, e->width, e->height, probes[i].u,
                                    probes[i].v);

      gl_log_px(probes[i].what, got);
      if (!gl_px_near(got, probes[i].want[0], probes[i].want[1],
                      probes[i].want[2], 6)) {
         LOG("  expected {%u,%u,%u}", probes[i].want[0], probes[i].want[1],
             probes[i].want[2]);
         ok = false;
      }
   }

   free(px);
   LOG("geometry shader: %s", ok ? "PASS" : "FAIL");
   return ok;
}

static void
draw_tess(const struct gl_egl *e, GLuint prog, GLuint vbo)
{
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         (void *)0);
   glEnableVertexAttribArray(0);
   glUseProgram(prog);
   glViewport(0, 0, e->width, e->height);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glPatchParameteri(GL_PATCH_VERTICES, 3);
   glDrawArrays(GL_PATCHES, 0, 3);
}

static bool
probe_tess(const struct gl_egl *e, GLuint prog, GLuint vbo)
{
   /* Patch centroid is NDC (0, -0.267).
    * 85% of the way to the first patch vertex is NDC (0, 0.64).
    */
   const struct {
      float u, v;
      uint8_t want[3];
      int tol;
      const char *what;
   } probes[] = {
      {0.5f,  0.3665f, {85, 85, 85},   24, "patch centroid (even barycentric)"},
      {0.5f,  0.8200f, {217, 19, 19},  28, "near the first patch vertex"},
      {0.05f, 0.0500f, {0, 0, 0},       6, "outside the patch"},
   };
   uint8_t *px;
   bool ok = true;

   draw_tess(e, prog, vbo);
   glFinish();
   if (!gl_no_error("tessellated draw"))
      return false;

   px = gl_read_rgba(e->width, e->height);
   if (!px)
      return false;

   for (unsigned i = 0; i < ARRAY_SIZE(probes); i++) {
      const uint8_t *got = gl_px_at(px, e->width, e->height, probes[i].u,
                                    probes[i].v);

      gl_log_px(probes[i].what, got);
      if (!gl_px_near(got, probes[i].want[0], probes[i].want[1],
                      probes[i].want[2], probes[i].tol)) {
         LOG("  expected {%u,%u,%u} +-%d", probes[i].want[0], probes[i].want[1],
             probes[i].want[2], probes[i].tol);
         ok = false;
      }
   }

   free(px);
   LOG("tessellation: %s", ok ? "PASS" : "FAIL");
   return ok;
}

int main(void)
{
   const struct gl_stage_src gs_stages[] = {
      {GL_VERTEX_SHADER,   GS_VS},
      {GL_GEOMETRY_SHADER, GS_GS},
      {GL_FRAGMENT_SHADER, GS_FS},
   };
   const struct gl_stage_src tess_stages[] = {
      {GL_VERTEX_SHADER,          TESS_VS},
      {GL_TESS_CONTROL_SHADER,    TESS_TCS},
      {GL_TESS_EVALUATION_SHADER, TESS_TES},
      {GL_FRAGMENT_SHADER,        TESS_FS},
   };
   const char *const attribs[] = {"aPos"};
   struct gl_egl e;
   GLuint vao = 0, point_vbo = 0, patch_vbo = 0, gs = 0, tess = 0;
   EGLint core_major = 0, core_minor = 0;
   bool ok_gs = false, ok_tess = false, tess_available = false;
   int rc = 0, presented = 0;

   nvk_log_open("sdmc:/gl_feat.log");
   LOG("=== gl_feat ===");
   gl_headless_env("sdmc:/gl_feat_mesa.log");

   if (!gl_egl_display_up(&e, EGL_OPENGL_API, true)) {
      rc = 1;
      goto out;
   }

   for (unsigned i = 0; i < ARRAY_SIZE(CORE_VERSIONS); i++) {
      if (gl_egl_context_up(&e, CORE_VERSIONS[i].major, CORE_VERSIONS[i].minor,
                            true)) {
         core_major = CORE_VERSIONS[i].major;
         core_minor = CORE_VERSIONS[i].minor;
         break;
      }
   }
   if (!core_major) {
      LOG("FAIL: no desktop GL core context could be created");
      rc = 1;
      goto out;
   }
   LOG("GL %d.%d core context", core_major, core_minor);
   LOG("GL_VERSION  = %s", (const char *)glGetString(GL_VERSION));
   LOG("GL_RENDERER = %s", (const char *)glGetString(GL_RENDERER));
   LOG("GLSL        = %s",
       (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

   tess_available = core_major >= 4;
   if (!tess_available)
      LOG("tessellation needs GL 4.0. this context is %d.%d, skipping",
          core_major, core_minor);

   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);

   glGenBuffers(1, &point_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, point_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(POINT), POINT, GL_STATIC_DRAW);
   glGenBuffers(1, &patch_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, patch_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(PATCH), PATCH, GL_STATIC_DRAW);

   LOG("-- geometry shader --");
   gs = gl_build_program(gs_stages, ARRAY_SIZE(gs_stages), attribs,
                         ARRAY_SIZE(attribs));
   ok_gs = gs && probe_gs(&e, gs, point_vbo);

   if (tess_available) {
      LOG("-- tessellation --");
      tess = gl_build_program(tess_stages, ARRAY_SIZE(tess_stages), attribs,
                              ARRAY_SIZE(attribs));
      ok_tess = tess && probe_tess(&e, tess, patch_vbo);
   }

   if (!ok_gs || (tess_available && !ok_tess))
      rc = 1;

   /* Leave whichever stage came up newest on screen. */
   if (tess || gs) {
      LOG("entering present loop (%d frames)", FRAMES);
      for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
         if (tess)
            draw_tess(&e, tess, patch_vbo);
         else
            draw_gs(&e, gs, point_vbo);

         if (!gl_present(&e, frame, FRAMES)) {
            rc = 1;
            break;
         }
         presented++;
      }
      LOG("presented %d/%d frames", presented, FRAMES);
   }

   LOG("geometry-shader=%s tessellation=%s", ok_gs ? "PASS" : "FAIL",
       tess_available ? (ok_tess ? "PASS" : "FAIL") : "n/a");
   if (rc == 0 && presented == FRAMES) {
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED ===");
      rc = 1;
   }

out:
   if (gs)        glDeleteProgram(gs);
   if (tess)      glDeleteProgram(tess);
   if (point_vbo) glDeleteBuffers(1, &point_vbo);
   if (patch_vbo) glDeleteBuffers(1, &patch_vbo);
   if (vao)       glDeleteVertexArrays(1, &vao);
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
