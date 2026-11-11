/*
 * CGL contexts on Mesa (moose-ripple, M4): see cgl_context.c.
 */
#ifndef CGL_CONTEXT_H
#define CGL_CONTEXT_H

#include <stdbool.h>

#include "frontend/api.h"

struct gl_context;
struct st_context;
struct ripple_cgl_drawable;

/* What a pixel format asks for (filled from Apple's pixel format attributes). */
struct ripple_cgl_pixel_format {
   unsigned color_bits;          /* 32 (BGRA8) for now */
   unsigned depth_bits;          /* 0, 16, 24 or 32 */
   unsigned stencil_bits;        /* 0 or 8 */
   unsigned accum_bits;          /* 0 or 64 */
   unsigned samples;             /* 0 = no multisampling */
   bool double_buffer;
};

#define RIPPLE_CGL_MAGIC 0x52434758u   /* "RCGX" */

struct ripple_cgl_context {
   struct gl_context *glctx;     /* first: struct ripple_cgl_rend in cgl_dispatch.h */
   unsigned magic;               /* RIPPLE_CGL_MAGIC: thunks can be handed a foreign rend */
   struct st_context *st;
   struct ripple_cgl_pixel_format pf;
   struct st_visual visual;      /* what its drawables are made of */
   struct ripple_cgl_drawable *draw;   /* window drawable, or NULL (FBOs only) */
   unsigned next_fence;          /* GL_APPLE_fence names (cgl_apple.c) */
   /* Window offset (Quartz Extreme, cgl_apple.c): drawing into the screen through a rectangle
    * of it (WindowServer's display surface), window coordinates are shifted by (win_ox,
    * win_oy), the rectangle's GL bottom-left on the screen. */
   uint32_t win_surface;         /* the screen-region surface, or 0 */
   uint32_t attached_surface;    /* the accelerator surface it draws into (any kind), or 0 */
   unsigned surf_h;              /* that surface's height if it isn't the screen region: its
                                  * window coordinates start at the top left (cgl_apple.c) */
   int win_ox, win_oy;
   unsigned win_w, win_h;
   /* Viewport and scissor as the program set them; Mesa holds them shifted while the window's
    * own framebuffer is bound, unshifted while a framebuffer object is. */
   int lv[4], ls[4];
   bool lv_set, ls_set;
   /* GL_UNPACK_CLIENT_STORAGE_APPLE as last set (refused, but emulated: cgl_apple.c). */
   bool client_storage;
};

struct ripple_cgl_context *
ripple_cgl_context_create(const struct ripple_cgl_pixel_format *pf,
                          struct ripple_cgl_context *share);
void ripple_cgl_context_destroy(struct ripple_cgl_context *ctx);

/* Make `ctx` (or nothing) current for Mesa on the calling thread. */
bool ripple_cgl_context_make_current(struct ripple_cgl_context *ctx);

/* The context for `rend` as libGL or CoreGraphics passes it (ours or GLEngine's), or NULL. */
struct ripple_cgl_context *ripple_cgl_context_for(void *rend);

/* Render into `d` from now on (NULL: no drawable). Rebinds if `ctx` is current here. */
void ripple_cgl_context_set_drawable(struct ripple_cgl_context *ctx,
                                     struct ripple_cgl_drawable *d);
/* Read from `read` until called again with NULL (`ctx` must be current). */
void ripple_cgl_context_set_read_drawable(struct ripple_cgl_context *ctx,
                                          struct ripple_cgl_drawable *read);

/* Pbuffers (CoreGraphics' "cgls" ones, which Core Image renders into in WindowServer), kept by
 * the GL plugin (ripple_gl_target.c): the drawable for an accelerator ID, or NULL. */
struct ripple_cgl_drawable *ripple_gld_pbuffer_lookup(uint32_t id, unsigned *width,
                                                      unsigned *height);

#endif
