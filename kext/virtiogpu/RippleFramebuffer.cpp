/*
 * RippleFramebuffer: see RippleFramebuffer.h. IOFramebuffer API as documented in the 10.6
 * SDK's IOFramebuffer.h and IOGraphicsTypes.h.
 */
#include "RippleFramebuffer.h"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <linux/virtio_gpu.h>

#define super IOFramebuffer
OSDefineMetaClassAndStructors(RippleFramebuffer, IOFramebuffer)

#define LOG(fmt, ...) IOLog("RippleFramebuffer: " fmt "\n", ##__VA_ARGS__)

static const struct { uint32_t w, h; } kStandardModes[] = {
    { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 },
    { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 },
    { 1920, 1080 }, { 1920, 1200 }, { 2048, 1152 }, { 2560, 1440 }, { 2560, 1600 },
};

static IOByteCount frameBytes(uint32_t w, uint32_t h) { return round_page((IOByteCount)w * h * 4); }

// The 10.6.8 kernel's memcmp compares one byte per loop iteration (mach_kernel 10K549,
// _memcmp at 0xffffff800022ac6c), which made the 4 MB damage scan take about 12 ms. Compare
// 8 bytes at a time instead. `n` is a multiple of 4 (whole pixels).
static bool sameBytes(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    const uint64_t *p = (const uint64_t *)a, *q = (const uint64_t *)b;
    uint32_t words = n / 8, i = 0;
    for (; i + 4 <= words; i += 4)
        if ((p[i] ^ q[i]) | (p[i + 1] ^ q[i + 1]) | (p[i + 2] ^ q[i + 2]) | (p[i + 3] ^ q[i + 3]))
            return false;
    for (; i < words; i++)
        if (p[i] != q[i])
            return false;
    if (n & 4)
        return *(const uint32_t *)(a + n - 4) == *(const uint32_t *)(b + n - 4);
    return true;
}

// ---- Setup ----------------------------------------------------------------------------------

bool RippleFramebuffer::buildModes(uint32_t prefW, uint32_t prefH)
{
    fModeCount = 0;
    fPreferred = 0;
    bool prefListed = false;
    for (unsigned i = 0; i < sizeof(kStandardModes) / sizeof(kStandardModes[0]); i++)
        if (kStandardModes[i].w == prefW && kStandardModes[i].h == prefH)
            prefListed = true;
    if (!prefListed && prefW && prefH && frameBytes(prefW, prefH) <= kVRAMSize) {
        fModes[fModeCount].width = prefW;
        fModes[fModeCount].height = prefH;
        fModeCount++;
    }
    for (unsigned i = 0; i < sizeof(kStandardModes) / sizeof(kStandardModes[0]) &&
                         fModeCount < kMaxModes; i++) {
        if (frameBytes(kStandardModes[i].w, kStandardModes[i].h) > kVRAMSize)
            continue;
        if (kStandardModes[i].w == prefW && kStandardModes[i].h == prefH)
            fPreferred = fModeCount;
        fModes[fModeCount].width = kStandardModes[i].w;
        fModes[fModeCount].height = kStandardModes[i].h;
        fModeCount++;
    }
    return fModeCount > 0;
}

bool RippleFramebuffer::start(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;

    fDev = VirtioGPUDevice::withPCIDevice(pci);
    if (!fDev) {
        LOG("virtio-gpu setup failed; leaving the device to the boot framebuffer");
        return false;
    }

    VirtioGPUDevice::DisplayInfo info[VirtioGPUDevice::kMaxScanouts];
    bzero(info, sizeof(info));
    uint32_t prefW = 1024, prefH = 768;
    if (fDev->getDisplayInfo(info, fDev->numScanouts()) && info[0].enabled && info[0].r.width) {
        prefW = info[0].r.width;
        prefH = info[0].r.height;
    }
    if (fDev->hasVirgl())
        publishCapsets();
    if (!buildModes(prefW, prefH))
        goto fail;
    // Cursor: software by default, so the guest draws Snow Leopard's pointer into the picture
    // and VNC consoles show it at the guest's scale (2026-11-10). The hardware cursor
    // (virtio cursor queue; QEMU or the VNC client draws it, no lag) is kept as an option:
    // personality property RippleHardwareCursor, overridden by the boot flag ripple_hwcursor=0|1.
    {
        OSBoolean *hw = OSDynamicCast(OSBoolean, getProperty("RippleHardwareCursor"));
        fHWCursor = hw && hw->isTrue();
        int arg;
        if (PE_parse_boot_argn("ripple_hwcursor", &arg, sizeof(arg)))
            fHWCursor = arg != 0;
        setProperty("RippleCursor", fHWCursor ? "hardware" : "software");
    }
    fCurMode = fStartupMode = fPreferred + 1;

    fVRAM = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared,
                kVRAMSize, 0xFFFFFFFFFFFFF000ULL);
    if (!fVRAM || fVRAM->prepare() != kIOReturnSuccess) {
        LOG("cannot allocate %u MB of contiguous VRAM", kVRAMSize >> 20);
        goto fail;
    }
    bzero(fVRAM->getBytesNoCopy(), kVRAMSize);
    fVRAMPA = fVRAM->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);

    fCursorMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                     kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                     kCursorSize * kCursorSize * 4, 0xFFFFFFFFFFFFF000ULL);
    if (!fCursorMem || fCursorMem->prepare() != kIOReturnSuccess)
        goto fail;

    LOG("host display %ux%u, %u modes, preferred %ux%u, VRAM %u MB at 0x%llx", prefW, prefH,
        fModeCount, fModes[fPreferred].width, fModes[fPreferred].height, kVRAMSize >> 20,
        (unsigned long long)fVRAMPA);

    if (!super::start(provider))
        goto fail;
    return true;

fail:
    if (fDev) {
        fDev->release();         // resets the device, so the boot framebuffer works again
        fDev = 0;
    }
    return false;
}

IOReturn RippleFramebuffer::enableController(void)
{
    if (fEnabled)
        return kIOReturnSuccess;

    // Cursor resource: 64x64 with alpha (hardware cursor only).
    if (fHWCursor) {
        fCursorRes = fDev->allocResourceId();
        if (!fDev->resourceCreate2D(fCursorRes, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM, kCursorSize, kCursorSize) ||
            !fDev->attachBacking(fCursorRes, fCursorMem, kCursorSize * kCursorSize * 4)) {
            LOG("cursor resource failed; using the software cursor");
            fCursorRes = 0;
        }
    }

    if (!applyMode(fCurMode))
        return kIOReturnError;

    fRefresh = IOTimerEventSource::timerEventSource(this, refreshAction);
    if (!fRefresh || getWorkLoop()->addEventSource(fRefresh) != kIOReturnSuccess) {
        LOG("no refresh timer");
        return kIOReturnError;
    }
    fEnabled = true;
    fRefresh->setTimeoutMS(kRefreshMS);
    LOG("enabled at %ux%u", mode(fCurMode).width, mode(fCurMode).height);
    return kIOReturnSuccess;
}

bool RippleFramebuffer::applyMode(IODisplayModeID m)
{
    const Mode &md = mode(m);
    VirtioGPUDevice::Rect none = { 0, 0, 0, 0 }, full = { 0, 0, md.width, md.height };

    fShownRes = 0;                            // a mode change gives the display back
    if (fScanoutRes) {
        fDev->setScanout(0, 0, none);
        fDev->detachBacking(fScanoutRes);
        fDev->resourceUnref(fScanoutRes);
        fScanoutRes = 0;
    }
    uint32_t id = fDev->allocResourceId();
    if (!fDev->resourceCreate2D(id, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, md.width, md.height) ||
        !fDev->attachBacking(id, fVRAM, frameBytes(md.width, md.height)) ||
        !fDev->setScanout(0, id, full)) {
        LOG("mode %ux%u failed", md.width, md.height);
        fDev->resourceUnref(id);              // whatever of it the host has made
        return false;
    }
    fScanoutRes = id;
    // For the 2D plugin (gldriver/ga), which blits in the VRAM itself.
    setProperty("RippleScreenWidth", md.width, 32);
    setProperty("RippleScreenHeight", md.height, 32);
    setProperty("RippleRowBytes", md.width * 4, 32);

    // Shadow copy of the last frame sent, for damage tracking; start with a full refresh.
    if (fShadow) {
        fShadow->release();
        fShadow = 0;
    }
    fShadow = IOBufferMemoryDescriptor::withCapacity(frameBytes(md.width, md.height), kIODirectionNone);
    if (!fShadow) {
        LOG("no memory for the shadow frame");
        return false;
    }
    fFullRefresh = true;
    refresh();
    return true;
}

bool RippleFramebuffer::refresh()
{
    if (!fScanoutRes || !fShadow || fShownRes)
        return false;                         // a GPU buffer is on the display
    const Mode &md = mode(fCurMode);
    const uint32_t stride = md.width * 4;
    const uint8_t *vram = (const uint8_t *)fVRAM->getBytesNoCopy();
    uint8_t *shadow = (uint8_t *)fShadow->getBytesNoCopy();

    // Damage: the bounding box of everything that differs from the last frame sent. Rows are
    // compared whole, then the changed rows column by column in 64-pixel chunks.
    enum { kChunk = 64 };
    uint32_t y0 = md.height, y1 = 0, x0 = md.width, x1 = 0;
    for (uint32_t y = 0; y < md.height; y++) {
        const uint8_t *v = vram + y * stride;
        uint8_t *s = shadow + y * stride;
        if (!fFullRefresh && sameBytes(v, s, stride))
            continue;
        if (y < y0) y0 = y;
        y1 = y;
        for (uint32_t x = 0; x < md.width; x += kChunk) {
            uint32_t n = (md.width - x < kChunk ? md.width - x : kChunk) * 4;
            if (fFullRefresh || !sameBytes(v + x * 4, s + x * 4, n)) {
                if (x < x0) x0 = x;
                if (x + n / 4 > x1) x1 = x + n / 4;
            }
        }
    }
    fFullRefresh = false;
    if (y0 > y1)
        return false;                         // nothing changed

    VirtioGPUDevice::Rect r = { x0, y0, x1 - x0, y1 + 1 - y0 };
    fStatFrames++;
    fStatPixels += (uint64_t)r.width * r.height;
    for (uint32_t y = y0; y <= y1; y++)
        memcpy(shadow + y * stride + x0 * 4, vram + y * stride + x0 * 4, r.width * 4);
    fDev->transferToHost2D(fScanoutRes, r, (uint64_t)y0 * stride + x0 * 4);
    fDev->resourceFlush(fScanoutRes, r);
    return true;
}

void RippleFramebuffer::refreshAction(OSObject *owner, IOTimerEventSource *sender)
{
    RippleFramebuffer *fb = OSDynamicCast(RippleFramebuffer, owner);
    if (!fb || !fb->fEnabled)
        return;
    uint64_t t0 = mach_absolute_time();
    bool changed = fb->refresh();
    fb->fUnchanged = changed ? 0 : fb->fUnchanged + 1;
    fb->fStatTime += mach_absolute_time() - t0;
    if (++fb->fStatTicks % 600 == 0) {      // statistics over the last 600 ticks
        uint64_t ns;
        absolutetime_to_nanoseconds(fb->fStatTime, &ns);
        // Published, not logged: `ioreg -c RippleFramebuffer -l | grep RippleRefresh`.
        fb->setProperty("RippleRefreshFramesSent", fb->fStatFrames, 32);
        fb->setProperty("RippleRefreshPixelsSent", fb->fStatPixels, 64);
        fb->setProperty("RippleRefreshAverageMicroseconds", ns / 600 / 1000, 32);
        uint32_t slowUs, slowType, over;
        fb->fDev->takeSlowest(&slowUs, &slowType, &over);
        fb->setProperty("RippleSlowestCommandMicroseconds", slowUs, 32);
        fb->setProperty("RippleSlowestCommandType", slowType, 32);
        fb->setProperty("RippleCommandsOver100ms", over, 32);
        uint32_t ints, byInt, byPoll;
        fb->fDev->interruptStats(&ints, &byInt, &byPoll);
        fb->setProperty("RippleInterrupts", ints, 32);
        fb->setProperty("RippleCompletedByInterrupt", byInt, 32);
        fb->setProperty("RippleCompletedByPolling", byPoll, 32);
        fb->fStatFrames = 0;
        fb->fStatPixels = 0;
        fb->fStatTime = 0;
    }
    // Poll fast while things change, slower once the screen has been still for a while.
    sender->setTimeoutMS(fb->fUnchanged >= kIdleFrames ? kIdleRefreshMS : kRefreshMS);
}

void RippleFramebuffer::stop(IOService *provider)
{
    fEnabled = false;
    if (fRefresh) {
        fRefresh->cancelTimeout();
        getWorkLoop()->removeEventSource(fRefresh);
    }
    super::stop(provider);
}

void RippleFramebuffer::free()
{
    if (fRefresh)    fRefresh->release();
    if (fDev)        fDev->release();
    if (fVRAM)       { fVRAM->complete();      fVRAM->release(); }
    if (fCursorMem)  { fCursorMem->complete(); fCursorMem->release(); }
    if (fShadow)     fShadow->release();
    super::free();
}

bool RippleFramebuffer::isConsoleDevice(void) { return true; }

// ---- Memory and modes -----------------------------------------------------------------------

// The "VRAM" is ordinary RAM, so hand out slices of the buffer descriptor itself rather than
// IODeviceMemory physical ranges, which get mapped as uncached device memory. Under KVM that
// only matters when the guest's cache attributes are honoured (a VM with a passed-through
// device), but RAM should be mapped as RAM. IOFramebuffer only uses IOMemoryDescriptor methods
// on the result; IODeviceMemory adds no virtual methods.
IODeviceMemory *RippleFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture || !validMode(fCurMode))
        return 0;
    const Mode &md = mode(fCurMode);
    return (IODeviceMemory *)IOSubMemoryDescriptor::withSubRange(fVRAM, 0,
                frameBytes(md.width, md.height), kIODirectionInOut);
}

IODeviceMemory *RippleFramebuffer::getVRAMRange(void)
{
    return (IODeviceMemory *)IOSubMemoryDescriptor::withSubRange(fVRAM, 0, kVRAMSize,
                                                                 kIODirectionInOut);
}

const char *RippleFramebuffer::getPixelFormats(void)
{
    static const char formats[] = IO32BitDirectPixels "\0";
    return formats;
}

IOItemCount RippleFramebuffer::getDisplayModeCount(void) { return fModeCount; }

IOReturn RippleFramebuffer::getDisplayModes(IODisplayModeID *all)
{
    for (unsigned i = 0; i < fModeCount; i++)
        all[i] = i + 1;
    return kIOReturnSuccess;
}

IOReturn RippleFramebuffer::getInformationForDisplayMode(IODisplayModeID m, IODisplayModeInformation *info)
{
    if (!validMode(m) || !info)
        return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->nominalWidth = mode(m).width;
    info->nominalHeight = mode(m).height;
    info->refreshRate = 60 << 16;
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag |
                  ((unsigned)(m - 1) == fPreferred ? kDisplayModeDefaultFlag : 0);
    return kIOReturnSuccess;
}

UInt64 RippleFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) { return 0; }

IOReturn RippleFramebuffer::getPixelInformation(IODisplayModeID m, IOIndex depth,
                                                IOPixelAperture aperture, IOPixelInformation *pi)
{
    if (!validMode(m) || depth != 0 || !pi)
        return kIOReturnBadArgument;
    if (aperture != kIOFBSystemAperture)
        return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow = mode(m).width * 4;
    pi->bitsPerPixel = 32;
    pi->pixelType = kIORGBDirectPixels;
    pi->componentCount = 3;
    pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00ff0000;
    pi->componentMasks[1] = 0x0000ff00;
    pi->componentMasks[2] = 0x000000ff;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    pi->activeWidth = mode(m).width;
    pi->activeHeight = mode(m).height;
    return kIOReturnSuccess;
}

IOReturn RippleFramebuffer::getCurrentDisplayMode(IODisplayModeID *m, IOIndex *depth)
{
    if (m)     *m = fCurMode;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

IOReturn RippleFramebuffer::setDisplayMode(IODisplayModeID m, IOIndex depth)
{
    if (!validMode(m) || depth != 0)
        return kIOReturnBadArgument;
    if (m == fCurMode && fScanoutRes)
        return kIOReturnSuccess;
    if (fRefresh)
        fRefresh->cancelTimeout();
    IODisplayModeID old = fCurMode;
    fCurMode = m;
    bool ok = !fEnabled || applyMode(m);
    if (!ok) {
        fCurMode = old;
        applyMode(old);
    }
    if (fEnabled && fRefresh)
        fRefresh->setTimeoutMS(kRefreshMS);
    LOG("mode %ux%u %s", mode(m).width, mode(m).height, ok ? "set" : "failed");
    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn RippleFramebuffer::setStartupDisplayMode(IODisplayModeID m, IOIndex depth)
{
    if (!validMode(m) || depth != 0)
        return kIOReturnBadArgument;
    fStartupMode = m;
    return kIOReturnSuccess;
}

IOReturn RippleFramebuffer::getStartupDisplayMode(IODisplayModeID *m, IOIndex *depth)
{
    if (m)     *m = fStartupMode;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

// ---- Attributes -----------------------------------------------------------------------------

// Four-character attribute codes, for the log.
static const char *fourcc(IOSelect a, char *buf)
{
    buf[0] = (char)(a >> 24); buf[1] = (char)(a >> 16); buf[2] = (char)(a >> 8); buf[3] = (char)a;
    buf[4] = 0;
    return buf;
}

IOReturn RippleFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value)
{
    if (attribute == kIOHardwareCursorAttribute) {
        if (value)
            *value = (fHWCursor && fCursorRes) ? 1 : 0;   // 0: IOFramebuffer draws the cursor
        return kIOReturnSuccess;
    }
    IOReturn r = super::getAttribute(attribute, value);
    char b[5];
    LOG("getAttribute '%s' -> 0x%x (value %lu)", fourcc(attribute, b), r, value ? (unsigned long)*value : 0UL);
    return r;
}

IOReturn RippleFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    IOReturn r = super::setAttribute(attribute, value);
    char b[5];
    LOG("setAttribute '%s' %lu -> 0x%x", fourcc(attribute, b), (unsigned long)value, r);
    return r;
}

IOItemCount RippleFramebuffer::getConnectionCount(void) { return 1; }

IOReturn RippleFramebuffer::getAttributeForConnection(IOIndex ci, IOSelect attribute, uintptr_t *value)
{
    switch (attribute) {
    case kConnectionEnable:
    case kConnectionCheckEnable:
        if (value)
            *value = 1;
        return kIOReturnSuccess;
    default:
        return super::getAttributeForConnection(ci, attribute, value);
    }
}

IOReturn RippleFramebuffer::setAttributeForConnection(IOIndex ci, IOSelect attribute, uintptr_t value)
{
    switch (attribute) {
    case kConnectionCheckEnable:
        return kIOReturnSuccess;
    default:
        return super::setAttributeForConnection(ci, attribute, value);
    }
}

// ---- Hardware cursor ------------------------------------------------------------------------

IOReturn RippleFramebuffer::setCursorImage(void *cursorImage)
{
    if (!fCursorRes)
        return kIOReturnUnsupported;

    IOHardwareCursorDescriptor desc;
    bzero(&desc, sizeof(desc));
    desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
    desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
    desc.width = kCursorSize;
    desc.height = kCursorSize;
    desc.bitDepth = 32;

    IOHardwareCursorInfo info;
    bzero(&info, sizeof(info));
    info.majorVersion = kHardwareCursorInfoMajorVersion;
    info.minorVersion = kHardwareCursorInfoMinorVersion;
    // convertCursorImage writes the image packed at its own width (cursorWidth pixels per row),
    // not at the descriptor's width: a 32-pixel cursor read as 64 wide showed two squashed
    // copies side by side (2026-11-10). Convert into a scratch buffer, then re-stride.
    const IOByteCount bytes = kCursorSize * kCursorSize * 4;
    UInt8 *tmp = (UInt8 *)IOMalloc(bytes);
    if (!tmp)
        return kIOReturnNoMemory;
    bzero(tmp, bytes);
    info.hardwareCursorData = tmp;
    if (!convertCursorImage(cursorImage, &desc, &info) ||
        info.cursorWidth > kCursorSize || info.cursorHeight > kCursorSize) {
        IOFree(tmp, bytes);
        return kIOReturnUnsupported;
    }
    UInt8 *dst = (UInt8 *)fCursorMem->getBytesNoCopy();
    bzero(dst, bytes);
    for (UInt32 y = 0; y < info.cursorHeight; y++)
        memcpy(dst + y * kCursorSize * 4, tmp + y * info.cursorWidth * 4, info.cursorWidth * 4);
    IOFree(tmp, bytes);
    if (fCursorLogs < 3) {
        fCursorLogs++;
        LOG("cursor image %ux%u, hot spot %u,%u", info.cursorWidth, info.cursorHeight,
            info.cursorHotSpotX, info.cursorHotSpotY);
    }
    fCursorHotX = info.cursorHotSpotX;
    fCursorHotY = info.cursorHotSpotY;

    VirtioGPUDevice::Rect r = { 0, 0, kCursorSize, kCursorSize };
    fDev->transferToHost2D(fCursorRes, r, 0);
    fCursorShown = false;              // the next showCursor() sends UPDATE_CURSOR
    if (fCursorVisible)
        showCursor();
    return kIOReturnSuccess;
}

void RippleFramebuffer::showCursor()
{
    // IOFramebuffer gives the image's top-left corner; virtio wants the pointer position (the
    // hot spot), which is what QEMU and VNC clients draw around.
    int32_t x = fCursorX + (int32_t)fCursorHotX, y = fCursorY + (int32_t)fCursorHotY;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (!fCursorShown) {
        fDev->updateCursor(0, fCursorRes, x, y, fCursorHotX, fCursorHotY);
        fCursorShown = true;
    } else {
        fDev->moveCursor(0, x, y);
    }
}

IOReturn RippleFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)
{
    if (!fCursorRes)
        return kIOReturnUnsupported;
    fCursorX = x;
    fCursorY = y;
    fCursorVisible = visible;
    if (visible) {
        showCursor();
    } else if (fCursorShown) {
        fDev->updateCursor(0, 0, 0, 0, 0, 0);   // resource 0 hides the cursor
        fCursorShown = false;
    }
    return kIOReturnSuccess;
}

// ---- Diagnostics ----------------------------------------------------------------------------

// Publish the host's capability sets as RippleCapset<id> data properties, so they can be read
// with ioreg before the 3D path exists (M3; virgl: id 1 = v1, id 2 = v2).
void RippleFramebuffer::publishCapsets()
{
    for (uint32_t i = 0; i < fDev->numCapsets(); i++) {
        uint32_t id, maxVer, maxSize;
        if (!fDev->getCapsetInfo(i, &id, &maxVer, &maxSize)) {
            LOG("capset %u: GET_CAPSET_INFO failed", i);
            continue;
        }
        void *buf = IOMalloc(maxSize);
        if (!buf)
            continue;
        uint32_t got = fDev->getCapset(id, maxVer, buf, maxSize);
        LOG("capset %u: id %u, max version %u, max size %u, got %u bytes", i, id, maxVer, maxSize, got);
        if (got) {
            char key[32];
            snprintf(key, sizeof(key), "RippleCapset%u", id);
            setProperty(key, buf, got);
        }
        IOFree(buf, maxSize);
    }
}

// ---- A GPU buffer on the display (uapi DISPLAY) ---------------------------------------------

IOReturn RippleFramebuffer::showAction(OSObject *owner, void *idp, void *rectp, void *, void *)
{
    RippleFramebuffer *fb = (RippleFramebuffer *)owner;
    uint32_t id = (uint32_t)(uintptr_t)idp;
    const VirtioGPUDevice::Rect *r = (const VirtioGPUDevice::Rect *)rectp;

    if (!id) {                                // back to the framebuffer
        if (!fb->fShownRes)
            return kIOReturnSuccess;
        fb->fShownRes = 0;
        const Mode &md = fb->mode(fb->fCurMode);
        VirtioGPUDevice::Rect full = { 0, 0, md.width, md.height };
        if (fb->fScanoutRes)
            fb->fDev->setScanout(0, fb->fScanoutRes, full);
        fb->fFullRefresh = true;
        fb->refresh();
        return kIOReturnSuccess;
    }
    // The rectangle has to lie on the screen. The host sizes its console to it without looking
    // at the buffer, and QEMU aborts on a size it cannot allocate: one DISPLAY with a huge
    // rectangle from any process would end the VM.
    const Mode &md = fb->mode(fb->fCurMode);
    if (!r->width || !r->height || (uint64_t)r->x + r->width > md.width ||
        (uint64_t)r->y + r->height > md.height)
        return kIOReturnBadArgument;
    if (fb->fShownRes != id || memcmp(&fb->fShownRect, r, sizeof(*r)) != 0) {
        if (!fb->fDev->setScanout(0, id, *r))
            return kIOReturnError;
        fb->fShownRes = id;
        fb->fShownRect = *r;
    }
    return fb->fDev->resourceFlush(id, *r) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn RippleFramebuffer::showResource(uint32_t id, uint32_t x, uint32_t y, uint32_t width,
                                         uint32_t height)
{
    VirtioGPUDevice::Rect r = { x, y, width, height };
    return getWorkLoop()->runAction(&RippleFramebuffer::showAction, this, (void *)(uintptr_t)id,
                                    &r);
}

void RippleFramebuffer::releaseResource(uint32_t id)
{
    if (id && fShownRes == id)
        getWorkLoop()->runAction(&RippleFramebuffer::showAction, this, 0, 0);
}

// ---- Surfaces (RippleSurfaceClient) ---------------------------------------------------------

void RippleFramebuffer::screenSize(uint32_t *width, uint32_t *height)
{
    const Mode &md = mode(fCurMode);
    *width = md.width;
    *height = md.height;
}

struct CopyArgs {
    IOMemoryDescriptor *src;
    uint32_t rowBytes;
    int32_t ox, oy;
    const IOAccelBounds *rects;
    uint32_t count;
};

IOReturn RippleFramebuffer::copyAction(OSObject *owner, void *argp, void *, void *, void *)
{
    RippleFramebuffer *fb = (RippleFramebuffer *)owner;
    const CopyArgs *a = (const CopyArgs *)argp;
    const Mode &md = fb->mode(fb->fCurMode);
    const uint32_t stride = md.width * 4;
    uint8_t *vram = (uint8_t *)fb->fVRAM->getBytesNoCopy();
    uint64_t srcLength = a->src->getLength();

    for (uint32_t i = 0; i < a->count; i++) {
        int32_t x0 = a->rects[i].x, y0 = a->rects[i].y;
        int32_t x1 = x0 + a->rects[i].w, y1 = y0 + a->rects[i].h;
        // Clip to the screen and to the part of the screen the backing covers.
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 < a->ox) x0 = a->ox;
        if (y0 < a->oy) y0 = a->oy;
        if (x1 > (int32_t)md.width) x1 = md.width;
        if (y1 > (int32_t)md.height) y1 = md.height;
        if (x1 <= x0 || y1 <= y0)
            continue;
        uint32_t bytes = (x1 - x0) * 4;
        for (int32_t y = y0; y < y1; y++) {
            uint64_t off = (uint64_t)(y - a->oy) * a->rowBytes + (uint64_t)(x0 - a->ox) * 4;
            if (off + bytes > srcLength)
                break;
            a->src->readBytes(off, vram + (size_t)y * stride + (size_t)x0 * 4, bytes);
        }
    }
    fb->refresh();                      // damage tracking finds what changed
    return kIOReturnSuccess;
}

IOReturn RippleFramebuffer::copyToScreen(IOMemoryDescriptor *src, uint32_t rowBytes, int32_t ox,
                                         int32_t oy, const IOAccelBounds *rects, uint32_t count)
{
    CopyArgs a = { src, rowBytes, ox, oy, rects, count };
    return getWorkLoop()->runAction(&RippleFramebuffer::copyAction, this, &a);
}
