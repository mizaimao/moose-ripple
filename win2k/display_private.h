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
    BOOL nativeD3D7;
    BOOL z16Storage, z16Render;
    HRESULT (CALLBACK *parseD3DExtension)(PVOID command, PVOID *next);
    RB2K_D3D7_DIAGNOSTICS d7;
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
