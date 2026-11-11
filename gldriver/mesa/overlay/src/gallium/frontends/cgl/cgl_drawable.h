/*
 * Window drawables for the CGL frontend: see cgl_drawable.c.
 */
#ifndef CGL_DRAWABLE_H
#define CGL_DRAWABLE_H

#include <stddef.h>
#include <stdint.h>

#include "frontend/api.h"

struct ripple_cgl_context;

/* Put a finished frame (BGRA8, premultiplied, top row first) into the window. */
typedef void (*ripple_cgl_present_fn)(void *data, const uint8_t *pixels, unsigned stride,
                                      unsigned width, unsigned height);

struct ripple_cgl_drawable {
   struct pipe_frontend_drawable base;     /* first: Mesa hands this pointer back */
   struct st_visual visual;
   unsigned width, height;
   struct pipe_resource *textures[ST_ATTACHMENT_COUNT];
   ripple_cgl_present_fn present;
   void *present_data;
   bool gpu_display;                        /* present by putting the texture on the display */
   uint8_t *pixels;                         /* readback buffer */
   size_t pixels_size;
};

/* A drawable with the context's visual; size 0x0 until set. */
struct ripple_cgl_drawable *ripple_cgl_drawable_create(const struct ripple_cgl_context *ctx);
void ripple_cgl_drawable_set_size(struct ripple_cgl_drawable *d, unsigned width, unsigned height);
void ripple_cgl_drawable_set_present(struct ripple_cgl_drawable *d, ripple_cgl_present_fn fn,
                                     void *data);
/* Present by showing the colour texture itself on the display (zero copy; for the display
 * surface WindowServer composites into) instead of reading it back. */
void ripple_cgl_drawable_set_gpu_display(struct ripple_cgl_drawable *d, bool on);
void ripple_cgl_drawable_destroy(struct ripple_cgl_drawable *d);

/* CGLFlushDrawable (swap_APPLE): present the back buffer (or the front, single-buffered). */
void ripple_cgl_context_swap(struct ripple_cgl_context *ctx);

#endif
