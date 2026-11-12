/*
 * RippleGPUClient: see RippleGPUClient.h and docs/kext-uapi.md.
 */
#include "RippleGPUClient.h"
#include "RippleAccelerator.h"
#include "RippleFramebuffer.h"
#include "VirtioGPUDevice.h"

#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RippleGPUClient, IOUserClient)

#define LOG(fmt, ...) IOLog("RippleGPUClient: " fmt "\n", ##__VA_ARGS__)

// ---- Lifecycle ------------------------------------------------------------------------------

bool RippleGPUClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    fTask = owningTask;
    fLock = IOLockAlloc();
    return fLock != 0;
}

bool RippleGPUClient::start(IOService *provider)
{
    fAccel = OSDynamicCast(RippleAccelerator, provider);
    if (!fAccel || !super::start(provider))
        return false;
    fDev = fAccel->device();
    return fDev != 0;
}

void RippleGPUClient::destroyBuffer(Buffer &b)
{
    if (!b.handle)
        return;
    // Never leave a buffer that is going away on the display, whatever fShown says.
    fAccel->framebuffer()->releaseResource(b.handle);
    bool detached = fDev->detachBacking(b.handle);
    bool gone = fDev->resourceUnref(b.handle);      // also drops it from any context
    if (b.mem) {
        if (detached || gone) {
            b.mem->complete();
            b.mem->release();
        } else {
            // Neither was answered (a stalled host): it runs its queue later, in order, and
            // an earlier transfer would then write into pages the kernel has reused. Keeping
            // the memory wired is the safe failure.
            LOG("buffer %u: the host did not let go of its memory; %llu bytes stay wired",
                b.handle, (unsigned long long)b.size);
        }
    }
    bzero(&b, sizeof(b));
}

void RippleGPUClient::destroyAll()
{
    if (fShown) {
        fAccel->framebuffer()->releaseResource(fShown);
        fShown = 0;
    }
    for (uint32_t i = 0; i < fBufferCap; i++)
        destroyBuffer(fBuffer[i]);
    fBufferCount = 0;
    for (int i = 0; i < kMaxContexts; i++) {
        if (fContext[i]) {
            fDev->ctxDestroy(fContext[i]);
            fContext[i] = 0;
        }
    }
}

IOReturn RippleGPUClient::clientClose(void)
{
    IOLockLock(fLock);
    if (fDev)
        destroyAll();
    IOLockUnlock(fLock);
    terminate();
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::clientDied(void)
{
    return clientClose();
}

void RippleGPUClient::free(void)
{
    if (fBuffer)
        IOFree(fBuffer, fBufferCap * sizeof(Buffer));
    if (fCmd) {
        fCmd->complete();
        fCmd->release();
    }
    if (fLock)
        IOLockFree(fLock);
    super::free();
}

// ---- Helpers --------------------------------------------------------------------------------

uint32_t RippleGPUClient::firstContext() const
{
    for (int i = 0; i < kMaxContexts; i++)
        if (fContext[i])
            return fContext[i];
    return 0;                           // sync() then uses the device's last submit context
}

int RippleGPUClient::contextSlot(uint32_t ctx) const
{
    if (!ctx)
        return -1;
    for (int i = 0; i < kMaxContexts; i++)
        if (fContext[i] == ctx)
            return i;
    return -1;
}

static inline uint32_t homeSlot(uint32_t handle, uint32_t mask)
{
    uint32_t h = handle * 2654435761u;
    return (h ^ (h >> 16)) & mask;
}

RippleGPUClient::Buffer *RippleGPUClient::findBuffer(uint32_t handle)
{
    if (!handle || !fBufferCap)
        return 0;
    uint32_t mask = fBufferCap - 1;
    for (uint32_t i = homeSlot(handle, mask); fBuffer[i].handle; i = (i + 1) & mask)
        if (fBuffer[i].handle == handle)
            return &fBuffer[i];
    return 0;
}

// Make room for one more buffer: the table stays at most half full.
bool RippleGPUClient::growBuffers()
{
    if ((fBufferCount + 1) * 2 <= fBufferCap)
        return true;
    uint32_t oldCap = fBufferCap, cap = oldCap ? oldCap * 2 : 256;
    Buffer *old = fBuffer, *table = (Buffer *)IOMalloc(cap * sizeof(Buffer));
    if (!table)
        return false;
    bzero(table, cap * sizeof(Buffer));
    fBuffer = table;
    fBufferCap = cap;
    fBufferCount = 0;
    for (uint32_t i = 0; i < oldCap; i++)
        if (old[i].handle)
            *insertBuffer(old[i].handle) = old[i];
    if (old)
        IOFree(old, oldCap * sizeof(Buffer));
    return true;
}

// Insert an empty entry for `handle`; growBuffers() must have made room.
RippleGPUClient::Buffer *RippleGPUClient::insertBuffer(uint32_t handle)
{
    uint32_t mask = fBufferCap - 1, i = homeSlot(handle, mask);
    while (fBuffer[i].handle)
        i = (i + 1) & mask;
    bzero(&fBuffer[i], sizeof(Buffer));
    fBuffer[i].handle = handle;
    fBufferCount++;
    return &fBuffer[i];
}

// Remove an entry, shifting later entries of its probe run back (no tombstones).
void RippleGPUClient::removeBuffer(Buffer *b)
{
    uint32_t mask = fBufferCap - 1, i = (uint32_t)(b - fBuffer), j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!fBuffer[j].handle)
            break;
        uint32_t k = homeSlot(fBuffer[j].handle, mask);
        bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
        if (!stays) {
            fBuffer[i] = fBuffer[j];
            i = j;
        }
    }
    bzero(&fBuffer[i], sizeof(Buffer));
    fBufferCount--;
}

// ---- Methods --------------------------------------------------------------------------------

IOReturn RippleGPUClient::getInfo(struct ripple_gpu_info *out)
{
    *out = fAccel->info();
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::getCaps(uint32_t id, uint32_t version, void *out, uint32_t *size)
{
    uint32_t got = fDev->getCapset(id, version, out, *size);
    if (!got)
        return kIOReturnBadArgument;
    *size = got;
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::contextCreate(uint32_t flags, uint64_t *ctx)
{
    if (flags)
        return kIOReturnBadArgument;
    int slot = contextSlot(0);
    for (int i = 0; i < kMaxContexts && slot < 0; i++)
        if (!fContext[i])
            slot = i;
    if (slot < 0)
        return kIOReturnNoResources;
    uint32_t id = fDev->allocContextId();
    if (!fDev->ctxCreate(id, "ripple-gpu-client"))
        return kIOReturnError;
    fContext[slot] = id;
    *ctx = id;
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::contextDestroy(uint32_t ctx)
{
    int slot = contextSlot(ctx);
    if (slot < 0)
        return kIOReturnBadArgument;
    for (uint32_t i = 0; i < fBufferCap; i++)
        fBuffer[i].attached &= ~(1u << slot);
    fDev->ctxDestroy(ctx);
    fContext[slot] = 0;
    fScreenAttached[slot] = 0;
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::bufferCreate(const struct ripple_gpu_buffer_create *in, uint64_t *handle,
                                       uint64_t *size)
{
    if (in->flags || in->params_size != sizeof(struct ripple_virgl_buffer_params) ||
        !in->size || in->size > kMaxBufferBytes)
        return kIOReturnBadArgument;
    if (fBufferCount >= kMaxBuffers || !growBuffers())
        return kIOReturnNoResources;

    const struct ripple_virgl_buffer_params *p = (const struct ripple_virgl_buffer_params *)in->params;
    uint64_t bytes = round_page(in->size);
    IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryKernelUserShared, bytes, PAGE_SIZE);
    if (!mem)
        return kIOReturnNoMemory;
    if (mem->prepare() != kIOReturnSuccess) {
        mem->release();
        return kIOReturnNoMemory;
    }
    bzero(mem->getBytesNoCopy(), bytes);

    uint32_t id = fDev->allocResourceId();
    if (!fDev->resourceCreate3D(id, p->target, p->format, p->bind, p->width, p->height, p->depth,
                                p->array_size, p->last_level, p->nr_samples, p->flags)) {
        mem->complete();
        mem->release();
        return kIOReturnError;
    }
    if (!fDev->attachBacking(id, mem, bytes)) {
        fDev->resourceUnref(id);
        mem->complete();
        mem->release();
        return kIOReturnError;
    }
    Buffer *b = insertBuffer(id);
    b->mem = mem;
    b->size = bytes;
    b->format = p->format;
    b->width = p->width;
    b->height = p->height;
    *handle = id;
    *size = bytes;
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::bufferDestroy(uint32_t handle)
{
    Buffer *b = findBuffer(handle);
    if (!b)
        return kIOReturnBadArgument;
    if (fShown == handle)
        fShown = 0;
    destroyBuffer(*b);                      // takes it off the display first
    removeBuffer(b);
    return kIOReturnSuccess;
}

// Copy part of the structure input: inline bytes (up to 4 KB) or, for larger inputs, the
// prepared memory descriptor IOKit passes instead.
IOReturn RippleGPUClient::readInput(IOExternalMethodArguments *a, uint32_t offset, void *dst,
                                    uint32_t len)
{
    if (a->structureInputDescriptor)
        return a->structureInputDescriptor->readBytes(offset, dst, len) == len
            ? kIOReturnSuccess : kIOReturnBadArgument;
    if ((uint64_t)offset + len > a->structureInputSize)
        return kIOReturnBadArgument;
    memcpy(dst, (const uint8_t *)a->structureInput + offset, len);
    return kIOReturnSuccess;
}

bool RippleGPUClient::ensureCommandBuffer()
{
    if (fCmd)
        return true;
    IOBufferMemoryDescriptor *cmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionOut | kIOMemoryPhysicallyContiguous, kMaxCommandBytes,
        0xFFFFFFFFFFFFF000ULL);
    if (!cmd)
        return false;
    if (cmd->prepare() != kIOReturnSuccess) {
        cmd->release();
        return false;
    }
    fCmd = cmd;
    fCmdPA = cmd->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
    return true;
}

IOReturn RippleGPUClient::submit(IOExternalMethodArguments *a, uint64_t *fence)
{
    uint64_t inSize = a->structureInputDescriptor ? a->structureInputDescriptor->getLength()
                                                  : a->structureInputSize;
    struct ripple_gpu_submit s;
    if (inSize < sizeof(s) || readInput(a, 0, &s, sizeof(s)) != kIOReturnSuccess)
        return kIOReturnBadArgument;
    uint64_t need = sizeof(s) + (uint64_t)s.num_handles * 4 + s.cmd_size;
    int slot = contextSlot(s.context);
    if (slot < 0 || s.flags || s.num_handles > kMaxHandles || !s.cmd_size ||
        s.cmd_size > kMaxCommandBytes || (s.cmd_size & 3) || need != inSize)
        return kIOReturnBadArgument;

    // Make the buffers the commands use visible to the context.
    if (s.num_handles) {
        uint32_t bytes = s.num_handles * 4;
        uint32_t *handles = (uint32_t *)IOMalloc(bytes);
        if (!handles)
            return kIOReturnNoMemory;
        IOReturn r = readInput(a, sizeof(s), handles, bytes);
        for (uint32_t i = 0; r == kIOReturnSuccess && i < s.num_handles; i++) {
            Buffer *b = findBuffer(handles[i]);
            if (!b)
                r = kIOReturnBadArgument;
            else if (!(b->attached & (1u << slot))) {
                if (fDev->ctxAttachResource(s.context, b->handle))
                    b->attached |= 1u << slot;
                else
                    r = kIOReturnError;
            }
        }
        IOFree(handles, bytes);
        if (r != kIOReturnSuccess)
            return r;
    }

    // The device reads commands from guest memory: copy them to this connection's
    // contiguous command buffer (made once, at the first submit).
    if (!ensureCommandBuffer())
        return kIOReturnNoMemory;
    if (readInput(a, sizeof(s) + s.num_handles * 4, fCmd->getBytesNoCopy(), s.cmd_size)
        != kIOReturnSuccess)
        return kIOReturnBadArgument;
    return fDev->submit3D(s.context, fCmdPA, s.cmd_size, fence) ? kIOReturnSuccess
                                                                 : kIOReturnError;
}

IOReturn RippleGPUClient::transfer(const struct ripple_gpu_transfer *t, uint64_t *fence)
{
    Buffer *b = findBuffer(t->buffer);
    int slot = contextSlot(t->context);
    if (!b || slot < 0 || t->direction > RIPPLE_GPU_FROM_DEVICE || t->offset >= b->size)
        return kIOReturnBadArgument;
    // The host refuses a transfer for a buffer its context has not seen, without saying so: a
    // buffer that no submit has named yet is attached here.
    if (!(b->attached & (1u << slot))) {
        if (!fDev->ctxAttachResource(t->context, b->handle))
            return kIOReturnError;
        b->attached |= 1u << slot;
    }
    uint32_t box[6] = { t->x, t->y, t->z, t->width, t->height, t->depth };
    bool ok = fDev->transfer3D(t->direction == RIPPLE_GPU_TO_DEVICE, t->context, b->handle,
                               t->level, box, t->stride, t->layer_stride, t->offset, fence);
    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn RippleGPUClient::display(uint32_t handle, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    RippleFramebuffer *fb = fAccel->framebuffer();
    if (!handle) {
        if (fShown)
            fb->releaseResource(fShown);
        fShown = 0;
        return kIOReturnSuccess;
    }
    if (!findBuffer(handle) || !w || !h)
        return kIOReturnBadArgument;
    // Show a finished frame: submits don't push the host's GPU work out by themselves; a
    // fenced empty submit ahead of the scanout flush does (QEMU handles commands in order),
    // without waiting for its reply.
    fDev->syncAsync(firstContext());
    // The framebuffer changes over from one shown buffer to the next by itself; giving the
    // display back to the console in between cost a full-frame upload and flashed the console
    // frame. Recorded before the call: if only the flush fails, the scanout already is this
    // buffer.
    fShown = handle;
    fb->setShownFormat(findBuffer(handle)->format);
    return fb->showResource(handle, x, y, w, h);
}

// Put a client's finished frame on the screen where its window's surface is: a copy on the host
// GPU from the buffer into the buffer on the display, for each visible rectangle of the
// surface, then the display is flushed. This is what a hardware driver does at a swap, with no
// part for WindowServer (docs/re/ioaccelerator-interface.md, trace 3). The copy runs in the
// caller's context, after whatever it has submitted. Host commands (virgl): one BLIT each.
enum { kPresentMaxRects = 64, kVirglBlit = 16, kVirglBlitDwords = 21, kVirglMaskRGBA = 0xf };

IOReturn RippleGPUClient::surfacePresent(uint32_t surface, uint32_t handle, uint32_t context,
                                         uint32_t flags, uint64_t *shown)
{
    *shown = 0;
    Buffer *b = findBuffer(handle);
    int slot = contextSlot(context);
    if (!b || slot < 0 || !b->width || !b->height || (flags & ~RIPPLE_GPU_PRESENT_TOP_FIRST))
        return kIOReturnBadArgument;

    IOAccelBounds bounds, rects[kPresentMaxRects];
    uint32_t count = 0, screenFormat = 0, sw = 0, sh = 0;
    if (!fAccel->surfaceScreenShape(surface, &bounds, rects, kPresentMaxRects, &count) || !count)
        return kIOReturnSuccess;            // not a plain region of the screen now
    RippleFramebuffer *fb = fAccel->framebuffer();
    uint32_t screen = fb->shownResource(&screenFormat);
    fb->screenSize(&sw, &sh);
    if (!screen || screen == handle || !screenFormat)
        return kIOReturnSuccess;            // no GPU buffer on the display to copy into

    if (!(b->attached & (1u << slot))) {
        if (!fDev->ctxAttachResource(context, b->handle))
            return kIOReturnError;
        b->attached |= 1u << slot;
    }
    if (fScreenAttached[slot] != screen) {
        if (!fDev->ctxAttachResource(context, screen))
            return kIOReturnError;
        fScreenAttached[slot] = screen;
    }
    if (!ensureCommandBuffer())
        return kIOReturnNoMemory;

    // Screen coordinates have their origin at the top left, and so do the host's copy
    // coordinates in the buffer on the display (found by trying: a copy to row 300 landed 300
    // rows from the top). A buffer whose first row is the picture's bottom row is turned over
    // by a negative source height.
    uint32_t *cmd = (uint32_t *)fCmd->getBytesNoCopy(), dw = 0;
    int32_t bw = bounds.w < (int32_t)b->width ? bounds.w : (int32_t)b->width;
    int32_t bh = bounds.h < (int32_t)b->height ? bounds.h : (int32_t)b->height;
    for (uint32_t i = 0; i < count; i++) {
        int32_t x0 = rects[i].x, y0 = rects[i].y, x1 = x0 + rects[i].w, y1 = y0 + rects[i].h;
        if (x0 < bounds.x) x0 = bounds.x;
        if (y0 < bounds.y) y0 = bounds.y;
        if (x1 > bounds.x + bw) x1 = bounds.x + bw;
        if (y1 > bounds.y + bh) y1 = bounds.y + bh;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)sw) x1 = (int32_t)sw;
        if (y1 > (int32_t)sh) y1 = (int32_t)sh;
        if (x1 <= x0 || y1 <= y0)
            continue;
        int32_t w = x1 - x0, h = y1 - y0, sx = x0 - bounds.x, top = y0 - bounds.y;
        uint32_t *c = cmd + dw;
        c[0] = kVirglBlit | (kVirglBlitDwords << 16);
        c[1] = kVirglMaskRGBA;              // nearest, no scissor
        c[2] = c[3] = 0;
        c[4] = screen;                      // destination: the buffer on the display
        c[5] = 0;
        c[6] = screenFormat;
        c[7] = (uint32_t)x0;
        c[8] = (uint32_t)y0;
        c[9] = 0;
        c[10] = (uint32_t)w;
        c[11] = (uint32_t)h;
        c[12] = 1;
        c[13] = b->handle;                  // source: the client's buffer
        c[14] = 0;
        c[15] = b->format;
        c[16] = (uint32_t)sx;
        if (flags & RIPPLE_GPU_PRESENT_TOP_FIRST) {
            c[17] = (uint32_t)top;
            c[20] = (uint32_t)h;
        } else {
            c[17] = b->height - (uint32_t)top;
            c[20] = (uint32_t)-h;
        }
        c[18] = 0;
        c[19] = (uint32_t)w;
        c[21] = 1;
        dw += kVirglBlitDwords + 1;
    }
    if (!dw)
        return kIOReturnSuccess;            // nothing of it on the screen
    if (!fDev->submit3D(context, fCmdPA, dw * 4, NULL))
        return kIOReturnError;
    fDev->syncAsync(context);              // push the copy out, as display() does
    IOReturn r = fb->flushShown(screen);
    if (r == kIOReturnSuccess)
        *shown = 1;
    return r == kIOReturnNotReady ? kIOReturnSuccess : r;
}

// ---- Dispatch -------------------------------------------------------------------------------

static const IOExternalMethodDispatch sDispatch[RIPPLE_GPU_METHOD_COUNT] = {
    /* GET_INFO */        { 0, 0, 0, 0, sizeof(struct ripple_gpu_info) },
    /* GET_CAPS */        { 0, 2, 0, 0, kIOUCVariableStructureSize },
    /* CONTEXT_CREATE */  { 0, 1, 0, 1, 0 },
    /* CONTEXT_DESTROY */ { 0, 1, 0, 0, 0 },
    /* BUFFER_CREATE */   { 0, 0, sizeof(struct ripple_gpu_buffer_create), 2, 0 },
    /* BUFFER_DESTROY */  { 0, 1, 0, 0, 0 },
    /* SUBMIT */          { 0, 0, kIOUCVariableStructureSize, 1, 0 },
    /* TRANSFER */        { 0, 0, sizeof(struct ripple_gpu_transfer), 1, 0 },
    /* WAIT */            { 0, 2, 0, 1, 0 },
    /* DISPLAY */         { 0, 5, 0, 0, 0 },
    /* SURFACE_INFO */    { 0, 1, 0, 0, sizeof(struct ripple_gpu_surface_info) },
    /* SURFACE_PRESENT */ { 0, 4, 0, 1, 0 },
};

IOReturn RippleGPUClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                         IOExternalMethodDispatch *, OSObject *, void *)
{
    if (selector >= RIPPLE_GPU_METHOD_COUNT)
        return kIOReturnUnsupported;
    IOExternalMethodDispatch d = sDispatch[selector];
    d.function = &RippleGPUClient::sMethod;
    return super::externalMethod(selector, args, &d, this, (void *)(uintptr_t)selector);
}

IOReturn RippleGPUClient::sMethod(OSObject *target, void *reference, IOExternalMethodArguments *a)
{
    RippleGPUClient *c = (RippleGPUClient *)target;
    uint32_t selector = (uint32_t)(uintptr_t)reference;
    IOReturn r = kIOReturnUnsupported;

    // Inputs over 4 KB arrive as a memory descriptor instead of inline bytes; only SUBMIT's
    // can be that large, and it reads them with readInput().
    IOMemoryDescriptor *desc = a->structureInputDescriptor;
    if (desc && selector != RIPPLE_GPU_SUBMIT)
        return kIOReturnBadArgument;
    if (desc && desc->prepare() != kIOReturnSuccess)
        return kIOReturnVMError;
    const uint8_t *in = (const uint8_t *)a->structureInput;

    IOLockLock(c->fLock);
    switch (selector) {
    case RIPPLE_GPU_GET_INFO:
        r = c->getInfo((struct ripple_gpu_info *)a->structureOutput);
        break;
    case RIPPLE_GPU_GET_CAPS: {
        uint32_t size = a->structureOutputSize;
        r = c->getCaps((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1],
                       a->structureOutput, &size);
        if (r == kIOReturnSuccess)
            a->structureOutputSize = size;
        break;
    }
    case RIPPLE_GPU_CONTEXT_CREATE:
        r = c->contextCreate((uint32_t)a->scalarInput[0], &a->scalarOutput[0]);
        break;
    case RIPPLE_GPU_CONTEXT_DESTROY:
        r = c->contextDestroy((uint32_t)a->scalarInput[0]);
        break;
    case RIPPLE_GPU_BUFFER_CREATE:
        r = c->bufferCreate((const struct ripple_gpu_buffer_create *)in, &a->scalarOutput[0],
                            &a->scalarOutput[1]);
        break;
    case RIPPLE_GPU_BUFFER_DESTROY:
        r = c->bufferDestroy((uint32_t)a->scalarInput[0]);
        break;
    case RIPPLE_GPU_SUBMIT:
        r = c->submit(a, &a->scalarOutput[0]);
        break;
    case RIPPLE_GPU_TRANSFER:
        r = c->transfer((const struct ripple_gpu_transfer *)in, &a->scalarOutput[0]);
        break;
    case RIPPLE_GPU_SURFACE_INFO:
        r = c->fAccel->describeSurface((uint32_t)a->scalarInput[0], c->fTask,
                                       (struct ripple_gpu_surface_info *)a->structureOutput)
            ? kIOReturnSuccess : kIOReturnNotFound;
        break;
    case RIPPLE_GPU_SURFACE_PRESENT:
        r = c->surfacePresent((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1],
                              (uint32_t)a->scalarInput[2], (uint32_t)a->scalarInput[3],
                              &a->scalarOutput[0]);
        break;
    case RIPPLE_GPU_DISPLAY:
        r = c->display((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1],
                       (uint32_t)a->scalarInput[2], (uint32_t)a->scalarInput[3],
                       (uint32_t)a->scalarInput[4]);
        break;
    case RIPPLE_GPU_WAIT: {
        // Done if the GPU has passed the fence; with a timeout, make it pass (one sync for
        // everything queued so far). A zero timeout only looks.
        uint64_t id = a->scalarInput[0];
        bool done = id <= c->fDev->signalled();
        if (!done && a->scalarInput[1])
            done = c->fDev->sync(c->firstContext()) && id <= c->fDev->signalled();
        a->scalarOutput[0] = done;
        r = kIOReturnSuccess;
        break;
    }
    }
    IOLockUnlock(c->fLock);
    if (desc)
        desc->complete();
    return r;
}

IOReturn RippleGPUClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                              IOMemoryDescriptor **memory)
{
    if (type == RIPPLE_GPU_MEMORY_FRAMEBUFFER) {
        IOMemoryDescriptor *v = fAccel->framebuffer()->vram();
        if (!v)
            return kIOReturnNotReady;
        v->retain();
        *memory = v;
        *options = 0;
        return kIOReturnSuccess;
    }
    IOLockLock(fLock);
    Buffer *b = findBuffer(type);
    if (b && b->mem) {
        b->mem->retain();
        *memory = b->mem;
        *options = 0;
    }
    IOLockUnlock(fLock);
    return b ? kIOReturnSuccess : kIOReturnBadArgument;
}
