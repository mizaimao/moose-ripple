/*
 * The ripple virgl winsys: a gallium screen for the virgl driver over a moose-ripple GPU
 * connection (docs/kext-uapi.md in the moose-ripple repo).
 */
#ifndef RIPPLE_VIRGL_PUBLIC_H
#define RIPPLE_VIRGL_PUBLIC_H

#include <IOKit/IOKitLib.h>

struct pipe_screen;
struct pipe_screen_config;
struct ripple_gpu_info;

/* `conn` is an open RIPPLE_GPU_CLIENT_TYPE connection and `info` its GET_INFO answer, with
 * backend RIPPLE_GPU_BACKEND_VIRGL. The screen owns the connection from here on: it is
 * closed when the screen is destroyed, or right away if this fails (NULL). */
struct pipe_screen *ripple_virgl_screen_create(io_connect_t conn,
                                               const struct ripple_gpu_info *info,
                                               const struct pipe_screen_config *config);

#endif
