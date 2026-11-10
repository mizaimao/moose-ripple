/*
 * RippleAccelerator: see RippleAccelerator.h.
 */
#include "RippleAccelerator.h"
#include "RippleFramebuffer.h"
#include "RippleGPUClient.h"

#include <IOKit/IOLib.h>

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
    registerService();
    LOG("started: backend virgl, %u capability blobs", fInfo.num_caps);
    return true;
}

void RippleAccelerator::stop(IOService *provider)
{
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
