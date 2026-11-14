/*
 * CGL contexts on Mesa (moose-ripple, M4). Each context on our renderer is a Mesa st_context
 * on the process's gallium screen (cgl_device.c). For now contexts render only into
 * framebuffer objects (no window drawable yet): the first M4 target is a program that
 * draws into an FBO and reads the pixels back.
 */
#include <stdbool.h>
#include <string.h>

#include <stdio.h>

#include "frontend/api.h"
#include "main/framebuffer.h"
#include "main/mtypes.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_context.h"
#include "util/u_memory.h"

#include "cgl_context.h"
#include "cgl_device.h"
#include "cgl_dispatch.h"
#include "cgl_drawable.h"
#include "cgl_takeover.h"

/* The first of `list` (ended by PIPE_FORMAT_NONE) the GPU can make a 2D buffer of for `bind`. */
static enum pipe_format
first_supported(struct pipe_screen *screen, const enum pipe_format *list, unsigned samples,
                unsigned bind)
{
   for (; *list != PIPE_FORMAT_NONE; list++)
      if (screen->is_format_supported(screen, *list, PIPE_TEXTURE_2D, samples, samples, bind))
         return *list;
   return PIPE_FORMAT_NONE;
}

/* The depth (and stencil) buffer of a window or pbuffer: a format the GPU has, asked of the
 * screen. Taking Z24_S8 unasked gave every pixel format with a stencil buffer a depth buffer
 * the host could not make (virglrenderer has S8_Z24, not Z24_S8): the first use of it was an
 * error on the host, the rest of that command buffer was dropped, and the process's context
 * stayed in error. Real Racing 2 (black intro, garbled menu) and Chess (black board) asked for
 * stencil. */
static enum pipe_format
depth_stencil_format(struct pipe_screen *screen, const struct ripple_cgl_pixel_format *pf)
{
   static const enum pipe_format stencil[] = { PIPE_FORMAT_Z24_UNORM_S8_UINT,
      PIPE_FORMAT_S8_UINT_Z24_UNORM, PIPE_FORMAT_Z32_FLOAT_S8X24_UINT, PIPE_FORMAT_NONE };
   static const enum pipe_format d32[] = { PIPE_FORMAT_Z32_UNORM, PIPE_FORMAT_Z32_FLOAT,
      PIPE_FORMAT_Z24X8_UNORM, PIPE_FORMAT_X8Z24_UNORM, PIPE_FORMAT_Z16_UNORM, PIPE_FORMAT_NONE };
   static const enum pipe_format d24[] = { PIPE_FORMAT_Z24X8_UNORM, PIPE_FORMAT_X8Z24_UNORM,
      PIPE_FORMAT_Z32_UNORM, PIPE_FORMAT_Z32_FLOAT, PIPE_FORMAT_Z16_UNORM, PIPE_FORMAT_NONE };
   static const enum pipe_format d16[] = { PIPE_FORMAT_Z16_UNORM, PIPE_FORMAT_Z24X8_UNORM,
      PIPE_FORMAT_X8Z24_UNORM, PIPE_FORMAT_Z32_UNORM, PIPE_FORMAT_NONE };
   enum pipe_format f = PIPE_FORMAT_NONE;

   if (pf->stencil_bits)
      f = first_supported(screen, stencil, pf->samples, PIPE_BIND_DEPTH_STENCIL);
   if (f == PIPE_FORMAT_NONE && (pf->depth_bits || pf->stencil_bits))
      f = first_supported(screen, pf->depth_bits > 24 ? d32 : pf->depth_bits > 16 ||
                          pf->stencil_bits ? d24 : d16, pf->samples, PIPE_BIND_DEPTH_STENCIL);
   return f;
}

/* The accumulation buffer, likewise. */
static enum pipe_format
accum_format(struct pipe_screen *screen, const struct ripple_cgl_pixel_format *pf)
{
   static const enum pipe_format accum[] = { PIPE_FORMAT_R16G16B16A16_SNORM,
      PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_NONE };

   return pf->accum_bits ? first_supported(screen, accum, 0, PIPE_BIND_RENDER_TARGET)
                         : PIPE_FORMAT_NONE;
}

struct ripple_cgl_context *
ripple_cgl_context_create(const struct ripple_cgl_pixel_format *pf,
                          struct ripple_cgl_context *share)
{
   struct pipe_frontend_screen *fscreen = ripple_cgl_device_fscreen();
   struct st_context_attribs attribs;
   enum st_context_error error = ST_CONTEXT_SUCCESS;
   struct ripple_cgl_context *ctx;

   if (!fscreen)
      return NULL;
   ctx = CALLOC_STRUCT(ripple_cgl_context);
   if (!ctx)
      return NULL;
   ctx->magic = RIPPLE_CGL_MAGIC;
   ctx->pf = *pf;

   /* Mac OS X 10.6 has only legacy (compatibility) contexts, GL 2.1 at most for apps, but
    * Mesa may expose more through extensions. */
   memset(&attribs, 0, sizeof(attribs));
   attribs.profile = API_OPENGL_COMPAT;
   attribs.major = 1;
   attribs.minor = 0;
   /* Every Mac GPU of the time has S3TC, so apps upload DXT textures without asking. A host
    * GPU without it (Apple silicon through ANGLE) gets Mesa's software decoding instead of a
    * failed upload. */
   attribs.options.allow_compressed_fallback = true;
   attribs.visual.color_format = PIPE_FORMAT_B8G8R8A8_UNORM;
   attribs.visual.depth_stencil_format = depth_stencil_format(fscreen->screen, pf);
   attribs.visual.accum_format = accum_format(fscreen->screen, pf);
   attribs.visual.samples = pf->samples;
   attribs.visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK |
      (pf->double_buffer ? ST_ATTACHMENT_BACK_LEFT_MASK : 0) |
      (attribs.visual.depth_stencil_format ? ST_ATTACHMENT_DEPTH_STENCIL_MASK : 0) |
      (attribs.visual.accum_format ? ST_ATTACHMENT_ACCUM_MASK : 0);

   ctx->visual = attribs.visual;
   ctx->st = st_api_create_context(fscreen, &attribs, &error, share ? share->st : NULL);
   if (!ctx->st) {
      FREE(ctx);
      return NULL;
   }
   ctx->st->frontend_context = ctx;
   ctx->glctx = ctx->st->ctx;
   return ctx;
}

void
ripple_cgl_context_destroy(struct ripple_cgl_context *ctx)
{
   if (!ctx)
      return;
   if (_glapi_get_context() == ctx->glctx)
      st_api_make_current(NULL, NULL, NULL);
   ripple_cgl_forget(ctx);
   ripple_cgl_client_storage_context_gone(ctx->glctx);
   st_destroy_context(ctx->st);
   ctx->magic = 0;
   FREE(ctx);
}

/* Debug log: a context that has a drawable but did not get its framebuffer (Mesa refused the
 * pair, or made the context current on its attachment-less placeholder). Everything such a
 * context draws is dropped with GL_INVALID_FRAMEBUFFER_OPERATION. */
static void
log_unbound(const struct ripple_cgl_context *ctx, bool ok)
{
   extern FILE *ripple_gld_dbg_file(void);
   static unsigned logged;
   FILE *f = ripple_gld_dbg_file();
   const struct gl_config *v = &ctx->glctx->Visual;
   const struct ripple_cgl_drawable *d = ctx->draw;

   if (!f || logged >= 40)
      return;
   logged++;
   fprintf(f, "RippleGLDriver: context %p did not get drawable %p (id %u, %ux%u): make current %s; "
           "context visual rgba %d %d %d %d depth %d stencil %d double %d samples %d, drawable "
           "visual buffers 0x%x colour format %d depth format %d samples %d\n", (void *)ctx,
           (void *)d, d->base.ID, d->width, d->height, ok ? "ok" : "refused", v->redBits,
           v->greenBits, v->blueBits, v->alphaBits, v->depthBits, v->stencilBits,
           v->doubleBufferMode, v->samples, d->visual.buffer_mask, d->visual.color_format,
           d->visual.depth_stencil_format, d->visual.samples);
}

bool
ripple_cgl_context_make_current(struct ripple_cgl_context *ctx)
{
   /* Without a drawable the context is surfaceless and renders into FBOs only. */
   struct pipe_frontend_drawable *d = ctx && ctx->draw ? &ctx->draw->base : NULL;
   bool ok = st_api_make_current(ctx ? ctx->st : NULL, d, d);
   if (d && (!ok || ctx->glctx->WinSysDrawBuffer == _mesa_get_incomplete_framebuffer()))
      log_unbound(ctx, ok);
   if (ok && ctx && unlikely(ctx->sync_gc))
      ripple_cgl_sync_from_glengine(ctx);
   return ok;
}

/* Read from `read` for a moment (a pbuffer used as a texture, cgl_apple.c); NULL reads from the
 * context's own drawable again. A context without a drawable draws into `read` meanwhile. */
void
ripple_cgl_context_set_read_drawable(struct ripple_cgl_context *ctx,
                                     struct ripple_cgl_drawable *read)
{
   struct pipe_frontend_drawable *d = ctx->draw ? &ctx->draw->base : NULL;
   struct pipe_frontend_drawable *r = read ? &read->base : d;
   /* Mesa sets a context's viewport and scissor box to the size of the first drawable it is
    * made current with. For a context without a drawable that would be this borrowed one,
    * in the middle of its drawing: the viewport the program had set was replaced by the
    * borrowed drawable's size. (Core Image, drawing for Quartz Composer into a 512x512
    * texture after reading a 551x551 buffer, drew its result 551/512 too large; the RSS
    * Visualizer saver's cloud pattern no longer tiled and showed a seam.) The borrowed
    * drawable is not the context's own: keep Mesa from taking it for that. */
   GLboolean initialized = ctx->glctx->ViewportInitialized;
   if (!d)
      ctx->glctx->ViewportInitialized = GL_TRUE;
   st_api_make_current(ctx->st, d ? d : r, r);
   if (!d)
      ctx->glctx->ViewportInitialized = initialized;
}

/* From OpenGL.framework (CGLCurrent.h); declared here to keep Apple's GL headers out. */
extern void *CGLGetCurrentContext(void);

void
ripple_cgl_context_set_drawable(struct ripple_cgl_context *ctx, struct ripple_cgl_drawable *d)
{
   bool current = _glapi_get_context() == ctx->glctx;

   ctx->draw = d;
   if (current)
      ripple_cgl_context_make_current(ctx);
}

void
ripple_cgl_make_current_slow(void *rend)
{
   struct ripple_cgl_context *ctx = rend;

   /* GLEngine itself calls a few entries with its own context (get_integerv under
    * kCGLCEStateValidation): fall back to the current CGL context if that is ours. */
   if (!ctx || ctx->magic != RIPPLE_CGL_MAGIC) {
      ctx = ripple_cgl_rend_for(rend);   /* GLEngine's context for one we took over */
      void **cur = ctx ? NULL : CGLGetCurrentContext();
      if (cur)
         ctx = cur[0];
      if (!ctx || ctx->magic != RIPPLE_CGL_MAGIC)
         return;              /* nothing of ours current: Mesa's no-op dispatch answers */
      if (_glapi_get_context() == ctx->glctx)
         return;
   }
   if (unlikely(ripple_cgl_tracing))      /* which context the next calls belong to */
      ripple_cgl_trace(1001, "---- context %p ----", (void *)ctx);
   ripple_cgl_context_make_current(ctx);
}

struct ripple_cgl_context *
ripple_cgl_context_for(void *rend)
{
   struct ripple_cgl_context *ctx = rend;
   if (ctx && ctx->magic == RIPPLE_CGL_MAGIC)
      return ctx;
   return ripple_cgl_rend_for(rend);
}
