/*
 * CGL contexts on Mesa (moose-ripple, M4): see cgl_context.c.
 */
#ifndef CGL_CONTEXT_H
#define CGL_CONTEXT_H

#include <stdbool.h>

struct gl_context;
struct st_context;

/* What a pixel format asks for (filled from Apple's pixel format attributes). */
struct ripple_cgl_pixel_format {
   unsigned color_bits;          /* 32 (BGRA8) for now */
   unsigned depth_bits;          /* 0, 16, 24 or 32 */
   unsigned stencil_bits;        /* 0 or 8 */
   unsigned accum_bits;          /* 0 or 64 */
   unsigned samples;             /* 0 = no multisampling */
   bool double_buffer;
};

struct ripple_cgl_context {
   struct gl_context *glctx;     /* first: struct ripple_cgl_rend in cgl_dispatch.h */
   struct st_context *st;
   struct ripple_cgl_pixel_format pf;
   unsigned next_fence;          /* GL_APPLE_fence names (cgl_apple.c) */
};

struct ripple_cgl_context *
ripple_cgl_context_create(const struct ripple_cgl_pixel_format *pf,
                          struct ripple_cgl_context *share);
void ripple_cgl_context_destroy(struct ripple_cgl_context *ctx);

/* Make `ctx` (or nothing) current for Mesa on the calling thread. */
bool ripple_cgl_context_make_current(struct ripple_cgl_context *ctx);

#endif
