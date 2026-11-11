/*
 * Dispatch takeover: see cgl_takeover.c.
 */
#ifndef CGL_TAKEOVER_H
#define CGL_TAKEOVER_H

#include <stdbool.h>

struct ripple_cgl_context;

/* GLEngine's context from gldCreateContext's 5th argument (unchecked until the takeover). */
void *ripple_cgl_gc_from_create_arg(void *arg5);

/* Point the CGL context that belongs to GLEngine context `gc` at Mesa (our dispatch table,
 * `rend` = ctx). Call once the CGL context is complete (gldSetInteger 0x2bd). 0 on success
 * (1: a CoreGraphics context, taken over by hand);
 * a negative number names the check that failed (see cgl_takeover.c), and the context then
 * stays with GLEngine. With `cgls_only`, a CGL context is left alone (-14): one may still be
 * inside CGLCreateContext, which would undo the takeover. */
int ripple_cgl_take_over(void *gc, struct ripple_cgl_context *ctx, bool cgls_only);

/* After a takeover in the middle of GLEngine's first GL calls (cgls_only): copy the bindings
 * GLEngine made into Mesa. Called once, from the next make-current (cgl_context.c). */
void ripple_cgl_sync_from_glengine(struct ripple_cgl_context *ctx);

/* The context taken over for GLEngine context `rend`, if a caller passes that as `rend`. */
struct ripple_cgl_context *ripple_cgl_rend_for(void *rend);
void ripple_cgl_forget(struct ripple_cgl_context *ctx);

#endif
