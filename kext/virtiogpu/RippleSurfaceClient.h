/*
 * RippleSurfaceClient: the standard IOAccelerator surface user client (type 0,
 * kIOAccelSurfaceClientType), as the 10.6 SDK's IOAccelSurfaceConnect.h defines it (M5,
 * docs/re/m5-plan.md). CoreGraphics in WindowServer uses it for window-server surfaces: id 1
 * is the display, others are GL windows and backing stores. A surface has a shape (a region
 * in screen coordinates and a framebuffer index) and a backing that Quartz draws into with the
 * CPU (the client's memory, or memory we allocate at the first lock); Flush puts the region on
 * the screen. Design notes: docs/re/vmsvga2-qe-design.md.
 */
#ifndef RIPPLE_SURFACE_CLIENT_H
#define RIPPLE_SURFACE_CLIENT_H

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOAccelSurfaceConnect.h>

class RippleAccelerator;

class RippleSurfaceClient : public IOUserClient
{
    OSDeclareDefaultStructors(RippleSurfaceClient)

    enum { kMaxRegionBytes = 4096, kDisplaySurfaceID = 1 };

    task_t             fTask;
    RippleAccelerator *fAccel;
    IOLock            *fLock;

    uint32_t           fID;            // window-server surface id (SetIDMode); 0 = none yet
    uint32_t           fMode;          // eIOAccelSurfaceModeBits
    uint32_t           fFramebuffer;   // framebuffer index from the shape calls
    IOAccelDeviceRegion *fRegion;      // last shape, kernel copy
    uint32_t           fRegionBytes;
    IOAccelSurfaceScaling fScale;
    bool               fHasScale;

    // Buffer geometry: what a lock hands out.
    uint32_t           fBufWidth, fBufHeight, fRowBytes;
    int32_t            fOriginX, fOriginY;   // screen position of buffer pixel (0,0)

    // Backing: the client's own memory, or ours mapped into the client.
    IOMemoryDescriptor *fClientMem;
    mach_vm_address_t  fClientAddr;
    uint64_t           fClientLength;
    IOBufferMemoryDescriptor *fOwnMem;
    IOMemoryMap       *fOwnMap;

    bool               fReadLocked, fWriteLocked;

    void releaseBacking();
    IOReturn setShape(uint32_t options, uint32_t framebuffer, mach_vm_address_t backing,
                      uint32_t rowBytes, uint64_t length, const void *region, uint32_t regionBytes);
    IOReturn lock(bool write, IOAccelSurfaceInformation *info);
    IOReturn flush(uint32_t framebufferMask, uint32_t options);
    IOReturn control(uint32_t selector, uint32_t arg, uint64_t *result);

public:
    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type);
    virtual bool start(IOService *provider);
    virtual IOReturn clientClose(void);
    virtual IOReturn clientDied(void);
    virtual void free(void);
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference);

    uint32_t surfaceID() const { return fID; }
};

#endif
