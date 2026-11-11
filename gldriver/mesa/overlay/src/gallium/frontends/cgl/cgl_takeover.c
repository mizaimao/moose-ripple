/*
 * Dispatch takeover (moose-ripple, M4; decision 0003, docs/re/gl-plugin-interface.md "Where
 * the takeover hooks in"). After Apple's CGLCreateContext has built a context's public
 * dispatch table, we install ours with OpenGL.framework's CGLSelectDispatch: it moves
 * GLEngine's table to the context's back table, where GLEngine's later rewrites land, and puts
 * ours in front, where libGL calls through it. Then the context's `rend` becomes our context,
 * which libGL passes as the first argument of every call.
 *
 * GLEngine never hands the driver the CGL object, so we find it through GLEngine's own
 * context. The offsets are those of 10.6.8 (10K549), x86_64 and i386; every step checks magic values
 * and refuses (leaving the context to GLEngine) if anything doesn't match.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/simple_mtx.h"

#include "cgl_context.h"
#include "cgl_dispatch.h"
#include "cgl_takeover.h"

#define GLI_DISPATCH_ENTRIES   764

#if defined(__x86_64__)
#define CGL_MAGIC_OFFSET       0x18a0        /* in the CGLContextObject */
#define CGL_MAGIC              0x1b7f1843u
#define GC_MAGIC_OFFSET        0x6588        /* in GLEngine's context */
#define GC_MAGIC               0x3463a223u
#define GC_FRONT_DISPATCH      0x6550        /* holds &cgl->disp */
#define GC_BACK_DISPATCH       0x6558        /* holds the back table */
#define GC_FROM_CREATE_ARG5    0x8b0         /* gldCreateContext's 5th argument is gc + this */
#define HAVE_TAKEOVER_LAYOUT   1
#elif defined(__i386__)
#define CGL_MAGIC_OFFSET       0xc60
#define CGL_MAGIC              0x1b7f1843u
#define GC_MAGIC_OFFSET        0x5844
#define GC_MAGIC               0x3463a223u
#define GC_FRONT_DISPATCH      0x5820
#define GC_BACK_DISPATCH       0x5824
#define GC_FROM_CREATE_ARG5    0x890
#define HAVE_TAKEOVER_LAYOUT   1
#else
#define HAVE_TAKEOVER_LAYOUT   0
#endif

typedef int (*cgl_select_dispatch_fn)(void *cgl, void *table);
typedef void *(*cgl_back_dispatch_fn)(void *cgl);

struct cgl_object_head {        /* public part of struct _CGLContextObject (CGLContext.h) */
   void *rend;
   void *disp[GLI_DISPATCH_ENTRIES];
};

static void *dispatch_table[GLI_DISPATCH_ENTRIES];
static cgl_select_dispatch_fn select_dispatch;
static cgl_back_dispatch_fn back_dispatch;

/* Contexts taken over, by the GLEngine context pointer callers might pass as `rend` (CoreGraphics'
 * own contexts in WindowServer may): ripple_cgl_rend_for() maps it back. */
#define MAX_TAKEN 64
static struct { void *gc; struct ripple_cgl_context *ctx; } taken[MAX_TAKEN];
static simple_mtx_t taken_lock = SIMPLE_MTX_INITIALIZER;

static void
remember(void *gc, struct ripple_cgl_context *ctx)
{
   simple_mtx_lock(&taken_lock);
   for (unsigned i = 0; i < MAX_TAKEN; i++) {
      if (!taken[i].gc || taken[i].gc == gc) {
         taken[i].gc = gc;
         taken[i].ctx = ctx;
         break;
      }
   }
   simple_mtx_unlock(&taken_lock);
}

struct ripple_cgl_context *
ripple_cgl_rend_for(void *rend)
{
   struct ripple_cgl_context *ctx = NULL;
   simple_mtx_lock(&taken_lock);
   for (unsigned i = 0; i < MAX_TAKEN && !ctx; i++)
      if (taken[i].gc == rend)
         ctx = taken[i].ctx;
   simple_mtx_unlock(&taken_lock);
   return ctx;
}

void
ripple_cgl_forget(struct ripple_cgl_context *ctx)
{
   simple_mtx_lock(&taken_lock);
   for (unsigned i = 0; i < MAX_TAKEN; i++)
      if (taken[i].ctx == ctx)
         taken[i].gc = NULL, taken[i].ctx = NULL;
   simple_mtx_unlock(&taken_lock);
}

__attribute__((constructor)) static void
ripple_cgl_takeover_init(void)
{
   if (ripple_cgl_dispatch_count != GLI_DISPATCH_ENTRIES) {
      fprintf(stderr, "RippleGLDriver: %u dispatch entries, expected %u\n",
              ripple_cgl_dispatch_count, GLI_DISPATCH_ENTRIES);
      return;
   }
   ripple_cgl_fill_dispatch(dispatch_table);
   select_dispatch = (cgl_select_dispatch_fn)dlsym(RTLD_DEFAULT, "CGLSelectDispatch");
   back_dispatch = (cgl_back_dispatch_fn)dlsym(RTLD_DEFAULT, "CGLBackDispatch");
}

void *
ripple_cgl_gc_from_create_arg(void *arg5)
{
#if HAVE_TAKEOVER_LAYOUT
   /* GLEngine writes its magic only after the renderers' contexts exist, so it is checked
    * at takeover time, not here. */
   if (arg5)
      return (uint8_t *)arg5 - GC_FROM_CREATE_ARG5;
#endif
   return NULL;
}

int
ripple_cgl_take_over(void *gc, struct ripple_cgl_context *ctx)
{
#if HAVE_TAKEOVER_LAYOUT
   if (!gc || !select_dispatch || !back_dispatch || !dispatch_table[0])
      return -1;
   if (*(uint32_t *)((uint8_t *)gc + GC_MAGIC_OFFSET) != GC_MAGIC)
      return -2;
   void **front = *(void ***)((uint8_t *)gc + GC_FRONT_DISPATCH);
   if (!front)
      return -3;
   struct cgl_object_head *cgl = (struct cgl_object_head *)((uint8_t *)front - sizeof(void *));
   if (*(uint32_t *)((uint8_t *)cgl + CGL_MAGIC_OFFSET) != CGL_MAGIC) {
      /* Not a CGL context: CoreGraphics makes WindowServer's contexts through GLEngine itself.
       * Same head ({rend, disp[764]}) and a back table at gc + GC_BACK_DISPATCH: do what
       * CGLSelectDispatch does by hand. GLEngine writes go wherever back[i] is set. */
      void **back = *(void ***)((uint8_t *)gc + GC_BACK_DISPATCH);
      if (cgl->rend != gc)
         return -4;
      if (!back)
         return -10;
      /* 2026-11-10: in WindowServer the front table is read-only (in CoreGraphics' image):
       * writing it crashed. Off until we know which table CoreGraphics calls through. */
      if (1)
         return -11;
      for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
         if (!back[i])
            back[i] = front[i];
      for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
         front[i] = dispatch_table[i];
      cgl->rend = ctx;
      remember(gc, ctx);
      return 1;
   }
   if (cgl->rend != gc)
      return -5;

   /* CGLCreateContext must have finished moving GLEngine's table to the front: the back
    * table is then all zero. */
   void **back = back_dispatch(cgl);
   if (!back)
      return -6;
   for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
      if (back[i])
         return -7;

   select_dispatch(cgl, dispatch_table);
   if (cgl->disp[0] != dispatch_table[0])
      return -8;
   cgl->rend = ctx;
   remember(gc, ctx);
   return 0;
#else
   return -9;
#endif
}
