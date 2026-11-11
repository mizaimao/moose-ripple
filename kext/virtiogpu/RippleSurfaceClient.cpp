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

#define super IOUserClient
OSDefineMetaClassAndStructors(RippleSurfaceClient, IOUserClient)

#define LOG(fmt, ...) IOLog("RippleSurfaceClient: " fmt "\n", ##__VA_ARGS__)

// ---- Lifecycle ------------------------------------------------------------------------------

bool RippleSurfaceClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    fTask = owningTask;
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
    if (fOwnMap) {
        fOwnMap->release();             // unmaps from the client
        fOwnMap = 0;
    }
    if (fOwnMem) {
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
    if (fRegion)
        IOFree(fRegion, fRegionBytes);
    if (fLock)
        IOLockFree(fLock);
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
    }

    bzero(info, sizeof(*info));
    info->address[0] = addr;
    info->rowBytes = fRowBytes;
    info->width = fBufWidth;
    info->height = fBufHeight;
    info->pixelFormat = 'BGRA';         // 8888: B, G, R, A in memory
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
        if (a->structureInputSize < sizeof(IOAccelSurfaceScaling))
            r = kIOReturnBadArgument;
        else {
            memcpy(&fScale, a->structureInput, sizeof(fScale));
            fHasScale = true;
            r = kIOReturnSuccess;
        }
        break;
    case kIOAccelSurfaceSetShape:
        r = nin < 2 ? kIOReturnBadArgument
                    : setShape((uint32_t)in[0], (uint32_t)in[1], 0, 0, 0, a->structureInput,
                               a->structureInputSize);
        break;
    case kIOAccelSurfaceSetShapeBacking:
        r = nin < 4 ? kIOReturnBadArgument
                    : setShape((uint32_t)in[0], (uint32_t)in[1], in[2], (uint32_t)in[3], 0,
                               a->structureInput, a->structureInputSize);
        break;
    case kIOAccelSurfaceSetShapeBackingAndLength:
        r = nin < 5 ? kIOReturnBadArgument
                    : setShape((uint32_t)in[0], (uint32_t)in[1], in[2], (uint32_t)in[3], in[4],
                               a->structureInput, a->structureInputSize);
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
            nin > 3 ? in[3] : 0, nin > 4 ? in[4] : 0, a->structureInputSize, r);
    IOLockUnlock(fLock);
    return r;
}

// ---- Description for GL drivers (RippleAccelerator::describeSurface) ----------------------

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
