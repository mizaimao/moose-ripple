/*
 * rippled3d.c's interface to the rest of rippledd.dll: the Direct3D 7 HAL.
 */
#ifndef RIPPLED3D_H
#define RIPPLED3D_H

/* Called from DriverInit: fills in the Direct3D parts of the HAL description and adds the 3D
   capabilities to DirectDraw's. */
void d3d_init(RDD_SHARED *s, DDCORECAPS *caps);

/* GetDriverInfo's Direct3D GUIDs: returns 1 if it answered. */
int d3d_driver_info(LPDDHAL_GETDRIVERINFODATA d);

/* A surface is about to be read or shown (Lock, Flip): rendering still on the GPU for it goes
   into its memory first. */
void d3d_surface_access(LPDDRAWI_DDRAWSURFACE_LCL s);

/* A trace line (RIPPLE_D3D_TRACE) with two numbers. */
void d3d_trace(const char *what, DWORD a, DWORD b);

/* DllMain: a process attaches (1) or detaches (0). */
void d3d_process(int begin);

/* A surface is locked for writing: textures made from it are uploaded again before use, and
   a render target's memory goes to the GPU's picture before the next 3D draw. */
void d3d_surface_written(LPDDRAWI_DDRAWSURFACE_LCL s);

/* May a surface in this pixel format go into video memory (textures, z-buffers)? */
int d3d_surface_format_ok(LPDDSURFACEDESC desc);

#endif
