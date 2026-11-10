/*
 * RippleGPUClient: see RippleGPUClient.h and docs/kext-uapi.md.
 */
#include "RippleGPUClient.h"
#include "RippleAccelerator.h"
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
    fDev->detachBacking(b.handle);
    fDev->resourceUnref(b.handle);          // also drops it from any context
    if (b.mem) {
        b.mem->complete();
        b.mem->release();
    }
    bzero(&b, sizeof(b));
}

void RippleGPUClient::destroyAll()
{
    for (int i = 0; i < kMaxBuffers; i++)
        destroyBuffer(fBuffer[i]);
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
    if (fLock)
        IOLockFree(fLock);
    super::free();
}

// ---- Helpers --------------------------------------------------------------------------------

int RippleGPUClient::contextSlot(uint32_t ctx) const
{
    if (!ctx)
        return -1;
    for (int i = 0; i < kMaxContexts; i++)
        if (fContext[i] == ctx)
            return i;
    return -1;
}

RippleGPUClient::Buffer *RippleGPUClient::findBuffer(uint32_t handle)
{
    if (!handle)
        return 0;
    for (int i = 0; i < kMaxBuffers; i++)
        if (fBuffer[i].handle == handle)
            return &fBuffer[i];
    return 0;
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
    for (int i = 0; i < kMaxBuffers; i++)
        fBuffer[i].attached &= ~(1u << slot);
    fDev->ctxDestroy(ctx);
    fContext[slot] = 0;
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::bufferCreate(const struct ripple_gpu_buffer_create *in, uint64_t *handle,
                                       uint64_t *size)
{
    if (in->flags || in->params_size != sizeof(struct ripple_virgl_buffer_params) ||
        !in->size || in->size > kMaxBufferBytes)
        return kIOReturnBadArgument;
    Buffer *b = findBuffer(0);
    for (int i = 0; i < kMaxBuffers && !b; i++)
        if (!fBuffer[i].handle)
            b = &fBuffer[i];
    if (!b)
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
    b->handle = id;
    b->mem = mem;
    b->size = bytes;
    b->attached = 0;
    *handle = id;
    *size = bytes;
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::bufferDestroy(uint32_t handle)
{
    Buffer *b = findBuffer(handle);
    if (!b)
        return kIOReturnBadArgument;
    destroyBuffer(*b);
    return kIOReturnSuccess;
}

IOReturn RippleGPUClient::submit(const uint8_t *in, uint32_t inSize, uint64_t *fence)
{
    if (inSize < sizeof(struct ripple_gpu_submit))
        return kIOReturnBadArgument;
    const struct ripple_gpu_submit *s = (const struct ripple_gpu_submit *)in;
    uint64_t need = sizeof(*s) + (uint64_t)s->num_handles * 4 + s->cmd_size;
    int slot = contextSlot(s->context);
    if (slot < 0 || s->flags || s->num_handles > kMaxHandles || !s->cmd_size ||
        s->cmd_size > kMaxCommandBytes || (s->cmd_size & 3) || need != inSize)
        return kIOReturnBadArgument;

    // Make the buffers the commands use visible to the context.
    const uint32_t *handles = (const uint32_t *)(in + sizeof(*s));
    for (uint32_t i = 0; i < s->num_handles; i++) {
        Buffer *b = findBuffer(handles[i]);
        if (!b)
            return kIOReturnBadArgument;
        if (!(b->attached & (1u << slot))) {
            if (!fDev->ctxAttachResource(s->context, b->handle))
                return kIOReturnError;
            b->attached |= 1u << slot;
        }
    }

    // The device reads commands from guest memory: copy them to a contiguous buffer.
    IOBufferMemoryDescriptor *cmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionOut | kIOMemoryPhysicallyContiguous, s->cmd_size, 0xFFFFFFFFFFFFF000ULL);
    if (!cmd || cmd->prepare() != kIOReturnSuccess) {
        if (cmd)
            cmd->release();
        return kIOReturnNoMemory;
    }
    memcpy(cmd->getBytesNoCopy(), in + sizeof(*s) + s->num_handles * 4, s->cmd_size);
    bool ok = fDev->submit3D(s->context, cmd->getPhysicalSegment(0, NULL, kIOMemoryMapperNone),
                             s->cmd_size, fence);
    cmd->complete();
    cmd->release();
    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn RippleGPUClient::transfer(const struct ripple_gpu_transfer *t, uint64_t *fence)
{
    Buffer *b = findBuffer(t->buffer);
    if (!b || contextSlot(t->context) < 0 || t->direction > RIPPLE_GPU_FROM_DEVICE ||
        t->offset >= b->size)
        return kIOReturnBadArgument;
    uint32_t box[6] = { t->x, t->y, t->z, t->width, t->height, t->depth };
    bool ok = fDev->transfer3D(t->direction == RIPPLE_GPU_TO_DEVICE, t->context, b->handle,
                               t->level, box, t->stride, t->layer_stride, t->offset, fence);
    return ok ? kIOReturnSuccess : kIOReturnError;
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

    // Large inputs (over 4 KB) arrive as a memory descriptor instead of inline bytes.
    uint8_t *big = 0;
    uint32_t inSize = a->structureInputSize;
    const uint8_t *in = (const uint8_t *)a->structureInput;
    if (a->structureInputDescriptor) {
        inSize = (uint32_t)a->structureInputDescriptor->getLength();
        if (inSize > kMaxCommandBytes + 4096)
            return kIOReturnBadArgument;
        big = (uint8_t *)IOMalloc(inSize);
        if (!big)
            return kIOReturnNoMemory;
        if (a->structureInputDescriptor->prepare() != kIOReturnSuccess) {
            IOFree(big, inSize);
            return kIOReturnVMError;
        }
        a->structureInputDescriptor->readBytes(0, big, inSize);
        a->structureInputDescriptor->complete();
        in = big;
    }

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
        r = c->submit(in, inSize, &a->scalarOutput[0]);
        break;
    case RIPPLE_GPU_TRANSFER:
        r = c->transfer((const struct ripple_gpu_transfer *)in, &a->scalarOutput[0]);
        break;
    case RIPPLE_GPU_WAIT:
        a->scalarOutput[0] = 1;             // version 1: fenced work is complete on return
        r = kIOReturnSuccess;
        break;
    }
    IOLockUnlock(c->fLock);
    if (big)
        IOFree(big, inSize);
    return r;
}

IOReturn RippleGPUClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                              IOMemoryDescriptor **memory)
{
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
