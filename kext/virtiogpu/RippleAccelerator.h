/*
 * RippleAccelerator: the GPU service user space talks to (docs/kext-uapi.md). An IOAccelerator
 * attached to RippleFramebuffer, sharing its VirtioGPUDevice; it starts only when the device
 * has 3D (VIRGL). From M5 it also carries the IOAccelerator side WindowServer needs.
 */
#ifndef RIPPLE_ACCELERATOR_H
#define RIPPLE_ACCELERATOR_H

#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/graphics/IOAccelTypes.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "VirtioGPUDevice.h"
#include "ripple_gpu_uapi.h"

class RippleFramebuffer;
class RippleSurfaceClient;

// A GPU buffer that several GPU connections hold (BUFFER_SHARE, BUFFER_OPEN; docs/kext-uapi.md).
// Only key and holders change after it is made, under the accelerator's shares lock.
struct RippleShare {
    RippleShare *next;
    uint64_t key;                       // 0 once a newer buffer has taken the key over
    uint32_t handle;
    uint32_t uid;                       // only connections of this user may open it
    uint32_t holders;                   // connections that hold it
    IOBufferMemoryDescriptor *mem;      // its backing, released by the last holder
    uint64_t size;
    struct ripple_virgl_buffer_params params;
};

class RippleAccelerator : public IOAccelerator
{
    OSDeclareDefaultStructors(RippleAccelerator)

    RippleFramebuffer *fFB;
    VirtioGPUDevice   *fDev;
    struct ripple_gpu_info fInfo;
    bool               fGL;          // GL bundle published (RippleGLBundle / ripple_gl)
    bool               fQE;          // surface clients for WindowServer (ripple_qe, M5)
    bool               fQEDebug;     // ripple_qe=2: log every surface call
    IOLock            *fSurfacesLock;
    uint32_t           fKernelContext;   // made at first use (kernelContext())
    OSArray           *fSurfaces;    // RippleSurfaceClients, for lookups by surface id
    RippleSurfaceClient *retainSurface(uint32_t id);    // retained, or 0
    IOLock            *fSharesLock;
    RippleShare       *fShares;
    uint32_t           fShareCount;

public:
    VirtioGPUDevice *device() const { return fDev; }
    const struct ripple_gpu_info &info() const { return fInfo; }
    RippleFramebuffer *framebuffer() const { return fFB; }
    bool qeDebug() const { return fQEDebug; }
    // A context on the host for copies the kernel makes on its own account (none of a
    // program's contexts is involved: a screen capture); 0 if it could not be made.
    uint32_t kernelContext();
    void addSurface(OSObject *s);
    void removeSurface(OSObject *s);
    // Describe surface `id` for task `asker` (backing address only for its owner).
    bool describeSurface(uint32_t id, task_t asker, struct ripple_gpu_surface_info *out);
    // A GPU client's finished frame for surface `id` (RippleSurfaceClient::presentFrame): true
    // if it went on the screen. And a buffer or context of a GPU client going away: no surface
    // may keep using it.
    bool presentSurfaceFrame(uint32_t id, uint32_t handle, uint32_t context, uint32_t format,
                             uint32_t width, uint32_t height, bool topFirst);
    void forgetSurfaceFrames(uint32_t handle, uint32_t context);
    // A compositor's buffer that surface `id` keeps filled with its frames, or fills once
    // (RippleSurfaceClient::setTexture); false if there is no such surface.
    bool setSurfaceTexture(uint32_t id, uint32_t handle, uint32_t format, uint32_t width,
                           uint32_t height, bool once, bool *filled);

    // Buffers shared between connections. addShare: a new share with one holder (a share
    // already under `key` loses it); 0 when there are too many. openShare: one more holder of
    // the share under `key` made by `uid`, or 0. releaseShare: one holder fewer; true when that
    // was the last, and the share is freed (the caller destroys the buffer).
    RippleShare *addShare(uint64_t key, uint32_t handle, uint32_t uid, IOBufferMemoryDescriptor *mem,
                          uint64_t size, const struct ripple_virgl_buffer_params &params);
    void rekeyShare(RippleShare *s, uint64_t key);
    RippleShare *openShare(uint64_t key, uint32_t uid);
    bool releaseShare(RippleShare *s);

    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler);
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   IOUserClient **handler);
};

#endif
