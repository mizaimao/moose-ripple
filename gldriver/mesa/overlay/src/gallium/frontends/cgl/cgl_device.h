/*
 * The GPU device behind the CGL frontend: see cgl_device.c.
 */
#ifndef CGL_DEVICE_H
#define CGL_DEVICE_H

#include <stdbool.h>

struct pipe_frontend_screen;
struct ripple_gpu_info;

/* Is there a usable GPU, and what is it? Opens and closes a connection; cheap enough for
 * renderer enumeration, which happens in every process that uses OpenGL. */
bool ripple_cgl_device_info(struct ripple_gpu_info *info);

/* The process's gallium screen, made on first use; NULL if there is no usable GPU. */
struct pipe_frontend_screen *ripple_cgl_device_fscreen(void);

/* Destroy the screen (library termination). */
void ripple_cgl_device_release(void);

#endif
