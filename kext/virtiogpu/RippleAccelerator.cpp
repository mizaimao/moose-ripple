/*
 * RippleAccelerator: see RippleAccelerator.h.
 */
#include "RippleAccelerator.h"
#include "RippleFramebuffer.h"
#include "RippleGPUClient.h"
#include "RippleSurfaceClient.h"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define super IOAccelerator
OSDefineMetaClassAndStructors(RippleAccelerator, IOAccelerator)

#define LOG(fmt, ...) IOLog("RippleAccelerator: " fmt "\n", ##__VA_ARGS__)

bool RippleAccelerator::start(IOService *provider)
{
    fFB = OSDynamicCast(RippleFramebuffer, provider);
    if (!fFB || !fFB->device() || !fFB->device()->hasVirgl())
        return false;                       // no 3D on this device: don't publish a GPU service
    if (!super::start(provider))
        return false;
    fDev = fFB->device();
    fDev->retain();
    fSurfacesLock = IOLockAlloc();
    fSurfaces = OSArray::withCapacity(8);
    if (!fSurfacesLock || !fSurfaces)
        return false;

    bzero(&fInfo, sizeof(fInfo));
    fInfo.api_version = RIPPLE_GPU_API_VERSION;
    fInfo.backend = RIPPLE_GPU_BACKEND_VIRGL;
    strlcpy(fInfo.name, "QEMU virtio-gpu (virgl)", sizeof(fInfo.name));
    for (uint32_t i = 0; i < fDev->numCapsets() && fInfo.num_caps < 8; i++) {
        uint32_t id, maxVer, maxSize;
        if (fDev->getCapsetInfo(i, &id, &maxVer, &maxSize)) {
            fInfo.caps_ids[fInfo.num_caps] = id;
            fInfo.caps_versions[fInfo.num_caps] = maxVer;
            fInfo.num_caps++;
        }
    }
    setProperty("RippleGPUAPIVersion", RIPPLE_GPU_API_VERSION, 32);
    setProperty("RippleGPUBackend", "virgl");

    // OpenGL loads the driver bundle named by IOGLBundleName for displays whose framebuffer
    // points here (IOAccelTypes = our registry path; docs/re/gl-plugin-interface.md).
    // WindowServer uses OpenGL too, so this stays opt-in until window presentation works:
    // personality property RippleGLBundle (default false), boot flag ripple_gl=0|1.
    OSBoolean *gl = OSDynamicCast(OSBoolean, getProperty("RippleGLBundle"));
    fGL = gl && gl->isTrue();
    int arg;
    if (PE_parse_boot_argn("ripple_gl", &arg, sizeof(arg)))
        fGL = arg != 0;
    if (fGL) {
        char path[512];
        int len = sizeof(path);
        if (getPath(path, &len, gIOServicePlane)) {
            setProperty("IOGLBundleName", "RippleGLDriver");
            fFB->setProperty("IOAccelTypes", path);
            fFB->setProperty("IOAccelIndex", 0ULL, 32);
            fFB->setProperty("IOAccelRevision", 2ULL, 32);
        } else {
            LOG("no registry path; GL bundle not published");
            fGL = false;
        }
    }
    setProperty("RippleGL", fGL ? "on" : "off");

    // Quartz Extreme work (M5): WindowServer's surface clients (type 0). Opt-in, needs the GL
    // bundle: personality property RippleQuartzExtreme (default false), boot flag ripple_qe=0|1.
    OSBoolean *qe = OSDynamicCast(OSBoolean, getProperty("RippleQuartzExtreme"));
    fQE = fGL && qe && qe->isTrue();
    if (PE_parse_boot_argn("ripple_qe", &arg, sizeof(arg)))
        fQE = fGL && arg != 0;
    fQEDebug = fQE && PE_parse_boot_argn("ripple_qe", &arg, sizeof(arg)) && arg >= 2;
    setProperty("RippleQE", fQE ? "on" : "off");
    if (fQE) {
        // What NVIDIA's accelerator publishes (M1, docs/re/ioaccelerator-interface.md) and
        // VMsvga2 sets only with its QE option: candidates for WindowServer's QE check (R1).
        setProperty("AccelCaps", 3ULL, 32);
        setProperty("IOAccelRevision", 2ULL, 32);
        // WindowServer composites the display with GL only if the framebuffer names a 2D plugin
        // that gives it copy and fill blitters (docs/re/qe-display-surface.md).
        OSDictionary *plugins = OSDictionary::withCapacity(1);
        if (plugins) {
            OSString *name = OSString::withCString("RippleGA.plugin");
            if (name) {
                plugins->setObject("ACCF0000-0000-0000-0000-000a2789904e", name);
                name->release();
            }
            fFB->setProperty("IOCFPlugInTypes", plugins);
            plugins->release();
        }
    }
    registerService();
    LOG("started: backend virgl, %u capability blobs", fInfo.num_caps);
    return true;
}

uint32_t RippleAccelerator::kernelContext()
{
    IOLockLock(fSurfacesLock);
    if (!fKernelContext) {
        uint32_t id = fDev->allocContextId();
        if (fDev->ctxCreate(id, "ripple-kernel"))
            fKernelContext = id;
    }
    uint32_t ctx = fKernelContext;
    IOLockUnlock(fSurfacesLock);
    return ctx;
}

void RippleAccelerator::stop(IOService *provider)
{
    if (fSurfaces) {
        fSurfaces->release();
        fSurfaces = 0;
    }
    if (fSurfacesLock) {
        IOLockFree(fSurfacesLock);
        fSurfacesLock = 0;
    }
    if (fGL && fFB) {
        fFB->removeProperty("IOAccelTypes");
        fFB->removeProperty("IOAccelIndex");
        fFB->removeProperty("IOAccelRevision");
        fFB->removeProperty("IOCFPlugInTypes");
        removeProperty("IOGLBundleName");
        fGL = false;
    }
    if (fDev) {
        fDev->release();
        fDev = 0;
    }
    super::stop(provider);
}

IOReturn RippleAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                          OSDictionary *, IOUserClient **handler)
{
    return newUserClient(owningTask, securityID, type, handler);
}

IOReturn RippleAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                          IOUserClient **handler)
{
    if (type == kIOAccelSurfaceClientType && fQE) {
        RippleSurfaceClient *sc = new RippleSurfaceClient;
        if (!sc)
            return kIOReturnNoMemory;
        if (!sc->initWithTask(owningTask, securityID, type) || !sc->attach(this)) {
            sc->release();
            return kIOReturnError;
        }
        if (!sc->start(this)) {
            sc->detach(this);
            sc->release();
            return kIOReturnError;
        }
        *handler = sc;
        return kIOReturnSuccess;
    }
    if (type != RIPPLE_GPU_CLIENT_TYPE)
        return kIOReturnUnsupported;        // Apple's IOAccelerator client types come with M5
    RippleGPUClient *client = new RippleGPUClient;
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) || !client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

// ---- Surfaces (RippleSurfaceClient) ---------------------------------------------------------

void RippleAccelerator::addSurface(OSObject *s)
{
    IOLockLock(fSurfacesLock);
    fSurfaces->setObject(s);
    IOLockUnlock(fSurfacesLock);
}

void RippleAccelerator::removeSurface(OSObject *s)
{
    IOLockLock(fSurfacesLock);
    for (unsigned i = 0; i < fSurfaces->getCount(); i++) {
        if (fSurfaces->getObject(i) == s) {
            fSurfaces->removeObject(i);
            break;
        }
    }
    IOLockUnlock(fSurfacesLock);
}

bool RippleAccelerator::presentSurfaceFrame(uint32_t id, uint32_t handle, uint32_t context,
                                            uint32_t format, uint32_t width, uint32_t height,
                                            bool topFirst)
{
    // The surface sends host commands for it: not with the list locked.
    RippleSurfaceClient *sc = retainSurface(id);
    if (!sc)
        return false;
    bool shown = sc->presentFrame(handle, context, format, width, height, topFirst);
    sc->release();
    return shown;
}

bool RippleAccelerator::setSurfaceTexture(uint32_t id, uint32_t handle, uint32_t format,
                                          uint32_t width, uint32_t height, bool once,
                                          bool *filled)
{
    RippleSurfaceClient *sc = retainSurface(id);
    if (!sc)
        return false;
    *filled = sc->setTexture(handle, format, width, height, once);
    sc->release();
    return true;
}

RippleSurfaceClient *RippleAccelerator::retainSurface(uint32_t id)
{
    RippleSurfaceClient *found = 0;
    IOLockLock(fSurfacesLock);
    for (unsigned i = 0; i < fSurfaces->getCount() && !found; i++) {
        RippleSurfaceClient *sc = (RippleSurfaceClient *)fSurfaces->getObject(i);
        if (sc->surfaceID() == id) {
            sc->retain();
            found = sc;
        }
    }
    IOLockUnlock(fSurfacesLock);
    return found;
}

void RippleAccelerator::forgetSurfaceFrames(uint32_t handle, uint32_t context)
{
    IOLockLock(fSurfacesLock);
    for (unsigned i = 0; i < fSurfaces->getCount(); i++)
        ((RippleSurfaceClient *)fSurfaces->getObject(i))->forgetFrame(handle, context);
    IOLockUnlock(fSurfacesLock);
}

bool RippleAccelerator::describeSurface(uint32_t id, task_t asker, struct ripple_gpu_surface_info *out)
{
    bool found = false;
    IOLockLock(fSurfacesLock);
    for (unsigned i = 0; i < fSurfaces->getCount() && !found; i++) {
        RippleSurfaceClient *sc = (RippleSurfaceClient *)fSurfaces->getObject(i);
        if (sc->surfaceID() == id)
            found = sc->describe(asker, out);
    }
    IOLockUnlock(fSurfacesLock);
    return found;
}
