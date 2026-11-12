/*
 * The GPU device behind the CGL frontend: see cgl_device.c.
 */
#ifndef CGL_DEVICE_H
#define CGL_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_frontend_screen;
struct ripple_gpu_info;
struct ripple_gpu_surface_info;

/* Is there a usable GPU, and what is it? Opens and closes a connection; cheap enough for
 * renderer enumeration, which happens in every process that uses OpenGL. */
bool ripple_cgl_device_info(struct ripple_gpu_info *info);

/* The process's gallium screen, made on first use; NULL if there is no usable GPU. */
struct pipe_frontend_screen *ripple_cgl_device_fscreen(void);

/* Describe accelerator surface `id` (the kernel's surface clients), for drawables on them. */
bool ripple_cgl_device_surface_info(uint32_t id, struct ripple_gpu_surface_info *out);

/* Show `res` (region x, y, w, h) on the display, or present it again; NULL gives the display
 * back to the console framebuffer. */
struct pipe_resource;
bool ripple_cgl_device_display(struct pipe_resource *res, unsigned x, unsigned y, unsigned w,
                               unsigned h);
struct pipe_context;
void ripple_cgl_device_display_dump(struct pipe_context *pipe, struct pipe_resource *res,
                                    bool bottom_up);

/* Quartz Extreme: put screen rectangle (x, y, w, h; top-left origin) into the screen texture
 * and show that. In `src` the rectangle is drawn GL-style with its bottom-left corner at
 * (src_x, src_y). */
struct st_context;
bool ripple_cgl_device_present_region(struct st_context *st, struct pipe_resource *src,
                                      int src_x, int src_y, int x, int y, unsigned w, unsigned h,
                                      unsigned screen_w, unsigned screen_h);

/* Destroy the screen (library termination). */
void ripple_cgl_device_release(void);

#endif
