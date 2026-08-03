/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * EGL scaffolding shared by the on-screen gl_ test apps.
 */
#ifndef GL_EGL_HARNESS_H
#define GL_EGL_HARNESS_H

#include "gl_harness.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

struct gl_egl {
   EGLDisplay dpy;
   EGLConfig  cfg;
   EGLSurface surf;
   EGLContext ctx;
   EGLint     width, height;
};

/* Latched, because the reset status is not reliably sticky. */
static bool g_gl_reset;

/* #pragma mark - bring-up */

static bool
gl_egl_display_up(struct gl_egl *e, EGLenum api, bool want_depth)
{
   const EGLint renderable =
      api == EGL_OPENGL_API ? EGL_OPENGL_BIT : EGL_OPENGL_ES2_BIT;
   const EGLint cfg_attribs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, renderable,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_DEPTH_SIZE,      want_depth ? 24 : 0,
      EGL_NONE,
   };
   EGLint major = 0, minor = 0, n_cfg = 0;

   memset(e, 0, sizeof(*e));
   e->dpy  = EGL_NO_DISPLAY;
   e->surf = EGL_NO_SURFACE;
   e->ctx  = EGL_NO_CONTEXT;
   g_gl_reset = false;

   e->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (e->dpy == EGL_NO_DISPLAY) {
      LOG("FAIL: eglGetDisplay returned EGL_NO_DISPLAY");
      return false;
   }

   if (!eglInitialize(e->dpy, &major, &minor)) {
      LOG("FAIL: eglInitialize returned 0x%04x", eglGetError());
      return false;
   }
   LOG("EGL %d.%d up (vendor='%s' version='%s')", major, minor,
       eglQueryString(e->dpy, EGL_VENDOR), eglQueryString(e->dpy, EGL_VERSION));

   if (!eglBindAPI(api)) {
      LOG("FAIL: eglBindAPI(0x%04x) returned 0x%04x", api, eglGetError());
      return false;
   }

   if (!eglChooseConfig(e->dpy, cfg_attribs, &e->cfg, 1, &n_cfg) || n_cfg < 1) {
      LOG("FAIL: eglChooseConfig returned 0x%04x (n=%d)", eglGetError(), n_cfg);
      return false;
   }

   e->surf = eglCreateWindowSurface(e->dpy, e->cfg, nwindowGetDefault(), NULL);
   if (e->surf == EGL_NO_SURFACE) {
      LOG("FAIL: eglCreateWindowSurface returned 0x%04x", eglGetError());
      return false;
   }

   eglQuerySurface(e->dpy, e->surf, EGL_WIDTH, &e->width);
   eglQuerySurface(e->dpy, e->surf, EGL_HEIGHT, &e->height);
   LOG("window surface %dx%d", e->width, e->height);
   return true;
}

/* Neutral on failure. */
static bool
gl_egl_context_up_profile(struct gl_egl *e, EGLint major, EGLint minor,
                          EGLint profile_bit)
{
   EGLint attribs[10];
   int n = 0, base;

   attribs[n++] = EGL_CONTEXT_MAJOR_VERSION;
   attribs[n++] = major;
   attribs[n++] = EGL_CONTEXT_MINOR_VERSION;
   attribs[n++] = minor;
   if (profile_bit) {
      attribs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
      attribs[n++] = profile_bit;
   }
   base = n;

   /* Without this, glGetGraphicsResetStatus() is specified to always answer
    * NO_ERROR, and a run whose GPU channel died would still report success.
    */
   if (eglQueryAPI() == EGL_OPENGL_API) {
      attribs[n++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY;
      attribs[n++] = EGL_LOSE_CONTEXT_ON_RESET;
   } else {
      attribs[n++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT;
      attribs[n++] = EGL_LOSE_CONTEXT_ON_RESET_EXT;
   }
   attribs[n] = EGL_NONE;

   e->ctx = eglCreateContext(e->dpy, e->cfg, EGL_NO_CONTEXT, attribs);
   if (e->ctx == EGL_NO_CONTEXT) {
      attribs[base] = EGL_NONE;
      e->ctx = eglCreateContext(e->dpy, e->cfg, EGL_NO_CONTEXT, attribs);
      if (e->ctx != EGL_NO_CONTEXT)
         LOG("WARN: no reset-notification context. A lost GPU channel will "
             "not be detectable");
   }
   if (e->ctx == EGL_NO_CONTEXT) {
      LOG("eglCreateContext(%d.%d%s) -> 0x%04x", major, minor,
          profile_bit == EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT ? " core" : "",
          eglGetError());
      return false;
   }

   if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
      LOG("eglMakeCurrent(%d.%d) -> 0x%04x", major, minor, eglGetError());
      eglDestroyContext(e->dpy, e->ctx);
      e->ctx = EGL_NO_CONTEXT;
      return false;
   }
   return true;
}

static bool
gl_egl_context_up(struct gl_egl *e, EGLint major, EGLint minor, bool core)
{
   return gl_egl_context_up_profile(e, major, minor,
                                    core ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT
                                         : 0);
}

static void
gl_egl_context_down(struct gl_egl *e)
{
   if (e->ctx == EGL_NO_CONTEXT)
      return;
   eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(e->dpy, e->ctx);
   e->ctx = EGL_NO_CONTEXT;
}

static bool
gl_egl_up(struct gl_egl *e, EGLenum api, EGLint major, EGLint minor)
{
   const bool core = api == EGL_OPENGL_API &&
                     (major > 3 || (major == 3 && minor >= 2));

   if (!gl_egl_display_up(e, api, true))
      return false;
   if (!gl_egl_context_up(e, major, minor, core)) {
      LOG("FAIL: no %s %d.%d context",
          api == EGL_OPENGL_API ? "GL" : "GLES", major, minor);
      return false;
   }

   LOG("GL_VERSION  = %s", (const char *)glGetString(GL_VERSION));
   LOG("GL_RENDERER = %s", (const char *)glGetString(GL_RENDERER));
   LOG("GL_VENDOR   = %s", (const char *)glGetString(GL_VENDOR));
   LOG("GLSL        = %s",
       (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
   return true;
}

static void
gl_egl_down(struct gl_egl *e)
{
   if (e->dpy == EGL_NO_DISPLAY)
      return;
   gl_egl_context_down(e);
   if (e->surf != EGL_NO_SURFACE) {
      eglDestroySurface(e->dpy, e->surf);
      e->surf = EGL_NO_SURFACE;
   }
   eglTerminate(e->dpy);
   e->dpy = EGL_NO_DISPLAY;
}

/* #pragma mark - programs */

struct gl_stage_src {
   GLenum      stage;
   const char *src;
};

static const char *
gl_stage_name(GLenum stage)
{
   switch (stage) {
   case GL_VERTEX_SHADER:          return "vertex";
   case GL_FRAGMENT_SHADER:        return "fragment";
   case GL_GEOMETRY_SHADER:        return "geometry";
   case GL_TESS_CONTROL_SHADER:    return "tess-control";
   case GL_TESS_EVALUATION_SHADER: return "tess-eval";
   case GL_COMPUTE_SHADER:         return "compute";
   default:                        return "?";
   }
}

static GLuint
gl_compile_stage(GLenum stage, const char *src)
{
   GLuint sh = glCreateShader(stage);
   GLint ok = 0;
   char log[2048];
   GLsizei n = 0;

   glShaderSource(sh, 1, &src, NULL);
   glCompileShader(sh);
   glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   glGetShaderInfoLog(sh, sizeof(log), &n, log);
   if (n > 0)
      LOG("%s shader log: %s", gl_stage_name(stage), log);
   if (!ok) {
      LOG("FAIL: %s shader did not compile", gl_stage_name(stage));
      glDeleteShader(sh);
      return 0;
   }
   return sh;
}

static GLuint
gl_build_program_ex(const struct gl_stage_src *stages, unsigned n_stages,
                    const char *const *attribs, unsigned n_attribs,
                    void (*pre_link)(GLuint prog, void *data), void *data)
{
   GLuint sh[6] = {0};
   GLuint prog;
   bool ok = true;

   if (n_stages > ARRAY_SIZE(sh))
      return 0;

   prog = glCreateProgram();
   for (unsigned i = 0; i < n_stages; i++) {
      sh[i] = gl_compile_stage(stages[i].stage, stages[i].src);
      if (!sh[i]) {
         ok = false;
         break;
      }
      glAttachShader(prog, sh[i]);
   }

   if (ok) {
      GLint linked = 0;
      char log[2048];
      GLsizei n = 0;

      for (unsigned i = 0; i < n_attribs; i++)
         glBindAttribLocation(prog, i, attribs[i]);
      if (pre_link)
         pre_link(prog, data);

      glLinkProgram(prog);
      glGetProgramiv(prog, GL_LINK_STATUS, &linked);
      glGetProgramInfoLog(prog, sizeof(log), &n, log);
      if (n > 0)
         LOG("program log: %s", log);
      if (!linked) {
         LOG("FAIL: program did not link");
         ok = false;
      }
   }

   for (unsigned i = 0; i < n_stages; i++)
      if (sh[i])
         glDeleteShader(sh[i]);

   if (!ok) {
      glDeleteProgram(prog);
      return 0;
   }
   return prog;
}

static GLuint
gl_build_program(const struct gl_stage_src *stages, unsigned n_stages,
                 const char *const *attribs, unsigned n_attribs)
{
   return gl_build_program_ex(stages, n_stages, attribs, n_attribs, NULL, NULL);
}

/* #pragma mark - device loss */

/* nvgpu latches a channel fault, resets the channel and then quietly drops
 * every later submit, advancing the syncpoint as it goes. Draws, presents and
 * glGetError all keep succeeding on a device whose work no longer runs, so
 * without this an app can present 180 empty frames and call itself green.
 */
static bool
gl_device_lost(const char *when)
{
   GLenum status;

   if (g_gl_reset)
      return true;

   status = glGetGraphicsResetStatus();
   if (status == GL_NO_ERROR)
      return false;

   g_gl_reset = true;

   LOG("FAIL: GPU context reset (0x%04x) at %s. The driver log has the "
       "channel fault behind it", status, when);
   gl_mark("context reset 0x%04x at %s", status, when);
   return true;
}

/* #pragma mark - present loop diagnostics */

/* The failure mode worth catching is a swapchain that dies mid-run.
 */
static void
gl_frame_mark(int frame, int total)
{
   if (frame < 3 || frame % 30 == 0)
      gl_mark("frame %d/%d", frame, total);
}

/* eglSwapBuffers failing is the app visible end of a swapchain that went out of
 * date.
 */
static bool
gl_present(const struct gl_egl *e, int frame, int total)
{
   char when[32];

   gl_frame_mark(frame, total);

   snprintf(when, sizeof(when), "frame %d/%d", frame, total);
   if (gl_device_lost(when))
      return false;

   if (eglSwapBuffers(e->dpy, e->surf))
      return true;

   EGLint err = eglGetError();

   LOG("FAIL: eglSwapBuffers frame %d/%d returned 0x%04x%s", frame, total, err,
       err == EGL_BAD_SURFACE ? " (EGL_BAD_SURFACE: swapchain out of date)"
                              : "");
   gl_mark("eglSwapBuffers frame %d FAILED 0x%04x", frame, err);
   return false;
}

/* #pragma mark - readback */

static bool
gl_no_error(const char *tag)
{
   GLenum err = glGetError();
   if (err != GL_NO_ERROR) {
      LOG("FAIL: glGetError after %s = 0x%04x", tag, err);
      return false;
   }
   return true;
}

/* A transfer that never ran leaves the destination untouched and raises no GL
 * error.
 * Pre-filling with a value no probe expects makes this concrete.
 */
#define GL_READ_SENTINEL 0x5a

static uint8_t *
gl_read_rgba(unsigned w, unsigned h)
{
   const size_t size = (size_t)w * h * 4;
   uint8_t *px = malloc(size);

   if (!px) {
      LOG("FAIL: out of memory reading back %ux%u", w, h);
      return NULL;
   }
   memset(px, GL_READ_SENTINEL, size);

   /* Bracketed so a device loss raised by the readback itself is attributable
    * to it rather than to the draw before it. */
   gl_mark("glReadPixels %ux%u enter", w, h);
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   gl_mark("glReadPixels %ux%u leave", w, h);
   if (!gl_no_error("glReadPixels") || gl_device_lost("glReadPixels")) {
      free(px);
      return NULL;
   }

   size_t untouched = 0;
   for (size_t i = 0; i < size; i++)
      untouched += px[i] == GL_READ_SENTINEL;
   if (untouched == size) {
      LOG("FAIL: glReadPixels %ux%u wrote nothing. The whole buffer is still "
          "the 0x%02x fill", w, h, GL_READ_SENTINEL);
      free(px);
      return NULL;
   }

   return px;
}

/* u/v are normalised. v=0 at the bottom row. */
static const uint8_t *
gl_px_at(const uint8_t *px, unsigned w, unsigned h, float u, float v)
{
   unsigned x = (unsigned)(u * (float)(w - 1) + 0.5f);
   unsigned y = (unsigned)(v * (float)(h - 1) + 0.5f);

   return &px[((size_t)y * w + x) * 4];
}

static bool
gl_px_near(const uint8_t *px, int r, int g, int b, int tol)
{
   return abs((int)px[0] - r) <= tol && abs((int)px[1] - g) <= tol &&
          abs((int)px[2] - b) <= tol;
}

static unsigned
gl_count_near(const uint8_t *px, unsigned w, unsigned h,
              int r, int g, int b, int tol)
{
   unsigned hits = 0;

   for (size_t p = 0; p < (size_t)w * h; p++)
      if (gl_px_near(&px[p * 4], r, g, b, tol))
         hits++;
   return hits;
}

static void
gl_log_px(const char *tag, const uint8_t *px)
{
   LOG("  %s = {%u,%u,%u,%u}", tag, px[0], px[1], px[2], px[3]);
}

#endif /* GL_EGL_HARNESS_H */
