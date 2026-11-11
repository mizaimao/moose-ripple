/*
 * RippleGA.plugin: the 2D graphics-accelerator plugin (IOGraphicsAcceleratorInterface, SDK
 * IOKit/graphics/IOGraphicsInterface.h) for RippleFramebuffer (M5).
 *
 * WindowServer composites the display with OpenGL only if IOPSAllocateBlitEngine() succeeds
 * for the framebuffer at start-up: it loads the plugin named by the framebuffer's
 * IOCFPlugInTypes and needs SetDestination(0, NULL), a copy blitter (kIOBlitTypeCopyRects,
 * kIOBlitSourceDefault) and a solid fill blitter (kIOBlitTypeRects, kIOBlitSourceSolid)
 * (docs/re/qe-display-surface.md). WindowServer then uses them at login. The blits run on the
 * CPU over the framebuffer's memory, mapped through IOFramebuffer's shared connection; the
 * framebuffer's refresh picks the changes up.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/graphics/IOGraphicsInterface.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include "ripple_gpu_uapi.h"
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>


#include <stdio.h>
#include <unistd.h>

/* Debug log when /Library/Logs/RippleGL exists (as RippleGLDriver does). */
static FILE *dbg(void)
{
    static FILE *f;
    static int checked;
    if (!checked) {
        checked = 1;
        if (access("/Library/Logs/RippleGL", W_OK) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "/Library/Logs/RippleGL/RippleGA-%s-%d.log", getprogname(),
                     (int)getpid());
            f = fopen(path, "a");
            if (f)
                setvbuf(f, NULL, _IOLBF, 0);
        }
    }
    return f;
}
#define DBG(...) do { FILE *df_ = dbg(); if (df_) fprintf(df_, __VA_ARGS__); } while (0)

/* Factory UUID (ours): 5A1E1D0A-7C1B-4E39-9C3E-52495050474C */
#define kRippleGAFactoryID \
    CFUUIDGetConstantUUIDWithBytes(NULL, 0x5A, 0x1E, 0x1D, 0x0A, 0x7C, 0x1B, 0x4E, 0x39, \
                                   0x9C, 0x3E, 0x52, 0x49, 0x50, 0x50, 0x47, 0x4C)

typedef struct {
    IOGraphicsAcceleratorInterface *vtbl;      /* first: what callers hold */
    IOGraphicsAcceleratorInterface iface;
    UInt32 refs;
    io_service_t fb;
    io_connect_t conn;
    mach_vm_address_t vram;
    mach_vm_size_t vram_size;
    uint32_t width, height, row_bytes;
} RippleGA;

static RippleGA *self_of(void *thisPointer)
{
    return (RippleGA *)thisPointer;
}

/* The framebuffer's current geometry, published by RippleFramebuffer. */
static void read_geometry(RippleGA *ga)
{
    static const char *const keys[3] = { "RippleScreenWidth", "RippleScreenHeight",
                                         "RippleRowBytes" };
    uint32_t *dst[3] = { &ga->width, &ga->height, &ga->row_bytes };
    for (int i = 0; i < 3; i++) {
        CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingASCII);
        CFTypeRef v = IORegistryEntryCreateCFProperty(ga->fb, k, kCFAllocatorDefault, 0);
        CFRelease(k);
        if (v) {
            if (CFGetTypeID(v) == CFNumberGetTypeID())
                CFNumberGetValue(v, kCFNumberSInt32Type, dst[i]);
            CFRelease(v);
        }
    }
}

static int clip(RippleGA *ga, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > (int32_t)ga->width) *w = ga->width - *x;
    if (*y + *h > (int32_t)ga->height) *h = ga->height - *y;
    return *w > 0 && *h > 0 &&
           (uint64_t)(*y + *h - 1) * ga->row_bytes + (uint64_t)(*x + *w) * 4 <= ga->vram_size;
}

/* ---- Blitters ---------------------------------------------------------------------------- */

static IOReturn fill_rects(void *thisPointer, IOOptionBits options, IOBlitType type,
                           IOBlitSourceType sourceType, IOBlitOperation *operation, void *source)
{
    RippleGA *ga = self_of(thisPointer);
    IOBlitRectangles *r = (IOBlitRectangles *)operation;
    uint32_t color = operation->color0;

    if (!ga->vram)
        return kIOReturnNotReady;
    for (IOItemCount i = 0; i < r->count; i++) {
        int32_t x = r->rects[i].x, y = r->rects[i].y, w = r->rects[i].width, h = r->rects[i].height;
        if (!clip(ga, &x, &y, &w, &h))
            continue;
        for (int32_t row = 0; row < h; row++) {
            uint32_t *p = (uint32_t *)(uintptr_t)(ga->vram + (uint64_t)(y + row) * ga->row_bytes) + x;
            for (int32_t col = 0; col < w; col++)
                p[col] = color;
        }
    }
    return kIOReturnSuccess;
}

static IOReturn copy_rects(void *thisPointer, IOOptionBits options, IOBlitType type,
                           IOBlitSourceType sourceType, IOBlitOperation *operation, void *source)
{
    RippleGA *ga = self_of(thisPointer);
    IOBlitCopyRectangles *r = (IOBlitCopyRectangles *)operation;

    if (!ga->vram)
        return kIOReturnNotReady;
    for (IOItemCount i = 0; i < r->count; i++) {
        IOBlitCopyRectangle c = r->rects[i];
        int32_t sx = c.sourceX, sy = c.sourceY, w = c.width, h = c.height;
        int32_t dx = c.x, dy = c.y;
        if (!clip(ga, &sx, &sy, &w, &h))
            continue;
        dx += sx - c.sourceX;            /* keep source and destination in step after clipping */
        dy += sy - c.sourceY;
        int32_t dw = w, dh = h, ox = dx, oy = dy;
        if (!clip(ga, &dx, &dy, &dw, &dh))
            continue;
        sx += dx - ox;
        sy += dy - oy;
        uint8_t *base = (uint8_t *)(uintptr_t)ga->vram;
        size_t bytes = (size_t)dw * 4;
        if (dy <= sy) {                  /* rows top-down unless moving down over itself */
            for (int32_t row = 0; row < dh; row++)
                memmove(base + (uint64_t)(dy + row) * ga->row_bytes + (uint64_t)dx * 4,
                        base + (uint64_t)(sy + row) * ga->row_bytes + (uint64_t)sx * 4, bytes);
        } else {
            for (int32_t row = dh - 1; row >= 0; row--)
                memmove(base + (uint64_t)(dy + row) * ga->row_bytes + (uint64_t)dx * 4,
                        base + (uint64_t)(sy + row) * ga->row_bytes + (uint64_t)sx * 4, bytes);
        }
    }
    return kIOReturnSuccess;
}

/* ---- IOGraphicsAcceleratorInterface ------------------------------------------------------ */

static HRESULT query_interface(void *thisPointer, REFIID iid, LPVOID *ppv)
{
    RippleGA *ga = self_of(thisPointer);
    CFUUIDRef want = CFUUIDCreateFromUUIDBytes(NULL, iid);
    int ok = CFEqual(want, kIOGraphicsAcceleratorInterfaceID) ||
             CFEqual(want, kIOCFPlugInInterfaceID) || CFEqual(want, IUnknownUUID);
    DBG("QueryInterface: %s\n", ok ? "ok" : "refused");
    CFRelease(want);
    if (!ok) {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ga->refs++;
    *ppv = thisPointer;
    return S_OK;
}

static ULONG add_ref(void *thisPointer)
{
    return ++self_of(thisPointer)->refs;
}

static IOReturn stop(void *thisPointer);

static ULONG release(void *thisPointer)
{
    RippleGA *ga = self_of(thisPointer);
    if (--ga->refs)
        return ga->refs;
    stop(thisPointer);
    free(ga);
    CFPlugInRemoveInstanceForFactory(kRippleGAFactoryID);
    return 0;
}

static IOReturn probe(void *thisPointer, CFDictionaryRef props, io_service_t service, SInt32 *order)
{
    DBG("Probe\n");
    return kIOReturnSuccess;
}

static IOReturn start(void *thisPointer, CFDictionaryRef props, io_service_t service)
{
    RippleGA *ga = self_of(thisPointer);
    kern_return_t kr;

    io_service_t accel = 0;
    UInt32 index = 0;

    ga->fb = service;
    IOObjectRetain(service);
    /* The frame's pixels come from our kext: IOFramebuffer's shared connection doesn't map
     * the VRAM on 10.6, and its server connection is WindowServer's. */
    kr = IOAccelFindAccelerator(service, &accel, &index);
    DBG("Start: IOAccelFindAccelerator 0x%x\n", kr);
    if (kr != KERN_SUCCESS)
        return kr;
    kr = IOServiceOpen(accel, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &ga->conn);
    IOObjectRelease(accel);
    DBG("Start: IOServiceOpen 0x%x\n", kr);
    if (kr != KERN_SUCCESS)
        return kr;
    kr = IOConnectMapMemory64(ga->conn, RIPPLE_GPU_MEMORY_FRAMEBUFFER, mach_task_self(), &ga->vram,
                              &ga->vram_size, kIOMapAnywhere);
    DBG("Start: map VRAM 0x%x -> 0x%llx, %llu bytes\n", kr, (unsigned long long)ga->vram,
        (unsigned long long)ga->vram_size);
    if (kr != KERN_SUCCESS) {
        ga->vram = 0;
        return kr;
    }
    read_geometry(ga);
    DBG("Start: %ux%u, row bytes %u\n", ga->width, ga->height, ga->row_bytes);
    return kIOReturnSuccess;
}

static IOReturn stop(void *thisPointer)
{
    RippleGA *ga = self_of(thisPointer);
    if (ga->vram)
        IOConnectUnmapMemory64(ga->conn, RIPPLE_GPU_MEMORY_FRAMEBUFFER, mach_task_self(), ga->vram);
    ga->vram = 0;
    if (ga->conn)
        IOServiceClose(ga->conn);
    ga->conn = 0;
    if (ga->fb)
        IOObjectRelease(ga->fb);
    ga->fb = 0;
    return kIOReturnSuccess;
}

static IOReturn reset(void *thisPointer, IOOptionBits options)
{
    read_geometry(self_of(thisPointer));
    return kIOReturnSuccess;
}

static IOReturn copy_capabilities(void *thisPointer, FourCharCode select, CFTypeRef *caps)
{
    DBG("CopyCapabilities(0x%x)\n", (unsigned)select);
    if (caps)
        *caps = NULL;
    return kIOReturnUnsupported;
}

static IOReturn flush(void *thisPointer, IOOptionBits options)
{
    return kIOReturnSuccess;
}

static IOReturn synchronize(void *thisPointer, UInt32 options, UInt32 x, UInt32 y, UInt32 w, UInt32 h)
{
    return kIOReturnSuccess;
}

static IOReturn get_beam_position(void *thisPointer, IOOptionBits options, SInt32 *position)
{
    return kIOReturnUnsupported;
}

static IOReturn allocate_surface(void *thisPointer, IOOptionBits options, IOBlitSurface *surface,
                                 void *cgsSurfaceID)
{
    return kIOReturnUnsupported;
}

static IOReturn free_surface(void *thisPointer, IOOptionBits options, IOBlitSurface *surface)
{
    return kIOReturnUnsupported;
}

static IOReturn lock_surface(void *thisPointer, IOOptionBits options, IOBlitSurface *surface,
                             vm_address_t *address)
{
    return kIOReturnUnsupported;
}

static IOReturn unlock_surface(void *thisPointer, IOOptionBits options, IOBlitSurface *surface,
                               IOOptionBits *swapFlags)
{
    return kIOReturnUnsupported;
}

static IOReturn swap_surface(void *thisPointer, IOOptionBits options, IOBlitSurface *surface,
                             IOOptionBits *swapFlags)
{
    return kIOReturnUnsupported;
}

static IOReturn set_destination(void *thisPointer, IOOptionBits options, IOBlitSurface *surface)
{
    DBG("SetDestination(0x%x, %p)\n", (unsigned)options, (void *)surface);
    if (surface)
        return kIOReturnUnsupported;     /* only the framebuffer itself */
    read_geometry(self_of(thisPointer));
    return kIOReturnSuccess;
}

static IOReturn get_blitter(void *thisPointer, IOOptionBits options, IOBlitType type,
                            IOBlitSourceType sourceType, IOBlitterPtr *blitter)
{
    DBG("GetBlitter(0x%x, type %u, source 0x%x)\n", (unsigned)options, (unsigned)type,
        (unsigned)sourceType);
    if (type == kIOBlitTypeCopyRects && sourceType == kIOBlitSourceDefault)
        *blitter = copy_rects;
    else if (type == kIOBlitTypeRects && sourceType == kIOBlitSourceSolid)
        *blitter = fill_rects;
    else
        return kIOReturnUnsupported;
    return kIOReturnSuccess;
}

static IOReturn wait_complete(void *thisPointer, IOOptionBits options)
{
    return kIOReturnSuccess;
}

/* ---- Factory ----------------------------------------------------------------------------- */

void *RippleGAFactory(CFAllocatorRef allocator, CFUUIDRef typeID);

void *RippleGAFactory(CFAllocatorRef allocator, CFUUIDRef typeID)
{
    DBG("Factory\n");
    if (!CFEqual(typeID, kIOGraphicsAcceleratorTypeID))
        return NULL;
    RippleGA *ga = calloc(1, sizeof(*ga));
    if (!ga)
        return NULL;
    IOGraphicsAcceleratorInterface *i = &ga->iface;
    i->QueryInterface = query_interface;
    i->AddRef = add_ref;
    i->Release = release;
    i->version = 1;
    i->revision = 0;
    i->Probe = probe;
    i->Start = start;
    i->Stop = stop;
    i->Reset = reset;
    i->CopyCapabilities = copy_capabilities;
    i->Flush = flush;
    i->Synchronize = synchronize;
    i->GetBeamPosition = get_beam_position;
    i->AllocateSurface = allocate_surface;
    i->FreeSurface = free_surface;
    i->LockSurface = lock_surface;
    i->UnlockSurface = unlock_surface;
    i->SwapSurface = swap_surface;
    i->SetDestination = set_destination;
    i->GetBlitter = get_blitter;
    i->WaitComplete = wait_complete;
    ga->vtbl = i;
    ga->refs = 1;
    CFPlugInAddInstanceForFactory(kRippleGAFactoryID);
    return ga;
}
