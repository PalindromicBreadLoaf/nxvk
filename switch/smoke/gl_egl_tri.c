/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Draw a triangle on screen through EGL.
 */
#include "gl_harness.h"

#include <math.h>

#include <EGL/egl.h>

#define FRAMES 300

static const char *VS_SRC =
   "#version 100\n"
   "attribute vec2 aPos;\n"
   "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS_SRC =
   "#version 100\n"
   "precision mediump float;\n"
   "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

static const float TRI[6] = {
   -0.5f, -0.5f,
    0.5f, -0.5f,
    0.0f,  0.5f,
};

static GLuint compile_shader(GLenum stage, const char *src, const char *tag)
{
   GLuint sh = glCreateShader(stage);
   glShaderSource(sh, 1, &src, NULL);
   glCompileShader(sh);

   GLint ok = 0;
   glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   char log[1024];
   GLsizei n = 0;
   glGetShaderInfoLog(sh, sizeof(log), &n, log);
   if (n > 0) LOG("%s shader log: %s", tag, log);
   if (!ok) {
      LOG("FAIL: %s shader did not compile", tag);
      glDeleteShader(sh);
      return 0;
   }
   return sh;
}

int main(void)
{
   static const EGLint cfg_attribs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_DEPTH_SIZE,      16,
      EGL_NONE,
   };
   static const EGLint ctx_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };

   EGLDisplay dpy = EGL_NO_DISPLAY;
   EGLSurface surf = EGL_NO_SURFACE;
   EGLContext ctx = EGL_NO_CONTEXT;
   EGLConfig cfg;
   EGLint major = 0, minor = 0, n_cfg = 0;
   EGLint width = 0, height = 0;
   GLuint vs = 0, fs = 0, prog = 0, vbo = 0;
   int rc = 0, presented = 0;

   nvk_log_open("sdmc:/gl_egl_tri.log");
   LOG("=== gl_egl_tri ===");
   gl_headless_env("sdmc:/gl_egl_tri_mesa.log");

   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (dpy == EGL_NO_DISPLAY) {
      LOG("FAIL: eglGetDisplay returned EGL_NO_DISPLAY");
      rc = 1; goto out;
   }

   if (!eglInitialize(dpy, &major, &minor)) {
      LOG("FAIL: eglInitialize returned 0x%04x", eglGetError());
      rc = 1; goto out;
   }
   LOG("EGL %d.%d up", major, minor);
   LOG("EGL_VENDOR  = %s", eglQueryString(dpy, EGL_VENDOR));
   LOG("EGL_VERSION = %s", eglQueryString(dpy, EGL_VERSION));

   if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      LOG("FAIL: eglBindAPI returned 0x%04x", eglGetError());
      rc = 1; goto out;
   }

   if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg < 1) {
      LOG("FAIL: eglChooseConfig returned 0x%04x (n=%d)", eglGetError(), n_cfg);
      rc = 1; goto out;
   }

   surf = eglCreateWindowSurface(dpy, cfg, nwindowGetDefault(), NULL);
   if (surf == EGL_NO_SURFACE) {
      LOG("FAIL: eglCreateWindowSurface returned 0x%04x", eglGetError());
      rc = 1; goto out;
   }

   eglQuerySurface(dpy, surf, EGL_WIDTH, &width);
   eglQuerySurface(dpy, surf, EGL_HEIGHT, &height);
   LOG("window surface %dx%d", width, height);

   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
   if (ctx == EGL_NO_CONTEXT) {
      LOG("FAIL: eglCreateContext returned 0x%04x", eglGetError());
      rc = 1; goto out;
   }

   if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
      LOG("FAIL: eglMakeCurrent returned 0x%04x", eglGetError());
      rc = 1; goto out;
   }
   LOG("context current");

   LOG("GL_VERSION  = %s", (const char *)glGetString(GL_VERSION));
   LOG("GL_RENDERER = %s", (const char *)glGetString(GL_RENDERER));
   LOG("GL_VENDOR   = %s", (const char *)glGetString(GL_VENDOR));

   vs = compile_shader(GL_VERTEX_SHADER, VS_SRC, "vertex");
   fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC, "fragment");
   if (!vs || !fs) { rc = 1; goto out; }

   prog = glCreateProgram();
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glBindAttribLocation(prog, 0, "aPos");
   glLinkProgram(prog);
   GLint linked = 0;
   glGetProgramiv(prog, GL_LINK_STATUS, &linked);
   if (!linked) {
      char log[1024]; GLsizei ln = 0;
      glGetProgramInfoLog(prog, sizeof(log), &ln, log);
      LOG("FAIL: program link failed: %s", ln > 0 ? log : "(no log)");
      rc = 1; goto out;
   }
   LOG("program linked");

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(TRI), TRI, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
   glEnableVertexAttribArray(0);

   glViewport(0, 0, width, height);
   glUseProgram(prog);

   LOG("entering present loop (%d frames)", FRAMES);
   for (int frame = 0; frame < FRAMES && appletMainLoop(); frame++) {
      float t = frame / 60.0f;
      glClearColor(0.5f + 0.5f * sinf(t),
                   0.5f + 0.5f * sinf(t + 2.094f),
                   0.5f + 0.5f * sinf(t + 4.188f), 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      glDrawArrays(GL_TRIANGLES, 0, 3);

      if (frame < 3 || frame % 30 == 0)
         gl_mark("frame %d/%d", frame, FRAMES);

      if (!eglSwapBuffers(dpy, surf)) {
         EGLint eerr = eglGetError();

         LOG("FAIL: eglSwapBuffers frame %d/%d returned 0x%04x%s", frame, FRAMES,
             eerr,
             eerr == EGL_BAD_SURFACE
                ? " (EGL_BAD_SURFACE: swapchain out of date)" : "");
         gl_mark("eglSwapBuffers frame %d FAILED 0x%04x", frame, eerr);
         rc = 1;
         break;
      }
      presented++;

      if (frame == 0) {
         GLenum gerr = glGetError();
         if (gerr != GL_NO_ERROR)
            LOG("WARN: glGetError after first frame = 0x%04x", gerr);
      }
   }

   LOG("presented %d/%d frames", presented, FRAMES);
   if (rc == 0 && presented == FRAMES) {
      LOG("Triangle presented through the nwindow swapchain");
      LOG("=== PASSED ===");
   } else {
      LOG("=== FAILED: presented %d of %d ===", presented, FRAMES);
      rc = 1;
   }

out:
   if (dpy != EGL_NO_DISPLAY) {
      eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (vbo)  glDeleteBuffers(1, &vbo);
      if (prog) glDeleteProgram(prog);
      if (vs)   glDeleteShader(vs);
      if (fs)   glDeleteShader(fs);
      if (ctx != EGL_NO_CONTEXT)  eglDestroyContext(dpy, ctx);
      if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
      eglTerminate(dpy);
   }
   if (g_nvk_log) fclose(g_nvk_log);
   return rc;
}
