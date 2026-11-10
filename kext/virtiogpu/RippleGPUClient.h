/*
 * RippleGPUClient: the user client behind docs/kext-uapi.md (IOServiceOpen type
 * RIPPLE_GPU_CLIENT_TYPE on RippleAccelerator). This is the virgl backend's kernel half:
 * contexts are virgl contexts, buffers are virgl resources with guest backing.
 * Everything a connection creates is destroyed when it closes or its task dies.
 */
#ifndef RIPPLE_GPU_CLIENT_H
#define RIPPLE_GPU_CLIENT_H

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "ripple_gpu_uapi.h"

class RippleAccelerator;
class VirtioGPUDevice;

class RippleGPUClient : public IOUserClient
{
    OSDeclareDefaultStructors(RippleGPUClient)

    enum { kMaxContexts = 16, kMaxBuffers = 256, kMaxHandles = 64,
           kMaxCommandBytes = 256 * 1024, kMaxBufferBytes = 256 * 1024 * 1024 };

    struct Buffer {
        uint32_t handle;                 // virgl resource id (0 = free slot)
        IOBufferMemoryDescriptor *mem;
        uint64_t size;
        uint32_t attached;               // bit i: attached to fContext[i]
    };

    task_t             fTask;
    RippleAccelerator *fAccel;
    VirtioGPUDevice   *fDev;
    IOLock            *fLock;
    uint32_t           fContext[kMaxContexts];   // virgl context ids (0 = free)
    Buffer             fBuffer[kMaxBuffers];

    int  contextSlot(uint32_t ctx) const;
    Buffer *findBuffer(uint32_t handle);
    void destroyBuffer(Buffer &b);
    void destroyAll();

    // Method implementations (called with fLock held).
    IOReturn getInfo(struct ripple_gpu_info *out);
    IOReturn getCaps(uint32_t id, uint32_t version, void *out, uint32_t *size);
    IOReturn contextCreate(uint32_t flags, uint64_t *ctx);
    IOReturn contextDestroy(uint32_t ctx);
    IOReturn bufferCreate(const struct ripple_gpu_buffer_create *in, uint64_t *handle, uint64_t *size);
    IOReturn bufferDestroy(uint32_t handle);
    IOReturn submit(const uint8_t *in, uint32_t inSize, uint64_t *fence);
    IOReturn transfer(const struct ripple_gpu_transfer *in, uint64_t *fence);

    static IOReturn sMethod(OSObject *target, void *reference, IOExternalMethodArguments *args);

public:
    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type);
    virtual bool start(IOService *provider);
    virtual IOReturn clientClose(void);
    virtual IOReturn clientDied(void);
    virtual void free(void);
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference);
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory);
};

#endif
