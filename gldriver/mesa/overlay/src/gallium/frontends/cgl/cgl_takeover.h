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
 * `rend` = ctx). Call once the CGL context is complete (gldSetInteger 0x2bd). False if any
 * check fails; the context then stays with GLEngine. */
bool ripple_cgl_take_over(void *gc, struct ripple_cgl_context *ctx);

#endif
