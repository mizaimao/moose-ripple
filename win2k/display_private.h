#ifndef RB2K_DISPLAY_PRIVATE_H
#define RB2K_DISPLAY_PRIVATE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>
#include "ddsurface.h"
#include "rb2kprobe.h"
#include "rb2kcursor.h"
#include "rb2kd3d7.h"

typedef struct {
    PDD_SURFACE_GLOBAL global;
    HANDLE owner;
    ULONG id, width, height;
    BOOL locked, writable, lost, depth16;
    BOOL stencil;               /* a depth buffer of 24 bits with 8 of stencil (depth16 set too) */
    BOOL alpha;                 /* a render target whose format keeps alpha */
    PDD_DIRECTDRAW_LOCAL local;
    ULONG d3dHandle;
} DD_SLOT;

typedef struct DD_FLIP_VIEW {
    struct DD_FLIP_VIEW *next;
    HANDLE owner, process;
    FLATPTR address;
    ULONG references;
} DD_FLIP_VIEW;

typedef struct {
    HANDLE miniport;
    HDEV device;
    HPALETTE palette;
    HSURF surface;
    HSURF bitmap;
    SURFOBJ *cpuSurface;
    PVOID mapping;
    ULONG mappingBytes;
    VIDEO_MODE_INFORMATION mode;
    ULONG bitmapFormat;
    ULONG paletteColors[256];
    BOOL paletteActive;
    HSEMAPHORE ddLock;
    HSEMAPHORE cursorLock;
    BOOL cursorOwned;
    BOOL sampleCursor;
    RB2K_CURSOR_SOURCE cursorSource;
    BOOL ddEnabled;
    BOOL glActive;
    BOOL gpuSurfaces;           /* offscreen surfaces as host textures (RB2K_DD_CAPS) */
    BOOL plainSurfaces;         /* plain 2D ones too, not only Direct3D's */
    BOOL d3d8;                  /* the Direct3D 8 driver interface is offered */
    BOOL vertexShaders;         /* and vertex shaders run on the host GPU */
    BOOL d3dDescribed;          /* the Direct3D HAL description is given in every depth */
    BOOL nativeD3D7;
    BOOL z16Storage, z16Render;
    BOOL z24s8;                 /* depth buffers with stencil */
    HRESULT (CALLBACK *parseD3DExtension)(PVOID command, PVOID *next);
    RB2K_D3D7_DIAGNOSTICS d7;
    RB2K_D7_DRAWSTATS d7draw;
    ULONG pageBytes, visiblePage, lockedPages;
    PDD_SURFACE_GLOBAL flipBack, flipFront;
    HANDLE flipOwner;
    DD_FLIP_VIEW *views, *activeView;
    DD_SLOT slots[RB2K_DD_SLOTS];
} DISPLAY;

BOOL rb_dd_caps(DISPLAY *display, RB2K_DD_CAPS *caps);
BOOL rb_cursor_stats(DISPLAY *d, RB2K_CURSOR_STATS *stats);
void rb_display_cursor_release(DISPLAY *d);
DD_SLOT *rb_dd_slot(DISPLAY *display, PDD_SURFACE_GLOBAL surface);
BOOL rb_dd_mapped_primary(const DISPLAY *display, const DD_SURFACE_LOCAL *surface);
void rb_d7_init(void);
void rb_d7_disable(DISPLAY *display);
void rb_d7_caps(DD_HALINFO *info, BOOL depth);
BOOL rb_d7_info(PDD_GETDRIVERINFODATA data);
ULONG rb_d7_stats(DISPLAY *display, ULONG size, PVOID out);
ULONG rb_d7_drawstats(DISPLAY *display, ULONG size, PVOID out);
/* Direct3D texture formats, each converted to B8G8R8A8 for the host. */
/* Texture formats; the DXT ones are stored as 4x4 blocks (DXT2 and DXT4
 * decode as DXT3 and DXT5). */
enum { TexNone, TexArgb8888, TexXrgb8888, TexRgb565, TexArgb1555, TexXrgb1555, TexArgb4444, TexDxt1, TexDxt3,
       TexDxt5, TexFormats };
ULONG rb_d7_texture_format(const DDPIXELFORMAT *format, ULONG desktopBits);
ULONG rb_d7_texel_bytes(ULONG format);
/* A level's bytes per row (of texels, or of 4x4 blocks) and its rows. */
ULONG rb_d7_row_bytes(ULONG format, ULONG width);
ULONG rb_d7_rows(ULONG format, ULONG height);
void rb_d7_texture_written(DISPLAY *d, PDD_SURFACE_GLOBAL g);
extern ULONG rb_dd_mode_stats[3][4];
void rb_d7_surface_destroyed(DISPLAY *display, PDD_SURFACE_GLOBAL surface);
HPALETTE rb_create_palette(DISPLAY *display);
BOOL rb_restore_palette(DISPLAY *display);
void rb_rvd_init(void);
ULONG rb_rvd_escape(DISPLAY *display, ULONG inSize, PVOID in, ULONG outSize, PVOID out);
ULONG rb_rvd_stats(DISPLAY *display, ULONG outSize, PVOID out);
ULONG rb_gl_stats(DISPLAY *display, ULONG operation, ULONG bytes, PVOID out);
BOOL rb_gl_device_surface(DISPLAY *d);
void rb_gl_cpu_lock(ULONG writer, ULONG rop);
void rb_gl_cpu_unlock(void);
BOOL rb_gl_cpu_access(DISPLAY *d, const RECTL *rect, ULONG access);
BOOL rb_gl_cpu_flush(DISPLAY *d);
void rb_dd_free_views(DISPLAY *display);
#endif
