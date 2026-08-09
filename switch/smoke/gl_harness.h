/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Headless OpenGL scaffolding shared by all gl_ test apps.
 */
#ifndef GL_HARNESS_H
#define GL_HARNESS_H

#include "nvk_harness.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_formats.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"

#include "frontend/api.h"
#include "state_tracker/st_context.h"

#include "zink_public.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

/* An offscreen default framebuffer */
struct gl_headless_fb {
   struct pipe_frontend_drawable base;
   struct st_visual              visual;
   struct pipe_screen           *pscreen;
   struct pipe_resource         *att[ST_ATTACHMENT_COUNT];
   unsigned                      width, height;
};

struct gl_headless {
   struct pipe_screen         *pscreen;
   struct pipe_frontend_screen fscreen;
   struct st_context          *st;
   struct gl_headless_fb       fb;
};

static struct pipe_resource *
gl_headless_get_texture(struct gl_headless_fb *fb, enum st_attachment_type statt)
{
   if (fb->att[statt])
      return fb->att[statt];

   struct pipe_resource tmpl;
   memset(&tmpl, 0, sizeof(tmpl));
   tmpl.target     = PIPE_TEXTURE_2D;
   tmpl.width0     = fb->width;
   tmpl.height0    = fb->height;
   tmpl.depth0     = 1;
   tmpl.array_size = 1;
   tmpl.usage      = PIPE_USAGE_DEFAULT;

   switch (statt) {
   case ST_ATTACHMENT_FRONT_LEFT:
   case ST_ATTACHMENT_BACK_LEFT:
      tmpl.format = fb->visual.color_format;
      tmpl.bind   = PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                    PIPE_BIND_SAMPLER_VIEW;
      break;
   case ST_ATTACHMENT_DEPTH_STENCIL:
      tmpl.format = fb->visual.depth_stencil_format;
      tmpl.bind   = PIPE_BIND_DEPTH_STENCIL;
      break;
   default:
      return NULL;
   }

   fb->att[statt] = fb->pscreen->resource_create(fb->pscreen, &tmpl);
   return fb->att[statt];
}

static bool
gl_headless_validate(struct st_context *st,
                     struct pipe_frontend_drawable *drawable,
                     const enum st_attachment_type *statts, unsigned count,
                     struct pipe_resource **out, struct pipe_resource **resolve)
{
   struct gl_headless_fb *fb = (struct gl_headless_fb *)drawable;
   (void)st;

   if (out) {
      for (unsigned i = 0; i < count; i++)
         pipe_resource_reference(&out[i], gl_headless_get_texture(fb, statts[i]));
   }
   if (resolve)
      *resolve = NULL;
   return true;
}

static bool
gl_headless_flush_front(struct st_context *st,
                        struct pipe_frontend_drawable *drawable,
                        enum st_attachment_type statt)
{
   (void)st; (void)drawable; (void)statt;
   return true;
}

static bool
gl_headless_flush_swapbuffers(struct st_context *st,
                              struct pipe_frontend_drawable *drawable)
{
   (void)st; (void)drawable;
   return true;
}

static int
gl_headless_get_param(struct pipe_frontend_screen *fscreen,
                      enum st_manager_param param)
{
   (void)fscreen; (void)param;
   return 0;
}

/* Bring up screen + GL context */
static bool
gl_headless_up(struct gl_headless *h, gl_api profile,
               unsigned w, unsigned hgt)
{
   struct st_context_attribs attribs;
   enum st_context_error sterr = ST_CONTEXT_SUCCESS;

   memset(h, 0, sizeof(*h));

   h->pscreen = zink_create_screen(NULL, NULL);
   fflush(stderr);
   if (!h->pscreen) {
      LOG("FAIL: zink_create_screen returned NULL");
      return false;
   }
   LOG("screen up: vendor='%s' name='%s'",
       h->pscreen->get_vendor(h->pscreen), h->pscreen->get_name(h->pscreen));

   h->fscreen.screen    = h->pscreen;
   h->fscreen.get_param = gl_headless_get_param;

   memset(&attribs, 0, sizeof(attribs));
   attribs.profile = profile;
   /* Take the driver's natural max version and log it rather than risk a spurious BAD_VERSION. */
   attribs.major = 1;
   attribs.minor = 0;
   attribs.visual.buffer_mask = ST_ATTACHMENT_BACK_LEFT_MASK |
                                ST_ATTACHMENT_DEPTH_STENCIL_MASK;
   attribs.visual.color_format         = PIPE_FORMAT_R8G8B8A8_UNORM;
   attribs.visual.depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;
   attribs.visual.accum_format         = PIPE_FORMAT_NONE;
   attribs.visual.samples              = 1;

   h->st = st_api_create_context(&h->fscreen, &attribs, &sterr, NULL);
   fflush(stderr);
   if (!h->st) {
      LOG("FAIL: st_api_create_context returned error %d", sterr);
      return false;
   }
   LOG("st context created (err=%d)", sterr);

   h->fb.pscreen = h->pscreen;
   h->fb.width   = w;
   h->fb.height  = hgt;
   h->fb.visual  = attribs.visual;
   h->fb.base.visual            = &h->fb.visual;
   h->fb.base.fscreen           = &h->fscreen;
   h->fb.base.ID                = 1;
   h->fb.base.validate          = gl_headless_validate;
   h->fb.base.flush_front       = gl_headless_flush_front;
   h->fb.base.flush_swapbuffers = gl_headless_flush_swapbuffers;
   p_atomic_set(&h->fb.base.stamp, 1);

   if (!st_api_make_current(h->st, &h->fb.base, &h->fb.base)) {
      LOG("FAIL: st_api_make_current returned false");
      return false;
   }
   LOG("context made current on %ux%u offscreen fb", w, hgt);

   LOG("GL_VERSION  = %s", (const char *)glGetString(GL_VERSION));
   LOG("GL_RENDERER = %s", (const char *)glGetString(GL_RENDERER));
   LOG("GL_VENDOR   = %s", (const char *)glGetString(GL_VENDOR));
   return true;
}

static void
gl_headless_down(struct gl_headless *h)
{
   if (h->st) {
      st_api_make_current(NULL, NULL, NULL);
      for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++)
         if (h->fb.att[i]) pipe_resource_reference(&h->fb.att[i], NULL);
      st_destroy_context(h->st);
      h->st = NULL;
   }
   if (h->pscreen) {
      h->pscreen->destroy(h->pscreen);
      h->pscreen = NULL;
   }
}

/* Redirect zink/mesa stderr diagnostics to a file next to the app log. */
static void
gl_headless_env(const char *mesa_log_path)
{
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_SHOW_STATS", "1", 1);
   setenv("MESA_LOG_LEVEL", "debug", 1);
   setenv("NVK_DEBUG", "errors", 1);
   if (!freopen(mesa_log_path, "w", stderr))
      LOG("WARN: could not redirect stderr to %s", mesa_log_path);
   setvbuf(stderr, NULL, _IONBF, 0);
   LOG("mesa/zink diagnostics -> %s", mesa_log_path);
}

PRINTFLIKE(1, 2) static void
gl_mark(const char *fmt, ...)
{
   va_list ap;

   fputs("APP: ", stderr);
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}

#endif /* GL_HARNESS_H */
