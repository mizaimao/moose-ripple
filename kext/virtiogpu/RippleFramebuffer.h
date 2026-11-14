/*
 * RippleFramebuffer: IOFramebuffer for QEMU's virtio-gpu (M2, 2D). It outranks the boot
 * IONDRVFramebuffer on the same PCI device. The framebuffer lives in one physically
 * contiguous block of guest RAM (the "VRAM"), attached as backing to a 2D host resource that
 * is scanned out; a 60 Hz timer copies it to the host. Hardware cursor via the cursor queue.
 */
#ifndef RIPPLE_FRAMEBUFFER_H
#define RIPPLE_FRAMEBUFFER_H

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOTimerEventSource.h>
#include "VirtioGPUDevice.h"
#include <IOKit/graphics/IOAccelTypes.h>

class RippleFramebuffer : public IOFramebuffer
{
    OSDeclareDefaultStructors(RippleFramebuffer)

    enum {
        kMaxModes = 24,
        kVRAMSize = 32 * 1024 * 1024,
        kCursorSize = 64,
        kRefreshMS = 16,             // while the screen is changing (about 60 Hz)
        kIdleRefreshMS = 50,         // after kIdleFrames unchanged frames (20 Hz)
        kIdleFrames = 30,
    };
    struct Mode { uint32_t width, height; };

    VirtioGPUDevice          *fDev;
    IOBufferMemoryDescriptor *fVRAM;
    uint64_t                  fVRAMPA;

    Mode            fModes[kMaxModes];
    unsigned        fModeCount;
    unsigned        fPreferred;          // index into fModes
    IODisplayModeID fCurMode, fStartupMode;
    uint32_t        fScanoutRes;
    uint32_t        fShownRes;       // a GPU buffer on the display instead of the VRAM (0: none)
    VirtioGPUDevice::Rect fShownRect; // its rectangle as last set as the scanout
    uint32_t        fShownFormat;    // its pixel format, as its owner made it (backend's number)

    IOTimerEventSource *fRefresh;
    bool                fEnabled;

    IOBufferMemoryDescriptor *fShadow;    // last frame sent to the host, for damage tracking
    bool                      fFullRefresh;
    uint32_t                  fStatTicks, fStatFrames;   // refresh statistics for the log
    uint32_t                  fUnchanged;                // consecutive frames with no damage
    uint64_t                  fStatPixels, fStatTime;

    bool                      fHWCursor;     // hardware cursor (else IOFramebuffer draws it)
    IOBufferMemoryDescriptor *fCursorMem;
    uint32_t fCursorRes;
    bool     fCursorVisible, fCursorShown;
    SInt32   fCursorX, fCursorY;
    uint32_t fCursorHotX, fCursorHotY;   // hot spot of the current image
    uint32_t fCursorLogs;

    bool  buildModes(uint32_t prefW, uint32_t prefH);
    bool  validMode(IODisplayModeID m) const { return m >= 1 && m <= (IODisplayModeID)fModeCount; }
    const Mode &mode(IODisplayModeID m) const { return fModes[m - 1]; }
    bool  applyMode(IODisplayModeID m);
    bool  refresh();             // send what changed since the last frame; false if nothing
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn showAction(OSObject *owner, void *id, void *rect, void *, void *);
    static IOReturn flushAction(OSObject *owner, void *id, void *queued, void *, void *);
    static IOReturn copyAction(OSObject *owner, void *args, void *, void *, void *);
    void  showCursor();
    void  publishCapsets();     // host GPU capability sets into the registry (diagnostics)

public:
    VirtioGPUDevice *device() const { return fDev; }   // for RippleAccelerator
    IOMemoryDescriptor *vram() const { return fVRAM; }   // for 2D blitters (uapi)

    // Show GPU resource `id` (region r of it) on the display instead of the framebuffer, or
    // present it again if it is already shown; id 0 gives the display back to the framebuffer.
    // For the GPU client (uapi DISPLAY); serialized with the refresh timer.
    IOReturn showResource(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    // The client is going away or destroying `id`: give the display back if it shows `id`.
    void releaseResource(uint32_t id);

    // Surfaces (RippleSurfaceClient): the current screen size, and copying rectangles of a
    // surface's backing (32-bit pixels, rowBytes apart, its pixel (0,0) at screen (ox, oy))
    // into the frame. Serialized with the refresh timer.
    void screenSize(uint32_t *width, uint32_t *height);

    // The GPU buffer on the display now (0: none) with its format, and showing it again after
    // something was copied into it (the GPU client's SURFACE_PRESENT); fails if another buffer
    // is on the display by then.
    uint32_t shownResource(uint32_t *format) const { *format = fShownFormat; return fShownRes; }
    void setShownFormat(uint32_t format) { fShownFormat = format; }
    IOReturn flushShown(uint32_t id, bool wait = true);   // !wait: queued only
    IOReturn copyToScreen(IOMemoryDescriptor *src, uint32_t rowBytes, int32_t ox, int32_t oy,
                          const IOAccelBounds *rects, uint32_t count);

    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();

    virtual IOReturn enableController(void);
    virtual bool isConsoleDevice(void);

    virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture);
    virtual IODeviceMemory *getVRAMRange(void);
    virtual const char *getPixelFormats(void);
    virtual IOItemCount getDisplayModeCount(void);
    virtual IOReturn getDisplayModes(IODisplayModeID *allDisplayModes);
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                                  IODisplayModeInformation *info);
    virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth);
    virtual IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                         IOPixelAperture aperture, IOPixelInformation *pixelInfo);
    virtual IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth);
    virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth);
    virtual IOReturn setStartupDisplayMode(IODisplayModeID displayMode, IOIndex depth);
    virtual IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth);

    virtual IOReturn getAttribute(IOSelect attribute, uintptr_t *value);
    virtual IOReturn setAttribute(IOSelect attribute, uintptr_t value);
    virtual IOItemCount getConnectionCount(void);
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t *value);
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t value);

    virtual IOReturn setCursorImage(void *cursorImage);
    virtual IOReturn setCursorState(SInt32 x, SInt32 y, bool visible);
};

#endif
