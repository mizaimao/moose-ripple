/*
 * CGL contexts on Mesa (moose-ripple, M4). Each context on our renderer is a Mesa st_context
 * on the process's gallium screen (cgl_device.c). For now contexts render only into
 * framebuffer objects (no window drawable yet): the first M4 target is a program that
 * draws into an FBO and reads the pixels back.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <stdio.h>

#include "frontend/api.h"
#include "main/framebuffer.h"
#include "main/glthread.h"
#include "main/mtypes.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_context.h"
#include "util/simple_mtx.h"
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

/* ---- Whose context is current ------------------------------------------------------------- *
 * A context is made current for Mesa lazily, at the first GL call that comes for it on a
 * thread (RIPPLE_CGL_ENTER), and Mesa keeps its pointer to it in that thread until another
 * context of ours is called there. The program meanwhile may have destroyed the context from
 * another thread: a garbage-collected program's collector does (ScreenSaverEngine, System
 * Preferences). The first thread is then left with a pointer to freed memory, which Mesa
 * follows at the next switch (it flushes the context it believes current), and which the next
 * context created can be given as its own address (issue 240: a crash in glClear, in a context
 * that looked current and had never been bound).
 *
 * So a context notes the thread it is current on, and there is a list of the contexts that
 * exist. Mesa's pointer in a thread counts only if it is in the list and its context names
 * this thread; anything else is taken out of the thread, unfollowed, before Mesa sees it. */
#define LIVE_MAX 512
static struct ripple_cgl_context *live[LIVE_MAX];
static unsigned live_used;               /* entries in use are below this */
static simple_mtx_t live_lock = SIMPLE_MTX_INITIALIZER;

_Static_assert(offsetof(struct ripple_cgl_context, thread) ==
               offsetof(struct ripple_cgl_rend, thread), "the thunks read the thread note");

static void
live_add(struct ripple_cgl_context *ctx)
{
   simple_mtx_lock(&live_lock);
   unsigned i = 0;
   while (i < live_used && live[i])
      i++;
   if (i < LIVE_MAX) {                   /* past the limit: never "already current", only slower */
      live[i] = ctx;
      if (i == live_used)
         live_used++;
   }
   simple_mtx_unlock(&live_lock);
}

static void
live_remove(struct ripple_cgl_context *ctx)
{
   simple_mtx_lock(&live_lock);
   for (unsigned i = 0; i < live_used; i++)
      if (live[i] == ctx)
         live[i] = NULL;
   while (live_used && !live[live_used - 1])
      live_used--;
   ctx->thread = NULL;
   simple_mtx_unlock(&live_lock);
}

static inline void thread_finish(struct gl_context *gl);

/* The context Mesa has current on the calling thread, if it is a living one that names this
 * thread. A pointer that is anything else is dropped from the thread here, with the dispatch
 * table that went with it. */
static struct ripple_cgl_context *
thread_current(void)
{
   struct gl_context *gl = _glapi_get_context();
   struct ripple_cgl_context *ctx = NULL;

   bool alive = false;

   if (!gl)
      return NULL;
   simple_mtx_lock(&live_lock);
   for (unsigned i = 0; i < live_used && !ctx; i++) {
      if (live[i] && live[i]->glctx == gl) {
         alive = true;
         if (live[i]->thread == (void *)pthread_self())
            ctx = live[i];
      }
   }
   simple_mtx_unlock(&live_lock);
   if (!ctx) {
      /* A context that still exists but has moved to another thread may have calls queued
       * here for its second GL thread: they run now, while it is still current here. */
      if (alive)
         thread_finish(gl);
      _glapi_set_context(NULL);
      _glapi_set_dispatch(NULL);         /* Mesa's table of no-ops */
   }
   return ctx;
}

bool
ripple_cgl_context_is_current(const struct ripple_cgl_context *ctx)
{
   return ctx && _glapi_get_context() == ctx->glctx && ctx->thread == (void *)pthread_self();
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
   /* At most 16 texture image units per stage (32 combined), as Apple's drivers of the time
    * report (an NVIDIA GeForce 9600M GT: 16 fragment, 16 vertex, 32 combined). Mesa offers 32
    * per stage here, and programs size their own tables by Mac GPUs: BioShock set up units up to
    * GL_TEXTURE16 and crashed one past the end of its table, the same way twice. */
   {
      struct gl_context *gl = ctx->glctx;
      for (int s = 0; s < MESA_SHADER_STAGES; s++)
         gl->Const.Program[s].MaxTextureImageUnits = MIN2(gl->Const.Program[s].MaxTextureImageUnits, 16);
      gl->Const.MaxCombinedTextureImageUnits = MIN2(gl->Const.MaxCombinedTextureImageUnits, 32);
      gl->Const.MaxTextureUnits = MIN2(gl->Const.MaxTextureUnits,
                                       gl->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits);
   }
   live_add(ctx);
   /* The second GL thread (cgl_dispatch.h), where it is wanted and the driver can do it. */
   if (ripple_cgl_device_second_thread()) {
      _mesa_glthread_init(ctx->glctx);
      if (ctx->glctx->GLThread.enabled)
         ripple_cgl_threaded = 1;
   }
   return ctx;
}

void
ripple_cgl_context_destroy(struct ripple_cgl_context *ctx)
{
   if (!ctx)
      return;
   /* Mesa saves and restores the calling thread's current context around the destruction:
    * a pointer left over in this thread must be gone before that. */
   thread_current();
   ripple_cgl_quiesce(ctx);              /* Mesa ends the context's second thread itself */
   if (_glapi_get_context() == ctx->glctx)
      st_api_make_current(NULL, NULL, NULL);
   live_remove(ctx);
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

/* The second GL thread of a context, if it has one, has to be done before the context is
 * touched from here. Mesa's finish runs the calls still waiting in the context's open batch
 * right here, on the calling thread, where they act on whatever context is current: for a
 * context that is not the current one they have to go to its own worker first (Photo Booth
 * crashed in glDeleteTextures run against another context when its live view switched
 * contexts). */
static inline void
thread_finish(struct gl_context *gl)
{
   if (!gl || !gl->GLThread.enabled)
      return;
   if (gl != _glapi_get_context())
      _mesa_glthread_flush_batch(gl);
   _mesa_glthread_finish(gl);
}

/* Making a context current leaves the calling thread with the table that queues calls. Inside
 * RIPPLE_CGL_DIRECT_BEGIN/END it has to be the one that runs them again. */
static void
direct_again(struct ripple_cgl_context *ctx)
{
   struct gl_context *gl = ctx ? ctx->glctx : NULL;

   if (gl && ctx->direct && gl->GLThread.enabled && _glapi_get_context() == gl) {
      _mesa_glthread_finish(gl);
      _glapi_set_dispatch(gl->Dispatch.Current);
   }
}

bool
ripple_cgl_context_make_current(struct ripple_cgl_context *ctx)
{
   /* What this thread had current, checked: Mesa flushes it on the way out, and binds the
    * new one's buffers, here on the calling thread. Both contexts' second GL threads have to
    * be done first. */
   struct ripple_cgl_context *prev = thread_current();
   thread_finish(prev ? prev->glctx : NULL);
   thread_finish(ctx ? ctx->glctx : NULL);
   /* Without a drawable the context is surfaceless and renders into FBOs only. */
   struct pipe_frontend_drawable *d = ctx && ctx->draw ? &ctx->draw->base : NULL;
   bool ok = st_api_make_current(ctx ? ctx->st : NULL, d, d);
   if (ok) {
      /* The thread notes. `prev` may have been destroyed from another thread meanwhile: it
       * is only touched if it is still in the list. */
      simple_mtx_lock(&live_lock);
      for (unsigned i = 0; prev && prev != ctx && i < live_used; i++)
         if (live[i] == prev && prev->thread == (void *)pthread_self())
            prev->thread = NULL;
      if (ctx)
         ctx->thread = (void *)pthread_self();
      simple_mtx_unlock(&live_lock);
   }
   if (d && (!ok || ctx->glctx->WinSysDrawBuffer == _mesa_get_incomplete_framebuffer()))
      log_unbound(ctx, ok);
   if (ok && ctx && unlikely(ctx->sync_gc))
      ripple_cgl_sync_from_glengine(ctx);
   direct_again(ctx);
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
   thread_finish(ctx->glctx);
   GLboolean initialized = ctx->glctx->ViewportInitialized;
   if (!d)
      ctx->glctx->ViewportInitialized = GL_TRUE;
   st_api_make_current(ctx->st, d ? d : r, r);
   if (!d)
      ctx->glctx->ViewportInitialized = initialized;
   direct_again(ctx);
}

/* From OpenGL.framework (CGLCurrent.h); declared here to keep Apple's GL headers out. */
extern void *CGLGetCurrentContext(void);

void
ripple_cgl_context_set_drawable(struct ripple_cgl_context *ctx, struct ripple_cgl_drawable *d)
{
   bool current = ripple_cgl_context_is_current(ctx);

   thread_finish(ctx->glctx);            /* it draws into the drawable the context had */
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
      if (!ctx || ctx->magic != RIPPLE_CGL_MAGIC) {
         /* Nothing of ours is current: Mesa's no-op dispatch answers, once a pointer this
          * thread may still hold to a context that is gone has been dropped. */
         thread_current();
         return;
      }
      if (ripple_cgl_context_is_current(ctx))
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

/* ---- The second GL thread (cgl_dispatch.h) ------------------------------------------------ */

int ripple_cgl_threaded;

void
ripple_cgl_quiesce(struct ripple_cgl_context *ctx)
{
   if (!ripple_cgl_threaded)
      return;
   struct ripple_cgl_context *cur = thread_current();   /* never a pointer that is gone */
   thread_finish(cur ? cur->glctx : NULL);
   thread_finish(ctx ? ctx->glctx : NULL);
}

/* RIPPLE_CGL_DIRECT_BEGIN and END, for the context current on the calling thread. */
void
ripple_cgl_direct_begin(void)
{
   struct gl_context *gl = _glapi_get_context();
   struct ripple_cgl_context *ctx = gl && gl->st ? gl->st->frontend_context : NULL;

   if (!ctx || !gl->GLThread.enabled)
      return;
   /* The queued calls first: they can change which table runs a call (glBegin, glNewList). */
   _mesa_glthread_finish(gl);
   if (ctx->direct++ == 0)
      _glapi_set_dispatch(gl->Dispatch.Current);
}

void
ripple_cgl_direct_end(void)
{
   struct gl_context *gl = _glapi_get_context();
   struct ripple_cgl_context *ctx = gl && gl->st ? gl->st->frontend_context : NULL;

   if (!ctx || !gl->GLThread.enabled || !ctx->direct)
      return;
   if (--ctx->direct == 0)
      _glapi_set_dispatch(gl->GLApi);
}
