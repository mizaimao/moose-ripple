/*
 * Dispatch takeover (moose-ripple, M4; decision 0003, docs/re/gl-plugin-interface.md "Where
 * the takeover hooks in"). After Apple's CGLCreateContext has built a context's public
 * dispatch table, we install ours with OpenGL.framework's CGLSelectDispatch: it moves
 * GLEngine's table to the context's back table, where GLEngine's later rewrites land, and puts
 * ours in front, where libGL calls through it. Then the context's `rend` becomes our context,
 * which libGL passes as the first argument of every call.
 *
 * GLEngine never hands the driver the CGL object, so we find it through GLEngine's own
 * context. The offsets are those of 10.6.8 (10K549), x86_64; every step checks magic values
 * and refuses (leaving the context to GLEngine) if anything doesn't match.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define GC_FROM_CREATE_ARG5    0x8b0         /* gldCreateContext's 5th argument is gc + this */
#define HAVE_TAKEOVER_LAYOUT   1
#else
/* i386: offsets not read yet. Contexts in 32-bit processes stay with GLEngine for now. */
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
   uint8_t *gc = (uint8_t *)arg5 - GC_FROM_CREATE_ARG5;
   if (arg5 && *(uint32_t *)(gc + GC_MAGIC_OFFSET) == GC_MAGIC)
      return gc;
#endif
   return NULL;
}

bool
ripple_cgl_take_over(void *gc, struct ripple_cgl_context *ctx)
{
#if HAVE_TAKEOVER_LAYOUT
   if (!gc || !select_dispatch || !back_dispatch || !dispatch_table[0])
      return false;
   if (*(uint32_t *)((uint8_t *)gc + GC_MAGIC_OFFSET) != GC_MAGIC)
      return false;
   void **front = *(void ***)((uint8_t *)gc + GC_FRONT_DISPATCH);
   if (!front)
      return false;
   struct cgl_object_head *cgl = (struct cgl_object_head *)((uint8_t *)front - sizeof(void *));
   if (*(uint32_t *)((uint8_t *)cgl + CGL_MAGIC_OFFSET) != CGL_MAGIC || cgl->rend != gc)
      return false;

   /* CGLCreateContext must have finished moving GLEngine's table to the front: the back
    * table is then all zero. */
   void **back = back_dispatch(cgl);
   if (!back)
      return false;
   for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
      if (back[i])
         return false;

   select_dispatch(cgl, dispatch_table);
   if (cgl->disp[0] != dispatch_table[0])
      return false;
   cgl->rend = ctx;
   return true;
#else
   return false;
#endif
}
