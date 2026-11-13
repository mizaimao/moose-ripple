/*
 * Window drawables for the CGL frontend (moose-ripple, M4). A drawable is a set of render
 * targets on the GPU sized like the window (colour, front and back if double-buffered, plus
 * depth/stencil), modelled on OSMesa's buffers. Presenting a frame reads the colour buffer
 * back into guest memory, top row first, and hands it to the present callback the gld layer
 * set (which puts it into the window with Apple's window-server calls). Zero-copy
 * presentation comes with Quartz Extreme (M5).
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "frontend/api.h"
#include "main/mtypes.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "state_tracker/st_context.h"
#include "util/u_atomic.h"
#include "util/box.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "cgl_context.h"
#include "cgl_device.h"
#include "cgl_drawable.h"
#include "virgl/ripple/ripple_gpu_uapi.h"

static uint32_t drawable_ids;

static inline struct ripple_cgl_drawable *
drawable(struct pipe_frontend_drawable *base)
{
   return (struct ripple_cgl_drawable *)base;
}

static bool
drawable_validate(struct st_context *st, struct pipe_frontend_drawable *base,
                  const enum st_attachment_type *statts, unsigned count,
                  struct pipe_resource **out, struct pipe_resource **resolve)
{
   struct ripple_cgl_drawable *d = drawable(base);
   struct pipe_screen *screen = base->fscreen->screen;
   struct pipe_resource templat;

   memset(&templat, 0, sizeof(templat));
   templat.target = PIPE_TEXTURE_2D;
   templat.width0 = d->width ? d->width : 1;
   templat.height0 = d->height ? d->height : 1;
   templat.depth0 = 1;
   templat.array_size = 1;
   templat.nr_samples = d->visual.samples;
   templat.usage = PIPE_USAGE_DEFAULT;

   for (unsigned i = 0; i < count; i++) {
      enum st_attachment_type a = statts[i];
      struct pipe_resource *res = d->textures[a];

      enum pipe_texture_target target = d->rect && (a == ST_ATTACHMENT_FRONT_LEFT ||
                                                    a == ST_ATTACHMENT_BACK_LEFT)
                                           ? PIPE_TEXTURE_RECT : PIPE_TEXTURE_2D;
      if (res && res->width0 == templat.width0 && res->height0 == templat.height0 &&
          res->target == target) {
         pipe_resource_reference(&out[i], res);
         continue;
      }
      templat.target = target;
      switch (a) {
      case ST_ATTACHMENT_FRONT_LEFT:
      case ST_ATTACHMENT_BACK_LEFT:
         templat.format = d->visual.color_format;
         templat.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
         break;
      case ST_ATTACHMENT_DEPTH_STENCIL:
         templat.format = d->visual.depth_stencil_format;
         templat.bind = PIPE_BIND_DEPTH_STENCIL;
         break;
      case ST_ATTACHMENT_ACCUM:
         templat.format = d->visual.accum_format;
         templat.bind = PIPE_BIND_RENDER_TARGET;
         break;
      default:
         pipe_resource_reference(&out[i], NULL);
         continue;
      }
      pipe_resource_reference(&d->textures[a], NULL);
      d->textures[a] = screen->resource_create(screen, &templat);
      pipe_resource_reference(&out[i], d->textures[a]);
   }
   return true;
}

/* Debug log only (the bundle's log is on): every 5 s, how many frames were presented, the frame
 * rate while something moves (intervals over 100 ms are idle time and left out) and what a
 * present costs on the CPU here (flush and DISPLAY, or the read-back and copy). */
#include "util/os_time.h"
static void
present_stats(const char *how, int64_t t0)
{
   extern FILE *ripple_gld_dbg_file(void);
   static int64_t start, last, busy, moving;
   static unsigned n, intervals;
   FILE *f = ripple_gld_dbg_file();
   if (!f)
      return;
   int64_t now = os_time_get_nano();
   if (!start)
      start = t0;
   if (last && t0 - last < 100000000) {
      moving += t0 - last;
      intervals++;
   }
   last = t0;
   busy += now - t0;
   n++;
   if (now - start >= 5000000000LL) {
      fprintf(f, "stats: %s: %u presents in %.1f s, %.1f frames/s while moving, %.2f ms per "
              "present\n", how, n, (now - start) / 1e9,
              intervals && moving ? intervals * 1e9 / moving : 0.0, busy / 1e6 / n);
      start = now;
      n = intervals = 0;
      busy = moving = 0;
   }
}

/* Read `res` back into d->pixels (BGRA8, top row first) and present it. */
static void
present_resource(struct st_context *st, struct ripple_cgl_drawable *d,
                 struct pipe_resource *res)
{
   int64_t t0 = os_time_get_nano();
   struct pipe_context *pipe = st->pipe;
   struct pipe_transfer *transfer = NULL;
   struct pipe_box box;
   unsigned w = res->width0, h = res->height0, stride = w * 4;

   if (d->screen_region) {
      struct ripple_gpu_surface_info si;
      extern FILE *ripple_gld_dbg_file(void);
      static unsigned presents;
      if (!ripple_cgl_device_surface_info(d->region_surface, &si))
         return;
      /* Where the rectangle was drawn: WindowServer usually sets the viewport to its size
       * (glOrtho over just that rectangle: drawn at the viewport origin); with a full-screen
       * viewport it is drawn at its own screen position. */
      const struct gl_viewport_attrib *vp = &st->ctx->ViewportArray[0];
      int vx = (int)vp->X, vy = (int)vp->Y, vw = (int)vp->Width, vh = (int)vp->Height;
      int src_x = vx, src_y = vy;
      const char *how = "at the viewport";
      if (vw == (int)si.screen_width && vh == (int)si.screen_height &&
          (vw != (int)si.width || vh != (int)si.height)) {
         src_x = si.x;
         src_y = (int)si.screen_height - si.y - (int)si.height;
         how = "at its screen place";
      } else if (vw != (int)si.width || vh != (int)si.height) {
         how = "VIEWPORT MISMATCH";
      }
      if (ripple_gld_dbg_file() && (presents++ < 400 || how[0] == 'V'))
         fprintf(ripple_gld_dbg_file(), "present: seq %u, %d,%d %ux%u, viewport %d,%d %dx%d: %s\n",
                 si.shape_seq, si.x, si.y, si.width, si.height, vx, vy, vw, vh, how);
      ripple_cgl_device_present_region(st, res, src_x, src_y, si.x, si.y, si.width, si.height,
                                       si.screen_width, si.screen_height);
      return;
   }
   if (d->gpu_display) {
      st_context_flush(st, 0, NULL, NULL, NULL);
      ripple_cgl_device_display_dump(pipe, res, false);   /* debug switch, cgl_device.c */
      ripple_cgl_device_display(res, 0, 0, w, h);
      present_stats("display (GPU)", t0);
      return;
   }
   if (d->present_surface) {
      /* The frame stays on the GPU: the kernel copies it into the screen where the window's
       * surface is, as a hardware driver does at a swap. With a swap interval, not more often
       * than the display refreshes (nothing else holds the program back on this path). */
      if (d->present_vsync) {
         int64_t next = d->last_present + 16666667, now = os_time_get_nano();
         if (now < next)
            os_time_sleep((next - now) / 1000);
         d->last_present = now < next ? next : now;
      }
      st_context_flush(st, 0, NULL, NULL, NULL);
      bool shown = ripple_cgl_device_surface_present(d->present_surface, res, true);
      /* Not shown: the surface is not a plain region of the screen just now (the window is
       * moving, or in Exposé). The kernel has kept the frame for WindowServer, which reads it
       * when it draws the window: tell it there is something new. */
      if (!shown && d->present_refused)
         d->present_refused(d->present_refused_data);
      present_stats(shown ? "window (on the GPU)" : "window (on the GPU, not shown)", t0);
      return;
   }
   if (!d->present || !w || !h)
      return;
   if (d->pixels_size < (size_t)stride * h) {
      FREE(d->pixels);
      d->pixels_size = (size_t)stride * h;
      d->pixels = MALLOC(d->pixels_size);
      if (!d->pixels) {
         d->pixels_size = 0;
         return;
      }
   }
   u_box_2d(0, 0, w, h, &box);
   uint8_t *src = pipe->texture_map(pipe, res, 0, PIPE_MAP_READ, &box, &transfer);
   if (!src)
      return;
   /* GL's rows run bottom-up; windows want the top row first. */
   for (unsigned y = 0; y < h; y++)
      memcpy(d->pixels + (size_t)(h - 1 - y) * stride, src + (size_t)y * transfer->stride, stride);
   pipe->texture_unmap(pipe, transfer);
   d->present(d->present_data, d->pixels, stride, w, h);
   present_stats("window (read back)", t0);
}

/* Single-buffered drawing: Mesa calls this whenever it flushes a context whose front buffer
 * changed. That is the program's glFlush and glFinish, which show the drawable as on Apple's GL,
 * but also Mesa's own flush when another context becomes current, which must not: WindowServer
 * changes contexts in the middle of a frame (Core Image's), and each change put the half-drawn
 * screen on the display, so a widget being added flickered and went missing for a few frames
 * (2026-11-11). Only the entry points that mean "show it" set present_ok (cgl_apple.c,
 * ripple_cgl_context_swap); refused here, the buffer stays marked as changed for the next one. */
static bool
drawable_flush_front(struct st_context *st, struct pipe_frontend_drawable *base,
                     enum st_attachment_type statt)
{
   struct ripple_cgl_drawable *d = drawable(base);
   struct ripple_cgl_context *ctx = st->frontend_context;

   if (statt != ST_ATTACHMENT_FRONT_LEFT || !d->textures[statt])
      return false;
   if (!ctx || !ctx->present_ok)
      return false;
   present_resource(st, d, d->textures[statt]);
   ctx->presented = true;
   return true;
}

struct ripple_cgl_drawable *
ripple_cgl_drawable_create(const struct ripple_cgl_context *ctx)
{
   struct pipe_frontend_screen *fscreen = ripple_cgl_device_fscreen();
   struct ripple_cgl_drawable *d;

   if (!fscreen)
      return NULL;
   d = CALLOC_STRUCT(ripple_cgl_drawable);
   if (!d)
      return NULL;
   d->visual = ctx->visual;
   d->base.visual = &d->visual;
   d->base.fscreen = fscreen;
   d->base.validate = drawable_validate;
   d->base.flush_front = drawable_flush_front;
   d->base.ID = p_atomic_inc_return(&drawable_ids);
   p_atomic_set(&d->base.stamp, 1);
   return d;
}

void
ripple_cgl_drawable_set_size(struct ripple_cgl_drawable *d, unsigned width, unsigned height)
{
   if (d->width == width && d->height == height)
      return;
   d->width = width;
   d->height = height;
   p_atomic_inc(&d->base.stamp);     /* Mesa revalidates: new render targets */
}

void
ripple_cgl_drawable_set_present(struct ripple_cgl_drawable *d, ripple_cgl_present_fn fn,
                                void *data)
{
   d->present = fn;
   d->present_data = data;
}

void
ripple_cgl_drawable_set_surface_present(struct ripple_cgl_drawable *d, uint32_t surface,
                                        bool vsync, void (*refused)(void *data), void *data)
{
   d->present_surface = surface;
   d->present_vsync = vsync;
   d->present_refused = refused;
   d->present_refused_data = data;
}

void
ripple_cgl_drawable_set_screen_region(struct ripple_cgl_drawable *d, uint32_t surface)
{
   d->screen_region = surface != 0;
   d->region_surface = surface;
}

void
ripple_cgl_drawable_set_gpu_display(struct ripple_cgl_drawable *d, bool on)
{
   if (d->gpu_display && !on)
      ripple_cgl_device_display(NULL, 0, 0, 0, 0);    /* give the display back */
   d->gpu_display = on;
}

void
ripple_cgl_drawable_destroy(struct ripple_cgl_drawable *d)
{
   if (!d)
      return;
   if (d->gpu_display)
      ripple_cgl_device_display(NULL, 0, 0, 0, 0);
   st_api_destroy_drawable(&d->base);
   for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++)
      pipe_resource_reference(&d->textures[i], NULL);
   FREE(d->pixels);
   FREE(d);
}

/* With /Library/Logs/RippleGL/swapdebug present: the GL state at a context's first swaps and
 * the drawable's centre pixel read straight from it, in the debug log (M6: Real Racing 2's
 * Core Animation context swapped empty frames). */
#include "main/glthread.h"
#include "main/dispatch.h"
#include "main/fbobject.h"
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
static void
swap_debug(struct ripple_cgl_context *ctx)
{
   static int enabled = -1;
   static struct { void *ctx; unsigned n; } seen[8];
   extern FILE *ripple_gld_dbg_file(void);
   /* /Library/Logs/RippleGL/swapdump: each context's draw buffer as a PPM (top row first) at
    * its next swap, once per creation of that file (swap-PID-CONTEXT.ppm); for looking at a
    * frame before it is presented. */
   struct stat dst;
   static struct { void *ctx; time_t done, checked; } dumps[8];
   time_t now = time(NULL);
   unsigned di = 0;
   while (di < 7 && dumps[di].ctx && dumps[di].ctx != ctx)
      di++;
   dumps[di].ctx = ctx;
   bool check = now != dumps[di].checked;     /* at most once a second for each context */
   dumps[di].checked = now;
   if (check && stat("/Library/Logs/RippleGL/swapdump", &dst) == 0 &&
       dst.st_mtime != dumps[di].done) {
      dumps[di].done = dst.st_mtime;
      struct gl_context *g = ctx->glctx;
      unsigned w = g->DrawBuffer ? g->DrawBuffer->Width : 0, h = g->DrawBuffer ? g->DrawBuffer->Height : 0;
      uint8_t *px = w && h ? malloc((size_t)w * h * 4) : NULL;
      if (px) {
         struct gl_pixelstore_attrib saved = g->Pack;
         memset(&g->Pack, 0, sizeof(g->Pack));
         g->Pack.Alignment = 1;
         GLuint rfb = g->ReadBuffer ? g->ReadBuffer->Name : 0;
         GLenum rb = g->ReadBuffer ? g->ReadBuffer->ColorReadBuffer : GL_BACK;
         CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_READ_FRAMEBUFFER_EXT,
                                                  g->DrawBuffer ? g->DrawBuffer->Name : 0));
         CALL_ReadBuffer(GET_DISPATCH(), (g->DrawBuffer->ColorDrawBuffer[0]));
         CALL_ReadPixels(GET_DISPATCH(), (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px));
         CALL_ReadBuffer(GET_DISPATCH(), (rb));
         CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_READ_FRAMEBUFFER_EXT, rfb));
         g->Pack = saved;
         char path[128];
         snprintf(path, sizeof(path), "/Library/Logs/RippleGL/swap-%d-%p.ppm", (int)getpid(),
                  (void *)ctx);
         FILE *f = fopen(path, "wb");
         if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (unsigned y = 0; y < h; y++)
               for (unsigned x = 0; x < w; x++)
                  fwrite(px + ((size_t)(h - 1 - y) * w + x) * 4, 1, 3, f);
            fclose(f);
         }
         free(px);
      }
   }
   if (enabled < 0)
      enabled = access("/Library/Logs/RippleGL/swapdebug", F_OK) == 0;
   if (!enabled || !ripple_gld_dbg_file())
      return;
   unsigned i = 0;
   while (i < 8 && seen[i].ctx && seen[i].ctx != ctx)
      i++;
   if (i == 8)
      return;
   seen[i].ctx = ctx;
   unsigned n = seen[i].n++;
   if (n >= 6 && (n % 40 || n > 1200))
      return;
   struct gl_context *gl = ctx->glctx;
   GLint fb = gl->DrawBuffer ? (GLint)gl->DrawBuffer->Name : -1;
   GLint rfb = gl->ReadBuffer ? (GLint)gl->ReadBuffer->Name : -1;
   GLenum db = gl->DrawBuffer ? gl->DrawBuffer->ColorDrawBuffer[0] : 0;
   unsigned w = gl->DrawBuffer ? gl->DrawBuffer->Width : 0, h = gl->DrawBuffer ? gl->DrawBuffer->Height : 0;
   GLubyte px[4] = { 0 };
   if (fb == 0 && rfb == 0 && w && h) {
      struct gl_pixelstore_attrib saved = gl->Pack;
      memset(&gl->Pack, 0, sizeof(gl->Pack));
      gl->Pack.Alignment = 1;
      GLenum rb = gl->ReadBuffer->ColorReadBuffer;
      CALL_ReadBuffer(GET_DISPATCH(), (db));
      CALL_ReadPixels(GET_DISPATCH(), (w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px));
      CALL_ReadBuffer(GET_DISPATCH(), (rb));
      gl->Pack = saved;
   }
   fprintf(ripple_gld_dbg_file(), "RippleGLDriver: swap %p: draw fb %d read fb %d buffer 0x%x "
           "%ux%u, viewport %g,%g %gx%g, program %u, centre RGBA %u %u %u %u, error 0x%x\n",
           (void *)ctx, fb, rfb, db, w, h, gl->ViewportArray[0].X, gl->ViewportArray[0].Y,
           gl->ViewportArray[0].Width, gl->ViewportArray[0].Height,
           gl->Shader.ActiveProgram ? gl->Shader.ActiveProgram->Name : 0, px[0], px[1], px[2],
           px[3], gl->ErrorValue);
   fprintf(ripple_gld_dbg_file(), "RippleGLDriver:   state: scissor %d (%d,%d %dx%d), stencil %d, "
           "depth %d, alpha test %d (func 0x%x ref %g), blend %d, colour mask 0x%x, cull %d, "
           "draw buffer 0x%x, polygon offset %d, logic op %d\n",
           gl->Scissor.EnableFlags & 1, gl->Scissor.ScissorArray[0].X, gl->Scissor.ScissorArray[0].Y,
           gl->Scissor.ScissorArray[0].Width, gl->Scissor.ScissorArray[0].Height,
           gl->Stencil.Enabled, gl->Depth.Test, gl->Color.AlphaEnabled, gl->Color.AlphaFunc,
           gl->Color.AlphaRefUnclamped, gl->Color.BlendEnabled & 1, gl->Color.ColorMask & 0xf,
           gl->Polygon.CullFlag, gl->DrawBuffer ? gl->DrawBuffer->ColorDrawBuffer[0] : 0,
           gl->Polygon.OffsetFill, gl->Color.ColorLogicOpEnabled);
   /* Whether the draw framebuffer is complete (an incomplete one turns clears and draws into
    * no-ops with only a GL error), and what each attachment is. The completeness test is for
    * framebuffer objects only: on a window framebuffer it looks at the object attachment
    * points, finds none unless there is a depth buffer, and leaves the framebuffer 0x0 and
    * incomplete. This log did exactly that to Core Animation's depth-less context in Real
    * Racing 2 and then reported the damage as the fault (2026-11-11). */
   if (gl->DrawBuffer) {
      struct gl_framebuffer *fbo = gl->DrawBuffer;
      if (_mesa_is_user_fbo(fbo))
         _mesa_test_framebuffer_completeness(gl, fbo);
      fprintf(ripple_gld_dbg_file(), "RippleGLDriver:   framebuffer %u status 0x%x %ux%u, visual "
              "depth %d stencil %d samples %d:", fbo->Name, fbo->_Status, fbo->Width, fbo->Height,
              fbo->Visual.depthBits, fbo->Visual.stencilBits, fbo->Visual.samples);
      for (unsigned b = 0; b < BUFFER_COUNT; b++) {
         struct gl_renderbuffer *rb = fbo->Attachment[b].Renderbuffer;
         if (rb)
            fprintf(ripple_gld_dbg_file(), " [%u: %ux%u fmt %d samples %u]", b, rb->Width,
                    rb->Height, rb->Format, rb->NumSamples);
      }
      fputc('\n', ripple_gld_dbg_file());
   }
   extern void ripple_cgl_cs_debug(struct ripple_cgl_context *ctx, FILE *f);   /* cgl_apple.c */
   ripple_cgl_cs_debug(ctx, ripple_gld_dbg_file());
   struct ripple_cgl_drawable *dd = ctx->draw;
   if (dd) {
      fprintf(ripple_gld_dbg_file(), "RippleGLDriver:   drawable %p (id %u, stamp %d) %ux%u rect %d, "
              "visual buffers 0x%x depth format %d:", (void *)dd, dd->base.ID,
              p_atomic_read(&dd->base.stamp), dd->width, dd->height, dd->rect,
              dd->visual.buffer_mask, dd->visual.depth_stencil_format);
      for (unsigned a = 0; a < ST_ATTACHMENT_COUNT; a++)
         if (dd->textures[a])
            fprintf(ripple_gld_dbg_file(), " [%u: %ux%u target %d format %d]", a,
                    dd->textures[a]->width0, dd->textures[a]->height0, dd->textures[a]->target,
                    dd->textures[a]->format);
      fputc('\n', ripple_gld_dbg_file());
   } else {
      fprintf(ripple_gld_dbg_file(), "RippleGLDriver:   no drawable\n");
   }
}

void
ripple_cgl_context_swap(struct ripple_cgl_context *ctx)
{
   struct ripple_cgl_drawable *d = ctx->draw;

   if (!d)
      return;
   if (_glapi_get_context() != ctx->glctx)
      ripple_cgl_context_make_current(ctx);
   swap_debug(ctx);
   ctx->present_ok = true;
   ctx->presented = false;
   st_context_flush(ctx->st, ST_FLUSH_FRONT, NULL, NULL, NULL);
   ctx->present_ok = false;
   struct pipe_resource *res = d->textures[ST_ATTACHMENT_BACK_LEFT];
   if (!res)
      res = d->textures[ST_ATTACHMENT_FRONT_LEFT];
   /* Not a second time if the flush has just shown that same front buffer (single-buffered). */
   if (res && !(ctx->presented && res == d->textures[ST_ATTACHMENT_FRONT_LEFT]))
      present_resource(ctx->st, d, res);
   /* Showing the back buffer itself: flip, so the next frame isn't drawn into the texture on
    * screen. Mesa picks up the swapped textures through the stamp. The same for a window the
    * kernel shows: it keeps the frame for WindowServer to read, which must not catch the next
    * one half drawn. */
   if ((d->gpu_display || d->present_surface) && d->textures[ST_ATTACHMENT_BACK_LEFT] &&
       d->textures[ST_ATTACHMENT_FRONT_LEFT]) {
      struct pipe_resource *t = d->textures[ST_ATTACHMENT_BACK_LEFT];
      d->textures[ST_ATTACHMENT_BACK_LEFT] = d->textures[ST_ATTACHMENT_FRONT_LEFT];
      d->textures[ST_ATTACHMENT_FRONT_LEFT] = t;
      p_atomic_inc(&d->base.stamp);
   }
}

struct pipe_resource *
ripple_cgl_drawable_color(struct ripple_cgl_drawable *d, unsigned source)
{
   struct pipe_resource *front = d->textures[ST_ATTACHMENT_FRONT_LEFT];
   struct pipe_resource *back = d->textures[ST_ATTACHMENT_BACK_LEFT];
   if (source == 0x0404 /* GL_FRONT */ || source == 0x0400 /* GL_FRONT_LEFT */)
      return front ? front : back;
   return back ? back : front;
}

void
ripple_cgl_drawable_set_rect(struct ripple_cgl_drawable *d, bool rect)
{
   if (d->rect == rect)
      return;
   d->rect = rect;
   p_atomic_inc(&d->base.stamp);     /* Mesa revalidates: buffers of the new kind */
}
