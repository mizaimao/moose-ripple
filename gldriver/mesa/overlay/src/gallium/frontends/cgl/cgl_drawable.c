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

      if (res && res->width0 == templat.width0 && res->height0 == templat.height0) {
         pipe_resource_reference(&out[i], res);
         continue;
      }
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
      ripple_cgl_device_display(res, 0, 0, w, h);
      present_stats("display (GPU)", t0);
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

/* Single-buffered drawing: Mesa calls this on glFlush/glFinish when the front buffer changed. */
static bool
drawable_flush_front(struct st_context *st, struct pipe_frontend_drawable *base,
                     enum st_attachment_type statt)
{
   struct ripple_cgl_drawable *d = drawable(base);

   if (statt != ST_ATTACHMENT_FRONT_LEFT || !d->textures[statt])
      return false;
   present_resource(st, d, d->textures[statt]);
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

void
ripple_cgl_context_swap(struct ripple_cgl_context *ctx)
{
   struct ripple_cgl_drawable *d = ctx->draw;

   if (!d)
      return;
   if (_glapi_get_context() != ctx->glctx)
      ripple_cgl_context_make_current(ctx);
   st_context_flush(ctx->st, ST_FLUSH_FRONT, NULL, NULL, NULL);
   struct pipe_resource *res = d->textures[ST_ATTACHMENT_BACK_LEFT];
   if (!res)
      res = d->textures[ST_ATTACHMENT_FRONT_LEFT];
   if (res)
      present_resource(ctx->st, d, res);
   /* Showing the back buffer itself: flip, so the next frame isn't drawn into the texture on
    * screen. Mesa picks up the swapped textures through the stamp. */
   if (d->gpu_display && d->textures[ST_ATTACHMENT_BACK_LEFT] && d->textures[ST_ATTACHMENT_FRONT_LEFT]) {
      struct pipe_resource *t = d->textures[ST_ATTACHMENT_BACK_LEFT];
      d->textures[ST_ATTACHMENT_BACK_LEFT] = d->textures[ST_ATTACHMENT_FRONT_LEFT];
      d->textures[ST_ATTACHMENT_FRONT_LEFT] = t;
      p_atomic_inc(&d->base.stamp);
   }
}
