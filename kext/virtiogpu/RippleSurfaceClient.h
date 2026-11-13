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

    // The frame a GPU client last presented on this surface (presentFrame). It lives on the GPU
    // only. From there it goes, by copies on the GPU in the client's context:
    // - onto the screen, where the surface is a plain region of it (at a present, and when
    //   WindowServer asks with Flush);
    // - into the compositor's texture of this surface, if WindowServer has made one (setTexture):
    //   a buffer of its GL driver, kept filled at every present. WindowServer attaches a surface
    //   to a texture once and then only draws with it; on Apple's drivers that texture is the
    //   surface's own front buffer;
    // - into the memory a read lock hands out (fetchFrame): a buffer of ours over that memory
    //   (fStage) is copied into and read back.
    uint32_t           fFrameHandle, fFrameContext, fFrameFormat, fFrameWidth, fFrameHeight;
    bool               fFrameTopFirst;
    uint32_t           fTexHandle, fTexFormat, fTexWidth, fTexHeight, fTexContext;
    uint32_t           fScreenRes, fScreenContext;   // the screen buffer, attached to that context
    uint32_t           fStage, fStageWidth, fStageHeight, fStageContext;
    bool               fOwnWired;        // fOwnMem is prepared (it backs fStage)
    enum { kCommandBytes = 2 * 4096, kBlitDwords = 22,
           kMaxShowRects = kCommandBytes / (4 * kBlitDwords) - 2 };
    IOBufferMemoryDescriptor *fCmd;      // for the copy commands
    uint64_t           fCmdPA;
    volatile UInt32    fCmdBusy;         // 1 while the device still has to read fCmd
    bool ensureCommands();
    bool attachTo(uint32_t buffer, uint32_t *attached);
    uint32_t blitCommand(uint32_t *c, uint32_t dst, uint32_t dstFormat, int32_t dx, int32_t dy,
                         int32_t w, int32_t h, int32_t sx, int32_t top);
    bool pushFrame(bool toTexture, bool toScreen);
    bool copyFrame(uint32_t dst, uint32_t dstFormat, uint32_t dstWidth, uint32_t dstHeight,
                   uint32_t *attached);
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
    // A GPU client's finished frame for this surface; true if it went on the screen. And its
    // buffer or context going away (0 matches nothing).
    bool presentFrame(uint32_t handle, uint32_t context, uint32_t format, uint32_t width,
                      uint32_t height, bool topFirst);
    void forgetFrame(uint32_t handle, uint32_t context);
    // A buffer to keep filled with the latest frame (0 = none), or with `once` to fill now
    // only; true if it holds a frame now.
    bool setTexture(uint32_t handle, uint32_t format, uint32_t width, uint32_t height, bool once);
};

#endif
