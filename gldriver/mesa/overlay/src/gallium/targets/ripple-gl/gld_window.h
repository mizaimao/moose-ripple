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
void gld_window_release(struct gld_window *w);
void gld_window_size(const struct gld_window *w, unsigned *width, unsigned *height);
void gld_window_set_vsync(struct gld_window *w, bool vsync);

/* A ripple_cgl_present_fn for drawables shown in `data` (a struct gld_window). */
void gld_window_present(void *data, const uint8_t *pixels, unsigned stride, unsigned width,
                        unsigned height);

#endif
