/*
 * RippleAccelerator: the GPU service user space talks to (docs/kext-uapi.md). An IOAccelerator
 * attached to RippleFramebuffer, sharing its VirtioGPUDevice; it starts only when the device
 * has 3D (VIRGL). From M5 it also carries the IOAccelerator side WindowServer needs.
 */
#ifndef RIPPLE_ACCELERATOR_H
#define RIPPLE_ACCELERATOR_H

#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/IOUserClient.h>
#include "VirtioGPUDevice.h"
#include "ripple_gpu_uapi.h"

class RippleFramebuffer;

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
    OSArray           *fSurfaces;    // RippleSurfaceClients, for lookups by surface id

public:
    VirtioGPUDevice *device() const { return fDev; }
    const struct ripple_gpu_info &info() const { return fInfo; }
    RippleFramebuffer *framebuffer() const { return fFB; }
    bool qeDebug() const { return fQEDebug; }
    void addSurface(OSObject *s);
    void removeSurface(OSObject *s);
    // Describe surface `id` for task `asker` (backing address only for its owner).
    bool describeSurface(uint32_t id, task_t asker, struct ripple_gpu_surface_info *out);

    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler);
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   IOUserClient **handler);
};

#endif
