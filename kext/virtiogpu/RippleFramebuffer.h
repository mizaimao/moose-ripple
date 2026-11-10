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
    uint32_t        fScanoutRes, fNextRes;

    IOTimerEventSource *fRefresh;
    bool                fEnabled;

    IOBufferMemoryDescriptor *fShadow;    // last frame sent to the host, for damage tracking
    bool                      fFullRefresh;
    uint32_t                  fStatTicks, fStatFrames;   // refresh statistics for the log
    uint32_t                  fUnchanged;                // consecutive frames with no damage
    uint64_t                  fStatPixels, fStatTime;

    IOBufferMemoryDescriptor *fCursorMem;
    uint32_t fCursorRes;
    bool     fCursorVisible, fCursorShown;
    SInt32   fCursorX, fCursorY;
    uint32_t fCursorHotX, fCursorHotY;

    bool  buildModes(uint32_t prefW, uint32_t prefH);
    bool  validMode(IODisplayModeID m) const { return m >= 1 && m <= (IODisplayModeID)fModeCount; }
    const Mode &mode(IODisplayModeID m) const { return fModes[m - 1]; }
    bool  applyMode(IODisplayModeID m);
    bool  refresh();             // send what changed since the last frame; false if nothing
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender);
    void  showCursor();

public:
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
