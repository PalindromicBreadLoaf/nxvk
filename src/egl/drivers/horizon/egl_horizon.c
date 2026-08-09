/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/**
 * EGL driver for Horizon, presenting to an libnx NWindow.
 *
 * Standalone in the sense as haiku and wgl.
 * Gallium is derived directly and never goes through DRI.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldevice.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "egllog.h"
#include "eglsurface.h"
#include "egltypedefs.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"

#include "frontend/api.h"
#include "state_tracker/st_context.h"

#include <vulkan/vulkan.h>

#include "zink/zink_public.h"
#include "zink/zink_kopper.h"

#include <switch.h>

_EGL_DRIVER_STANDARD_TYPECASTS(horizon_egl)

struct horizon_egl_display {
   int ref_count;
   struct pipe_screen *pscreen;
   struct pipe_frontend_screen fscreen;
};

struct horizon_egl_config {
   _EGLConfig base;
};

struct horizon_egl_context {
   _EGLContext base;
   struct st_context *st;
};

struct horizon_egl_surface {
   _EGLSurface base;
   struct pipe_frontend_drawable drawable;
   struct st_visual visual;
   struct kopper_loader_info info;
   struct pipe_screen *pscreen;
   struct pipe_resource *att[ST_ATTACHMENT_COUNT];
   NWindow *win;
   bool is_window;
};

static struct horizon_egl_surface *
horizon_surface_from_drawable(struct pipe_frontend_drawable *drawable)
{
   return (struct horizon_egl_surface *)((char *)drawable -
      offsetof(struct horizon_egl_surface, drawable));
}

/* #pragma mark - framebuffer */

static struct pipe_resource *
horizon_get_texture(struct horizon_egl_surface *surf,
                    enum st_attachment_type statt)
{
   if (surf->att[statt])
      return surf->att[statt];

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = surf->base.Width;
   templ.height0 = surf->base.Height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.usage = PIPE_USAGE_DEFAULT;

   switch (statt) {
   case ST_ATTACHMENT_FRONT_LEFT:
   case ST_ATTACHMENT_BACK_LEFT:
      templ.format = surf->visual.color_format;
      templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                   PIPE_BIND_SAMPLER_VIEW;
      break;
   case ST_ATTACHMENT_DEPTH_STENCIL:
      templ.format = surf->visual.depth_stencil_format;
      templ.bind = PIPE_BIND_DEPTH_STENCIL;
      break;
   default:
      return NULL;
   }

   if (templ.format == PIPE_FORMAT_NONE)
      return NULL;

   /* Only the colour target is a swapchain image; depth/stencil is ordinary. */
   if (surf->is_window && statt < ST_ATTACHMENT_DEPTH_STENCIL) {
      surf->att[statt] = surf->pscreen->resource_create_drawable(
         surf->pscreen, &templ, &surf->info);
   } else {
      templ.bind &= ~PIPE_BIND_DISPLAY_TARGET;
      surf->att[statt] = surf->pscreen->resource_create(surf->pscreen, &templ);
   }

   return surf->att[statt];
}

static bool
horizon_validate(struct st_context *st,
                 struct pipe_frontend_drawable *drawable,
                 const enum st_attachment_type *statts, unsigned count,
                 struct pipe_resource **out, struct pipe_resource **resolve)
{
   struct horizon_egl_surface *surf = horizon_surface_from_drawable(drawable);

   if (out) {
      for (unsigned i = 0; i < count; i++)
         pipe_resource_reference(&out[i], horizon_get_texture(surf, statts[i]));
   }
   if (resolve)
      *resolve = NULL;
   return true;
}

static bool
horizon_flush_front(struct st_context *st,
                    struct pipe_frontend_drawable *drawable,
                    enum st_attachment_type statt)
{
   return true;
}

static bool
horizon_flush_swapbuffers(struct st_context *st,
                          struct pipe_frontend_drawable *drawable)
{
   return true;
}

static int
horizon_get_param(struct pipe_frontend_screen *fscreen,
                  enum st_manager_param param)
{
   return 0;
}

/* #pragma mark - EGLSurface */

static _EGLSurface *
horizon_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                       void *native_window, const EGLint *attrib_list)
{
   struct horizon_egl_display *hdpy = horizon_egl_display(disp);
   struct horizon_egl_surface *surf;
   u32 width = 0, height = 0;

   surf = calloc(1, sizeof(*surf));
   if (!surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSurface");
      return NULL;
   }

   if (!_eglInitSurface(&surf->base, disp, type, conf, attrib_list, NULL))
      goto fail;

   surf->pscreen = hdpy->pscreen;
   surf->is_window = type == EGL_WINDOW_BIT;

   if (surf->is_window) {
      surf->win = native_window ? native_window : nwindowGetDefault();
      if (!surf->win || R_FAILED(nwindowGetDimensions(surf->win, &width,
                                                      &height))) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "eglCreateWindowSurface");
         goto fail;
      }
      surf->base.Width = width;
      surf->base.Height = height;

      VkViSurfaceCreateInfoNN *vi = (VkViSurfaceCreateInfoNN *)&surf->info.bos;
      vi->sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
      vi->pNext = NULL;
      vi->flags = 0;
      vi->window = surf->win;
      surf->info.has_alpha = conf->AlphaSize > 0;
      surf->info.initial_swap_interval = 1;
      surf->info.present_opaque = surf->base.PresentOpaque;
   }

   surf->visual.buffer_mask = ST_ATTACHMENT_BACK_LEFT_MASK;
   surf->visual.color_format = PIPE_FORMAT_R8G8B8A8_UNORM;
   if (conf->DepthSize > 0 || conf->StencilSize > 0) {
      surf->visual.buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;
      surf->visual.depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;
   } else {
      surf->visual.depth_stencil_format = PIPE_FORMAT_NONE;
   }
   surf->visual.accum_format = PIPE_FORMAT_NONE;
   surf->visual.samples = 1;

   surf->drawable.visual = &surf->visual;
   surf->drawable.fscreen = &hdpy->fscreen;
   surf->drawable.ID = (uintptr_t)surf;
   surf->drawable.validate = horizon_validate;
   surf->drawable.flush_front = horizon_flush_front;
   surf->drawable.flush_swapbuffers = horizon_flush_swapbuffers;
   p_atomic_set(&surf->drawable.stamp, 1);

   return &surf->base;

fail:
   free(surf);
   return NULL;
}

static _EGLSurface *
horizon_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                              void *native_window, const EGLint *attrib_list)
{
   return horizon_create_surface(disp, EGL_WINDOW_BIT, conf, native_window,
                                 attrib_list);
}

static _EGLSurface *
horizon_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                               const EGLint *attrib_list)
{
   return horizon_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL,
                                 attrib_list);
}

static _EGLSurface *
horizon_create_pixmap_surface(_EGLDisplay *disp, _EGLConfig *conf,
                              void *native_pixmap, const EGLint *attrib_list)
{
   return NULL;
}

static EGLBoolean
horizon_destroy_surface(_EGLDisplay *disp, _EGLSurface *base)
{
   if (_eglPutSurface(base)) {
      struct horizon_egl_surface *surf = horizon_egl_surface(base);

      for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++)
         pipe_resource_reference(&surf->att[i], NULL);

      /* Drop the framebuffer st caches for this drawable before it is freed. */
      st_api_destroy_drawable(&surf->drawable);

      free(surf);
   }
   return EGL_TRUE;
}

struct horizon_flush_args {
   struct pipe_context *pipe;
   struct pipe_resource *back;
};

static void
horizon_before_flush_cb(void *args)
{
   struct horizon_flush_args *a = args;

   a->pipe->flush_resource(a->pipe, a->back);
}

static EGLBoolean
horizon_swap_buffers(_EGLDisplay *disp, _EGLSurface *base)
{
   struct horizon_egl_display *hdpy = horizon_egl_display(disp);
   struct horizon_egl_surface *surf = horizon_egl_surface(base);
   struct horizon_egl_context *hctx =
      horizon_egl_context(base->CurrentContext);
   struct pipe_resource *back = surf->att[ST_ATTACHMENT_BACK_LEFT];

   if (!hctx || !back)
      return EGL_FALSE;

   struct st_context *st = hctx->st;
   struct horizon_flush_args args = { st->pipe, back };

   /* flush_resource has to land after the pending GL work is submitted but
    * before the pipe flush.
    */
   st_context_flush(st, ST_FLUSH_END_OF_FRAME, NULL,
                    horizon_before_flush_cb, &args);

   hdpy->pscreen->flush_frontbuffer(hdpy->pscreen, st->pipe, back, 0, 0,
                                    &surf->drawable, 0, NULL);

   /* The swapchain rotates the VkImage behind this resource. The resource
    * itself is persistent. A failed check means it went out of date. */
   if (surf->is_window && !zink_kopper_check(back))
      return _eglError(EGL_BAD_SURFACE, "eglSwapBuffers");

   p_atomic_inc(&surf->drawable.stamp);
   st_context_invalidate_state(st, ST_INVALIDATE_FB_STATE);

   return EGL_TRUE;
}

static EGLBoolean
horizon_swap_interval(_EGLDisplay *disp, _EGLSurface *base, EGLint interval)
{
   struct horizon_egl_surface *surf = horizon_egl_surface(base);
   struct pipe_resource *back = surf->att[ST_ATTACHMENT_BACK_LEFT];

   surf->info.initial_swap_interval = interval;
   if (surf->is_window && back)
      zink_kopper_set_swap_interval(surf->pscreen, back, interval);

   return EGL_TRUE;
}

/* #pragma mark - EGLDisplay */

static EGLBoolean
horizon_add_configs(_EGLDisplay *disp)
{
   static const EGLint depth_sizes[] = {0, 24};
   EGLint id = 1;

   for (unsigned d = 0; d < ARRAY_SIZE(depth_sizes); d++) {
      struct horizon_egl_config *conf = calloc(1, sizeof(*conf));
      if (!conf)
         return _eglError(EGL_BAD_ALLOC, "eglInitialize");

      _eglInitConfig(&conf->base, disp, id);

      conf->base.RedSize = 8;
      conf->base.GreenSize = 8;
      conf->base.BlueSize = 8;
      conf->base.AlphaSize = 8;
      conf->base.BufferSize = 32;
      conf->base.ColorBufferType = EGL_RGB_BUFFER;
      conf->base.ConfigCaveat = EGL_NONE;
      conf->base.ConfigID = id;
      conf->base.DepthSize = depth_sizes[d];
      conf->base.StencilSize = depth_sizes[d] ? 8 : 0;
      conf->base.TransparentType = EGL_NONE;
      conf->base.NativeRenderable = EGL_TRUE;
      conf->base.NativeVisualID = 0;
      conf->base.NativeVisualType = EGL_NONE;
      conf->base.RenderableType = disp->ClientAPIs;
      conf->base.Conformant = disp->ClientAPIs;
      conf->base.SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
      conf->base.MinSwapInterval = 0;
      conf->base.MaxSwapInterval = 1;
      conf->base.MaxPbufferWidth = _EGL_MAX_PBUFFER_WIDTH;
      conf->base.MaxPbufferHeight = _EGL_MAX_PBUFFER_HEIGHT;

      if (!_eglValidateConfig(&conf->base, EGL_FALSE)) {
         _eglLog(_EGL_DEBUG, "Horizon: failed to validate config %d", id);
         free(conf);
         continue;
      }

      _eglLinkConfig(&conf->base);
      id++;
   }

   if (!_eglGetArraySize(disp->Configs))
      return _eglError(EGL_NOT_INITIALIZED, "Horizon: no configs");

   return EGL_TRUE;
}

static EGLBoolean
horizon_initialize_impl(_EGLDisplay *disp)
{
   struct horizon_egl_display *hdpy;

   hdpy = calloc(1, sizeof(*hdpy));
   if (!hdpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   hdpy->ref_count = 1;
   disp->DriverData = hdpy;

   hdpy->pscreen = zink_create_screen(NULL, NULL);
   if (!hdpy->pscreen) {
      free(hdpy);
      disp->DriverData = NULL;
      return _eglError(EGL_NOT_INITIALIZED, "Horizon: failed to create screen");
   }

   hdpy->fscreen.screen = hdpy->pscreen;
   hdpy->fscreen.get_param = horizon_get_param;

   disp->ClientAPIs = 0;
   if (_eglIsApiValid(EGL_OPENGL_API))
      disp->ClientAPIs |= EGL_OPENGL_BIT;
   if (_eglIsApiValid(EGL_OPENGL_ES_API))
      disp->ClientAPIs |= EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT |
                          EGL_OPENGL_ES3_BIT_KHR;

   disp->Extensions.KHR_create_context = EGL_TRUE;
   disp->Extensions.KHR_no_config_context = EGL_TRUE;
   disp->Extensions.KHR_surfaceless_context = EGL_TRUE;

   disp->Extensions.EXT_create_context_robustness =
      hdpy->pscreen->caps.device_reset_status_query;
   disp->RobustBufferAccess = hdpy->pscreen->caps.robust_buffer_access_behavior;

   return horizon_add_configs(disp);
}

static EGLBoolean
horizon_initialize(_EGLDisplay *disp)
{
   struct horizon_egl_display *hdpy = horizon_egl_display(disp);

   if (hdpy) {
      hdpy->ref_count++;
      return EGL_TRUE;
   }

   if (disp->Platform != _EGL_PLATFORM_HORIZON)
      return _eglError(EGL_NOT_INITIALIZED, "Horizon: unsupported platform");

   return horizon_initialize_impl(disp);
}

static EGLBoolean
horizon_terminate(_EGLDisplay *disp)
{
   struct horizon_egl_display *hdpy = horizon_egl_display(disp);

   if (!hdpy)
      return EGL_TRUE;

   if (--hdpy->ref_count > 0)
      return EGL_TRUE;

   if (hdpy->pscreen)
      hdpy->pscreen->destroy(hdpy->pscreen);

   free(hdpy);
   disp->DriverData = NULL;

   return EGL_TRUE;
}

/* #pragma mark - EGLContext */

static EGLBoolean
horizon_context_attribs(_EGLContext *base, struct st_context_attribs *attribs)
{
   memset(attribs, 0, sizeof(*attribs));

   switch (base->ClientAPI) {
   case EGL_OPENGL_ES_API:
      attribs->profile = base->ClientMajorVersion > 1 ? API_OPENGLES2
                                                      : API_OPENGLES;
      break;
   case EGL_OPENGL_API:
      attribs->profile =
         base->Profile == EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR
            ? API_OPENGL_CORE : API_OPENGL_COMPAT;
      break;
   default:
      return EGL_FALSE;
   }

   attribs->major = base->ClientMajorVersion;
   attribs->minor = base->ClientMinorVersion;

   if (base->Flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR)
      attribs->flags |= ST_CONTEXT_FLAG_DEBUG;
   if (base->Flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR)
      attribs->flags |= ST_CONTEXT_FLAG_FORWARD_COMPATIBLE;
   if (base->NoError)
      attribs->flags |= ST_CONTEXT_FLAG_NO_ERROR;

   if (base->Flags & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)
      attribs->context_flags |= PIPE_CONTEXT_ROBUST_BUFFER_ACCESS;
   if (base->ResetNotificationStrategy != EGL_NO_RESET_NOTIFICATION_KHR)
      attribs->context_flags |= PIPE_CONTEXT_LOSE_CONTEXT_ON_RESET;

   attribs->visual.buffer_mask = ST_ATTACHMENT_BACK_LEFT_MASK |
                                 ST_ATTACHMENT_DEPTH_STENCIL_MASK;
   attribs->visual.color_format = PIPE_FORMAT_R8G8B8A8_UNORM;
   attribs->visual.depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;
   attribs->visual.accum_format = PIPE_FORMAT_NONE;
   attribs->visual.samples = 1;

   return EGL_TRUE;
}

static _EGLContext *
horizon_create_context(_EGLDisplay *disp, _EGLConfig *conf,
                       _EGLContext *share_list, const EGLint *attrib_list)
{
   struct horizon_egl_display *hdpy = horizon_egl_display(disp);
   struct horizon_egl_context *ctx;
   struct st_context_attribs attribs;
   enum st_context_error sterr = ST_CONTEXT_SUCCESS;

   ctx = calloc(1, sizeof(*ctx));
   if (!ctx) {
      _eglError(EGL_BAD_ALLOC, "eglCreateContext");
      return NULL;
   }

   if (!_eglInitContext(&ctx->base, disp, conf, share_list, attrib_list))
      goto fail;

   if (!horizon_context_attribs(&ctx->base, &attribs)) {
      _eglError(EGL_BAD_MATCH, "eglCreateContext");
      goto fail;
   }

   ctx->st = st_api_create_context(
      &hdpy->fscreen, &attribs, &sterr,
      share_list ? horizon_egl_context(share_list)->st : NULL);
   if (!ctx->st) {
      _eglError(sterr == ST_CONTEXT_ERROR_BAD_VERSION ? EGL_BAD_MATCH
                                                      : EGL_BAD_ALLOC,
                "eglCreateContext");
      goto fail;
   }

   return &ctx->base;

fail:
   free(ctx);
   return NULL;
}

static EGLBoolean
horizon_destroy_context(_EGLDisplay *disp, _EGLContext *base)
{
   if (_eglPutContext(base)) {
      struct horizon_egl_context *ctx = horizon_egl_context(base);

      st_destroy_context(ctx->st);
      free(ctx);
   }
   return EGL_TRUE;
}

static EGLBoolean
horizon_make_current(_EGLDisplay *disp, _EGLSurface *dsurf, _EGLSurface *rsurf,
                     _EGLContext *base)
{
   struct horizon_egl_context *ctx = horizon_egl_context(base);
   struct horizon_egl_surface *dsurf_h = horizon_egl_surface(dsurf);
   struct horizon_egl_surface *rsurf_h = horizon_egl_surface(rsurf);
   _EGLContext *old_ctx;
   _EGLSurface *old_dsurf, *old_rsurf;

   if (!_eglBindContext(base, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))
      return EGL_FALSE;

   if (old_ctx == base && old_dsurf == dsurf && old_rsurf == rsurf) {
      _eglPutSurface(old_dsurf);
      _eglPutSurface(old_rsurf);
      _eglPutContext(old_ctx);
      return EGL_TRUE;
   }

   if (!base) {
      st_api_make_current(NULL, NULL, NULL);
   } else if (!st_api_make_current(ctx->st,
                                   dsurf_h ? &dsurf_h->drawable : NULL,
                                   rsurf_h ? &rsurf_h->drawable : NULL)) {
      return _eglError(EGL_BAD_MATCH, "eglMakeCurrent");
   }

   if (old_dsurf)
      horizon_destroy_surface(disp, old_dsurf);
   if (old_rsurf)
      horizon_destroy_surface(disp, old_rsurf);
   if (old_ctx)
      horizon_destroy_context(disp, old_ctx);

   return EGL_TRUE;
}

const _EGLDriver _eglDriver = {
   .Initialize = horizon_initialize,
   .Terminate = horizon_terminate,
   .CreateContext = horizon_create_context,
   .DestroyContext = horizon_destroy_context,
   .MakeCurrent = horizon_make_current,
   .CreateWindowSurface = horizon_create_window_surface,
   .CreatePixmapSurface = horizon_create_pixmap_surface,
   .CreatePbufferSurface = horizon_create_pbuffer_surface,
   .DestroySurface = horizon_destroy_surface,
   .SwapBuffers = horizon_swap_buffers,
   .SwapInterval = horizon_swap_interval,
};
