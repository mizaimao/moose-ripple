/*
 * The virgl winsys for Windows 9x (moose-ripple). MIT licence, as the rest of Mesa's virgl code.
 */
#ifndef RVD_VIRGL_PUBLIC_H
#define RVD_VIRGL_PUBLIC_H

struct pipe_screen;

/* Opens the mini-VDD, makes a context on the host and a virgl screen on it. NULL when the VxD
   isn't loaded or the host has no virgl. */
struct pipe_screen *rvd_virgl_screen_create(void);

#endif
