/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Draw a triangle offscreen
 */
#include "gl_harness.h"

#define FB_W 128u
#define FB_H 128u

static const char *VS_SRC =
   "#version 330\n"
   "layout(location = 0) in vec2 aPos;\n"
   "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS_SRC =
   "#version 330\n"
   "out vec4 fragColor;\n"
   "void main() { fragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

/* Cover the centre of the fb. */
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
   struct gl_headless h;
   static uint8_t pixels[FB_W * FB_H * 4];
   GLuint vs = 0, fs = 0, prog = 0, vao = 0, vbo = 0;
   int rc = 0;

   nvk_log_open("sdmc:/gl_tri.log");
   LOG("=== gl_tri ===");
   gl_headless_env("sdmc:/gl_tri_mesa.log");

   if (!gl_headless_up(&h, API_OPENGL_COMPAT, FB_W, FB_H)) { rc = 1; goto out; }

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
      char log[1024]; GLsizei n = 0;
      glGetProgramInfoLog(prog, sizeof(log), &n, log);
      LOG("FAIL: program link failed: %s", n > 0 ? log : "(no log)");
      rc = 1; goto out;
   }
   LOG("program linked");

   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(TRI), TRI, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
   glEnableVertexAttribArray(0);

   glViewport(0, 0, FB_W, FB_H);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   glUseProgram(prog);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();

   GLenum gerr = glGetError();
   if (gerr != GL_NO_ERROR)
      LOG("WARN: glGetError after draw = 0x%04x", gerr);

   memset(pixels, 0, sizeof(pixels));
   glReadPixels(0, 0, FB_W, FB_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   gerr = glGetError();
   if (gerr != GL_NO_ERROR) {
      LOG("FAIL: glReadPixels glGetError = 0x%04x", gerr);
      rc = 1; goto out;
   }

#define PX(x, y) (&pixels[((y) * FB_W + (x)) * 4])
   const uint8_t *center = PX(FB_W / 2, FB_H / 2);
   const uint8_t *corner = PX(2, 2);
   bool center_green = center[0] < 64 && center[1] > 192 && center[2] < 64;
   bool corner_bg    = corner[0] < 16 && corner[1] < 16 && corner[2] < 16;

   unsigned green = 0;
   for (unsigned p = 0; p < FB_W * FB_H; p++) {
      const uint8_t *px = &pixels[p * 4];
      if (px[0] < 64 && px[1] > 192 && px[2] < 64) green++;
   }
   LOG("center={%u,%u,%u} corner={%u,%u,%u} green_px=%u/%u",
       center[0], center[1], center[2], corner[0], corner[1], corner[2],
       green, FB_W * FB_H);

   /* ~12% coverage with centre lit and corner not. */
   if (center_green && corner_bg && green > 500) {
      LOG("VERIFY OK: triangle rendered");
      LOG("=== PASSED ===");
   } else {
      LOG("VERIFY FAIL: center_green=%d corner_bg=%d green_px=%u",
          center_green, corner_bg, green);
      LOG("=== FAILED at verify ===");
      rc = 1;
   }
#undef PX

out:
   if (vbo)  glDeleteBuffers(1, &vbo);
   if (vao)  glDeleteVertexArrays(1, &vao);
   if (prog) glDeleteProgram(prog);
   if (vs)   glDeleteShader(vs);
   if (fs)   glDeleteShader(fs);
   gl_headless_down(&h);
   if (g_nvk_log) fclose(g_nvk_log);
   return rc;
}
