/*
 * CGL contexts on Mesa (moose-ripple, M4). Each context on our renderer is a Mesa st_context
 * on the process's gallium screen (cgl_device.c). For now contexts render only into
 * framebuffer objects (no window drawable yet): the first M4 target is a program that
 * draws into an FBO and reads the pixels back.
 */
#include <stdbool.h>
#include <string.h>

#include "frontend/api.h"
#include "main/mtypes.h"
#include "state_tracker/st_context.h"
#include "util/u_memory.h"

#include "cgl_context.h"
#include "cgl_device.h"
#include "cgl_dispatch.h"
#include "cgl_drawable.h"

static enum pipe_format
depth_stencil_format(const struct ripple_cgl_pixel_format *pf)
{
   if (pf->stencil_bits)
      return PIPE_FORMAT_Z24_UNORM_S8_UINT;
   if (pf->depth_bits > 24)
      return PIPE_FORMAT_Z32_UNORM;
   if (pf->depth_bits > 16)
      return PIPE_FORMAT_Z24X8_UNORM;
   if (pf->depth_bits)
      return PIPE_FORMAT_Z16_UNORM;
   return PIPE_FORMAT_NONE;
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
   attribs.visual.color_format = PIPE_FORMAT_B8G8R8A8_UNORM;
   attribs.visual.depth_stencil_format = depth_stencil_format(pf);
   attribs.visual.accum_format = pf->accum_bits ? PIPE_FORMAT_R16G16B16A16_SNORM
                                                : PIPE_FORMAT_NONE;
   attribs.visual.samples = pf->samples;
   attribs.visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK |
      (pf->double_buffer ? ST_ATTACHMENT_BACK_LEFT_MASK : 0) |
      (attribs.visual.depth_stencil_format ? ST_ATTACHMENT_DEPTH_STENCIL_MASK : 0) |
      (pf->accum_bits ? ST_ATTACHMENT_ACCUM_MASK : 0);

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
   st_destroy_context(ctx->st);
   ctx->magic = 0;
   FREE(ctx);
}

bool
ripple_cgl_context_make_current(struct ripple_cgl_context *ctx)
{
   /* Without a drawable the context is surfaceless and renders into FBOs only. */
   struct pipe_frontend_drawable *d = ctx && ctx->draw ? &ctx->draw->base : NULL;
   return st_api_make_current(ctx ? ctx->st : NULL, d, d);
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
      void **cur = CGLGetCurrentContext();
      ctx = cur ? cur[0] : NULL;
      if (!ctx || ctx->magic != RIPPLE_CGL_MAGIC)
         return;              /* nothing of ours current: Mesa's no-op dispatch answers */
      if (_glapi_get_context() == ctx->glctx)
         return;
   }
   ripple_cgl_context_make_current(ctx);
}
