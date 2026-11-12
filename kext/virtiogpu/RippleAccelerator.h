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
#include "VirtioGPUDevice.h"
#include "ripple_gpu_uapi.h"

class RippleFramebuffer;
class RippleSurfaceClient;

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
    RippleSurfaceClient *retainSurface(uint32_t id);    // retained, or 0

public:
    VirtioGPUDevice *device() const { return fDev; }
    const struct ripple_gpu_info &info() const { return fInfo; }
    RippleFramebuffer *framebuffer() const { return fFB; }
    bool qeDebug() const { return fQEDebug; }
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

    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler);
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   IOUserClient **handler);
};

#endif
