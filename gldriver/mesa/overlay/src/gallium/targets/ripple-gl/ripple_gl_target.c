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
#include <IOKit/IOKitLib.h>
#include <OpenGL/CGLTypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/simple_mtx.h"
#include "virgl/ripple/ripple_gpu_uapi.h"

#include "cgl/cgl_context.h"
#include "cgl/cgl_device.h"
#include "cgl/cgl_takeover.h"

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
#define GLD_FLAG_PBUFFER        (1u << 13)
#define GLD_FLAG_GPU_VERTEX     (1u << 14)
#define GLD_FLAG_GPU_FRAGMENT   (1u << 15)

/* gldInitDispatch / gldUpdateDispatch return bit: no fallback to the float renderer. */
#define GLD_DISPATCH_NO_FALLBACK 0x4

/* GLEngine's driver render-ops table and the slots both Apple drivers leave NULL. */
#define GLD_RENDER_OPS          33

/* ---- Library ------------------------------------------------------------------------- */

static simple_mtx_t lib_lock = SIMPLE_MTX_INITIALIZER;
static uint32_t lib_display_mask;

GLD_EXPORT void
gldInitializeLibrary(const io_service_t *accel, const uint8_t *accel_index,
                     uint32_t display_mask, void *io_flush, void *io_bind_surface,
                     uint32_t flags)
{
   struct ripple_gpu_info info;

   /* Offer the renderer only if our GPU client is there and answers. */
   simple_mtx_lock(&lib_lock);
   lib_display_mask = ripple_cgl_device_info(&info) ? display_mask : 0;
   simple_mtx_unlock(&lib_lock);
}

GLD_EXPORT void
gldTerminateLibrary(void)
{
   simple_mtx_lock(&lib_lock);
   lib_display_mask = 0;
   simple_mtx_unlock(&lib_lock);
}

GLD_EXPORT int
gldGetVersion(int *major, int *minor, int *rev, uint32_t *renderer_id)
{
   *major = 3;
   *minor = 1;
   *rev = 0;
   *renderer_id = RIPPLE_RENDERER_ID;
   return 1;
}

static bool
mask_ok(uint32_t mask)
{
   return lib_display_mask && (mask & lib_display_mask) && !(mask & ~lib_display_mask);
}

GLD_EXPORT int
gldGetRendererInfo(struct gld_renderer_info *rec, uint32_t display_mask)
{
   if (!mask_ok(display_mask))
      return kCGLBadDisplay;
   memset(rec, 0, sizeof(*rec));
   rec->renderer_id = RIPPLE_RENDERER_ID_FULL;
   rec->flags = GLD_FLAG_WINDOW | GLD_FLAG_OFFSCREEN | GLD_FLAG_BACKING_STORE |
                GLD_FLAG_ROBUST | GLD_FLAG_FALLBACK | GLD_FLAG_ACCELERATED |
                GLD_FLAG_COMPLIANT | GLD_FLAG_GPU_VERTEX | GLD_FLAG_GPU_FRAGMENT;
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

#define GLD_CONTEXT_MAGIC 0x52474c44u    /* "RGLD" */

struct gld_context {
   uint32_t magic;
   struct ripple_cgl_context *mesa;
   struct gld_shared *shared;
   struct gld_context *next_member;
   void *gc;                             /* GLEngine's context, for the takeover */
   bool taken_over, takeover_failed;
};

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
      return kCGLBadAlloc;
   }
   c->magic = GLD_CONTEXT_MAGIC;
   c->gc = ripple_cgl_gc_from_create_arg(gl_state);
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

GLD_EXPORT int
gldSetInteger(struct gld_context *c, int pname, const int *value)
{
   /* OpenGL sends 0x2bd at the very end of CGLCreateContext: the dispatch table is final
    * now, so this is where the context moves to Mesa. */
   if (c && c->magic == GLD_CONTEXT_MAGIC && !c->taken_over && !c->takeover_failed) {
      c->taken_over = ripple_cgl_take_over(c->gc, c->mesa);
      if (!c->taken_over) {
         c->takeover_failed = true;
         fprintf(stderr, "RippleGLDriver: could not take over the GL context; it stays "
                         "with GLEngine and renders nothing\n");
      }
   }
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

GLD_EXPORT int
gldAttachDrawable(struct gld_context *c, int type, void *data, void *opts)
{
   /* Window and surface drawables come with presentation, later in M4. */
   return 0;
}

static int
render_op_noop(void)
{
   return 0;
}

GLD_EXPORT int
gldInitDispatch(struct gld_context *c, void **render_ops, uint32_t *limits)
{
   for (unsigned i = 0; i < GLD_RENDER_OPS; i++) {
      /* GLRendererFloat leaves slots 16-19 and 25-29 empty (byte offsets 0x80-0x98 and
       * 0xc8-0xe8 on x86_64, half that on i386). */
      bool keep_null = (i >= 16 && i <= 19) || (i >= 25 && i <= 29);
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
   return GLD_DISPATCH_NO_FALLBACK;
}

/* ---- Objects ------------------------------------------------------------------------- *
 * GLEngine creates its own objects (textures, programs, vertex arrays, ...) through these
 * and only stores the handles; with Mesa doing the GL work they need no backing. */

#define GLD_PLACEHOLDER ((void *)(uintptr_t)0x52)

#define GLD_CREATE(name)                                                     \
   GLD_EXPORT int name(void *shared, void **out)                             \
   {                                                                         \
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
#define GLD_NOOP(name) GLD_EXPORT int name(void) { return 0; }
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
