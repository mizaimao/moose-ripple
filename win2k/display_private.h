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

typedef struct {
    HANDLE miniport;
    HDEV device;
    HPALETTE palette;
    HSURF surface;
    PVOID mapping;
    VIDEO_MODE_INFORMATION mode;
    HSEMAPHORE ddLock;
    BOOL ddEnabled;
    DD_SLOT slots[RB2K_DD_SLOTS];
} DISPLAY;

BOOL rb_dd_caps(DISPLAY *display, RB2K_DD_CAPS *caps);
#endif
