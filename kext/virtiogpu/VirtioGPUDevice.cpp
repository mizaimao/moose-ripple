/*
 * VirtioGPUDevice: see VirtioGPUDevice.h. Protocol definitions come from Linux's BSD-licensed
 * virtio headers in third_party/virtio (virtio 1.0 spec: PCI transport, GPU device).
 */
#include "VirtioGPUDevice.h"

#include <IOKit/IOLib.h>

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
    // virtio 1.0 initialisation (spec 3.1.1). M2 is 2D only: accept just VERSION_1.
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
    wr32(fCommon, CC(guest_feature_select), 0);
    wr32(fCommon, CC(guest_feature), 0);
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
    fCursorLock = IOLockAlloc();
    fCmdBuf = contiguous(kCmdBufSize);
    fCursorBuf = contiguous(kCursorSlots * sizeof(struct virtio_gpu_update_cursor), kIODirectionOut);
    if (!fCmdLock || !fCursorLock || !fCmdBuf || !fCursorBuf)
        return false;
    fCmdBufPA = physAddr(fCmdBuf);
    fCursorBufPA = physAddr(fCursorBuf);
    return mapCapabilities() && negotiate();
}

void VirtioGPUDevice::free()
{
    reset();
    for (unsigned i = 0; i < 2; i++) {
        if (fQueue[i].mem) {
            fQueue[i].mem->complete();
            fQueue[i].mem->release();
        }
    }
    if (fCmdBuf)    { fCmdBuf->complete();    fCmdBuf->release(); }
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
                                  uint64_t extraPA, uint32_t extraLen)
{
    enum { kRespOffset = kCmdBufSize / 2 };
    if (reqLen > kRespOffset || respLen > kCmdBufSize - kRespOffset)
        return 0;

    IOLockLock(fCmdLock);
    uint8_t *buf = (uint8_t *)fCmdBuf->getBytesNoCopy();
    memcpy(buf, req, reqLen);
    bzero(buf + kRespOffset, respLen);

    uint64_t pa[3];
    uint32_t len[3];
    unsigned nout = 0;
    pa[nout] = fCmdBufPA;               len[nout++] = reqLen;
    if (extraLen) { pa[nout] = extraPA;  len[nout++] = extraLen; }
    pa[nout] = fCmdBufPA + kRespOffset; len[nout] = respLen;

    Queue &q = fQueue[0];
    uint32_t type = 0;
    if (ripple_vq_add(&q.vq, pa, len, nout, 1) >= 0) {
        *q.notify = q.index;
        for (int i = 0; i < 200000; i++) {      // up to about 2 s
            if (ripple_vq_get_used(&q.vq, NULL, NULL)) {
                memcpy(resp, buf + kRespOffset, respLen);
                (void)rd8(fISR, 0);             // reading the ISR acknowledges (deasserts) the interrupt
                type = ((struct virtio_gpu_ctrl_hdr *)resp)->type;
                break;
            }
            IODelay(10);
        }
        if (!type)
            LOG("command 0x%x timed out", ((const struct virtio_gpu_ctrl_hdr *)req)->type);
    } else {
        LOG("control queue full");
    }
    IOLockUnlock(fCmdLock);
    return type;
}

bool VirtioGPUDevice::simple(const void *req, uint32_t reqLen, uint64_t extraPA, uint32_t extraLen)
{
    struct virtio_gpu_ctrl_hdr resp;
    uint32_t type = command(req, reqLen, &resp, sizeof(resp), extraPA, extraLen);
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

bool VirtioGPUDevice::moveCursor(uint32_t scanout, int32_t x, int32_t y)
{
    struct virtio_gpu_update_cursor c;
    bzero(&c, sizeof(c));
    c.hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
    c.pos.scanout_id = scanout;
    c.pos.x = (uint32_t)x;
    c.pos.y = (uint32_t)y;
    return cursorCommand(&c, sizeof(c));
}
