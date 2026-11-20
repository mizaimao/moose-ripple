#ifndef RB2K_DISPLAY_PRIVATE_H
#define RB2K_DISPLAY_PRIVATE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>
#include "ddsurface.h"
#include "rb2kprobe.h"

typedef struct {
    PDD_SURFACE_GLOBAL global;
    HANDLE owner;
    ULONG id, width, height;
    BOOL locked, writable, lost;
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
    PVOID mapping;
    ULONG mappingBytes;
    VIDEO_MODE_INFORMATION mode;
    HSEMAPHORE ddLock;
    BOOL ddEnabled;
    ULONG pageBytes, visiblePage, lockedPages;
    PDD_SURFACE_GLOBAL flipBack, flipFront;
    HANDLE flipOwner;
    DD_FLIP_VIEW *views, *activeView;
    DD_SLOT slots[RB2K_DD_SLOTS];
} DISPLAY;

BOOL rb_dd_caps(DISPLAY *display, RB2K_DD_CAPS *caps);
void rb_rvd_init(void);
ULONG rb_rvd_escape(DISPLAY *display, ULONG inSize, PVOID in, ULONG outSize, PVOID out);
ULONG rb_rvd_stats(ULONG outSize, PVOID out);
void rb_dd_free_views(DISPLAY *display);
#endif
