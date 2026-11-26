/*
 * The virgl winsys for Windows 9x (moose-ripple). MIT licence, as the rest of Mesa's virgl code.
 */
#ifndef RVD_VIRGL_PUBLIC_H
#define RVD_VIRGL_PUBLIC_H

#include <stdbool.h>

struct pipe_context;
struct pipe_resource;
struct pipe_screen;

/* Opens the mini-VDD, makes a context on the host and a virgl screen on it. NULL when the VxD
   isn't loaded or the host has no virgl. */
struct pipe_screen *rvd_virgl_screen_create(void);

/* Show the Direct3D HAL's top-first colour buffer. The caller owns the cached scanout image. */
bool rvd_virgl_present_color(struct pipe_context *pipe, struct pipe_resource *color,
                             struct pipe_resource **image);

/* Whether a full-screen window may be shown from the GPU at all (the VxD's display path on
   Windows 9x, the display driver's GPU desktop on Windows 2000 and XP), and the desktop back
   on the display after such frames. */
bool rvd_virgl_window_scanout(struct pipe_screen *screen);
bool rvd_virgl_present_window(struct pipe_context *pipe, struct pipe_resource *color,
                              struct pipe_resource **image, long x, long y,
                              const long *rects, unsigned count);
void rvd_virgl_show_desktop(struct pipe_screen *screen);

#endif
