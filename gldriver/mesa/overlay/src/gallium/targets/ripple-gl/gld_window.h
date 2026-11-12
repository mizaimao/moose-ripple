/*
 * Showing GL frames in windows: see gld_window.c.
 */
#ifndef GLD_WINDOW_H
#define GLD_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

struct gld_window;

/* gldAttachDrawable type 0x50: ids = {connection, window, surface}. Returns the window record
 * (shared by every context attached to the same surface; `current` is the caller's present
 * one, if any) or NULL. *changed: new, or a new size. */
struct gld_window *gld_window_attach(const uint32_t ids[3], struct gld_window *current,
                                     bool *changed);
/* The context leaves the surface. The record stays (it holds what the surface shows) until
 * the surface itself is gone. */
void gld_window_release(struct gld_window *w);
/* Data the caller keeps with the surface (its render targets), freed with the record. */
void *gld_window_user(const struct gld_window *w);
void gld_window_set_user(struct gld_window *w, void *user, void (*user_free)(void *));
void gld_window_size(const struct gld_window *w, unsigned *width, unsigned *height);
void gld_window_set_vsync(struct gld_window *w, bool vsync);
/* kCGLCESwapRectangle and kCGLCPSwapRectangle: with `on`, a present updates only `rect` of the
 * window (x, y from the bottom, width, height); the rest keeps what it showed. Core Animation
 * redraws just the changed area and relies on that (M6, Real Racing 2's intro video). */
void gld_window_set_swap_rect(struct gld_window *w, bool on, const int rect[4]);
/* The window's accelerator surface ID if the kernel shows its frames itself (the window was
 * bound as accelerated), else 0: then frames go through gld_window_present. */
uint32_t gld_window_direct_surface(const struct gld_window *w);

/* A ripple_cgl_present_fn for drawables shown in `data` (a struct gld_window). */
void gld_window_present(void *data, const uint8_t *pixels, unsigned stride, unsigned width,
                        unsigned height);

/* gld_opacity.c: has the app made this surface non-opaque (CGSSetSurfaceOpacity)? */
bool gld_surface_translucent(uint32_t cid, uint32_t wid, uint32_t sid);

#endif
