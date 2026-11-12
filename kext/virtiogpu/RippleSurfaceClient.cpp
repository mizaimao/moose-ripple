/*
 * RippleSurfaceClient: see RippleSurfaceClient.h.
 *
 * Methods (IOAccelSurfaceConnect.h order) and the arguments IOKit's client functions send:
 *   0 ReadLockOptions   in: options              out: IOAccelSurfaceInformation
 *   1 ReadUnlockOptions in: options
 *   2 GetState                                   out: state scalar (0)
 *   3 WriteLockOptions  in: options              out: IOAccelSurfaceInformation
 *   4 WriteUnlockOptions in: options
 *   5 Read              in: IOAccelSurfaceReadData (unsupported)
 *   6 SetShapeBacking   in: options, framebuffer, backing, rowBytes + IOAccelDeviceRegion
 *   7 SetIDMode         in: id, mode
 *   8 SetScale          in: options + IOAccelSurfaceScaling
 *   9 SetShape          in: options, framebuffer + IOAccelDeviceRegion
 *  10 Flush             in: framebuffer mask, options
 *  11 QueryLock
 *  12-15 ReadLock, ReadUnlock, WriteLock, WriteUnlock (no options)
 *  16 Control           in: selector, argument   out: result
 *  17 SetShapeBackingAndLength in: options, framebuffer, backing, rowBytes, length + region
 * Implemented on externalMethod so 64-bit backing addresses arrive whole.
 */
#include "RippleSurfaceClient.h"
#include "RippleAccelerator.h"
#include "RippleFramebuffer.h"

#include <IOKit/IOLib.h>
#include <kern/task.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RippleSurfaceClient, IOUserClient)

#define LOG(fmt, ...) IOLog("RippleSurfaceClient: " fmt "\n", ##__VA_ARGS__)

// ---- Lifecycle ------------------------------------------------------------------------------

bool RippleSurfaceClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    // The task is used later (its memory is wrapped and mapped into), so hold it: the
    // connection can outlive the task if its send right was handed to another process.
    fTask = owningTask;
    task_reference(fTask);
    fLock = IOLockAlloc();
    return fLock != 0;
}

bool RippleSurfaceClient::start(IOService *provider)
{
    fAccel = OSDynamicCast(RippleAccelerator, provider);
    if (!fAccel || !super::start(provider))
        return false;
    fAccel->addSurface(this);
    return true;
}

void RippleSurfaceClient::releaseBacking()
{
    if (fClientMem) {
        fClientMem->complete();
        fClientMem->release();
        fClientMem = 0;
    }
    fClientAddr = 0;
    fClientLength = 0;
    dropStage();                        // it reads into fOwnMem
    if (fOwnMap) {
        fOwnMap->release();             // unmaps from the client
        fOwnMap = 0;
    }
    if (fOwnMem) {
        if (fOwnWired)
            fOwnMem->complete();
        fOwnWired = false;
        fOwnMem->release();
        fOwnMem = 0;
    }
}

IOReturn RippleSurfaceClient::clientClose(void)
{
    if (fAccel)
        fAccel->removeSurface(this);
    IOLockLock(fLock);
    releaseBacking();
    IOLockUnlock(fLock);
    terminate();
    return kIOReturnSuccess;
}

IOReturn RippleSurfaceClient::clientDied(void)
{
    return clientClose();
}

void RippleSurfaceClient::free(void)
{
    releaseBacking();
    if (fStageCmd) {
        fStageCmd->complete();
        fStageCmd->release();
        fStageCmd = 0;
    }
    if (fRegion)
        IOFree(fRegion, fRegionBytes);
    if (fLock)
        IOLockFree(fLock);
    if (fTask) {
        task_deallocate(fTask);
        fTask = 0;
    }
    super::free();
}

// ---- Shape and backing ----------------------------------------------------------------------

IOReturn RippleSurfaceClient::setShape(uint32_t options, uint32_t framebuffer,
                                       mach_vm_address_t backing, uint32_t rowBytes,
                                       uint64_t length, const void *region, uint32_t regionBytes)
{
    const IOAccelDeviceRegion *rgn = (const IOAccelDeviceRegion *)region;

    if (!rgn || regionBytes < sizeof(IOAccelDeviceRegion) || regionBytes > kMaxRegionBytes ||
        IOACCEL_SIZEOF_DEVICE_REGION(rgn) > regionBytes)
        return kIOReturnBadArgument;
    const IOAccelBounds &b = rgn->bounds;
    if (fAccel->qeDebug())
        IOLog("RippleSurfaceClient: surface %u mode 0x%x shape: options 0x%x fb %u backing 0x%llx rowbytes %u, bounds %d,%d %dx%d, %u rects\n",
              fID, fMode, options, framebuffer, backing, rowBytes, b.x, b.y, b.w, b.h, rgn->num_rects);
    if (b.w <= 0 || b.h <= 0)
        return kIOReturnSuccess;        // empty shape: nothing to do

    // Buffer geometry: the size of the bounds (or of the buffer SetScale gave), placed at the
    // bounds' screen position. WindowServer shapes its display surface (id 1) to each frame's
    // dirty rectangle and renders just that rectangle (M5, 2026-11-10).
    uint32_t w, h;
    int32_t ox, oy;
    if (fHasScale && !(options & kIOAccelSurfaceShapeIdentityScaleBit)) {
        w = fScale.buffer.w > 0 ? fScale.buffer.w : b.w;
        h = fScale.buffer.h > 0 ? fScale.buffer.h : b.h;
        ox = b.x;
        oy = b.y;
    } else {
        w = b.w;
        h = b.h;
        ox = b.x;
        oy = b.y;
    }
    uint32_t minRow = w * 4;

    if (backing) {
        if (!rowBytes)
            rowBytes = minRow;
        if (!length)
            length = (uint64_t)rowBytes * h;
        if (rowBytes < minRow || length < (uint64_t)rowBytes * h)
            return kIOReturnBadArgument;
        if (backing != fClientAddr || length != fClientLength) {
            releaseBacking();
            IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(backing, length,
                                                                          kIODirectionInOut, fTask);
            if (!md)
                return kIOReturnNoMemory;
            if (md->prepare() != kIOReturnSuccess) {
                md->release();
                return kIOReturnVMError;
            }
            fClientMem = md;
            fClientAddr = backing;
            fClientLength = length;
        }
    } else {
        rowBytes = minRow;
        if (fClientMem || (fOwnMem && fOwnMem->getLength() < (uint64_t)rowBytes * h))
            releaseBacking();           // made again, bigger, at the next lock
    }

    void *copy = IOMalloc(regionBytes);
    if (!copy)
        return kIOReturnNoMemory;
    memcpy(copy, region, regionBytes);
    if (fRegion)
        IOFree(fRegion, fRegionBytes);
    fRegion = (IOAccelDeviceRegion *)copy;
    fRegionBytes = regionBytes;
    fFramebuffer = framebuffer;
    fBufWidth = w;
    fBufHeight = h;
    fRowBytes = rowBytes;
    fOriginX = ox;
    fOriginY = oy;
    fScreenRegion = !backing;           // SetShape without backing: a region of the screen
    fShapeOptions = options;
    fShapeSeq++;
    return kIOReturnSuccess;
}

// ---- Locks ----------------------------------------------------------------------------------

IOReturn RippleSurfaceClient::lock(bool write, IOAccelSurfaceInformation *info)
{
    if (!fID || !fBufWidth || !fBufHeight)
        return kIOReturnNotReady;
    if (write ? fWriteLocked : fReadLocked)
        return kIOReturnCannotLock;

    mach_vm_address_t addr;
    if (fClientMem) {
        addr = fClientAddr;
    } else {
        uint64_t bytes = (uint64_t)fRowBytes * fBufHeight;
        if (!fOwnMem) {
            fOwnMem = IOBufferMemoryDescriptor::withOptions(
                kIODirectionInOut | kIOMemoryKernelUserShared, round_page(bytes), PAGE_SIZE);
            if (!fOwnMem)
                return kIOReturnNoMemory;
            bzero(fOwnMem->getBytesNoCopy(), fOwnMem->getLength());
            fOwnMap = fOwnMem->createMappingInTask(fTask, 0, kIOMapAnywhere);
            if (!fOwnMap) {
                fOwnMem->release();
                fOwnMem = 0;
                return kIOReturnNoMemory;
            }
        }
        addr = fOwnMap->getAddress();
        if (!write)
            fetchFrame();               // a GPU client's window: its latest frame
    }

    bzero(info, sizeof(*info));
    info->address[0] = addr;
    info->rowBytes = fRowBytes;
    info->width = fBufWidth;
    info->height = fBufHeight;
    info->pixelFormat = 'BGRA';         // 8888: B, G, R, A in memory
    // A read lock is how WindowServer takes a window's surface to draw the window itself (a
    // moving window: CGXNextSurface, then CGXGLCreateSurfaceTextureReference), and that makes
    // its texture from this structure only if the format is one of its depth codes: 3 (16 bit),
    // 4 (32 bit, B G R A in memory), 5, 6, or '2vuy'/'yuvs' (CoreGraphics 10.6.8, disassembly).
    // With 'BGRA' it made none and drew the window blank. It is drawn opaque while the second
    // address stays 0. Only for a surface a GPU client presents on: the other surfaces'
    // read locks have always answered 'BGRA', and WindowServer's handling of those is left as
    // it is.
    if (!write && fFrameHandle)
        info->pixelFormat = 4;
    info->colorTemperature[0] = 0x1CCCC;
    if (write)
        fWriteLocked = true;
    else
        fReadLocked = true;
    return kIOReturnSuccess;
}

// ---- Flush ----------------------------------------------------------------------------------

IOReturn RippleSurfaceClient::flush(uint32_t framebufferMask, uint32_t options)
{
    if (!fID || !fRegion)
        return kIOReturnNotReady;
    IOMemoryDescriptor *src = fClientMem ? fClientMem : (IOMemoryDescriptor *)fOwnMem;
    if (!src)
        return kIOReturnSuccess;        // never drawn into: nothing to show
    const IOAccelBounds *rects = fRegion->num_rects ? fRegion->rect : &fRegion->bounds;
    uint32_t n = fRegion->num_rects ? fRegion->num_rects : 1;
    return fAccel->framebuffer()->copyToScreen(src, fRowBytes, fOriginX, fOriginY, rects, n);
}

IOReturn RippleSurfaceClient::control(uint32_t selector, uint32_t arg, uint64_t *result)
{
    // Known callers: 1 _CGXSynchronizeAcceleratedSurface, 4 CGLSetPBufferVolatileState,
    // 5 surface volatility (CoreGraphics backing stores). Nothing to do for any of them.
    switch (selector) {
    case 1:
    case 4:
    case 5:
        *result = 1;
        return kIOReturnSuccess;
    default:
        return kIOReturnBadArgument;
    }
}

// ---- Dispatch -------------------------------------------------------------------------------

IOReturn RippleSurfaceClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                             IOExternalMethodDispatch *, OSObject *, void *)
{
    IOReturn r = kIOReturnUnsupported;
    const uint64_t *in = a->scalarInput;
    uint32_t nin = a->scalarInputCount;
    IOAccelSurfaceInformation *info = (IOAccelSurfaceInformation *)a->structureOutput;
    bool wantsInfo = selector == kIOAccelSurfaceReadLockOptions ||
                     selector == kIOAccelSurfaceWriteLockOptions ||
                     selector == kIOAccelSurfaceReadLock || selector == kIOAccelSurfaceWriteLock;

    if (wantsInfo && (!info || a->structureOutputSize < sizeof(IOAccelSurfaceInformation)))
        return kIOReturnBadArgument;
    // No dispatch table checks the counts here, and IOKit copies back as much output as the
    // caller asked for from a reply buffer it does not clear: without this, any selector
    // returned up to 4 KB of stale kernel memory. Zero the scalars, and return only the
    // structure a lock fills in.
    for (uint32_t i = 0; i < a->scalarOutputCount; i++)
        a->scalarOutput[i] = 0;
    if (wantsInfo) {
        bzero(info, sizeof(IOAccelSurfaceInformation));
        a->structureOutputSize = sizeof(IOAccelSurfaceInformation);
    } else {
        a->structureOutputSize = 0;
    }

    // A shape of more than about 510 rectangles is over 4 KB, and IOKit then passes it as a
    // memory descriptor instead of inline bytes.
    const void *sin = a->structureInput;
    uint32_t sinSize = a->structureInputSize;
    void *big = 0;
    uint32_t bigSize = 0;
    if (a->structureInputDescriptor) {
        IOMemoryDescriptor *md = a->structureInputDescriptor;
        uint64_t len = md->getLength();
        if (len > kMaxRegionBytes)
            return kIOReturnBadArgument;
        bigSize = (uint32_t)len;
        big = IOMalloc(bigSize);
        if (!big)
            return kIOReturnNoMemory;
        bool got = md->prepare() == kIOReturnSuccess;
        if (got) {
            got = md->readBytes(0, big, bigSize) == bigSize;
            md->complete();
        }
        if (!got) {
            IOFree(big, bigSize);
            return kIOReturnVMError;
        }
        sin = big;
        sinSize = bigSize;
    }

    IOLockLock(fLock);
    switch (selector) {
    case kIOAccelSurfaceReadLockOptions:
    case kIOAccelSurfaceReadLock:
        r = lock(false, info);
        break;
    case kIOAccelSurfaceWriteLockOptions:
    case kIOAccelSurfaceWriteLock:
        r = lock(true, info);
        break;
    case kIOAccelSurfaceReadUnlockOptions:
    case kIOAccelSurfaceReadUnlock:
        fReadLocked = false;
        r = kIOReturnSuccess;
        break;
    case kIOAccelSurfaceWriteUnlockOptions:
    case kIOAccelSurfaceWriteUnlock:
        fWriteLocked = false;
        r = kIOReturnSuccess;
        break;
    case kIOAccelSurfaceGetState:
        if (a->scalarOutputCount >= 1)
            a->scalarOutput[0] = 0;
        r = kIOReturnSuccess;
        break;
    case kIOAccelSurfaceRead:
        r = kIOReturnUnsupported;
        break;
    case kIOAccelSurfaceSetIDMode:
        if (nin < 2)
            r = kIOReturnBadArgument;
        else if (!(in[1] & kIOAccelSurfaceModeWindowedBit) ||
                 ((in[1] & kIOAccelSurfaceModeColorDepthBits) != kIOAccelSurfaceModeColorDepth8888 &&
                  (in[1] & kIOAccelSurfaceModeColorDepthBits) != kIOAccelSurfaceModeColorDepthBGRA32))
            r = kIOReturnUnsupported;   // 32-bit windowed surfaces only, for now
        else {
            fID = (uint32_t)in[0];
            fMode = (uint32_t)in[1];
            r = kIOReturnSuccess;
        }
        break;
    case kIOAccelSurfaceSetScale:
        if (sinSize < sizeof(IOAccelSurfaceScaling))
            r = kIOReturnBadArgument;
        else {
            memcpy(&fScale, sin, sizeof(fScale));
            fHasScale = true;
            r = kIOReturnSuccess;
        }
        break;
    case kIOAccelSurfaceSetShape:
        r = nin < 2 ? kIOReturnBadArgument
                    : setShape((uint32_t)in[0], (uint32_t)in[1], 0, 0, 0, sin, sinSize);
        break;
    case kIOAccelSurfaceSetShapeBacking:
        r = nin < 4 ? kIOReturnBadArgument
                    : setShape((uint32_t)in[0], (uint32_t)in[1], in[2], (uint32_t)in[3], 0,
                               sin, sinSize);
        break;
    case kIOAccelSurfaceSetShapeBackingAndLength:
        r = nin < 5 ? kIOReturnBadArgument
                    : setShape((uint32_t)in[0], (uint32_t)in[1], in[2], (uint32_t)in[3], in[4],
                               sin, sinSize);
        break;
    case kIOAccelSurfaceFlush:
        r = flush(nin > 0 ? (uint32_t)in[0] : 1, nin > 1 ? (uint32_t)in[1] : 0);
        break;
    case kIOAccelSurfaceQueryLock:
        r = (fReadLocked || fWriteLocked) ? kIOReturnCannotLock : kIOReturnSuccess;
        break;
    case kIOAccelSurfaceControl:
        if (nin < 2 || a->scalarOutputCount < 1)
            r = kIOReturnBadArgument;
        else
            r = control((uint32_t)in[0], (uint32_t)in[1], &a->scalarOutput[0]);
        break;
    default:
        r = kIOReturnUnsupported;
        break;
    }
    if (fAccel->qeDebug())
        LOG("surface %u (pid task %p) selector %u in %u: 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx, struct %u -> 0x%x",
            fID, fTask, selector, nin, nin > 0 ? in[0] : 0, nin > 1 ? in[1] : 0, nin > 2 ? in[2] : 0,
            nin > 3 ? in[3] : 0, nin > 4 ? in[4] : 0, sinSize, r);
    IOLockUnlock(fLock);
    if (big)
        IOFree(big, bigSize);
    return r;
}

// ---- A GPU client's frames --------------------------------------------------------------------

void RippleSurfaceClient::setFrame(uint32_t handle, uint32_t context, uint32_t format,
                                   uint32_t width, uint32_t height, bool topFirst)
{
    IOLockLock(fLock);
    fFrameHandle = handle;
    fFrameContext = context;
    fFrameFormat = format;
    fFrameWidth = width;
    fFrameHeight = height;
    fFrameTopFirst = topFirst;
    IOLockUnlock(fLock);
}

void RippleSurfaceClient::forgetFrame(uint32_t handle, uint32_t context)
{
    IOLockLock(fLock);
    if ((handle && fFrameHandle == handle) || (context && fFrameContext == context))
        fFrameHandle = fFrameContext = 0;
    if (context && fStageContext == context)
        fStageContext = 0;              // the context took its attachments with it
    IOLockUnlock(fLock);
}

// Our GPU buffer over the lock memory goes (before that memory does). Called with fLock held.
void RippleSurfaceClient::dropStage()
{
    if (fStage) {
        VirtioGPUDevice *dev = fAccel->framebuffer()->device();
        dev->detachBacking(fStage);
        dev->resourceUnref(fStage);
        fStage = 0;
        fStageContext = 0;
    }
}

// The client's latest frame into the memory a lock hands out (fOwnMem), top row first: a copy on
// the GPU into our buffer over that memory, then that buffer is read back, which writes the
// memory. Host commands (virgl): one BLIT. Called with fLock held.
void RippleSurfaceClient::fetchFrame()
{
    if (!fFrameHandle || !fFrameContext || !fOwnMem || fRowBytes != fBufWidth * 4)
        return;
    VirtioGPUDevice *dev = fAccel->framebuffer()->device();
    if (fStage && (fStageWidth != fBufWidth || fStageHeight != fBufHeight))
        dropStage();
    if (!fStage) {
        if (!fOwnWired) {
            if (fOwnMem->prepare() != kIOReturnSuccess)
                return;
            fOwnWired = true;
        }
        uint32_t id = dev->allocResourceId();
        // A 2D texture, B8G8R8A8, render target and sampler view.
        if (!dev->resourceCreate3D(id, 2, 1, (1 << 1) | (1 << 3), fBufWidth, fBufHeight, 1, 1, 0, 0, 0))
            return;
        if (!dev->attachBacking(id, fOwnMem, (IOByteCount)fRowBytes * fBufHeight)) {
            dev->resourceUnref(id);
            return;
        }
        fStage = id;
        fStageWidth = fBufWidth;
        fStageHeight = fBufHeight;
    }
    if (fStageContext != fFrameContext) {
        if (!dev->ctxAttachResource(fFrameContext, fStage))
            return;
        fStageContext = fFrameContext;
    }
    if (!fStageCmd) {
        IOBufferMemoryDescriptor *cmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionOut | kIOMemoryPhysicallyContiguous, PAGE_SIZE,
            0xFFFFFFFFFFFFF000ULL);
        if (!cmd)
            return;
        if (cmd->prepare() != kIOReturnSuccess) {
            cmd->release();
            return;
        }
        fStageCmd = cmd;
        fStageCmdPA = cmd->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
    }
    uint32_t w = fFrameWidth < fBufWidth ? fFrameWidth : fBufWidth;
    uint32_t h = fFrameHeight < fBufHeight ? fFrameHeight : fBufHeight;
    uint32_t *c = (uint32_t *)fStageCmd->getBytesNoCopy();
    bzero(c, 22 * 4);
    c[0] = 16 | (21 << 16);             // BLIT
    c[1] = 0xf;                         // all channels, nearest
    c[4] = fStage;
    c[6] = 1;
    c[10] = w;
    c[11] = h;
    c[12] = 1;
    c[13] = fFrameHandle;
    c[15] = fFrameFormat;
    c[19] = w;
    c[21] = 1;
    if (fFrameTopFirst) {
        c[17] = 0;
        c[20] = h;
    } else {
        c[17] = fFrameHeight;           // rows turned over: a negative height
        c[20] = (uint32_t)-(int32_t)h;
    }
    bool sent = dev->submit3D(fFrameContext, fStageCmdPA, 22 * 4, NULL);
    uint32_t box[6] = { 0, 0, 0, fBufWidth, fBufHeight, 1 };
    uint64_t fence = 0;
    bool read = sent && dev->transfer3D(false, fFrameContext, fStage, 0, box, 0, 0, 0, &fence);
    if (fAccel->qeDebug()) {
        const uint32_t *m = (const uint32_t *)fOwnMem->getBytesNoCopy();
        LOG("surface %u read lock: frame %ux%u format %u of buffer %u (context %u) into %ux%u via %u: copy %d, read back %d; pixels top left 0x%08x, centre 0x%08x",
            fID, fFrameWidth, fFrameHeight, fFrameFormat, fFrameHandle, fFrameContext, fBufWidth,
            fBufHeight, fStage, sent, read, m[0], m[(fBufHeight / 2) * fBufWidth + fBufWidth / 2]);
    }
}

// ---- Description for GL drivers (RippleAccelerator::describeSurface) ----------------------

// For showing a client's buffer on the screen (RippleGPUClient::surfacePresent): the surface's
// place on the screen and the visible rectangles of it, when it is a plain region of the screen
// now. Not when WindowServer has shaped it non-simple (it draws the window itself then, as
// while a window moves), scaled it, or given it more rectangles than the caller has room for.
bool RippleSurfaceClient::screenShape(IOAccelBounds *bounds, IOAccelBounds *rects,
                                      uint32_t maxRects, uint32_t *count)
{
    IOLockLock(fLock);
    bool ok = fID && fScreenRegion && fRegion && fBufWidth && fBufHeight && !fHasScale &&
              !(fShapeOptions & kIOAccelSurfaceShapeNonSimpleBit) &&
              fRegion->num_rects <= maxRects;
    if (ok) {
        bounds->x = (SInt16)fOriginX;
        bounds->y = (SInt16)fOriginY;
        bounds->w = (SInt16)fBufWidth;
        bounds->h = (SInt16)fBufHeight;
        *count = fRegion->num_rects;
        memcpy(rects, fRegion->rect, fRegion->num_rects * sizeof(IOAccelBounds));
    }
    IOLockUnlock(fLock);
    return ok;
}

bool RippleSurfaceClient::describe(task_t asker, struct ripple_gpu_surface_info *out)
{
    IOLockLock(fLock);
    bool ok = fID && fBufWidth && fBufHeight;
    if (ok) {
        uint32_t sw, sh;
        fAccel->framebuffer()->screenSize(&sw, &sh);
        bzero(out, sizeof(*out));
        out->id = fID;
        out->width = fBufWidth;
        out->height = fBufHeight;
        out->row_bytes = fRowBytes;
        out->x = fOriginX;
        out->y = fOriginY;
        out->screen_width = sw;
        out->screen_height = sh;
        out->shape_seq = fShapeSeq;
        if (fClientMem || fOwnMem)
            out->flags |= RIPPLE_GPU_SURFACE_BACKING;
        if (asker == fTask)
            out->backing_address = fClientMem ? fClientAddr : (fOwnMap ? fOwnMap->getAddress() : 0);
        if (fScreenRegion)
            out->flags |= RIPPLE_GPU_SURFACE_SCREEN;
    }
    IOLockUnlock(fLock);
    return ok;
}
