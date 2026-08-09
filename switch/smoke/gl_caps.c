/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Report the GL/GLES version and cap set that comes out of Zink on this part.
 */
#include "gl_egl_harness.h"

struct gl_version {
   EGLint major, minor;
};

/* The first one that creates is the ceiling. */
static const struct gl_version GL_CORE_VERSIONS[] = {
   {4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0},
   {3, 3}, {3, 2},
};
static const struct gl_version GLES_VERSIONS[] = {
   {3, 2}, {3, 1}, {3, 0}, {2, 0},
};

struct gl_limit {
   GLenum      token;
   const char *name;
};

static const struct gl_limit LIMITS[] = {
   {GL_MAX_TEXTURE_SIZE,                            "MAX_TEXTURE_SIZE"},
   {GL_MAX_3D_TEXTURE_SIZE,                         "MAX_3D_TEXTURE_SIZE"},
   {GL_MAX_CUBE_MAP_TEXTURE_SIZE,                   "MAX_CUBE_MAP_TEXTURE_SIZE"},
   {GL_MAX_ARRAY_TEXTURE_LAYERS,                    "MAX_ARRAY_TEXTURE_LAYERS"},
   {GL_MAX_RENDERBUFFER_SIZE,                       "MAX_RENDERBUFFER_SIZE"},
   {GL_MAX_SAMPLES,                                 "MAX_SAMPLES"},
   {GL_MAX_VERTEX_ATTRIBS,                          "MAX_VERTEX_ATTRIBS"},
   {GL_MAX_TEXTURE_IMAGE_UNITS,                     "MAX_TEXTURE_IMAGE_UNITS"},
   {GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,            "MAX_COMBINED_TEXTURE_IMAGE_UNITS"},
   {GL_MAX_DRAW_BUFFERS,                            "MAX_DRAW_BUFFERS"},
   {GL_MAX_COLOR_ATTACHMENTS,                       "MAX_COLOR_ATTACHMENTS"},
   {GL_MAX_ELEMENTS_VERTICES,                       "MAX_ELEMENTS_VERTICES"},
   {GL_MAX_ELEMENTS_INDICES,                        "MAX_ELEMENTS_INDICES"},
   {GL_MAX_UNIFORM_BLOCK_SIZE,                      "MAX_UNIFORM_BLOCK_SIZE"},
   {GL_MAX_UNIFORM_BUFFER_BINDINGS,                 "MAX_UNIFORM_BUFFER_BINDINGS"},
   {GL_MAX_VERTEX_UNIFORM_BLOCKS,                   "MAX_VERTEX_UNIFORM_BLOCKS"},
   {GL_MAX_FRAGMENT_UNIFORM_BLOCKS,                 "MAX_FRAGMENT_UNIFORM_BLOCKS"},
   {GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS,     "MAX_XFB_SEPARATE_ATTRIBS"},
   {GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, "MAX_XFB_INTERLEAVED_COMPONENTS"},
   {GL_MAX_GEOMETRY_OUTPUT_VERTICES,                "MAX_GEOMETRY_OUTPUT_VERTICES"},
   {GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS,        "MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS"},
   {GL_MAX_GEOMETRY_SHADER_INVOCATIONS,             "MAX_GEOMETRY_SHADER_INVOCATIONS"},
   {GL_MAX_TESS_GEN_LEVEL,                          "MAX_TESS_GEN_LEVEL"},
   {GL_MAX_PATCH_VERTICES,                          "MAX_PATCH_VERTICES"},
   {GL_MAX_TESS_PATCH_COMPONENTS,                   "MAX_TESS_PATCH_COMPONENTS"},
   {GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS,          "MAX_SSBO_BINDINGS"},
   {GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS,          "MAX_COMPUTE_WORK_GROUP_INVOCATIONS"},
   {GL_MAX_COMPUTE_SHARED_MEMORY_SIZE,              "MAX_COMPUTE_SHARED_MEMORY_SIZE"},
};

static void
log_limits(void)
{
   LOG("-- limits --");
   for (unsigned i = 0; i < ARRAY_SIZE(LIMITS); i++) {
      GLint v = -1;

      while (glGetError() != GL_NO_ERROR)
         ;
      glGetIntegerv(LIMITS[i].token, &v);
      if (glGetError() != GL_NO_ERROR)
         LOG("  %-38s n/a", LIMITS[i].name);
      else
         LOG("  %-38s %d", LIMITS[i].name, v);
   }
}

/* One extension per line beats a 4 KB string the log buffer would truncate. */
static void
log_extensions(void)
{
   GLint count = -1;

   while (glGetError() != GL_NO_ERROR)
      ;
   glGetIntegerv(GL_NUM_EXTENSIONS, &count);

   if (glGetError() == GL_NO_ERROR && count > 0) {
      LOG("-- %d extensions --", count);
      for (GLint i = 0; i < count; i++) {
         const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, i);
         if (glGetError() != GL_NO_ERROR)
            break;
         LOG("  %s", ext ? ext : "(null)");
      }
      return;
   }

   const char *exts = (const char *)glGetString(GL_EXTENSIONS);
   if (!exts) {
      LOG("-- no extension string --");
      return;
   }

   LOG("-- extensions --");
   while (*exts) {
      const char *end = strchr(exts, ' ');
      int len = end ? (int)(end - exts) : (int)strlen(exts);

      if (len > 0)
         LOG("  %.*s", len, exts);
      exts += len;
      while (*exts == ' ')
         exts++;
   }
}

static void
log_strings(const char *tag)
{
   LOG("%s: GL_VERSION  = %s", tag, (const char *)glGetString(GL_VERSION));
   LOG("%s: GLSL        = %s", tag,
       (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
   LOG("%s: GL_RENDERER = %s", tag, (const char *)glGetString(GL_RENDERER));
   LOG("%s: GL_VENDOR   = %s", tag, (const char *)glGetString(GL_VENDOR));
}

static bool
probe(struct gl_egl *e, EGLenum api, const struct gl_version *versions,
      unsigned n, bool core, bool dump)
{
   const char *tag = api == EGL_OPENGL_API ? "GL" : "GLES";

   if (!eglBindAPI(api)) {
      LOG("%s: eglBindAPI -> 0x%04x (not built in?)", tag, eglGetError());
      return false;
   }

   for (unsigned i = 0; i < n; i++) {
      if (!gl_egl_context_up(e, versions[i].major, versions[i].minor, core))
         continue;

      LOG("=== %s %d.%d context created ===", tag, versions[i].major,
          versions[i].minor);
      log_strings(tag);
      if (dump) {
         log_limits();
         log_extensions();
      }
      gl_egl_context_down(e);
      return true;
   }

   LOG("=== no %s context could be created ===", tag);
   return false;
}

int main(void)
{
   struct gl_egl e;
   int rc = 0;
   bool got_gl, got_gles;

   nvk_log_open("sdmc:/gl_caps.log");
   LOG("=== gl_caps ===");
   gl_headless_env("sdmc:/gl_caps_mesa.log");

   if (!gl_egl_display_up(&e, EGL_OPENGL_ES_API, true)) {
      rc = 1;
      goto out;
   }

   if (eglBindAPI(EGL_OPENGL_API) &&
       gl_egl_context_up_profile(&e, 1, 0,
                                 EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT)) {
      LOG("=== GL compatibility profile ===");
      log_strings("GL-compat");
      log_limits();
      log_extensions();
      gl_egl_context_down(&e);
   } else {
      LOG("=== no GL compatibility context ===");
   }

   got_gl = probe(&e, EGL_OPENGL_API, GL_CORE_VERSIONS,
                  ARRAY_SIZE(GL_CORE_VERSIONS), true, false);
   got_gles = probe(&e, EGL_OPENGL_ES_API, GLES_VERSIONS,
                    ARRAY_SIZE(GLES_VERSIONS), false, true);

   if (got_gl || got_gles) {
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED: no client API context at all ===");
      rc = 1;
   }

out:
   gl_egl_down(&e);
   if (g_nvk_log)
      fclose(g_nvk_log);
   return rc;
}
