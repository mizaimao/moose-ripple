/*
 * Showing GL frames in windows (moose-ripple, M4), the way Apple's software renderer does it
 * (docs/re/gl-plugin-interface.md, "Window drawables"): for each (connection, window,
 * surface) a context is attached to, make a private window-server window bound to that
 * surface and keep its bits locked; a frame is copied into the bits, then the surface is
 * flushed. No kernel support needed. Zero-copy presentation through IOAccelSurface is M5.
 *
 * The CGS functions are private CoreGraphics calls, looked up at run time (prototypes as
 * read from 10.6.8's binaries). Missing ones make attaching fail cleanly.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/simple_mtx.h"

#include "gld_window.h"

#if defined(__LP64__)
typedef double cg_float;
#else
typedef float cg_float;
#endif
typedef struct { cg_float x, y, w, h; } cg_rect;
typedef int cgs_conn, cgs_window, cgs_surface;
typedef void *cgs_region;

static struct {
   int (*GetSurfaceBounds)(cgs_conn, cgs_window, cgs_surface, cg_rect *);
   int (*NewRegionWithRect)(const cg_rect *, cgs_region *);
   int (*ReleaseRegion)(cgs_region);
   int (*NewWindow)(cgs_conn, int, cg_float, cg_float, cgs_region, cgs_window *);
   int (*SetWindowDepth)(cgs_conn, cgs_window, int);
   int (*BindSurface)(cgs_conn, cgs_window, cgs_surface, int, int, cgs_window);
   /* The last two point to small structs: {void *base; ...} (16 bytes on x86_64) and
    * {int32 rowBytes; int32 ...}; lock_bits() passes roomy buffers. */
   int (*LockWindowBits)(cgs_conn, cgs_window, cg_rect *, int *, void *, void *);
   int (*UnlockWindowBits)(cgs_conn, cgs_window, cgs_region);
   int (*FlushSurfaceWithOptions)(cgs_conn, cgs_window, cgs_surface, cgs_region, int);
   int (*SetWindowShape)(cgs_conn, cgs_window, cg_float, cg_float, cgs_region);
   int (*ReleaseWindow)(cgs_conn, cgs_window);
} cgs;
static bool cgs_ok;

#define CGS_DEPTH_32 4         /* CGSSetWindowDepth / CGSLockWindowBits format code */

FILE *ripple_gld_dbg_file(void);   /* ripple_gl_target.c */
#define DBG(...) do { FILE *df_ = ripple_gld_dbg_file(); if (df_) fprintf(df_, "RippleGLDriver: " __VA_ARGS__); } while (0)

__attribute__((constructor)) static void
gld_window_init(void)
{
#define LOOK(name) (*(void **)&cgs.name = dlsym(RTLD_DEFAULT, "CGS" #name))
   cgs_ok = LOOK(GetSurfaceBounds) && LOOK(NewRegionWithRect) && LOOK(ReleaseRegion) &&
            LOOK(NewWindow) && LOOK(SetWindowDepth) && LOOK(BindSurface) &&
            LOOK(LockWindowBits) && LOOK(UnlockWindowBits) &&
            LOOK(FlushSurfaceWithOptions) && LOOK(SetWindowShape) && LOOK(ReleaseWindow);
#undef LOOK
}

struct gld_window {
   uint32_t cid, wid, sid;
   cgs_window priv;            /* our private window, bound to the surface */
   cgs_region rgn;
   void *base;                 /* locked bits */
   int row_bytes;
   unsigned width, height;
   int refs;
   bool vsync;
   bool swap_rect_on;          /* kCGLCESwapRectangle: only swap_rect reaches the window */
   int swap_rect[4];           /* x, y (GL: from the bottom), width, height */
   struct gld_window *next;
};

static simple_mtx_t windows_lock = SIMPLE_MTX_INITIALIZER;
static struct gld_window *windows;

static bool
lock_bits(struct gld_window *w)
{
   cg_rect r;
   int fmt = 0;
   union { void *base; uint64_t raw[4]; } bits;
   union { int32_t row_bytes; uint64_t raw[2]; } row;

   memset(&bits, 0, sizeof(bits));
   memset(&row, 0, sizeof(row));
   w->base = NULL;
   int e = cgs.LockWindowBits(w->cid, w->priv, &r, &fmt, &bits, &row);
   w->base = bits.base;
   w->row_bytes = row.row_bytes;
   if (e || !w->base || w->row_bytes <= 0 || fmt != CGS_DEPTH_32) {
      fprintf(stderr, "RippleGLDriver: CGSLockWindowBits failed (format %d)\n", fmt);
      w->base = NULL;
      return false;
   }
   return true;
}

static void
destroy(struct gld_window *w)
{
   if (w->base)
      cgs.UnlockWindowBits(w->cid, w->priv, NULL);
   if (w->rgn)
      cgs.ReleaseRegion(w->rgn);
   if (w->priv)
      cgs.ReleaseWindow(w->cid, w->priv);
   free(w);
}

/* (Re)size, bind and lock: the first attach and every attach after it (CGLUpdateContext). */
static int
setup(struct gld_window *w, bool fresh, bool *changed)
{
   cg_rect bounds;

   int e = cgs.GetSurfaceBounds(w->cid, w->wid, w->sid, &bounds);
   DBG("CGSGetSurfaceBounds(%u, %u, %u) = %d: %g %g %g %g\n", w->cid, w->wid, w->sid, e,
       (double)bounds.x, (double)bounds.y, (double)bounds.w, (double)bounds.h);
   if (e)
      return -1;
   unsigned width = bounds.w > 0 ? (unsigned)bounds.w : 0;
   unsigned height = bounds.h > 0 ? (unsigned)bounds.h : 0;
   cg_rect shape = { 0, 0, bounds.w, bounds.h };

   *changed = fresh || width != w->width || height != w->height;
   if (w->base) {
      cgs.UnlockWindowBits(w->cid, w->priv, NULL);
      w->base = NULL;
   }
   if (fresh) {
      int e1 = cgs.NewRegionWithRect(&shape, &w->rgn);
      int e2 = e1 ? -1 : cgs.NewWindow(w->cid, 2, 0, 0, w->rgn, &w->priv);
      int e3 = e2 ? -1 : cgs.SetWindowDepth(w->cid, w->priv, CGS_DEPTH_32);
      DBG("region %d, CGSNewWindow %d -> %d, depth %d\n", e1, e2, w->priv, e3);
      if (e1 || e2 || e3)
         return -1;
   } else if (*changed) {
      cgs.ReleaseRegion(w->rgn);
      w->rgn = NULL;
      if (cgs.NewRegionWithRect(&shape, &w->rgn) ||
          cgs.SetWindowShape(w->cid, w->priv, 0, 0, w->rgn))
         return -1;
   }
   e = cgs.BindSurface(w->cid, w->wid, w->sid, 1, 0, w->priv);
   DBG("CGSBindSurface = %d\n", e);
   if (e || !lock_bits(w))
      return -1;
   DBG("bits %p, row bytes %d\n", w->base, w->row_bytes);
   w->width = width;
   w->height = height;
   return 0;
}

struct gld_window *
gld_window_attach(const uint32_t ids[3], struct gld_window *current, bool *changed)
{
   struct gld_window *w;

   *changed = false;
   if (!cgs_ok || !ids || !ids[0] || !ids[1] || !ids[2])
      return NULL;
   simple_mtx_lock(&windows_lock);
   for (w = windows; w; w = w->next)
      if (w->cid == ids[0] && w->wid == ids[1] && w->sid == ids[2])
         break;
   if (w) {
      if (w != current)
         w->refs++;
      if (setup(w, false, changed)) {
         fprintf(stderr, "RippleGLDriver: re-attaching window %u failed\n", ids[1]);
         w->base = NULL;
      }
   } else {
      w = calloc(1, sizeof(*w));
      if (w) {
         w->cid = ids[0];
         w->wid = ids[1];
         w->sid = ids[2];
         w->refs = 1;
         if (setup(w, true, changed)) {
            fprintf(stderr, "RippleGLDriver: attaching window %u failed\n", ids[1]);
            destroy(w);
            w = NULL;
         } else {
            w->next = windows;
            windows = w;
         }
      }
   }
   simple_mtx_unlock(&windows_lock);
   return w;
}

void
gld_window_release(struct gld_window *w)
{
   if (!w)
      return;
   simple_mtx_lock(&windows_lock);
   if (--w->refs == 0) {
      for (struct gld_window **p = &windows; *p; p = &(*p)->next) {
         if (*p == w) {
            *p = w->next;
            break;
         }
      }
      destroy(w);
   }
   simple_mtx_unlock(&windows_lock);
}

void
gld_window_size(const struct gld_window *w, unsigned *width, unsigned *height)
{
   *width = w ? w->width : 0;
   *height = w ? w->height : 0;
}

void
gld_window_set_vsync(struct gld_window *w, bool vsync)
{
   if (w)
      w->vsync = vsync;
}

void
gld_window_set_swap_rect(struct gld_window *w, bool on, const int rect[4])
{
   if (!w)
      return;
   w->swap_rect_on = on;
   memcpy(w->swap_rect, rect, sizeof(w->swap_rect));
}

/* ripple_cgl_present_fn: pixels are BGRA8, top row first. */
void
gld_window_present(void *data, const uint8_t *pixels, unsigned stride, unsigned width,
                   unsigned height)
{
   struct gld_window *w = data;

   simple_mtx_lock(&windows_lock);
   static int presents;
   if (presents < 3 || presents % 120 == 0) {
      const uint8_t *mid = pixels + (size_t)(height / 2) * stride + (width / 2) * 4;
      /* Over a 16x16 grid: the brightest colour channel and the alpha range (a picture with
       * alpha 0 shows as nothing; M6). */
      unsigned cmax = 0, amin = 255, amax = 0;
      for (unsigned gy = 0; gy < 16 && height; gy++)
         for (unsigned gx = 0; gx < 16 && width; gx++) {
            const uint8_t *p = pixels + (size_t)(gy * height / 16) * stride + (gx * width / 16) * 4;
            for (int c = 0; c < 3; c++)
               cmax = p[c] > cmax ? p[c] : cmax;
            amin = p[3] < amin ? p[3] : amin;
            amax = p[3] > amax ? p[3] : amax;
         }
      DBG("present %d: %ux%u into %p (%ux%u, row bytes %d), first pixel %02x %02x %02x %02x, "
          "centre %02x %02x %02x %02x (BGRA); grid: brightest %u, alpha %u-%u\n", presents,
          width, height, w->base, w->width, w->height, w->row_bytes, pixels[0], pixels[1],
          pixels[2], pixels[3], mid[0], mid[1], mid[2], mid[3], cmax, amin, amax);
   }
   presents++;
   if (w->base) {
      /* WindowServer reads a GL surface's memory bottom row first (GL's order): M4 and M5
       * showed every GL window upside down until 2026-11-10 (gl-gears' blue gear at the
       * bottom, Front Row's menu mirrored). `pixels` is top row first. */
      unsigned rows = height < w->height ? height : w->height;
      unsigned cols = width < w->width ? width : w->width;
      /* Memory row y is GL row y (bottom first); with a swap rectangle only its rows and
       * columns are written. */
      unsigned y0 = 0, y1 = rows, x0 = 0, x1 = cols;
      if (w->swap_rect_on && (w->swap_rect[2] || w->swap_rect[3] || w->swap_rect[0] || w->swap_rect[1])) {
         int rx = w->swap_rect[0], ry = w->swap_rect[1];
         int rw = w->swap_rect[2], rh = w->swap_rect[3];
         x0 = rx < 0 ? 0 : (unsigned)rx > cols ? cols : (unsigned)rx;
         y0 = ry < 0 ? 0 : (unsigned)ry > rows ? rows : (unsigned)ry;
         x1 = rw <= 0 ? x0 : (unsigned)(rx + rw) > cols ? cols : (unsigned)(rx + rw);
         y1 = rh <= 0 ? y0 : (unsigned)(ry + rh) > rows ? rows : (unsigned)(ry + rh);
      }
      for (unsigned y = y0; y < y1 && x1 > x0; y++)
         memcpy((uint8_t *)w->base + (size_t)y * w->row_bytes + x0 * 4,
                pixels + (size_t)(height - 1 - y) * stride + x0 * 4, (x1 - x0) * 4);
      cgs.UnlockWindowBits(w->cid, w->priv, NULL);
      w->base = NULL;
      int fe = cgs.FlushSurfaceWithOptions(w->cid, w->wid, w->sid, NULL, w->vsync);
      if (presents < 4)
         DBG("CGSFlushSurfaceWithOptions = %d\n", fe);
      lock_bits(w);
   }
   simple_mtx_unlock(&windows_lock);
}
