/*
 * RippleVirtioGPU: see RippleVirtioGPU.h. Protocol definitions come from Linux's BSD-licensed
 * virtio headers in third_party/virtio (virtio 1.0 spec, PCI transport and GPU device).
 */
#include "RippleVirtioGPU.h"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>
#ifndef offsetof
#define offsetof(t, m) __builtin_offsetof(t, m)   /* no stddef.h in Kernel.framework */
#endif

#include <linux/virtio_config.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_gpu.h>

#define super IOService
OSDefineMetaClassAndStructors(RippleVirtioGPU, IOService)

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

// Physical address of the start of a physically contiguous buffer.
static uint64_t physAddr(IOBufferMemoryDescriptor *md, IOByteCount offset = 0)
{
    return md->getPhysicalSegment(offset, NULL, kIOMemoryMapperNone);
}

// ---- Device setup ----------------------------------------------------------------------------

uint8_t RippleVirtioGPU::status()            { return rd8(fCommon, CC(device_status)); }
void    RippleVirtioGPU::setStatus(uint8_t s) { wr8(fCommon, CC(device_status), s); }

void RippleVirtioGPU::resetDevice()
{
    if (!fCommon)
        return;
    setStatus(0);
    for (int i = 0; i < 1000 && status() != 0; i++)
        IODelay(100);
}

bool RippleVirtioGPU::mapCapabilities()
{
    // Walk the PCI capability list for virtio's vendor-specific capabilities.
    if (!(fPCI->configRead16(kIOPCIConfigStatus) & 0x10)) {
        LOG("no PCI capability list");
        return false;
    }
    uint8_t ptr = fPCI->configRead8(0x34) & 0xfc;
    for (int guard = 0; ptr && guard < 48; guard++) {
        uint8_t id = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_VNDR);
        uint8_t next = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_NEXT) & 0xfc;
        if (id == 0x09) {
            uint8_t type = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_CFG_TYPE);
            uint8_t bar = fPCI->configRead8(ptr + VIRTIO_PCI_CAP_BAR);
            uint32_t off = fPCI->configRead32(ptr + VIRTIO_PCI_CAP_OFFSET);
            uint32_t len = fPCI->configRead32(ptr + VIRTIO_PCI_CAP_LENGTH);
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
                LOG("cap type %u: BAR %u offset 0x%x length 0x%x", type, bar, off, len);
            }
        }
        ptr = next;
    }
    if (!fCommon || !fNotify || !fISR || !fDevCfg) {
        LOG("missing virtio capabilities (common %p notify %p isr %p device %p)",
            fCommon, fNotify, fISR, fDevCfg);
        return false;
    }
    return true;
}

bool RippleVirtioGPU::setupQueue(unsigned index)
{
    Queue &q = fQueue[index];

    wr16(fCommon, CC(queue_select), (uint16_t)index);
    uint16_t max = rd16(fCommon, CC(queue_size));
    if (max == 0) {
        LOG("queue %u not available", index);
        return false;
    }
    unsigned num = max < kQueueSize ? max : kQueueSize;
    wr16(fCommon, CC(queue_size), (uint16_t)num);

    unsigned bytes = ripple_vq_bytes(num);
    q.mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0xFFFFFFFFFFFFF000ULL);
    if (!q.mem || q.mem->prepare() != kIOReturnSuccess) {
        LOG("queue %u: no memory", index);
        return false;
    }
    bzero(q.mem->getBytesNoCopy(), bytes);
    ripple_vq_init(&q.vq, num, q.mem->getBytesNoCopy(), physAddr(q.mem));

    wr64(fCommon, CC(queue_desc_lo), q.vq.desc_pa);
    wr64(fCommon, CC(queue_avail_lo), q.vq.avail_pa);
    wr64(fCommon, CC(queue_used_lo), q.vq.used_pa);
    uint16_t notifyOff = rd16(fCommon, CC(queue_notify_off));
    q.notify = (volatile uint16_t *)(fNotify + (uint32_t)notifyOff * fNotifyMult);
    wr16(fCommon, CC(queue_enable), 1);
    LOG("queue %u: %u entries (device max %u), ring at 0x%llx", index, num, max,
        (unsigned long long)q.vq.desc_pa);
    return true;
}

// ---- Commands -------------------------------------------------------------------------------

uint32_t RippleVirtioGPU::command(const void *req, uint32_t reqLen, void *resp, uint32_t respLen,
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
    pa[nout] = fCmdBufPA;              len[nout++] = reqLen;
    if (extraLen) { pa[nout] = extraPA; len[nout++] = extraLen; }
    pa[nout] = fCmdBufPA + kRespOffset; len[nout] = respLen;

    Queue &q = fQueue[0];
    uint32_t type = 0;
    if (ripple_vq_add(&q.vq, pa, len, nout, 1) >= 0) {
        *q.notify = 0;                          // notify queue 0
        uint16_t head;
        for (int i = 0; i < 100000; i++) {      // up to about 1 s
            if (ripple_vq_get_used(&q.vq, &head, NULL)) {
                memcpy(resp, buf + kRespOffset, respLen);
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

// ---- 2D test pattern ------------------------------------------------------------------------

bool RippleVirtioGPU::drawTestPattern(uint32_t width, uint32_t height)
{
    const uint32_t resId = 1, bytes = width * height * 4;
    struct virtio_gpu_ctrl_hdr ok;

    fFB = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, bytes, PAGE_SIZE);
    if (!fFB || fFB->prepare() != kIOReturnSuccess)
        return false;

    // Fill: eight vertical colour bars with a dark-to-light ramp from top to bottom.
    static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                      0xff00ff, 0xff0000, 0x0000ff, 0x202020 };
    uint32_t *px = (uint32_t *)fFB->getBytesNoCopy();
    for (uint32_t y = 0; y < height; y++) {
        uint32_t shade = 64 + (191 * y) / height;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t c = bars[(x * 8) / width];
            uint32_t r = ((c >> 16) & 0xff) * shade / 255, g = ((c >> 8) & 0xff) * shade / 255,
                     b = (c & 0xff) * shade / 255;
            px[y * width + x] = 0xff000000 | (r << 16) | (g << 8) | b;   // B,G,R,X in memory
        }
    }

    struct virtio_gpu_resource_create_2d create;
    bzero(&create, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = resId;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = width;
    create.height = height;
    if (command(&create, sizeof(create), &ok, sizeof(ok)) != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("RESOURCE_CREATE_2D failed: 0x%x", ok.type);
        return false;
    }

    // Backing pages: one mem_entry per physically contiguous run.
    unsigned maxEntries = bytes / PAGE_SIZE + 1;
    fEntries = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                   kIODirectionOut | kIOMemoryPhysicallyContiguous,
                   maxEntries * sizeof(struct virtio_gpu_mem_entry), 0xFFFFFFFFFFFFF000ULL);
    if (!fEntries || fEntries->prepare() != kIOReturnSuccess)
        return false;
    struct virtio_gpu_mem_entry *ent = (struct virtio_gpu_mem_entry *)fEntries->getBytesNoCopy();
    unsigned n = 0;
    for (IOByteCount off = 0; off < bytes; ) {
        IOByteCount segLen = 0;
        addr64_t seg = fFB->getPhysicalSegment(off, &segLen, kIOMemoryMapperNone);
        if (!seg || !segLen)
            return false;
        if (segLen > bytes - off)
            segLen = bytes - off;
        if (n && ent[n - 1].addr + ent[n - 1].length == seg) {
            ent[n - 1].length += (uint32_t)segLen;
        } else {
            ent[n].addr = seg;
            ent[n].length = (uint32_t)segLen;
            ent[n].padding = 0;
            n++;
        }
        off += segLen;
    }

    struct virtio_gpu_resource_attach_backing attach;
    bzero(&attach, sizeof(attach));
    attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.resource_id = resId;
    attach.nr_entries = n;
    if (command(&attach, sizeof(attach), &ok, sizeof(ok), physAddr(fEntries),
                n * sizeof(struct virtio_gpu_mem_entry)) != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("RESOURCE_ATTACH_BACKING failed: 0x%x", ok.type);
        return false;
    }

    struct virtio_gpu_set_scanout scanout;
    bzero(&scanout, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.width = width;
    scanout.r.height = height;
    scanout.scanout_id = 0;
    scanout.resource_id = resId;
    if (command(&scanout, sizeof(scanout), &ok, sizeof(ok)) != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("SET_SCANOUT failed: 0x%x", ok.type);
        return false;
    }

    struct virtio_gpu_transfer_to_host_2d xfer;
    bzero(&xfer, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.r.width = width;
    xfer.r.height = height;
    xfer.offset = 0;
    xfer.resource_id = resId;
    if (command(&xfer, sizeof(xfer), &ok, sizeof(ok)) != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("TRANSFER_TO_HOST_2D failed: 0x%x", ok.type);
        return false;
    }

    struct virtio_gpu_resource_flush flush;
    bzero(&flush, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.width = width;
    flush.r.height = height;
    flush.resource_id = resId;
    if (command(&flush, sizeof(flush), &ok, sizeof(ok)) != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("RESOURCE_FLUSH failed: 0x%x", ok.type);
        return false;
    }
    LOG("test pattern %ux%u on scanout 0 (%u backing entries)", width, height, n);
    return true;
}

// ---- IOService ------------------------------------------------------------------------------

bool RippleVirtioGPU::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);

    fCmdLock = IOLockAlloc();
    fCmdBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                  kIODirectionInOut | kIOMemoryPhysicallyContiguous, kCmdBufSize,
                  0xFFFFFFFFFFFFF000ULL);
    if (!fCmdLock || !fCmdBuf || fCmdBuf->prepare() != kIOReturnSuccess)
        goto fail;
    fCmdBufPA = physAddr(fCmdBuf);

    if (!mapCapabilities())
        goto fail;

    // virtio 1.0 initialisation (spec 3.1.1).
    resetDevice();
    setStatus(VIRTIO_CONFIG_S_ACKNOWLEDGE);
    setStatus(VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
    {
        wr32(fCommon, CC(device_feature_select), 0);
        uint32_t lo = rd32(fCommon, CC(device_feature));
        wr32(fCommon, CC(device_feature_select), 1);
        uint32_t hi = rd32(fCommon, CC(device_feature));
        LOG("device features 0x%08x%08x (VIRGL %u, EDID %u, VERSION_1 %u)", hi, lo,
            !!(lo & (1u << VIRTIO_GPU_F_VIRGL)), !!(lo & (1u << VIRTIO_GPU_F_EDID)),
            !!(hi & (1u << (VIRTIO_F_VERSION_1 - 32))));
        if (!(hi & (1u << (VIRTIO_F_VERSION_1 - 32)))) {
            LOG("device lacks VIRTIO_F_VERSION_1");
            goto fail;
        }
        // M2 is 2D only: accept just VERSION_1.
        wr32(fCommon, CC(guest_feature_select), 0);
        wr32(fCommon, CC(guest_feature), 0);
        wr32(fCommon, CC(guest_feature_select), 1);
        wr32(fCommon, CC(guest_feature), 1u << (VIRTIO_F_VERSION_1 - 32));
    }
    setStatus(status() | VIRTIO_CONFIG_S_FEATURES_OK);
    if (!(status() & VIRTIO_CONFIG_S_FEATURES_OK)) {
        LOG("device refused our features");
        goto fail;
    }
    LOG("%u queues", rd16(fCommon, CC(num_queues)));
    if (!setupQueue(0) || !setupQueue(1))
        goto fail;
    setStatus(status() | VIRTIO_CONFIG_S_DRIVER_OK);

    {
        uint32_t scanouts = rd32(fDevCfg, offsetof(struct virtio_gpu_config, num_scanouts));
        uint32_t capsets = rd32(fDevCfg, offsetof(struct virtio_gpu_config, num_capsets));
        LOG("ready: status 0x%x, %u scanouts, %u capsets", status(), scanouts, capsets);

        struct virtio_gpu_ctrl_hdr req;
        bzero(&req, sizeof(req));
        req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
        struct virtio_gpu_resp_display_info *info =
            (struct virtio_gpu_resp_display_info *)IOMalloc(sizeof(*info));
        if (!info)
            goto fail;
        uint32_t type = command(&req, sizeof(req), info, sizeof(*info));
        if (type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
            LOG("GET_DISPLAY_INFO failed: 0x%x", type);
            IOFree(info, sizeof(*info));
            goto fail;
        }
        fWidth = 1024;
        fHeight = 768;
        for (unsigned i = 0; i < scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
            LOG("scanout %u: %ux%u at %u,%u enabled %u flags 0x%x", i,
                info->pmodes[i].r.width, info->pmodes[i].r.height, info->pmodes[i].r.x,
                info->pmodes[i].r.y, info->pmodes[i].enabled, info->pmodes[i].flags);
            if (i == 0 && info->pmodes[0].enabled && info->pmodes[0].r.width) {
                fWidth = info->pmodes[0].r.width;
                fHeight = info->pmodes[0].r.height;
            }
        }
        IOFree(info, sizeof(*info));
        setProperty("RippleScanout0", (unsigned long long)fWidth << 32 | fHeight, 64);
    }

    if (!drawTestPattern(fWidth, fHeight))
        LOG("test pattern failed");
    registerService();
    return true;

fail:
    resetDevice();
    super::stop(provider);
    return false;
}

void RippleVirtioGPU::stop(IOService *provider)
{
    resetDevice();          // the device drops its resources; QEMU shows VGA again
    LOG("stopped");
    super::stop(provider);
}

void RippleVirtioGPU::free()
{
    for (unsigned i = 0; i < 2; i++) {
        if (fQueue[i].mem) {
            fQueue[i].mem->complete();
            fQueue[i].mem->release();
        }
    }
    if (fFB)      { fFB->complete();      fFB->release(); }
    if (fEntries) { fEntries->complete(); fEntries->release(); }
    if (fCmdBuf)  { fCmdBuf->complete();  fCmdBuf->release(); }
    if (fCmdLock) IOLockFree(fCmdLock);
    for (unsigned i = 0; i < kNumBars; i++)
        if (fBarMap[i])
            fBarMap[i]->release();
    if (fPCI)
        fPCI->release();
    super::free();
}
