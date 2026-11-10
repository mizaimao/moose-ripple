/*
 * RippleAccelProbe: M1 loader experiment. Attaches to the virtio-gpu PCI device as an
 * IOAccelerator whose IOGLBundleName (from Info.plist) is "RippleGLDriver", and points the
 * framebuffer on the same device at itself, which is all IOAccelFindAccelerator needs
 * (IOKit 10K549: framebuffer "IOAccelTypes" = registry path of an IOAccelerator, plus
 * "IOAccelIndex"; docs/re/gl-plugin-interface.md). Question: will OpenGL then load our bundle?
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/graphics/IOFramebuffer.h>

class RippleAccelProbe : public IOAccelerator
{
    OSDeclareDefaultStructors(RippleAccelProbe)
    IOFramebuffer *fFramebuffer;
public:
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
};

OSDefineMetaClassAndStructors(RippleAccelProbe, IOAccelerator)

bool RippleAccelProbe::start(IOService *provider)
{
    if (!IOAccelerator::start(provider))
        return false;

    fFramebuffer = 0;
    OSIterator *it = provider->getClientIterator();
    if (it) {
        OSObject *o;
        while ((o = it->getNextObject())) {
            IOFramebuffer *fb = OSDynamicCast(IOFramebuffer, o);
            if (fb) {
                fFramebuffer = fb;
                fb->retain();
                break;
            }
        }
        it->release();
    }

    registerService();

    char path[256];
    int len = sizeof(path);
    if (!getPath(path, &len, gIOServicePlane)) {
        IOLog("RippleAccelProbe: getPath failed\n");
        return true;
    }
    if (fFramebuffer) {
        fFramebuffer->setProperty("IOAccelTypes", path);
        fFramebuffer->setProperty("IOAccelIndex", 0ULL, 32);
        fFramebuffer->setProperty("IOAccelRevision", 2ULL, 32);
        IOLog("RippleAccelProbe: started; framebuffer %s now points at %s\n",
              fFramebuffer->getName(), path);
    } else {
        IOLog("RippleAccelProbe: started at %s, but no framebuffer on the device\n", path);
    }
    return true;
}

void RippleAccelProbe::stop(IOService *provider)
{
    if (fFramebuffer) {
        fFramebuffer->removeProperty("IOAccelTypes");
        fFramebuffer->removeProperty("IOAccelIndex");
        fFramebuffer->removeProperty("IOAccelRevision");
        fFramebuffer->release();
        fFramebuffer = 0;
    }
    IOLog("RippleAccelProbe: stopped\n");
    IOAccelerator::stop(provider);
}
