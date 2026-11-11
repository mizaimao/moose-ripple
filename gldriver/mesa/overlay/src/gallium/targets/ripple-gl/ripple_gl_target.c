/*
 * RippleGLDriver: the 80 gld* entry points Apple's OpenGL (libGFXShared, GLEngine) calls in a
 * renderer plugin (moose-ripple, M4). Interface as read from 10.6.8's binaries:
 * docs/re/gl-plugin-interface.md in the moose-ripple repo.
 *
 * Design (decision 0003): GLEngine still manages renderers, pixel formats, contexts and
 * drawables through these calls, so they hand it valid answers and placeholder objects; the
 * GL calls themselves go to Mesa, because once a context is complete (gldSetInteger 0x2bd)
 * its public dispatch table is replaced with ours (frontends/cgl/cgl_takeover.c).
 *
 * Apple's structures are declared here from those notes (they are in no SDK header).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <OpenGL/CGLTypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/simple_mtx.h"
#include "virgl/ripple/ripple_gpu_uapi.h"

#include "cgl/cgl_context.h"
#include "cgl/cgl_device.h"
#include "cgl/cgl_drawable.h"
#include "cgl/cgl_dispatch.h"
#include "cgl/cgl_device.h"
#include "cgl/cgl_takeover.h"
#include "gld_window.h"

/* The gld* functions are entry points Apple's OpenGL finds by name; no header declares them. */
#pragma clang diagnostic ignored "-Wmissing-prototypes"

#define GLD_EXPORT __attribute__((visibility("default")))

/* Our renderer ID: unused by Apple's drivers, (id & 0xffff00ff) == 0, not the float
 * renderer's 0x400. Renderer info and pixel formats carry 0x01000000 | id. */
#define RIPPLE_RENDERER_ID      0x5000u
#define RIPPLE_RENDERER_ID_FULL (0x01000000u | RIPPLE_RENDERER_ID)
#define RIPPLE_VRAM_MB          512

/* gldGetRendererInfo's record (0x88 bytes on x86_64, 0x84 on i386). */
struct gld_renderer_info {
   void *next;                   /* set by GLEngine */
   uint32_t renderer_id;
   uint32_t flags;               /* GLD_FLAG_* */
   uint32_t buffer_modes;
   uint32_t color_modes;
   uint32_t accum_modes;
   uint32_t depth_modes;
   uint32_t stencil_modes;
   uint32_t display_mask;
   int16_t max_aux_buffers;
   int16_t max_sample_buffers;
   int16_t max_samples;
   uint8_t sample_alpha;
   uint8_t pad0;
   uint32_t sample_modes;
   uint32_t unused34[6];
   uint32_t video_memory_mb;
   uint32_t unused50;
   uint32_t texture_memory_mb;
   uint8_t unused58[0x88 - 0x58];
};

/* gldChoosePixelFormat's nodes (0x38 bytes on x86_64, 0x34 on i386). */
struct gld_pixel_format {
   struct gld_pixel_format *next;
   uint32_t renderer_id;
   uint32_t flags;
   uint32_t buffer_mode;         /* 0, or kCGLDoubleBufferBit; must equal the request */
   uint32_t color_mode;          /* one bit */
   uint32_t accum_mode;
   uint32_t depth_mode;
   uint32_t stencil_mode;
   uint16_t zero24;
   uint16_t aux_buffers;
   uint16_t sample_buffers;
   uint16_t samples;
   uint32_t sample_mode;
   uint8_t sample_alpha;
   uint8_t pad31[3];
   uint32_t display_mask;
};

#if defined(__x86_64__)
_Static_assert(sizeof(struct gld_renderer_info) == 0x88, "renderer info layout");
_Static_assert(offsetof(struct gld_renderer_info, video_memory_mb) == 0x4c, "renderer info layout");
_Static_assert(sizeof(struct gld_pixel_format) == 0x38, "pixel format layout");
_Static_assert(offsetof(struct gld_pixel_format, display_mask) == 0x34, "pixel format layout");
#elif defined(__i386__)
_Static_assert(sizeof(struct gld_renderer_info) == 0x84, "renderer info layout");
_Static_assert(offsetof(struct gld_renderer_info, video_memory_mb) == 0x48, "renderer info layout");
_Static_assert(sizeof(struct gld_pixel_format) == 0x34, "pixel format layout");
_Static_assert(offsetof(struct gld_pixel_format, display_mask) == 0x30, "pixel format layout");
#endif

/* Flag bits shared by renderer info and pixel formats. */
#define GLD_FLAG_WINDOW         (1u << 0)
#define GLD_FLAG_FULLSCREEN     (1u << 1)
#define GLD_FLAG_OFFSCREEN      (1u << 2)
#define GLD_FLAG_BACKING_STORE  (1u << 3)
#define GLD_FLAG_ROBUST         (1u << 6)
#define GLD_FLAG_FALLBACK       (1u << 7)    /* inferred: can stand in as fallback renderer */
#define GLD_FLAG_ACCELERATED    (1u << 8)
#define GLD_FLAG_COMPLIANT      (1u << 10)
#define GLD_FLAG_QUARTZ_EXTREME (1u << 12)    /* Apple's drivers set it; documented by VMsvga2 */
#define GLD_FLAG_PBUFFER        (1u << 13)
#define GLD_FLAG_GPU_VERTEX     (1u << 14)
#define GLD_FLAG_GPU_FRAGMENT   (1u << 15)

/* gldInitDispatch / gldUpdateDispatch return bit: no fallback to the float renderer. */
#define GLD_DISPATCH_NO_FALLBACK 0x4

/* GLEngine's driver render-ops table and the slots both Apple drivers leave NULL. */
#define GLD_RENDER_OPS          33

/* ---- Library ------------------------------------------------------------------------- */

FILE *ripple_gld_dbg_file(void);

/* Debug log of the management calls: RIPPLE_GL_DEBUG=1 sends it to stderr; for processes
 * started without our environment (WindowServer, at boot), the directory /Library/Logs/RippleGL
 * existing (world-writable) sends it to /Library/Logs/RippleGL/PROGRAM-PID.log. */
FILE *
ripple_gld_dbg_file(void)
{
   static FILE *f;
   static int checked;
   if (!checked) {
      checked = 1;
      if (getenv("RIPPLE_GL_DEBUG")) {
         f = stderr;
      } else if (access("/Library/Logs/RippleGL", W_OK) == 0) {
         char path[256];
         snprintf(path, sizeof(path), "/Library/Logs/RippleGL/%s-%d.log", getprogname(), (int)getpid());
         f = fopen(path, "a");
         if (f)
            setvbuf(f, NULL, _IOLBF, 0);
      }
   }
   return f;
}
#define DBG(...) do { FILE *df_ = ripple_gld_dbg_file(); if (df_) fprintf(df_, "RippleGLDriver: " __VA_ARGS__); } while (0)

static simple_mtx_t lib_lock = SIMPLE_MTX_INITIALIZER;
static uint32_t lib_display_mask;
static bool lib_qe;     /* the kext runs Quartz Extreme work (RippleQE = on, boot flag ripple_qe=1) */

/* Does the accelerator say Quartz Extreme work is on (M5)? */
static bool
accelerator_qe(const io_service_t *accel)
{
   bool on = false;
   for (unsigned i = 0; accel && i < 32 && !on; i++) {
      if (!accel[i])
         continue;
      CFTypeRef v = IORegistryEntryCreateCFProperty(accel[i], CFSTR("RippleQE"),
                                                    kCFAllocatorDefault, 0);
      if (v) {
         on = CFGetTypeID(v) == CFStringGetTypeID() &&
              CFStringCompare(v, CFSTR("on"), 0) == kCFCompareEqualTo;
         CFRelease(v);
      }
   }
   return on;
}

GLD_EXPORT void
gldInitializeLibrary(const io_service_t *accel, const uint8_t *accel_index,
                     uint32_t display_mask, void *io_flush, void *io_bind_surface,
                     uint32_t flags)
{
   struct ripple_gpu_info info;
   bool offer;

   /* Offer the renderer only if our GPU client is there and answers. With Quartz Extreme work
    * on in the kext (M5), to every process, WindowServer included; otherwise (M4) only to
    * processes started with RIPPLE_GL=1, never to WindowServer. */
   bool qe = accelerator_qe(accel);
   offer = (qe || (getenv("RIPPLE_GL") && strcmp(getprogname(), "WindowServer") != 0)) &&
           ripple_cgl_device_info(&info);
   lib_qe = qe;
   simple_mtx_lock(&lib_lock);
   lib_display_mask = offer ? display_mask : 0;
   simple_mtx_unlock(&lib_lock);
   DBG("gldInitializeLibrary(accel %p [0x%x], index %p, mask 0x%x, %p, %p, flags 0x%x) in %s: %s\n",
       (void *)accel, accel ? accel[0] : 0, (void *)accel_index, display_mask, io_flush,
       io_bind_surface, flags, getprogname(), offer ? "offered" : "not offered");
}

GLD_EXPORT void
gldTerminateLibrary(void)
{
   DBG("gldTerminateLibrary\n");
   simple_mtx_lock(&lib_lock);
   lib_display_mask = 0;
   simple_mtx_unlock(&lib_lock);
}

GLD_EXPORT int
gldGetVersion(int *major, int *minor, int *rev, uint32_t *renderer_id)
{
   DBG("gldGetVersion -> %d\n", lib_display_mask != 0);
   *major = 3;
   *minor = 1;
   *rev = 0;
   *renderer_id = RIPPLE_RENDERER_ID;
   /* 0 makes libGFXShared drop this plugin for the process (gldTerminateLibrary, dlclose):
    * the clean way out when we don't offer the renderer. An error from gldGetRendererInfo
    * instead would abort renderer queries for every renderer. */
   return lib_display_mask != 0;
}

static bool
mask_ok(uint32_t mask)
{
   return lib_display_mask && (mask & lib_display_mask) && !(mask & ~lib_display_mask);
}

GLD_EXPORT int
gldGetRendererInfo(struct gld_renderer_info *rec, uint32_t display_mask)
{
   DBG("gldGetRendererInfo(%p, mask 0x%x), library mask 0x%x\n", (void *)rec, display_mask,
       lib_display_mask);
   if (!mask_ok(display_mask))
      return kCGLBadDisplay;
   memset(rec, 0, sizeof(*rec));
   rec->renderer_id = RIPPLE_RENDERER_ID_FULL;
   rec->flags = GLD_FLAG_WINDOW | GLD_FLAG_OFFSCREEN | GLD_FLAG_BACKING_STORE |
                GLD_FLAG_ROBUST | GLD_FLAG_FALLBACK | GLD_FLAG_ACCELERATED |
                GLD_FLAG_COMPLIANT | GLD_FLAG_GPU_VERTEX | GLD_FLAG_GPU_FRAGMENT;
   if (lib_qe)
      rec->flags |= GLD_FLAG_QUARTZ_EXTREME;
   rec->buffer_modes = kCGLMonoscopicBit | kCGLSingleBufferBit | kCGLDoubleBufferBit;
   rec->color_modes = kCGLARGB8888Bit;
   rec->depth_modes = kCGL0Bit | kCGL16Bit | kCGL24Bit;
   rec->stencil_modes = kCGL0Bit | kCGL8Bit;
   rec->display_mask = display_mask;
   rec->video_memory_mb = RIPPLE_VRAM_MB;
   rec->texture_memory_mb = RIPPLE_VRAM_MB;
   return 0;
}

GLD_EXPORT const char *
gldGetString(uint32_t display_mask, uint32_t name)
{
   /* Only GLEngine's own glGetString asks; after the takeover Mesa answers apps. */
   switch (name) {
   case 0x1f00: return "moose-ripple";
   case 0x1f01: return "Ripple GPU (Mesa virgl)";
   case 0x1f02: return "2.1 RippleGLDriver";
   case 0x1f04: return "RippleGLDriver";
   default:     return NULL;
   }
}

/* ---- Pixel formats ------------------------------------------------------------------- */

GLD_EXPORT int
gldChoosePixelFormat(struct gld_pixel_format **out, const uint32_t *attrs)
{
   uint32_t flags = GLD_FLAG_ACCELERATED | GLD_FLAG_COMPLIANT | GLD_FLAG_ROBUST;
   uint32_t buffer_mode = 0, depth = 0, stencil = 0, mask = lib_display_mask;
   bool minimum = false, ok = true;

   if (ripple_gld_dbg_file()) {
      fprintf(ripple_gld_dbg_file(), "RippleGLDriver: gldChoosePixelFormat attrs:");
      for (const uint32_t *a = attrs; a && *a; a++)
         fprintf(ripple_gld_dbg_file(), " %u", *a);
      fprintf(ripple_gld_dbg_file(), "\n");
   }
   *out = NULL;
   for (const uint32_t *a = attrs; a && *a && ok; a++) {
      switch (*a) {
      case kCGLPFADoubleBuffer: buffer_mode = kCGLDoubleBufferBit; break;
      case kCGLPFAWindow:       flags |= GLD_FLAG_WINDOW; break;
      case kCGLPFAOffScreen:    flags |= GLD_FLAG_OFFSCREEN; break;
      case kCGLPFABackingStore: flags |= GLD_FLAG_BACKING_STORE; break;
      case kCGLPFAPBuffer:      flags |= GLD_FLAG_PBUFFER; break;
      case kCGLPFAMinimumPolicy: minimum = true; break;
      case kCGLPFAMaximumPolicy: break;
      case 4:                   break;  /* always appended by OpenGL; meaning unknown */
      case kCGLPFAColorSize:    ok = *++a <= 32; break;
      case kCGLPFAAlphaSize:    ok = *++a <= 8; break;
      case kCGLPFADepthSize:    depth = *++a; ok = !minimum || depth <= 24; break;
      case kCGLPFAStencilSize:  stencil = *++a; ok = stencil <= 8; break;
      case kCGLPFAAuxBuffers:   ok = *++a == 0; break;
      case kCGLPFAAccumSize:    ok = *++a == 0; break;
      case kCGLPFASampleBuffers: ok = *++a == 0; break;
      case kCGLPFASamples:      ok = *++a == 0; break;
      case kCGLPFADisplayMask:  mask &= *++a; break;
      /* Not offered yet: stereo, full screen, float color, multisampling, aux depth. */
      case kCGLPFAStereo: case kCGLPFAFullScreen: case kCGLPFAColorFloat:
      case kCGLPFAMultisample: case kCGLPFASupersample: case kCGLPFASampleAlpha:
      case kCGLPFAAuxDepthStencil:
         ok = false;
         break;
      default:
         break;                         /* flags we don't know: ignore */
      }
   }
   DBG("gldChoosePixelFormat: %s (mask 0x%x)\n", ok && mask ? "match" : "no match", mask);
   /* Window-capable unless off-screen was asked for, as GLRendererFloat does: OpenGL requires
    * the Window bit whenever an app names none of Window, FullScreen, OffScreen, PBuffer. */
   if (!(flags & GLD_FLAG_OFFSCREEN))
      flags |= GLD_FLAG_WINDOW;
   if (!ok || !mask)
      return 0;                         /* no match: not an error */

   struct gld_pixel_format *pf = calloc(1, sizeof(*pf));
   if (!pf)
      return kCGLBadAlloc;
   pf->renderer_id = RIPPLE_RENDERER_ID_FULL;
   pf->flags = flags;
   pf->buffer_mode = buffer_mode;
   pf->color_mode = kCGLARGB8888Bit;
   pf->depth_mode = !depth ? kCGL0Bit : depth <= 16 ? kCGL16Bit : kCGL24Bit;
   pf->stencil_mode = stencil ? kCGL8Bit : kCGL0Bit;
   pf->display_mask = mask;
   *out = pf;
   return 0;
}

GLD_EXPORT int
gldDestroyPixelFormat(struct gld_pixel_format *pf)
{
   free(pf);                            /* GLEngine may have linked other lists to ->next */
   return 0;
}

/* ---- Shared state (one per CGL share group) ------------------------------------------ */

struct gld_shared {
   simple_mtx_t lock;
   struct gld_context *members;          /* contexts of this share group */
};

GLD_EXPORT int
gldCreateShared(struct gld_shared **out, uint32_t display_mask, int unknown)
{
   struct gld_shared *s = calloc(1, sizeof(*s));
   if (!s)
      return kCGLBadAlloc;
   simple_mtx_init(&s->lock, mtx_plain);
   *out = s;
   return 0;
}

GLD_EXPORT int
gldDestroyShared(struct gld_shared *s)
{
   if (s) {
      simple_mtx_destroy(&s->lock);
      free(s);
   }
   return 0;
}

/* ---- Contexts ------------------------------------------------------------------------ */

/* Drawables of accelerator surfaces, one per surface for the whole process: WindowServer
 * moves its contexts between surfaces every frame and redraws only what changed, so each
 * surface's render targets must persist (one drawable per context, resized on every switch,
 * lost the rest: black areas and leftovers). The least recently used unbound one goes when
 * the table is full. */
#define GLD_MAX_SURFACES 32
struct gld_surface {
   uint32_t sid;
   struct ripple_cgl_drawable *d;
   struct ripple_gpu_surface_info info;
   unsigned long last_use;
   unsigned binds;                       /* contexts drawing into it now */
};
static struct gld_surface surfaces[GLD_MAX_SURFACES];
static unsigned long surface_clock;
static simple_mtx_t surfaces_lock = SIMPLE_MTX_INITIALIZER;

#define GLD_CONTEXT_MAGIC 0x52474c44u    /* "RGLD" */

struct gld_context {
   uint32_t magic;
   struct ripple_cgl_context *mesa;
   struct gld_shared *shared;
   struct gld_context *next_member;
   void *gc;                             /* GLEngine's context, for the takeover */
   bool taken_over, takeover_failed;
   unsigned takeover_tries;
   struct gld_window *win;               /* attached window, if any */
   struct ripple_cgl_drawable *drawable; /* its render targets */
   int swap_interval;                    /* kCGLCPSwapInterval: 1 = wait for vblank */
   struct gld_surface *cur_surface;      /* accelerator surface drawn into (WindowServer) */
   struct gld_pbuffer *cur_pbuffer;      /* pbuffer drawn into (Core Image in WindowServer) */
};

/* Pbuffers. CoreGraphics' cgls pbuffers (Core Image's intermediate images in WindowServer) are
 * attached as drawable kind 0x5a: {0, 0, accelerator ID, target, internal format, ...,
 * width (word 9), height (word 10), ...}, and used as textures with gliSetInteger(998, {ID,
 * target, format, width, height, ...}) (cgl_apple.c). Each ID gets an offscreen drawable,
 * kept until the table is full (the least recently used unbound one goes). */
#define GLD_DRAWABLE_PBUFFER  0x5a
#define GLD_MAX_PBUFFERS      64
struct gld_pbuffer {
   uint32_t id;
   unsigned width, height;
   struct ripple_cgl_drawable *d;
   unsigned long last_use;
   unsigned binds;
};
static struct gld_pbuffer pbuffers[GLD_MAX_PBUFFERS];

struct ripple_cgl_drawable *
ripple_gld_pbuffer_lookup(uint32_t id, unsigned *width, unsigned *height)
{
   struct ripple_cgl_drawable *d = NULL;
   if (!id)
      return NULL;
   simple_mtx_lock(&surfaces_lock);
   for (unsigned i = 0; i < GLD_MAX_PBUFFERS && !d; i++) {
      if (pbuffers[i].d && pbuffers[i].id == id) {
         d = pbuffers[i].d;
         *width = pbuffers[i].width;
         *height = pbuffers[i].height;
         pbuffers[i].last_use = ++surface_clock;
      }
   }
   simple_mtx_unlock(&surfaces_lock);
   return d;
}

static void
unbind_pbuffer(struct gld_context *c)
{
   if (!c->cur_pbuffer)
      return;
   simple_mtx_lock(&surfaces_lock);
   c->cur_pbuffer->binds--;
   simple_mtx_unlock(&surfaces_lock);
   c->cur_pbuffer = NULL;
   c->mesa->attached_surface = 0;
}

static void
unbind_surface(struct gld_context *c)
{
   unbind_pbuffer(c);
   if (!c->cur_surface)
      return;
   /* Present what was drawn into a screen region before leaving it: WindowServer may switch to
    * another surface before flushing, and that rectangle would never reach the screen. */
   if (c->cur_surface->info.flags & RIPPLE_GPU_SURFACE_SCREEN)
      ripple_cgl_context_swap(c->mesa);
   simple_mtx_lock(&surfaces_lock);
   c->cur_surface->binds--;
   simple_mtx_unlock(&surfaces_lock);
   c->cur_surface = NULL;
   c->mesa->win_surface = 0;
   c->mesa->attached_surface = 0;
   c->mesa->surf_h = 0;
   ripple_cgl_context_set_drawable(c->mesa, c->drawable);
}

static void
detach(struct gld_context *c)
{
   unbind_surface(c);
   if (c->drawable) {
      ripple_cgl_context_set_drawable(c->mesa, NULL);
      ripple_cgl_drawable_destroy(c->drawable);
      c->drawable = NULL;
   }
   gld_window_release(c->win);
   c->win = NULL;
}

void ripple_gld_fill_config(void *cfg, const struct gld_pixel_format *pf);   /* gld_config.c */

static struct ripple_cgl_context *
share_partner(struct gld_shared *s)
{
   return s && s->members ? s->members->mesa : NULL;
}

GLD_EXPORT int
gldCreateContext(struct gld_context **out, const struct gld_pixel_format *pf,
                 struct gld_shared *shared, void *cfg, void *gl_state, void *aux)
{
   struct ripple_cgl_pixel_format rpf;
   struct gld_context *c = calloc(1, sizeof(*c));

   if (!c)
      return kCGLBadAlloc;
   memset(&rpf, 0, sizeof(rpf));
   rpf.color_bits = 32;
   rpf.depth_bits = pf->depth_mode & kCGL24Bit ? 24 : pf->depth_mode & kCGL16Bit ? 16 : 0;
   rpf.stencil_bits = pf->stencil_mode & kCGL8Bit ? 8 : 0;
   rpf.double_buffer = pf->buffer_mode == kCGLDoubleBufferBit;

   if (shared)
      simple_mtx_lock(&shared->lock);
   c->mesa = ripple_cgl_context_create(&rpf, share_partner(shared));
   if (c->mesa && shared) {
      c->shared = shared;
      c->next_member = shared->members;
      shared->members = c;
   }
   if (shared)
      simple_mtx_unlock(&shared->lock);
   if (!c->mesa) {
      free(c);
      DBG("gldCreateContext: no Mesa context\n");
      return kCGLBadAlloc;
   }
   c->magic = GLD_CONTEXT_MAGIC;
   c->gc = ripple_cgl_gc_from_create_arg(gl_state);
   DBG("gldCreateContext: mesa %p, gc %p, shared %p\n", (void *)c->mesa, c->gc, (void *)shared);
   ripple_gld_fill_config(cfg, pf);
   *out = c;
   return 0;
}

GLD_EXPORT int
gldDestroyContext(struct gld_context *c)
{
   if (!c || c->magic != GLD_CONTEXT_MAGIC)
      return 0;
   if (c->shared) {
      simple_mtx_lock(&c->shared->lock);
      for (struct gld_context **p = &c->shared->members; *p; p = &(*p)->next_member) {
         if (*p == c) {
            *p = c->next_member;
            break;
         }
      }
      simple_mtx_unlock(&c->shared->lock);
   }
   detach(c);
   ripple_cgl_context_destroy(c->mesa);
   c->magic = 0;
   free(c);
   return 0;
}

GLD_EXPORT int
gldReclaimContext(struct gld_context *c)
{
   return 0;
}

/* Move the context to Mesa as soon as its CGL object is complete. Apps' CGLCreateContext ends
 * with gldSetInteger 0x2bd; WindowServer's contexts never send it, so attach and dispatch
 * calls try too. Every attempt checks Apple's magic values and the empty back table first. */
static void
try_takeover(struct gld_context *c, const char *from)
{
   if (!c || c->magic != GLD_CONTEXT_MAGIC || c->taken_over || c->takeover_failed)
      return;
   int r = ripple_cgl_take_over(c->gc, c->mesa);
   c->taken_over = r >= 0;
   if (c->takeover_tries < 3 || (c->takeover_tries % 100) == 0)
      DBG("takeover from %s: %d\n", from, r);
   if (c->taken_over) {
      DBG("takeover: done (from %s, try %u)\n", from, c->takeover_tries + 1);
   } else if (++c->takeover_tries >= 1000) {
      c->takeover_failed = true;
      fprintf(stderr, "RippleGLDriver: could not take over the GL context; it stays "
                      "with GLEngine and renders nothing\n");
      DBG("takeover: FAILED\n");
   }
}

GLD_EXPORT int
gldSetInteger(struct gld_context *c, int pname, const int *value)
{
   DBG("gldSetInteger(%p, 0x%x)\n", (void *)c, pname);
   if (c && c->magic == GLD_CONTEXT_MAGIC && pname == 222 /* kCGLCPSwapInterval */ && value) {
      c->swap_interval = *value;
      gld_window_set_vsync(c->win, c->swap_interval != 0);
   }
   /* OpenGL sends 0x2bd at the very end of CGLCreateContext: the dispatch table is final
    * now, so this is where the context moves to Mesa. */
   try_takeover(c, "gldSetInteger");
   return 0;
}

GLD_EXPORT int
gldGetInteger(struct gld_context *c, int pname, int *value)
{
   return 0;
}

GLD_EXPORT uint32_t
gldGetError(struct gld_context *c)
{
   return 0;
}

GLD_EXPORT int gldFlush(struct gld_context *c) { return 0; }
GLD_EXPORT int gldFinish(struct gld_context *c) { return 0; }

/* Drawable kinds (CGLSetSurface and friends; docs/re/gl-plugin-interface.md). */
#define GLD_DRAWABLE_NONE     0x00
#define GLD_DRAWABLE_WINDOW   0x50      /* data: u32 {connection, window, surface} */
#define GLD_DRAWABLE_NO_DRAW  0x5b      /* GLEngine's own: FBOs only */
#define GLD_DRAWABLE_PROBE    0x5c      /* fallback probe */
#define GLD_ATTACH_CHANGED    2
#define GLD_ATTACH_DETACHED   1

/* Frames for an accelerator surface with CPU backing (in this process): copy them in, bottom
 * row first. WindowServer treats accelerator surface memory in GL's row order, like GL window
 * memory (gld_window.c): it draws into these surfaces with GL and binds them as textures
 * without flipping (the menu bar's background, with the Apple menu's logo, came out upside
 * down with the top row first, VM 210 2026-11-10). `pixels` come top row first. */
static void
backing_present(void *data, const uint8_t *pixels, unsigned stride, unsigned width,
                unsigned height)
{
   struct gld_surface *e = data;
   uint8_t *dst = (uint8_t *)(uintptr_t)e->info.backing_address;
   unsigned rows = height < e->info.height ? height : e->info.height;
   unsigned bytes = (width < e->info.width ? width : e->info.width) * 4;

   if (!dst || !height)
      return;
   for (unsigned y = 0; y < rows; y++)
      memcpy(dst + (size_t)y * e->info.row_bytes, pixels + (size_t)(height - 1 - y) * stride,
             bytes);
}

/* A window-server surface given as {0, 0, accelerator surface id}: what WindowServer's own
 * compositor attaches to under Quartz Extreme (M5). The display surface is shown straight from
 * the GPU; a surface with CPU backing gets each frame copied into it. */
static int
attach_surface(struct gld_context *c, uint32_t sid)
{
   struct ripple_gpu_surface_info si;
   struct gld_surface *e = NULL, *spare = NULL;
   bool changed = false;

   unbind_pbuffer(c);
   if (!ripple_cgl_device_surface_info(sid, &si)) {
      DBG("  accelerator surface %u: unknown\n", sid);
      return kCGLBadDrawable;
   }
   if (!(si.flags & RIPPLE_GPU_SURFACE_SCREEN) && !si.backing_address) {
      DBG("  accelerator surface %u: neither the screen nor backed (flags 0x%x)\n", sid, si.flags);
      return kCGLBadDrawable;
   }
   if (c->win) {
      gld_window_release(c->win);
      c->win = NULL;
   }

   simple_mtx_lock(&surfaces_lock);
   for (unsigned i = 0; i < GLD_MAX_SURFACES && !e; i++) {
      if (surfaces[i].d && surfaces[i].sid == sid)
         e = &surfaces[i];
      else if (!surfaces[i].d && !spare)
         spare = &surfaces[i];
   }
   if (!e) {
      if (!spare) {                     /* full: the least recently used unbound one goes */
         for (unsigned i = 0; i < GLD_MAX_SURFACES; i++)
            if (!surfaces[i].binds && (!spare || surfaces[i].last_use < spare->last_use))
               spare = &surfaces[i];
         if (spare) {
            ripple_cgl_drawable_destroy(spare->d);
            memset(spare, 0, sizeof(*spare));
         }
      }
      if (spare)
         spare->d = ripple_cgl_drawable_create(c->mesa);
      if (!spare || !spare->d) {
         simple_mtx_unlock(&surfaces_lock);
         return kCGLBadAlloc;
      }
      e = spare;
      e->sid = sid;
      changed = true;
   }
   if (!(si.flags & RIPPLE_GPU_SURFACE_SCREEN) &&
       (e->info.width != si.width || e->info.height != si.height))
      changed = true;
   e->info = si;
   e->last_use = ++surface_clock;
   if (c->cur_surface && c->cur_surface != e &&
       (c->cur_surface->info.flags & RIPPLE_GPU_SURFACE_SCREEN)) {
      simple_mtx_unlock(&surfaces_lock);
      ripple_cgl_context_swap(c->mesa);   /* see unbind_surface */
      simple_mtx_lock(&surfaces_lock);
   }
   if (c->cur_surface != e) {
      if (c->cur_surface)
         c->cur_surface->binds--;
      e->binds++;
      c->cur_surface = e;
      changed = true;
   }
   simple_mtx_unlock(&surfaces_lock);

   if (si.flags & RIPPLE_GPU_SURFACE_SCREEN) {
      static unsigned attaches;
      if (attaches++ < 400)
         DBG("attach: seq %u, %d,%d %ux%u\n", si.shape_seq, si.x, si.y, si.width, si.height);
   }
   if (si.flags & RIPPLE_GPU_SURFACE_SCREEN) {
      /* A screen region (WindowServer's display surface, shaped to each frame's dirty
       * rectangle): the drawable stays screen-sized, WindowServer draws the rectangle at its
       * bottom-left, and presenting copies it into place on the screen texture. */
      ripple_cgl_drawable_set_present(e->d, NULL, NULL);
      ripple_cgl_drawable_set_screen_region(e->d, 0);
      ripple_cgl_drawable_set_gpu_display(e->d, true);
      ripple_cgl_drawable_set_size(e->d, si.screen_width, si.screen_height);
      c->mesa->win_surface = sid;
      c->mesa->surf_h = 0;
      ripple_cgl_refresh_window_offset(c->mesa);
   } else {
      ripple_cgl_drawable_set_gpu_display(e->d, false);
      ripple_cgl_drawable_set_screen_region(e->d, 0);
      ripple_cgl_drawable_set_present(e->d, backing_present, e);
      ripple_cgl_drawable_set_size(e->d, si.width, si.height);
      c->mesa->win_surface = 0;
      c->mesa->surf_h = si.height;
   }
   c->mesa->attached_surface = sid;
   ripple_cgl_context_set_drawable(c->mesa, e->d);
   if (changed)
      DBG("  accelerator surface %u: %ux%u at %d,%d, %s\n", sid, si.width, si.height, si.x, si.y,
          (si.flags & RIPPLE_GPU_SURFACE_SCREEN) ? "the display" : "backed");
   return changed ? GLD_ATTACH_CHANGED : 0;
}

/* A cgls pbuffer: an offscreen drawable of its size, the same one each time its ID comes. */
static int
attach_pbuffer(struct gld_context *c, const uint32_t *w)
{
   uint32_t id = w[2];
   unsigned width = w[9], height = w[10];
   struct gld_pbuffer *e = NULL, *spare = NULL;
   bool changed = false;

   /* Public CGL pbuffers (CGLCreatePBuffer: Quartz Composer, M6) keep the size in words 6 and
    * 7, repeated in 14 and 15: {0, 0, ID, target, format, 0, width, height, 1, 0, ...,
    * width, height, target}. WindowServer's cgls ones have it in words 9 and 10. */
   if (w[6] && w[7] && w[6] == w[14] && w[7] == w[15]) {
      width = w[6];
      height = w[7];
   }
   static unsigned dumped;
   if (dumped < 4 && ripple_gld_dbg_file()) {
      dumped++;
      DBG("  pbuffer attach data:");
      for (unsigned i = 0; i < 18; i++)
         fprintf(ripple_gld_dbg_file(), " %08x", w[i]);
      fprintf(ripple_gld_dbg_file(), "\n");
   }
   if (!id || !width || !height || width > 16384 || height > 16384) {
      DBG("  pbuffer %u: %ux%u refused\n", id, width, height);
      return kCGLBadDrawable;
   }
   unbind_surface(c);                    /* also leaves any pbuffer */
   if (c->win) {
      gld_window_release(c->win);
      c->win = NULL;
   }
   simple_mtx_lock(&surfaces_lock);
   for (unsigned i = 0; i < GLD_MAX_PBUFFERS && !e; i++) {
      if (pbuffers[i].d && pbuffers[i].id == id)
         e = &pbuffers[i];
      else if (!pbuffers[i].d && !spare)
         spare = &pbuffers[i];
   }
   if (!e) {
      if (!spare) {
         for (unsigned i = 0; i < GLD_MAX_PBUFFERS; i++)
            if (!pbuffers[i].binds && (!spare || pbuffers[i].last_use < spare->last_use))
               spare = &pbuffers[i];
         if (spare) {
            ripple_cgl_drawable_destroy(spare->d);
            memset(spare, 0, sizeof(*spare));
         }
      }
      if (spare)
         spare->d = ripple_cgl_drawable_create(c->mesa);
      if (!spare || !spare->d) {
         simple_mtx_unlock(&surfaces_lock);
         return kCGLBadAlloc;
      }
      e = spare;
      e->id = id;
      changed = true;
   }
   if (e->width != width || e->height != height)
      changed = true;
   e->width = width;
   e->height = height;
   e->last_use = ++surface_clock;
   e->binds++;
   c->cur_pbuffer = e;
   simple_mtx_unlock(&surfaces_lock);

   ripple_cgl_drawable_set_gpu_display(e->d, false);
   ripple_cgl_drawable_set_screen_region(e->d, 0);
   ripple_cgl_drawable_set_present(e->d, NULL, NULL);
   ripple_cgl_drawable_set_size(e->d, width, height);
   c->mesa->win_surface = 0;
   c->mesa->surf_h = 0;                  /* a GL drawable: origin bottom left */
   c->mesa->attached_surface = id;
   ripple_cgl_context_set_drawable(c->mesa, e->d);
   static unsigned logged;
   if (changed && logged++ < 50)
      DBG("  pbuffer %u: %ux%u (target 0x%x, format 0x%x)\n", id, width, height, w[3], w[4]);
   return changed ? GLD_ATTACH_CHANGED : 0;
}

GLD_EXPORT int
gldAttachDrawable(struct gld_context *c, int type, void *data, void *opts)
{
   DBG("gldAttachDrawable(%p, 0x%x, %p)\n", (void *)c, type, data);
   if (!c || c->magic != GLD_CONTEXT_MAGIC)
      return 0;
   try_takeover(c, "gldAttachDrawable");
   switch (type) {
   case GLD_DRAWABLE_NONE:
      detach(c);
      return GLD_ATTACH_DETACHED;
   case GLD_DRAWABLE_WINDOW: {
      bool changed;
      const uint32_t *ids = data;
      DBG("  surface: connection %u, window %u, surface %u\n", ids ? ids[0] : 0, ids ? ids[1] : 0,
          ids ? ids[2] : 0);
      if (ids && !ids[0] && !ids[1] && ids[2])
         return attach_surface(c, ids[2]);
      unbind_surface(c);
      struct gld_window *w = gld_window_attach(data, c->win, &changed);
      if (!w) {
         DBG("  attach failed\n");
         return kCGLBadDrawable;
      }
      if (w != c->win) {
         gld_window_release(c->win);
         c->win = w;
      }
      gld_window_set_vsync(w, c->swap_interval != 0);
      if (!c->drawable) {
         c->drawable = ripple_cgl_drawable_create(c->mesa);
         if (!c->drawable)
            return kCGLBadAlloc;
         changed = true;
      }
      unsigned width, height;
      gld_window_size(w, &width, &height);
      ripple_cgl_drawable_set_present(c->drawable, gld_window_present, w);
      ripple_cgl_drawable_set_size(c->drawable, width, height);
      ripple_cgl_context_set_drawable(c->mesa, c->drawable);
      DBG("  window %ux%u%s\n", width, height, changed ? " (new or resized)" : "");
      return changed ? GLD_ATTACH_CHANGED : 0;
   }
   case GLD_DRAWABLE_NO_DRAW:
   case GLD_DRAWABLE_PROBE:
      return 0;
   case GLD_DRAWABLE_PBUFFER:
      return data ? attach_pbuffer(c, data) : kCGLBadDrawable;
   default: {
      /* Off-screen memory, pbuffers, IOSurfaces, full screen: not yet. The first words of the
       * description, once per kind, to learn their layout. */
      static uint32_t logged[8];
      unsigned slot = (unsigned)type & 7;
      if (data && logged[slot] != (uint32_t)type + 1 && ripple_gld_dbg_file()) {
         const uint32_t *w = data;
         logged[slot] = (uint32_t)type + 1;
         DBG("  drawable kind 0x%x refused; data:", type);
         for (unsigned i = 0; i < 16; i++)
            fprintf(ripple_gld_dbg_file(), " %08x", w[i]);
         fprintf(ripple_gld_dbg_file(), "\n");
      }
      return kCGLBadDrawable;
   }
   }
}

static int
render_op_noop(void)
{
   return 0;
}

GLD_EXPORT int
gldInitDispatch(struct gld_context *c, void **render_ops, uint32_t *limits)
{
   try_takeover(c, "gldInitDispatch");
   for (unsigned i = 0; i < GLD_RENDER_OPS; i++) {
      /* Empty like Apple's GMA 950 driver: slots 16-19, 25, 26. Slots 27-29 (flush and swap)
       * must not be: GLEngine calls them once a window is attached (GLRendererFloat fills
       * them at attach time), and a context GLEngine still drives would jump to 0. */
      bool keep_null = (i >= 16 && i <= 19) || i == 25 || i == 26;
      render_ops[i] = keep_null ? NULL : (void *)render_op_noop;
   }
   limits[0] = 0x4000;
   limits[1] = 0x4000;
   limits[2] = limits[3] = limits[4] = limits[5] = 0;
   return GLD_DISPATCH_NO_FALLBACK;
}

GLD_EXPORT int
gldUpdateDispatch(struct gld_context *c, void **render_ops, void *dirty)
{
   try_takeover(c, "gldUpdateDispatch");
   return GLD_DISPATCH_NO_FALLBACK;
}

/* ---- Objects ------------------------------------------------------------------------- *
 * GLEngine creates its own objects (textures, programs, vertex arrays, ...) through these
 * and only stores the handles; with Mesa doing the GL work they need no backing. */

#define GLD_PLACEHOLDER ((void *)(uintptr_t)0x52)

/* With the debug log on, the first calls of each are logged: GLEngine-side object work (e.g.
 * textures CoreGraphics makes through CGL calls outside the dispatch table) never reaches Mesa. */
#define GLD_SEEN(name)                                                        \
   do {                                                                      \
      static unsigned seen_;                                                 \
      if (seen_ < 3 && ++seen_)                                              \
         DBG("object call %s (%u)\n", #name, seen_);                         \
   } while (0)

#define GLD_CREATE(name)                                                     \
   GLD_EXPORT int name(void *shared, void **out)                             \
   {                                                                         \
      GLD_SEEN(name);                                                        \
      if (out)                                                               \
         *out = GLD_PLACEHOLDER;                                             \
      return 0;                                                              \
   }
GLD_CREATE(gldCreateTexture)
GLD_CREATE(gldCreateBuffer)
GLD_CREATE(gldCreateVertexArray)
GLD_CREATE(gldCreatePipelineProgram)
GLD_CREATE(gldCreateFramebuffer)
GLD_CREATE(gldCreateQuery)
GLD_CREATE(gldCreateFence)
GLD_CREATE(gldCreateProgram)

/* Everything else: accept and do nothing. */
#define GLD_NOOP(name) GLD_EXPORT int name(void) { GLD_SEEN(name); return 0; }
GLD_NOOP(gldBufferSubData)
GLD_NOOP(gldCopyTexSubImage)
GLD_NOOP(gldCreateComputeContext)
GLD_NOOP(gldDestroyBuffer)
GLD_NOOP(gldDestroyComputeContext)
GLD_NOOP(gldDestroyFence)
GLD_NOOP(gldDestroyFramebuffer)
GLD_NOOP(gldDestroyMemoryPlugin)
GLD_NOOP(gldDestroyPipelineProgram)
GLD_NOOP(gldDestroyProgram)
GLD_NOOP(gldDestroyQuery)
GLD_NOOP(gldDestroyTexture)
GLD_NOOP(gldDestroyVertexArray)
GLD_NOOP(gldDiscardFramebuffer)
GLD_NOOP(gldFinishObject)
GLD_NOOP(gldFlushBuffer)
GLD_NOOP(gldFlushMemoryPlugin)
GLD_NOOP(gldFlushObject)
GLD_NOOP(gldFlushVertexArray)
GLD_NOOP(gldGenerateTexMipmaps)
GLD_NOOP(gldGetMemoryPlugin)
GLD_NOOP(gldGetPipelineProgramInfo)
GLD_NOOP(gldGetQueryInfo)
GLD_NOOP(gldGetTextureLevelImage)
GLD_NOOP(gldGetTextureLevelInfo)
GLD_NOOP(gldIsTextureResident)
GLD_NOOP(gldLoadBuffer)
GLD_NOOP(gldLoadHostBuffer)
GLD_NOOP(gldLoadTexture)
GLD_NOOP(gldModifyPipelineProgram)
GLD_NOOP(gldModifyQuery)
GLD_NOOP(gldModifyTexSubImage)
GLD_NOOP(gldModifyTexture)
GLD_NOOP(gldModifyVertexArray)
GLD_NOOP(gldObjectPurgeable)
GLD_NOOP(gldObjectUnpurgeable)
GLD_NOOP(gldPageoffBuffer)
GLD_NOOP(gldReclaimBuffer)
GLD_NOOP(gldReclaimFramebuffer)
GLD_NOOP(gldReclaimTexture)
GLD_NOOP(gldReclaimVertexArray)
GLD_NOOP(gldSetMemoryPlugin)
GLD_NOOP(gldSyncBufferObject)
GLD_NOOP(gldSyncTexture)
GLD_NOOP(gldTestMemoryPlugin)
GLD_NOOP(gldTestObject)
GLD_NOOP(gldUnbindBuffer)
GLD_NOOP(gldUnbindFramebuffer)
GLD_NOOP(gldUnbindPipelineProgram)
GLD_NOOP(gldUnbindTexture)
GLD_NOOP(gldUnbindVertexArray)
GLD_NOOP(gldWaitObject)
