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
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

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
   /* Optional (attaching works without them). The colour space is a CGColorSpaceRef. */
   int (*SetWindowOpacity)(cgs_conn, cgs_window, int);
   int (*CopySurfaceColorSpace)(cgs_conn, cgs_window, cgs_surface, void **);
   int (*SetWindowColorSpace)(cgs_conn, cgs_window, void *);
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
   LOOK(SetWindowOpacity);
   LOOK(CopySurfaceColorSpace);
   LOOK(SetWindowColorSpace);
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
   bool translucent;           /* the surface is not opaque, and our window was told so */
   bool swap_rect_on;          /* kCGLCESwapRectangle: only swap_rect reaches the window */
   int swap_rect[4];           /* x, y (GL: from the bottom), width, height */
   void *user;                 /* the caller's per-surface data (its render targets) */
   void (*user_free)(void *);
   unsigned long last_use;     /* for dropping idle records, oldest first */
   long dumped;                /* presentdump: that file's time when this surface was last dumped */
   struct gld_window *next;
};

/* Records no context is attached to are kept (see gld_window_release), up to this many. */
#define GLD_MAX_IDLE_WINDOWS 32
static unsigned long window_clock;

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

/* Our private window is what WindowServer composites for the surface, and a new window is
 * opaque: a surface the app made non-opaque (Core Animation does, for a layer-backed view
 * with transparent parts) was drawn without blending, its transparent parts black. The
 * Finder's toolbar animation showed a black frame around the window, and Photo Booth's round
 * shutter button a black square (2026-11-11). So the window follows the surface
 * (gld_opacity.c knows which surfaces are non-opaque). With the window non-opaque
 * WindowServer goes by the surface's own setting, but it costs a third of gl-gears' frame
 * rate, so it is only done for surfaces that need it.
 *
 * A non-opaque window's pixels are colour-matched from the window's own colour space, and a
 * new window's default is not the display's (pure green came out as 46 255 4), so it also
 * gets the surface's colour space (AppKit gives a view's surface the window's).
 *
 * Call with the bits unlocked: the window-server calls wait for them. */
static void
sync_opacity(struct gld_window *w)
{
   bool translucent = gld_surface_translucent(w->cid, w->wid, w->sid);

   if (translucent == w->translucent || !cgs.SetWindowOpacity || !w->priv)
      return;
   if (cgs.SetWindowOpacity(w->cid, w->priv, !translucent))
      return;
   w->translucent = translucent;
   DBG("surface %u of window %u is %s: private window %d set to match\n", w->sid, w->wid,
       translucent ? "not opaque" : "opaque", w->priv);
   if (translucent && cgs.CopySurfaceColorSpace && cgs.SetWindowColorSpace) {
      void *space = NULL;
      if (cgs.CopySurfaceColorSpace(w->cid, w->wid, w->sid, &space) == 0 && space) {
         cgs.SetWindowColorSpace(w->cid, w->priv, space);
         CFRelease(space);
      }
   }
}

static void
destroy(struct gld_window *w)
{
   if (w->user && w->user_free)
      w->user_free(w->user);
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
   if (e)
      return -1;
   sync_opacity(w);                /* while the bits are not locked: it waits for them */
   if (!lock_bits(w))
      return -1;
   DBG("bits %p, row bytes %d\n", w->base, w->row_bytes);
   w->width = width;
   w->height = height;
   return 0;
}

static void
unlink_window(struct gld_window *w)
{
   for (struct gld_window **p = &windows; *p; p = &(*p)->next) {
      if (*p == w) {
         *p = w->next;
         break;
      }
   }
}

/* Drop idle records whose surface is gone, and the oldest ones beyond the limit. Hold
 * windows_lock. */
static void
sweep_idle(void)
{
   unsigned idle = 0;
   struct gld_window *next;

   for (struct gld_window *w = windows; w; w = next) {
      cg_rect r;
      next = w->next;
      if (w->refs)
         continue;
      if (cgs.GetSurfaceBounds(w->cid, w->wid, w->sid, &r)) {
         unlink_window(w);
         destroy(w);
      } else {
         idle++;
      }
   }
   while (idle > GLD_MAX_IDLE_WINDOWS) {
      struct gld_window *oldest = NULL;
      for (struct gld_window *w = windows; w; w = w->next)
         if (!w->refs && (!oldest || w->last_use < oldest->last_use))
            oldest = w;
      if (!oldest)
         break;
      unlink_window(oldest);
      destroy(oldest);
      idle--;
   }
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
      w->last_use = ++window_clock;
      if (setup(w, false, changed)) {
         fprintf(stderr, "RippleGLDriver: re-attaching window %u failed\n", ids[1]);
         w->base = NULL;
      }
   } else {
      sweep_idle();
      w = calloc(1, sizeof(*w));
      if (w) {
         w->cid = ids[0];
         w->wid = ids[1];
         w->sid = ids[2];
         w->refs = 1;
         w->last_use = ++window_clock;
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
      /* Kept: our private window holds what the surface shows. Core Animation draws all of
       * an app's layer-backed views with one context, attached to each view's surface in
       * turn; when the record went with the context, every surface but the last attached lost
       * its picture (Photo Booth: the camera view, the shutter button and the photo strip all
       * white; 2026-11-11). The record goes when its surface does (sweep_idle). */
      if (w->base) {
         cgs.UnlockWindowBits(w->cid, w->priv, NULL);
         w->base = NULL;
      }
      w->last_use = ++window_clock;
   }
   simple_mtx_unlock(&windows_lock);
}

void *
gld_window_user(const struct gld_window *w)
{
   return w ? w->user : NULL;
}

void
gld_window_set_user(struct gld_window *w, void *user, void (*user_free)(void *))
{
   if (!w)
      return;
   w->user = user;
   w->user_free = user_free;
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

static double
ms_since(uint64_t t0)
{
   static mach_timebase_info_data_t tb;
   if (!tb.denom)
      mach_timebase_info(&tb);
   return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

/* ripple_cgl_present_fn: pixels are BGRA8, bottom row first (see below). */
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
   /* Debug: with /Library/Logs/RippleGL/presentdump, the next frame handed to each surface as
    * present-PID-SURFACE.ppm (top row first), once per change of that file: what the driver
    * gives the window, to tell a wrong frame from a wrong showing (M6, Real Racing 2). */
   {
      static time_t checked;
      static long stamp;
      time_t now = time(NULL);
      if (now != checked) {
         struct stat sb;
         checked = now;
         stamp = stat("/Library/Logs/RippleGL/presentdump", &sb) == 0 ? (long)sb.st_mtime : 0;
      }
      if (stamp && w->dumped != stamp && width && height) {
         char path[160];
         w->dumped = stamp;
         snprintf(path, sizeof(path), "/Library/Logs/RippleGL/present-%d-%u.ppm", (int)getpid(), w->sid);
         FILE *f = fopen(path, "wb");
         if (f) {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (unsigned y = 0; y < height; y++) {
               const uint8_t *row = pixels + (size_t)(height - 1 - y) * stride;
               for (unsigned x = 0; x < width; x++) {
                  uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] };
                  fwrite(rgb, 1, 3, f);
               }
            }
            fclose(f);
         }
      }
   }
   if (w->base) {
      /* `pixels` arrives bottom row first (Mesa keeps a window's rows top first and
       * present_resource turns them over; its "top row first" is not what it does), and the
       * window's memory wants the top row first, so memory row y is pixels row height-1-y.
       * Getting this wrong showed every GL window upside down until 2026-11-10. */
      unsigned rows = height < w->height ? height : w->height;
      unsigned cols = width < w->width ? width : w->width;
      unsigned y0 = 0, y1 = rows, x0 = 0, x1 = cols;
      if (w->swap_rect_on && (w->swap_rect[2] || w->swap_rect[3] || w->swap_rect[0] || w->swap_rect[1])) {
         /* The swap rectangle is in GL window coordinates, y from the bottom; memory rows
          * count from the top. Until 2026-11-11 the mirrored band was written: a partial
          * update by Core Animation went to the wrong rows (swaprect-check against Apple's
          * software renderer). */
         long rx = w->swap_rect[0], ry = w->swap_rect[1];
         long rw = w->swap_rect[2] > 0 ? w->swap_rect[2] : 0;
         long rh = w->swap_rect[3] > 0 ? w->swap_rect[3] : 0;
         long left = rx, right = rx + rw;
         long top = (long)height - (ry + rh), bottom = (long)height - ry;
         x0 = left < 0 ? 0 : left > (long)cols ? cols : (unsigned)left;
         x1 = right < (long)x0 ? x0 : right > (long)cols ? cols : (unsigned)right;
         y0 = top < 0 ? 0 : top > (long)rows ? rows : (unsigned)top;
         y1 = bottom < (long)y0 ? y0 : bottom > (long)rows ? rows : (unsigned)bottom;
      }
      for (unsigned y = y0; y < y1 && x1 > x0; y++)
         memcpy((uint8_t *)w->base + (size_t)y * w->row_bytes + x0 * 4,
                pixels + (size_t)(height - 1 - y) * stride + x0 * 4, (x1 - x0) * 4);
      /* The three window-server calls are timed: a present that takes over 60 ms in them is
       * logged (a frozen GL window; performance pass, 2026-11-12). */
      uint64_t t0 = mach_absolute_time();
      cgs.UnlockWindowBits(w->cid, w->priv, NULL);
      double unlock_ms = ms_since(t0);
      w->base = NULL;
      sync_opacity(w);
      t0 = mach_absolute_time();
      int fe = cgs.FlushSurfaceWithOptions(w->cid, w->wid, w->sid, NULL, w->vsync);
      double flush_ms = ms_since(t0);
      if (presents < 4)
         DBG("CGSFlushSurfaceWithOptions = %d\n", fe);
      t0 = mach_absolute_time();
      lock_bits(w);
      double lock_ms = ms_since(t0);
      if (unlock_ms + flush_ms + lock_ms > 60)
         DBG("slow present %d: unlock %.0f ms, flush %.0f ms, lock %.0f ms\n", presents, unlock_ms,
             flush_ms, lock_ms);
   }
   simple_mtx_unlock(&windows_lock);
}
