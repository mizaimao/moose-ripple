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
#ifndef DDSCAPS2_VOLUME
#define DDSCAPS2_VOLUME 0x00200000L     /* DirectX 8: a volume texture (the Me DDK predates it) */
#endif

/* Whether the HAL offers the Direct3D 8 interface (GetDriverInfo2); read at d3d_init. */
int d3d8_enabled(void);
/* A DXT format's 4x4 block bytes (8 or 16) when Direct3D 8 is on, else 0. */
DWORD d3d_dxt_block(const DDPIXELFORMAT *pf);
/* An uncompressed format named by FOURCC (a D3DFORMAT): its bytes a texel, else 0. */
DWORD d3d_fourcc_bytes(const DDPIXELFORMAT *pf);

/* Direct3D 8 vertex and index buffers the driver allocates (with hardware vertex processing,
   unless D3D8Buffers is "0"): memory for one (NULL if none is left), its end, and a write to
   it (Lock), after which a draw uploads it to the GPU again. */
#define D3D_BUFFER_MARK 0x46554252UL    /* a buffer surface's GBL dwReserved1: the driver allocated it */
int d3d_buffers_on(void);
void *d3d_buffer_new(DWORD size);
void d3d_buffer_free(FLATPTR mem);
void d3d_buffer_written(FLATPTR mem);
void d3d_buffer_lock(FLATPTR mem, int locked);

/* A surface is about to be read or shown (Lock, Flip): rendering still on the GPU for it goes
   into its memory first. */
int d3d_surface_access(LPDDRAWI_DDRAWSURFACE_LCL s);
int d3d_present(LPDDRAWI_DDRAWSURFACE_LCL front, LPDDRAWI_DDRAWSURFACE_LCL target);
int d3d_leave_exclusive(void);
int rdd_prepare_scanout(void);
int rdd_desktop(DWORD offset);
/* The memory is a whole page that can be shown: in the card's memory at a line the display can
   start at, or a back buffer in the system heap. */
int rdd_shown_page(FLATPTR mem);
void d3d_profile_flip(int gpu);

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
