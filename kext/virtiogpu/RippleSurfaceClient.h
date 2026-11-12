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
#include "ripple_gpu_uapi.h"

class RippleAccelerator;

class RippleSurfaceClient : public IOUserClient
{
    OSDeclareDefaultStructors(RippleSurfaceClient)

    enum { kMaxRegionBytes = 65536, kDisplaySurfaceID = 1 };   // a shape of up to 4094 rectangles

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
    bool               fScreenRegion;    // shaped without backing: its contents go on screen
    uint32_t           fShapeOptions;    // eIOAccelSurfaceShapeBits of the last shape

    // The frame a GPU client last presented on this surface (RippleGPUClient::surfacePresent):
    // what a read lock hands out when WindowServer draws the window itself (a moving window,
    // Exposé). The client's buffer lives on the GPU only, so a read lock copies it, on the GPU,
    // into a buffer of ours whose backing is the lock memory (fOwnMem), and reads that back.
    uint32_t           fFrameHandle, fFrameContext, fFrameFormat, fFrameWidth, fFrameHeight;
    bool               fFrameTopFirst;
    uint32_t           fStage, fStageWidth, fStageHeight, fStageContext;
    bool               fOwnWired;        // fOwnMem is prepared (it backs fStage)
    IOBufferMemoryDescriptor *fStageCmd; // one page for the copy command
    uint64_t           fStageCmdPA;
    void dropStage();
    void fetchFrame();
    uint32_t           fShapeSeq;

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
    // For RippleAccelerator::describeSurface (called with the accelerator's surface lock held).
    bool describe(task_t asker, struct ripple_gpu_surface_info *out);
    bool screenShape(IOAccelBounds *bounds, IOAccelBounds *rects, uint32_t maxRects,
                     uint32_t *count);
    // The GPU client's latest frame for this surface, and its buffer or context going away
    // (0 matches nothing).
    void setFrame(uint32_t handle, uint32_t context, uint32_t format, uint32_t width,
                  uint32_t height, bool topFirst);
    void forgetFrame(uint32_t handle, uint32_t context);
};

#endif
