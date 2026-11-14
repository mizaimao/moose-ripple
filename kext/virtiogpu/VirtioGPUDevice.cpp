/*
 * VirtioGPUDevice: see VirtioGPUDevice.h. Protocol definitions come from Linux's BSD-licensed
 * virtio headers in third_party/virtio (virtio 1.0 spec: PCI transport, GPU device).
 */
#include "VirtioGPUDevice.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#include <linux/virtio_config.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_gpu.h>

#ifndef offsetof
#define offsetof(t, m) __builtin_offsetof(t, m)   /* no stddef.h in Kernel.framework */
#endif

#define super OSObject
OSDefineMetaClassAndStructors(VirtioGPUDevice, OSObject)

#define LOG(fmt, ...) IOLog("RippleVirtioGPU: " fmt "\n", ##__VA_ARGS__)

// ---- MMIO helpers --------------------------------------------------------------------------

static inline uint8_t  rd8(volatile uint8_t *b, size_t o)  { return *(volatile uint8_t *)(b + o); }
static inline uint16_t rd16(volatile uint8_t *b, size_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t rd32(volatile uint8_t *b, size_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr8(volatile uint8_t *b, size_t o, uint8_t v)   { *(volatile uint8_t *)(b + o) = v; }
static inline void wr16(volatile uint8_t *b, size_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void wr32(volatile uint8_t *b, size_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void wr64(volatile uint8_t *b, size_t o, uint64_t v)
{
    wr32(b, o, (uint32_t)v);
    wr32(b, o + 4, (uint32_t)(v >> 32));
}

#define CC(field) offsetof(struct virtio_pci_common_cfg, field)

static uint64_t physAddr(IOMemoryDescriptor *md, IOByteCount offset = 0)
{
    return md->getPhysicalSegment(offset, NULL, kIOMemoryMapperNone);
}

static IOBufferMemoryDescriptor *contiguous(IOByteCount size, IOOptionBits dir = kIODirectionInOut)
{
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        dir | kIOMemoryPhysicallyContiguous, size, 0xFFFFFFFFFFFFF000ULL);
    if (md && md->prepare() != kIOReturnSuccess) {
        md->release();
        md = 0;
    }
    if (md)
        bzero(md->getBytesNoCopy(), size);
    return md;
}

// ---- Setup ----------------------------------------------------------------------------------

VirtioGPUDevice *VirtioGPUDevice::withPCIDevice(IOPCIDevice *pci)
{
    VirtioGPUDevice *d = new VirtioGPUDevice;
    if (d && !d->init(pci)) {
        d->release();
        d = 0;
    }
    return d;
}

uint8_t VirtioGPUDevice::status()            { return rd8(fCommon, CC(device_status)); }
void    VirtioGPUDevice::setStatus(uint8_t s) { wr8(fCommon, CC(device_status), s); }

void VirtioGPUDevice::reset()
{
    if (!fCommon)
        return;
    setStatus(0);
    for (int i = 0; i < 1000 && status() != 0; i++)
        IODelay(100);
}

bool VirtioGPUDevice::mapCapabilities()
{
    if (!(fPCI->configRead16(kIOPCIConfigStatus) & 0x10)) {
        LOG("no PCI capability list");
        return false;
    }
    uint8_t ptr = fPCI->configRead8(0x34) & 0xfc;
    for (int guard = 0; ptr && guard < 48; guard++) {
        uint8_t id = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_VNDR);
        uint8_t next = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_NEXT) & 0xfc;
        if (id == 0x09) {                               // vendor-specific: virtio
            uint8_t type = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_CFG_TYPE);
            uint8_t bar = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_BAR);
            uint32_t off = fPCI->configRead32(ptr + VIRTIO_PCI_CAP_OFFSET);
            if (bar < kNumBars && type >= VIRTIO_PCI_CAP_COMMON_CFG && type <= VIRTIO_PCI_CAP_DEVICE_CFG) {
                if (!fBarMap[bar])
                    fBarMap[bar] = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + 4 * bar);
                if (!fBarMap[bar]) {
                    LOG("cannot map BAR %u", bar);
                    return false;
                }
                // A BAR above 4 GB works as long as the guest's physical address width fits the
                // host CPU's: with QEMU's default of 40 bits on a 39-bit host, 10.6 moved the BAR
                // to 0xff10100000, beyond what KVM can back, and the first access faulted forever
                // (decision 0007, KI-2). With -cpu ...,host-phys-bits=on it lands within reach
                // (#18: UTM's 3D display needs a 64-bit PCI window). Logged, not refused.
                if (fBarMap[bar]->getPhysicalAddress() + fBarMap[bar]->getLength() > 0x100000000ULL)
                    LOG("BAR %u is above 4 GB (0x%llx); if the guest hangs here, give it the host's "
                        "physical address width (-cpu ...,host-phys-bits=on) or keep BARs below 4 GB",
                        bar, (unsigned long long)fBarMap[bar]->getPhysicalAddress());
                volatile uint8_t *base = (volatile uint8_t *)fBarMap[bar]->getVirtualAddress() + off;
                switch (type) {
                case VIRTIO_PCI_CAP_COMMON_CFG: if (!fCommon) fCommon = base; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    if (!fNotify) {
                        fNotify = base;
                        fNotifyMult = fPCI->configRead32(ptr + offsetof(struct virtio_pci_notify_cap,
                                                                        notify_off_multiplier));
                    }
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:    if (!fISR) fISR = base; break;
                case VIRTIO_PCI_CAP_DEVICE_CFG: if (!fDevCfg) fDevCfg = base; break;
                }
            }
        }
        ptr = next;
    }
    if (!fCommon || !fNotify || !fISR || !fDevCfg) {
        LOG("missing virtio capabilities");
        return false;
    }
    return true;
}

bool VirtioGPUDevice::setupQueue(unsigned index)
{
    Queue &q = fQueue[index];

    wr16(fCommon, CC(queue_select), (uint16_t)index);
    uint16_t max = rd16(fCommon, CC(queue_size));
    if (max == 0) {
        LOG("queue %u not available", index);
        return false;
    }
    unsigned num = max < kCtrlQueueSize ? max : kCtrlQueueSize;
    wr16(fCommon, CC(queue_size), (uint16_t)num);

    q.mem = contiguous(ripple_vq_bytes(num));
    if (!q.mem) {
        LOG("queue %u: no memory", index);
        return false;
    }
    ripple_vq_init(&q.vq, num, q.mem->getBytesNoCopy(), physAddr(q.mem));
    wr64(fCommon, CC(queue_desc_lo), q.vq.desc_pa);
    wr64(fCommon, CC(queue_avail_lo), q.vq.avail_pa);
    wr64(fCommon, CC(queue_used_lo), q.vq.used_pa);
    q.notify = (volatile uint16_t *)(fNotify + (uint32_t)rd16(fCommon, CC(queue_notify_off)) * fNotifyMult);
    q.index = (uint16_t)index;
    wr16(fCommon, CC(queue_enable), 1);
    return true;
}

bool VirtioGPUDevice::negotiate()
{
    // virtio 1.0 initialisation (spec 3.1.1). Accept VERSION_1, plus VIRGL when offered (3D).
    reset();
    setStatus(VIRTIO_CONFIG_S_ACKNOWLEDGE);
    setStatus(VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
    wr32(fCommon, CC(device_feature_select), 0);
    uint32_t lo = rd32(fCommon, CC(device_feature));
    wr32(fCommon, CC(device_feature_select), 1);
    uint32_t hi = rd32(fCommon, CC(device_feature));
    if (!(hi & (1u << (VIRTIO_F_VERSION_1 - 32)))) {
        LOG("device lacks VIRTIO_F_VERSION_1");
        return false;
    }
    fFeatures = lo;
    fVirgl = (lo & (1u << VIRTIO_GPU_F_VIRGL)) != 0;
    wr32(fCommon, CC(guest_feature_select), 0);
    wr32(fCommon, CC(guest_feature), fVirgl ? (1u << VIRTIO_GPU_F_VIRGL) : 0);
    wr32(fCommon, CC(guest_feature_select), 1);
    wr32(fCommon, CC(guest_feature), 1u << (VIRTIO_F_VERSION_1 - 32));
    setStatus(status() | VIRTIO_CONFIG_S_FEATURES_OK);
    if (!(status() & VIRTIO_CONFIG_S_FEATURES_OK)) {
        LOG("device refused our features");
        return false;
    }
    if (!setupQueue(0) || !setupQueue(1))
        return false;
    setStatus(status() | VIRTIO_CONFIG_S_DRIVER_OK);
    (void)rd8(fISR, 0);                         // clear anything pending
    fNumScanouts = rd32(fDevCfg, offsetof(struct virtio_gpu_config, num_scanouts));
    fNumCapsets = rd32(fDevCfg, offsetof(struct virtio_gpu_config, num_capsets));
    LOG("device ready: features 0x%08x%08x, %u scanouts, %u capsets", hi, lo,
        fNumScanouts, fNumCapsets);
    return true;
}

bool VirtioGPUDevice::init(IOPCIDevice *pci)
{
    if (!super::init() || !pci)
        return false;
    fPCI = pci;
    fPCI->retain();
    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);

    fCmdLock = IOLockAlloc();
    uint32_t arg = 0;
    fAsync = PE_parse_boot_argn("ripple_async", &arg, sizeof(arg)) && arg != 0;
    fCursorLock = IOLockAlloc();
    fSlotMem = contiguous(kSlots * (kSlotReq + kSlotResp));
    for (int i = 0; i < kCtrlQueueSize; i++)
        fHeadSlot[i] = -1;
    fCursorBuf = contiguous(kCursorSlots * sizeof(struct virtio_gpu_update_cursor), kIODirectionOut);
    if (!fCmdLock || !fCursorLock || !fSlotMem || !fCursorBuf)
        return false;
    fSlotPA = physAddr(fSlotMem);
    fCursorBufPA = physAddr(fCursorBuf);
    if (!mapCapabilities() || !negotiate())
        return false;
    if (!setupInterrupts())
        LOG("no interrupt; completing commands by polling");
    return true;
}

void VirtioGPUDevice::free()
{
    reset();
    if (fIntSrc) {                      // before the rings it reads go away
        fIntSrc->disable();
        if (fWorkLoop)
            fWorkLoop->removeEventSource(fIntSrc);
        fIntSrc->release();
    }
    for (unsigned i = 0; i < 2; i++) {
        if (fQueue[i].mem) {
            fQueue[i].mem->complete();
            fQueue[i].mem->release();
        }
    }
    if (fWorkLoop)  fWorkLoop->release();
    if (fSlotMem)   { fSlotMem->complete();   fSlotMem->release(); }
    if (fCursorBuf) { fCursorBuf->complete(); fCursorBuf->release(); }
    if (fCmdLock)    IOLockFree(fCmdLock);
    if (fCursorLock) IOLockFree(fCursorLock);
    for (unsigned i = 0; i < kNumBars; i++)
        if (fBarMap[i])
            fBarMap[i]->release();
    if (fPCI)
        fPCI->release();
    super::free();
}

// ---- Control queue --------------------------------------------------------------------------

uint32_t VirtioGPUDevice::command(const void *req, uint32_t reqLen, void *resp, uint32_t respLen,
                                  uint64_t extraPA, uint32_t extraLen, uint64_t *fence,
                                  unsigned async, volatile UInt32 *release)
{
    if (reqLen > kSlotReq || respLen > kSlotResp)
        return 0;
    const uint32_t cmdType = ((const struct virtio_gpu_ctrl_hdr *)req)->type;

    IOLockLock(fCmdLock);
    // A free slot (up to kSlots commands in flight; callers queue here otherwise).
    int s = -1;
    for (;;) {
        for (int i = 0; i < kSlots; i++)
            if (!fSlot[i].busy) { s = i; break; }
        if (s >= 0)
            break;
        // None free. Slots sent without a waiter (async) or given up on after a timeout (lost)
        // are only freed by a drain, and with the interrupt off nobody else drains: do it
        // here, and look again soon rather than sleep until a wakeup that may not come.
        drainControlLocked(false);
        for (int i = 0; i < kSlots; i++)
            if (!fSlot[i].busy) { s = i; break; }
        if (s >= 0)
            break;
        uint64_t deadline;
        clock_interval_to_deadline(20, kMillisecondScale, &deadline);
        IOLockSleepDeadline(fCmdLock, fSlot, deadline, THREAD_UNINT);
    }
    fSlot[s].busy = true;
    fSlot[s].done = false;
    uint8_t *reqBuf = (uint8_t *)fSlotMem->getBytesNoCopy() + s * (kSlotReq + kSlotResp);
    uint8_t *respBuf = reqBuf + kSlotReq;
    uint64_t reqPA = fSlotPA + s * (kSlotReq + kSlotResp), respPA = reqPA + kSlotReq;
    memcpy(reqBuf, req, reqLen);
    bzero(respBuf, respLen);
    if (fence) {                       // numbered here, under the lock: queue order
        uint64_t id = (uint64_t)++fNextFence;
        ((struct virtio_gpu_ctrl_hdr *)reqBuf)->fence_id = id;
        *fence = id;
    }

    uint64_t pa[3];
    uint32_t len[3];
    unsigned nout = 0;
    pa[nout] = reqPA;                  len[nout++] = reqLen;
    if (extraLen) { pa[nout] = extraPA; len[nout++] = extraLen; }
    pa[nout] = respPA;                 len[nout] = respLen;

    Queue &q = fQueue[0];
    uint32_t type = 0;
    int head = ripple_vq_add(&q.vq, pa, len, nout, 1);
    if (head < 0) {
        LOG("control queue full");
        fSlot[s].busy = false;
        IOLockWakeup(fCmdLock, fSlot, false);
        IOLockUnlock(fCmdLock);
        return 0;
    }
    fHeadSlot[head] = (int8_t)s;
    uint64_t t0 = mach_absolute_time(), timeout;
    nanoseconds_to_absolutetime(5000000000ULL, &timeout);
    if (async) {
        // Nobody waits: the drain frees the slot when the reply comes (and, for a fenced
        // command, passes its fence then).
        fSlot[s].async = true;
        fSlot[s].fence = (async == kAsyncFence && fence) ? *fence : 0;
        fSlot[s].release = release;
        if (release)
            *release = 1;
        *q.notify = q.index;
        IOLockUnlock(fCmdLock);
        return VIRTIO_GPU_RESP_OK_NODATA;
    }
    *q.notify = q.index;

    while (!fSlot[s].done) {
        drainControlLocked(false);              // polling fallback
        if (fSlot[s].done)
            break;
        if (mach_absolute_time() - t0 > timeout)
            break;
        uint64_t deadline;
        clock_interval_to_deadline(fIntEnabled ? 20 : 1, kMillisecondScale, &deadline);
        IOLockSleepDeadline(fCmdLock, &fSlot[s], deadline, THREAD_UNINT);
    }

    if (fSlot[s].done) {
        memcpy(resp, respBuf, respLen);
        type = ((struct virtio_gpu_ctrl_hdr *)resp)->type;
        uint64_t dt = mach_absolute_time() - t0, ns;
        if (dt > fSlowestAbs) {
            fSlowestAbs = dt;
            fSlowestType = cmdType;
        }
        absolutetime_to_nanoseconds(dt, &ns);
        if (ns > 100000000ULL)
            fOver100ms++;
        fSlot[s].busy = false;
    } else {
        // The device still owns the buffers: never reuse this slot.
        LOG("command 0x%x timed out; slot %d retired", cmdType, s);
        fSlot[s].lost = true;
    }
    IOLockWakeup(fCmdLock, fSlot, false);       // anyone waiting for a free slot
    IOLockUnlock(fCmdLock);
    return type;
}

// Take completed chains off the control queue and wake their waiters. Hold fCmdLock.
void VirtioGPUDevice::drainControlLocked(bool fromInterrupt)
{
    uint16_t head;
    while (ripple_vq_get_used(&fQueue[0].vq, &head, NULL)) {
        int s = head < kCtrlQueueSize ? fHeadSlot[head] : -1;
        if (s < 0 || s >= kSlots)
            continue;
        fHeadSlot[head] = -1;
        if (fSlot[s].lost) {                    // finished after we gave up on it
            fSlot[s].lost = false;
            fSlot[s].busy = false;
            IOLockWakeup(fCmdLock, fSlot, false);   // anyone waiting for a free slot
            continue;
        }
        if (fSlot[s].async) {                   // nobody waits for this one
            const struct virtio_gpu_ctrl_hdr *r = (const struct virtio_gpu_ctrl_hdr *)
                ((uint8_t *)fSlotMem->getBytesNoCopy() + s * (kSlotReq + kSlotResp) + kSlotReq);
            if (r->type != VIRTIO_GPU_RESP_OK_NODATA && fAsyncFailed++ < 20)
                LOG("queued command failed: 0x%x", r->type);
            if (fSlot[s].release) {             // the sender's buffer is its own again
                *fSlot[s].release = 0;
                IOLockWakeup(fCmdLock, (void *)fSlot[s].release, false);
                fSlot[s].release = 0;
            }
            for (; fSlot[s].fence;) {           // syncAsync(): its fence has retired
                SInt64 old = fSignalled;
                if (old >= (SInt64)fSlot[s].fence ||
                    OSCompareAndSwap64((UInt64)old, fSlot[s].fence, (volatile UInt64 *)&fSignalled))
                    break;
            }
            fSlot[s].async = false;
            fSlot[s].busy = false;
            IOLockWakeup(fCmdLock, fSlot, false);   // anyone waiting for a free slot
            continue;
        }
        fSlot[s].done = true;
        if (fromInterrupt) fIntDone++; else fPollDone++;
        IOLockWakeup(fCmdLock, &fSlot[s], false);
    }
}

bool VirtioGPUDevice::simple(const void *req, uint32_t reqLen, uint64_t extraPA, uint32_t extraLen,
                             uint64_t *fence)
{
    struct virtio_gpu_ctrl_hdr resp;
    uint32_t type = command(req, reqLen, &resp, sizeof(resp), extraPA, extraLen, fence);
    if (type != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("command 0x%x failed: 0x%x", ((const struct virtio_gpu_ctrl_hdr *)req)->type, type);
        return false;
    }
    return true;
}

bool VirtioGPUDevice::getDisplayInfo(DisplayInfo *out, unsigned count)
{
    struct virtio_gpu_ctrl_hdr req;
    bzero(&req, sizeof(req));
    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    struct virtio_gpu_resp_display_info *info =
        (struct virtio_gpu_resp_display_info *)IOMalloc(sizeof(*info));
    if (!info)
        return false;
    bool ok = command(&req, sizeof(req), info, sizeof(*info), 0, 0) == VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    if (ok) {
        for (unsigned i = 0; i < count && i < kMaxScanouts; i++) {
            out[i].r.x = info->pmodes[i].r.x;
            out[i].r.y = info->pmodes[i].r.y;
            out[i].r.width = info->pmodes[i].r.width;
            out[i].r.height = info->pmodes[i].r.height;
            out[i].enabled = info->pmodes[i].enabled;
        }
    }
    IOFree(info, sizeof(*info));
    return ok;
}

bool VirtioGPUDevice::resourceCreate2D(uint32_t id, uint32_t format, uint32_t width, uint32_t height)
{
    struct virtio_gpu_resource_create_2d c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    c.resource_id = id;
    c.format = format;
    c.width = width;
    c.height = height;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::resourceUnref(uint32_t id)
{
    struct virtio_gpu_resource_unref c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    c.resource_id = id;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::attachBacking(uint32_t id, IOMemoryDescriptor *md, IOByteCount length)
{
    // One mem_entry per physically contiguous run of the descriptor.
    unsigned maxEntries = (unsigned)(length / PAGE_SIZE) + 2;
    IOByteCount entBytes = maxEntries * sizeof(struct virtio_gpu_mem_entry);
    IOBufferMemoryDescriptor *entries = contiguous(entBytes, kIODirectionOut);
    if (!entries)
        return false;
    struct virtio_gpu_mem_entry *ent = (struct virtio_gpu_mem_entry *)entries->getBytesNoCopy();
    unsigned n = 0;
    bool ok = true;
    for (IOByteCount off = 0; off < length && ok; ) {
        IOByteCount segLen = 0;
        addr64_t seg = md->getPhysicalSegment(off, &segLen, kIOMemoryMapperNone);
        if (!seg || !segLen) {
            ok = false;
            break;
        }
        if (segLen > length - off)
            segLen = length - off;
        if (n && ent[n - 1].addr + ent[n - 1].length == seg) {
            ent[n - 1].length += (uint32_t)segLen;
        } else if (n < maxEntries) {
            ent[n].addr = seg;
            ent[n].length = (uint32_t)segLen;
            n++;
        } else {
            ok = false;
        }
        off += segLen;
    }
    if (ok) {
        struct virtio_gpu_resource_attach_backing c;
        bzero(&c, sizeof(c));
        c.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        c.resource_id = id;
        c.nr_entries = n;
        ok = simple(&c, sizeof(c), physAddr(entries), n * sizeof(struct virtio_gpu_mem_entry));
    }
    entries->complete();
    entries->release();
    return ok;
}

bool VirtioGPUDevice::detachBacking(uint32_t id)
{
    struct virtio_gpu_resource_detach_backing c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    c.resource_id = id;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::setScanout(uint32_t scanout, uint32_t id, const Rect &r)
{
    struct virtio_gpu_set_scanout c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    c.r.x = r.x; c.r.y = r.y; c.r.width = r.width; c.r.height = r.height;
    c.scanout_id = scanout;
    c.resource_id = id;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::transferToHost2D(uint32_t id, const Rect &r, uint64_t offset)
{
    struct virtio_gpu_transfer_to_host_2d c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    c.r.x = r.x; c.r.y = r.y; c.r.width = r.width; c.r.height = r.height;
    c.offset = offset;
    c.resource_id = id;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::resourceFlush(uint32_t id, const Rect &r)
{
    struct virtio_gpu_resource_flush c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    c.r.x = r.x; c.r.y = r.y; c.r.width = r.width; c.r.height = r.height;
    c.resource_id = id;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::resourceFlushAsync(uint32_t id, const Rect &r)
{
    struct virtio_gpu_resource_flush c;
    struct virtio_gpu_ctrl_hdr resp;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    c.r.x = r.x; c.r.y = r.y; c.r.width = r.width; c.r.height = r.height;
    c.resource_id = id;
    return command(&c, sizeof(c), &resp, sizeof(resp), 0, 0, 0, kAsync) ==
           VIRTIO_GPU_RESP_OK_NODATA;
}

// ---- Cursor queue ---------------------------------------------------------------------------

bool VirtioGPUDevice::cursorCommand(const void *req, uint32_t len)
{
    Queue &q = fQueue[1];
    IOLockLock(fCursorLock);
    while (ripple_vq_get_used(&q.vq, NULL, NULL))
        ;                                       // reclaim finished requests
    unsigned slot = fCursorNext++ % kCursorSlots;
    uint8_t *buf = (uint8_t *)fCursorBuf->getBytesNoCopy() + slot * sizeof(struct virtio_gpu_update_cursor);
    memcpy(buf, req, len);
    uint64_t pa = fCursorBufPA + slot * sizeof(struct virtio_gpu_update_cursor);
    uint32_t l = len;
    bool ok = ripple_vq_add(&q.vq, &pa, &l, 1, 0) >= 0;
    if (ok)
        *q.notify = q.index;
    IOLockUnlock(fCursorLock);
    return ok;
}

bool VirtioGPUDevice::updateCursor(uint32_t scanout, uint32_t id, int32_t x, int32_t y,
                                   uint32_t hotX, uint32_t hotY)
{
    struct virtio_gpu_update_cursor c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    c.pos.scanout_id = scanout;
    c.pos.x = (uint32_t)x;
    c.pos.y = (uint32_t)y;
    c.resource_id = id;
    c.hot_x = hotX;
    c.hot_y = hotY;
    return cursorCommand(&c, sizeof(c));
}

bool VirtioGPUDevice::moveCursor(uint32_t scanout, uint32_t id, int32_t x, int32_t y)
{
    struct virtio_gpu_update_cursor c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
    // QEMU takes the pointer's visibility from resource_id on a move too (dpy_mouse_set):
    // 0 hides it in displays that honour that (SPICE in UTM, SDL). Linux sends it as well.
    c.resource_id = id;
    c.pos.scanout_id = scanout;
    c.pos.x = (uint32_t)x;
    c.pos.y = (uint32_t)y;
    return cursorCommand(&c, sizeof(c));
}

void VirtioGPUDevice::takeSlowest(uint32_t *us, uint32_t *type, uint32_t *over100ms)
{
    IOLockLock(fCmdLock);
    uint64_t ns;
    absolutetime_to_nanoseconds(fSlowestAbs, &ns);
    *us = (uint32_t)(ns / 1000);
    *type = fSlowestType;
    *over100ms = fOver100ms;
    fSlowestAbs = 0;
    fSlowestType = 0;
    IOLockUnlock(fCmdLock);
}

// ---- Interrupts -----------------------------------------------------------------------------

bool VirtioGPUDevice::setupInterrupts()
{
    // Legacy INTx (index 0). MSI-X stays off, so the ISR register reports and acknowledges
    // (virtio 1.0, 4.1.4.5). Bit 16 of the type marks a message-signalled interrupt
    // (kIOInterruptTypePCIMessaged in later SDKs; not defined in 10.6's).
    int type = 0;
    if (fPCI->getInterruptType(0, &type) != kIOReturnSuccess || (type & 0x10000))
        return false;
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        return false;
    fIntSrc = IOFilterInterruptEventSource::filterInterruptEventSource(this, interruptAction,
                                                                      interruptFilter, fPCI, 0);
    if (!fIntSrc || fWorkLoop->addEventSource(fIntSrc) != kIOReturnSuccess)
        return false;
    fIntSrc->enable();
    ripple_vq_set_interrupts(&fQueue[0].vq, 1);
    ripple_vq_set_interrupts(&fQueue[1].vq, 1);
    fIntEnabled = true;
    LOG("interrupts on (legacy INTx, type %d)", type);
    return true;
}

// Primary interrupt context: reading the ISR acknowledges our interrupt; 0 means not ours
// (the line may be shared).
// The device's interrupt line is level-triggered, and in QEMU's Q35 machine every PCI interrupt
// line also drives one of the old PC interrupts (10 or 11), which 10.6 has switched on for
// other devices (the USB controllers and more). IOKit's usual way with a level interrupt is to
// mask our line when the filter returns true and unmask it after the action has run on the work
// loop. If the device raised its line again in between (the next command finishing), our
// masked line stayed quiet but interrupt 10 fired without end: its handlers found nothing to
// do, nobody could quiet the device, and the CPU that takes interrupts ran nothing else, some
// 10000 interrupts in 0.2 s, until KVM held the interrupt back for a moment. That was the
// "0.17 s stall" (docs/re/perf-notes.md).
// So our line is never masked: the filter quiets the device itself and schedules the action
// with signalInterrupt(), which IOKit provides for exactly this, and returns false.
bool VirtioGPUDevice::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src)
{
    VirtioGPUDevice *d = (VirtioGPUDevice *)owner;
    uint8_t isr = rd8(d->fISR, 0);      // reading it lowers the line
    if (!isr)
        return false;
    d->fIntCount++;
    if (!(isr & 1))                 // bit 0: a queue was used; bit 1: config change (ignored)
        return false;
    // No more interrupts until the action has drained the queues (it turns them on again);
    // the second read takes back a raise that slipped in before the device saw that.
    ripple_vq_set_interrupts(&d->fQueue[0].vq, 0);
    ripple_vq_set_interrupts(&d->fQueue[1].vq, 0);
    (void)rd8(d->fISR, 0);
    src->signalInterrupt();
    return false;
}

// Our work loop's thread: finish completed commands.
void VirtioGPUDevice::interruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
    VirtioGPUDevice *d = (VirtioGPUDevice *)owner;
    // Twice: the second pass, with the device's interrupts on again, takes what finished
    // while they were off and so raised none.
    for (int pass = 0; pass < 2; pass++) {
        IOLockLock(d->fCmdLock);
        d->drainControlLocked(true);
        IOLockUnlock(d->fCmdLock);
        IOLockLock(d->fCursorLock);
        while (ripple_vq_get_used(&d->fQueue[1].vq, NULL, NULL))
            ;
        IOLockUnlock(d->fCursorLock);
        if (pass == 0) {
            ripple_vq_set_interrupts(&d->fQueue[0].vq, 1);
            ripple_vq_set_interrupts(&d->fQueue[1].vq, 1);
        }
    }
}

// ---- Capability sets ------------------------------------------------------------------------

bool VirtioGPUDevice::getCapsetInfo(uint32_t index, uint32_t *id, uint32_t *maxVersion, uint32_t *maxSize)
{
    struct virtio_gpu_get_capset_info req;
    struct virtio_gpu_resp_capset_info resp;
    bzero(&req, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    req.capset_index = index;
    if (command(&req, sizeof(req), &resp, sizeof(resp), 0, 0) != VIRTIO_GPU_RESP_OK_CAPSET_INFO)
        return false;
    *id = resp.capset_id;
    *maxVersion = resp.capset_max_version;
    *maxSize = resp.capset_max_size;
    return true;
}

uint32_t VirtioGPUDevice::getCapset(uint32_t id, uint32_t version, void *buf, uint32_t size)
{
    const uint32_t hdr = sizeof(struct virtio_gpu_ctrl_hdr);
    if (size + hdr > kSlotResp)
        size = kSlotResp - hdr;
    struct virtio_gpu_get_capset req;
    bzero(&req, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    req.capset_id = id;
    req.capset_version = version;
    uint8_t *resp = (uint8_t *)IOMalloc(hdr + size);
    if (!resp)
        return 0;
    uint32_t got = 0;
    if (command(&req, sizeof(req), resp, hdr + size, 0, 0) == VIRTIO_GPU_RESP_OK_CAPSET) {
        memcpy(buf, resp + hdr, size);
        got = size;
    }
    IOFree(resp, hdr + size);
    return got;
}

// ---- 3D (virgl) -----------------------------------------------------------------------------

bool VirtioGPUDevice::ctxCreate(uint32_t ctx, const char *name)
{
    struct virtio_gpu_ctx_create c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    c.hdr.ctx_id = ctx;
    strlcpy(c.debug_name, name ? name : "ripple", sizeof(c.debug_name));
    c.nlen = (uint32_t)strlen(c.debug_name);
    c.context_init = 0;                 // virgl
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::ctxDestroy(uint32_t ctx)
{
    struct virtio_gpu_ctx_destroy c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    c.hdr.ctx_id = ctx;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::ctxAttachResource(uint32_t ctx, uint32_t id)
{
    struct virtio_gpu_ctx_resource c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    c.hdr.ctx_id = ctx;
    c.resource_id = id;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::ctxDetachResource(uint32_t ctx, uint32_t id)
{
    struct virtio_gpu_ctx_resource c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    c.hdr.ctx_id = ctx;
    c.resource_id = id;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::resourceCreate3D(uint32_t id, uint32_t target, uint32_t format, uint32_t bind,
                                       uint32_t width, uint32_t height, uint32_t depth,
                                       uint32_t arraySize, uint32_t lastLevel, uint32_t nrSamples,
                                       uint32_t flags)
{
    struct virtio_gpu_resource_create_3d c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    c.resource_id = id;
    c.target = target;
    c.format = format;
    c.bind = bind;
    c.width = width;
    c.height = height;
    c.depth = depth;
    c.array_size = arraySize;
    c.last_level = lastLevel;
    c.nr_samples = nrSamples;
    c.flags = flags;
    return simple(&c, sizeof(c));
}

bool VirtioGPUDevice::submit3D(uint32_t ctx, uint64_t pa, uint32_t size, uint64_t *fence)
{
    struct virtio_gpu_cmd_submit c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    c.hdr.ctx_id = ctx;                  // not fenced: see sync()
    c.size = size;
    fLastCtx = ctx;
    uint64_t id = 0;
    bool ok = simple(&c, sizeof(c), pa, size, &id);
    if (fence)
        *fence = id;
    return ok;
}

// Like sync(), but return once queued. QEMU still handles the fenced empty submit before any
// later command (creating the fence pushes the host GPU's queued work out), and its reply,
// when the GPU is done, passes the fence in the drain (interrupt or the next waiter's poll).
bool VirtioGPUDevice::syncAsync(uint32_t ctx)
{
    struct virtio_gpu_cmd_submit c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    c.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    c.hdr.ctx_id = ctx ? ctx : fLastCtx;
    c.size = 0;
    if (!c.hdr.ctx_id)
        return true;
    uint64_t id = 0;
    struct virtio_gpu_ctrl_hdr resp;
    return command(&c, sizeof(c), &resp, sizeof(resp), 0, 0, &id, kAsyncFence) ==
           VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtioGPUDevice::submit3DAsync(uint32_t ctx, uint64_t pa, uint32_t size, uint64_t *fence,
                                    volatile UInt32 *busy)
{
    struct virtio_gpu_cmd_submit c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    c.hdr.ctx_id = ctx;
    c.size = size;
    fLastCtx = ctx;
    uint64_t id = 0;
    struct virtio_gpu_ctrl_hdr resp;
    bool ok = command(&c, sizeof(c), &resp, sizeof(resp), pa, size, &id, kAsync, busy) ==
              VIRTIO_GPU_RESP_OK_NODATA;
    if (fence)
        *fence = id;
    return ok;
}

// Wait until the device has given back what a queued command used. The interrupt's drain
// wakes us; the polling is for a lost interrupt.
bool VirtioGPUDevice::waitRelease(volatile UInt32 *busy)
{
    if (!*busy)
        return true;
    uint64_t t0 = mach_absolute_time(), timeout;
    nanoseconds_to_absolutetime(5000000000ULL, &timeout);
    IOLockLock(fCmdLock);
    while (*busy) {
        drainControlLocked(false);
        if (!*busy || mach_absolute_time() - t0 > timeout)
            break;
        uint64_t deadline;
        clock_interval_to_deadline(fIntEnabled ? 20 : 1, kMillisecondScale, &deadline);
        IOLockSleepDeadline(fCmdLock, (void *)busy, deadline, THREAD_UNINT);
    }
    bool released = !*busy;
    IOLockUnlock(fCmdLock);
    return released;
}

// For a flag that is still set when its owner is freed (the host has stopped answering):
// a late reply must not write to it.
void VirtioGPUDevice::abandonRelease(volatile UInt32 *busy)
{
    IOLockLock(fCmdLock);
    for (int i = 0; i < kSlots; i++)
        if (fSlot[i].release == busy)
            fSlot[i].release = 0;
    IOLockUnlock(fCmdLock);
}

// Wait until the host GPU has finished everything queued so far. QEMU holds the reply to a
// fenced command until its fence retires, and a fence retires after all work queued before
// it (any context); fence numbers follow queue order, so every number up to this one is done.
bool VirtioGPUDevice::sync(uint32_t ctx)
{
    struct virtio_gpu_cmd_submit c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    c.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    c.hdr.ctx_id = ctx ? ctx : fLastCtx;
    c.size = 0;                          // no commands, just the fence
    if (!c.hdr.ctx_id)
        return true;                     // nothing was ever submitted
    uint64_t id = 0;
    if (!simple(&c, sizeof(c), 0, 0, &id))
        return false;
    for (;;) {
        SInt64 old = fSignalled;
        if (old >= (SInt64)id ||
            OSCompareAndSwap64((UInt64)old, id, (volatile UInt64 *)&fSignalled))
            break;
    }
    return true;
}

bool VirtioGPUDevice::transfer3D(bool toHost, uint32_t ctx, uint32_t id, uint32_t level,
                                 const uint32_t box[6], uint32_t stride, uint32_t layerStride,
                                 uint64_t offset, uint64_t *fence)
{
    struct virtio_gpu_transfer_host_3d c;
    bzero(&c, sizeof(c));
    c.hdr.type = toHost ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    // No VIRTIO_GPU_FLAG_FENCE: QEMU copies the data while it processes the command (into the
    // host texture, or out of it into guest memory), so the reply alone means the transfer is
    // done. A fence made QEMU hold the reply until the host GPU had finished everything queued
    // before (performance stage, 2026-11-11). It still gets a number, in queue order.
    c.hdr.ctx_id = ctx;
    c.box.x = box[0]; c.box.y = box[1]; c.box.z = box[2];
    c.box.w = box[3]; c.box.h = box[4]; c.box.d = box[5];
    c.offset = offset;
    c.resource_id = id;
    c.level = level;
    c.stride = stride;
    c.layer_stride = layerStride;
    uint64_t n = 0;
    bool ok = simple(&c, sizeof(c), 0, 0, &n);
    if (fence)
        *fence = n;
    return ok;
}
