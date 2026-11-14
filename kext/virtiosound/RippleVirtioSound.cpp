/*
 * RippleVirtioSound: see RippleVirtioSound.h. Protocol definitions come from Linux's
 * BSD-licensed virtio headers in third_party/virtio (virtio 1.0 PCI transport, virtio-sound).
 */
#include "RippleVirtioSound.h"
#include "RippleVirtioSoundEngine.h"

#include <IOKit/IOLib.h>
#include <IOKit/audio/IOAudioTypes.h>

#include <linux/virtio_config.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_snd.h>

#ifndef offsetof
#define offsetof(t, m) __builtin_offsetof(t, m)   /* no stddef.h in Kernel.framework */
#endif

#define super IOAudioDevice
OSDefineMetaClassAndStructors(RippleVirtioSound, IOAudioDevice)

#define LOG(fmt, ...) IOLog("RippleVirtioSound: " fmt "\n", ##__VA_ARGS__)

// ---- MMIO helpers (as in RippleVirtioGPU) ----------------------------------------------------

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

static IOBufferMemoryDescriptor *contiguous(IOByteCount size)
{
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0xFFFFFFFFFFFFF000ULL);
    if (md && md->prepare() != kIOReturnSuccess) {
        md->release();
        md = 0;
    }
    if (md)
        bzero(md->getBytesNoCopy(), size);
    return md;
}

static void freeMem(IOBufferMemoryDescriptor *&md)
{
    if (md) {
        md->complete();
        md->release();
        md = 0;
    }
}

// TX slot layout: the xfer header, then the PCM data, then the device-written status.
enum { kTxHdr = 16 };
static inline uint32_t txStatusOffset(void) { return kTxHdr + RippleVirtioSound::kMaxPeriodBytes; }

// ---- Transport --------------------------------------------------------------------------------

uint8_t RippleVirtioSound::status()            { return rd8(fCommon, CC(device_status)); }
void    RippleVirtioSound::setStatus(uint8_t s) { wr8(fCommon, CC(device_status), s); }

void RippleVirtioSound::reset()
{
    if (!fCommon)
        return;
    setStatus(0);
    for (int i = 0; i < 1000 && status() != 0; i++)
        IODelay(100);
}

bool RippleVirtioSound::mapCapabilities()
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

bool RippleVirtioSound::setupQueue(Queue &q, unsigned index)
{
    wr16(fCommon, CC(queue_select), (uint16_t)index);
    uint16_t max = rd16(fCommon, CC(queue_size));
    if (max == 0) {
        LOG("queue %u not available", index);
        return false;
    }
    unsigned num = max < kQueueSize ? max : kQueueSize;
    wr16(fCommon, CC(queue_size), (uint16_t)num);
    q.mem = contiguous(ripple_vq_bytes(num));
    if (!q.mem)
        return false;
    ripple_vq_init(&q.vq, num, q.mem->getBytesNoCopy(), physAddr(q.mem));
    wr64(fCommon, CC(queue_desc_lo), q.vq.desc_pa);
    wr64(fCommon, CC(queue_avail_lo), q.vq.avail_pa);
    wr64(fCommon, CC(queue_used_lo), q.vq.used_pa);
    q.notify = (volatile uint16_t *)(fNotify + (uint32_t)rd16(fCommon, CC(queue_notify_off)) * fNotifyMult);
    q.index = (uint16_t)index;
    wr16(fCommon, CC(queue_enable), 1);
    return true;
}

bool RippleVirtioSound::negotiate()
{
    // virtio 1.0 initialisation (spec 3.1.1): VERSION_1 only, no device features.
    reset();
    setStatus(VIRTIO_CONFIG_S_ACKNOWLEDGE);
    setStatus(VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
    wr32(fCommon, CC(device_feature_select), 1);
    uint32_t hi = rd32(fCommon, CC(device_feature));
    if (!(hi & (1u << (VIRTIO_F_VERSION_1 - 32)))) {
        LOG("device lacks VIRTIO_F_VERSION_1");
        return false;
    }
    wr32(fCommon, CC(guest_feature_select), 0);
    wr32(fCommon, CC(guest_feature), 0);
    wr32(fCommon, CC(guest_feature_select), 1);
    wr32(fCommon, CC(guest_feature), 1u << (VIRTIO_F_VERSION_1 - 32));
    setStatus(status() | VIRTIO_CONFIG_S_FEATURES_OK);
    if (!(status() & VIRTIO_CONFIG_S_FEATURES_OK)) {
        LOG("device refused our features");
        return false;
    }
    // Control and TX queues only: the event queue (1) and RX (3) stay unused for now.
    if (!setupQueue(fCtl, VIRTIO_SND_VQ_CONTROL) || !setupQueue(fTx, VIRTIO_SND_VQ_TX))
        return false;
    ripple_vq_set_interrupts(&fCtl.vq, 0);       // control commands are polled
    setStatus(status() | VIRTIO_CONFIG_S_DRIVER_OK);
    (void)rd8(fISR, 0);
    LOG("device ready: %u jacks, %u streams, %u channel maps",
        rd32(fDevCfg, offsetof(struct virtio_snd_config, jacks)),
        rd32(fDevCfg, offsetof(struct virtio_snd_config, streams)),
        rd32(fDevCfg, offsetof(struct virtio_snd_config, chmaps)));
    return true;
}

// ---- Control queue (polled) ------------------------------------------------------------------

uint32_t RippleVirtioSound::control(const void *req, uint32_t reqLen, void *resp, uint32_t respLen)
{
    if (reqLen + respLen > kCtlBytes)
        return 0;
    IOLockLock(fCtlLock);
    uint8_t *buf = (uint8_t *)fCtlMem->getBytesNoCopy();
    memcpy(buf, req, reqLen);
    bzero(buf + reqLen, respLen);
    uint64_t pa[2] = { physAddr(fCtlMem), physAddr(fCtlMem) + reqLen };
    uint32_t len[2] = { reqLen, respLen };
    uint32_t code = 0;
    if (ripple_vq_add(&fCtl.vq, pa, len, 1, 1) >= 0) {
        *fCtl.notify = fCtl.index;
        for (int i = 0; i < 40000; i++) {           // up to 2 s
            if (ripple_vq_get_used(&fCtl.vq, NULL, NULL)) {
                memcpy(resp, buf + reqLen, respLen);
                code = ((struct virtio_snd_hdr *)resp)->code;
                break;
            }
            IODelay(50);
        }
    }
    IOLockUnlock(fCtlLock);
    if (!code)
        LOG("control request 0x%x: no answer", ((const struct virtio_snd_hdr *)req)->code);
    return code;
}

bool RippleVirtioSound::findOutputStream()
{
    uint32_t streams = rd32(fDevCfg, offsetof(struct virtio_snd_config, streams));
    if (streams == 0 || streams > 16)
        return false;
    struct virtio_snd_query_info q;
    bzero(&q, sizeof(q));
    q.hdr.code = VIRTIO_SND_R_PCM_INFO;
    q.start_id = 0;
    q.count = streams;
    q.size = sizeof(struct virtio_snd_pcm_info);
    uint32_t respLen = sizeof(struct virtio_snd_hdr) + streams * sizeof(struct virtio_snd_pcm_info);
    uint8_t resp[sizeof(struct virtio_snd_hdr) + 16 * sizeof(struct virtio_snd_pcm_info)];
    if (control(&q, sizeof(q), resp, respLen) != VIRTIO_SND_S_OK) {
        LOG("PCM_INFO failed");
        return false;
    }
    struct virtio_snd_pcm_info *info = (struct virtio_snd_pcm_info *)(resp + sizeof(struct virtio_snd_hdr));
    for (uint32_t i = 0; i < streams; i++) {
        LOG("stream %u: direction %u, channels %u-%u, formats 0x%llx, rates 0x%llx", i,
            info[i].direction, info[i].channels_min, info[i].channels_max,
            (unsigned long long)info[i].formats, (unsigned long long)info[i].rates);
        if (info[i].direction == VIRTIO_SND_D_OUTPUT && info[i].channels_max >= 2 &&
            (info[i].formats & (1ULL << VIRTIO_SND_PCM_FMT_S16)) &&
            (info[i].rates & (1ULL << VIRTIO_SND_PCM_RATE_48000))) {
            fStream = i;
            return true;
        }
    }
    LOG("no stereo 16-bit 48 kHz output stream");
    return false;
}

bool RippleVirtioSound::pcmSetParams(uint32_t bufferBytes, uint32_t periodBytes, uint8_t channels,
                                     uint8_t format, uint8_t rate)
{
    struct virtio_snd_pcm_set_params p;
    struct virtio_snd_hdr r;
    bzero(&p, sizeof(p));
    p.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    p.hdr.stream_id = fStream;
    p.buffer_bytes = bufferBytes;
    p.period_bytes = periodBytes;
    p.channels = channels;
    p.format = format;
    p.rate = rate;
    return control(&p, sizeof(p), &r, sizeof(r)) == VIRTIO_SND_S_OK;
}

bool RippleVirtioSound::pcmCommand(uint32_t code)
{
    struct virtio_snd_pcm_hdr h;
    struct virtio_snd_hdr r;
    bzero(&h, sizeof(h));
    h.hdr.code = code;
    h.stream_id = fStream;
    return control(&h, sizeof(h), &r, sizeof(r)) == VIRTIO_SND_S_OK;
}

// ---- TX queue ----------------------------------------------------------------------------------

void *RippleVirtioSound::txData(unsigned slot)
{
    return (uint8_t *)fTxMem->getBytesNoCopy() + slot * fTxSlotBytes + kTxHdr;
}

bool RippleVirtioSound::txSubmit(unsigned slot, uint32_t bytes)
{
    if (slot >= kTxSlots || bytes > kMaxPeriodBytes)
        return false;
    uint8_t *base = (uint8_t *)fTxMem->getBytesNoCopy() + slot * fTxSlotBytes;
    uint64_t pa = physAddr(fTxMem) + slot * fTxSlotBytes;
    ((struct virtio_snd_pcm_xfer *)base)->stream_id = fStream;
    uint64_t pas[3] = { pa, pa + kTxHdr, pa + txStatusOffset() };
    uint32_t lens[3] = { sizeof(struct virtio_snd_pcm_xfer), bytes, sizeof(struct virtio_snd_pcm_status) };
    int head = ripple_vq_add(&fTx.vq, pas, lens, 2, 1);
    if (head < 0)
        return false;
    fTxHeadSlot[head] = (int8_t)slot;
    fTxInFlight++;
    *fTx.notify = fTx.index;
    return true;
}

// Hand completed slots to the engine. Work loop thread (or the engine's own, while stopping).
void RippleVirtioSound::txCompleted()
{
    uint16_t head;
    while (ripple_vq_get_used(&fTx.vq, &head, NULL)) {
        int slot = head < kQueueSize ? fTxHeadSlot[head] : -1;
        if (slot < 0)
            continue;
        fTxHeadSlot[head] = -1;
        if (fTxInFlight)
            fTxInFlight--;
        if (fEngine)
            fEngine->txDone((unsigned)slot);
    }
}

void RippleVirtioSound::drainTx()
{
    txCompleted();
}

// ---- Interrupts (the pattern of RippleVirtioGPU after #22) --------------------------------------

bool RippleVirtioSound::setupInterrupts()
{
    int type = 0;
    if (fPCI->getInterruptType(0, &type) != kIOReturnSuccess || (type & 0x10000))
        return false;
    IOWorkLoop *wl = getWorkLoop();
    if (!wl)
        return false;
    fIntSrc = IOFilterInterruptEventSource::filterInterruptEventSource(this, interruptAction,
                                                                      interruptFilter, fPCI, 0);
    if (!fIntSrc || wl->addEventSource(fIntSrc) != kIOReturnSuccess)
        return false;
    fIntSrc->enable();
    ripple_vq_set_interrupts(&fTx.vq, 1);
    LOG("interrupts on (legacy INTx, type %d)", type);
    return true;
}

// Primary interrupt context. Reading the ISR lowers the line. Our line is never left masked:
// the filter quiets the device (no TX interrupts until the action has drained the queue),
// re-reads the ISR to take back a raise that slipped in, schedules the action and returns false.
bool RippleVirtioSound::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src)
{
    RippleVirtioSound *d = (RippleVirtioSound *)owner;
    uint8_t isr = rd8(d->fISR, 0);
    if (!isr)
        return false;
    d->fIntCount++;
    if (!(isr & 1))
        return false;
    ripple_vq_set_interrupts(&d->fTx.vq, 0);
    (void)rd8(d->fISR, 0);
    src->signalInterrupt();
    return false;
}

void RippleVirtioSound::interruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
    RippleVirtioSound *d = (RippleVirtioSound *)owner;
    // Twice: the second pass, with interrupts on again, takes what finished while they were off.
    for (int pass = 0; pass < 2; pass++) {
        d->txCompleted();
        if (pass == 0)
            ripple_vq_set_interrupts(&d->fTx.vq, 1);
    }
}

// ---- IOAudioDevice -------------------------------------------------------------------------------

bool RippleVirtioSound::initHardware(IOService *provider)
{
    if (!super::initHardware(provider))
        return false;
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);
    for (unsigned i = 0; i < kQueueSize; i++)
        fTxHeadSlot[i] = -1;

    setDeviceName("Virtio Sound");
    setDeviceShortName("VirtioSound");
    setManufacturerName("moose-ripple");
    setDeviceTransportType(kIOAudioDeviceTransportTypePCI);

    fCtlLock = IOLockAlloc();
    fCtlMem = contiguous(kCtlBytes);
    fTxSlotBytes = (txStatusOffset() + sizeof(struct virtio_snd_pcm_status) + 63) & ~63u;
    fTxMem = contiguous(kTxSlots * fTxSlotBytes);
    if (!fCtlLock || !fCtlMem || !fTxMem)
        return false;
    if (!mapCapabilities() || !negotiate() || !findOutputStream())
        return false;
    if (!setupInterrupts()) {
        LOG("no usable interrupt");
        return false;
    }

    fEngine = new RippleVirtioSoundEngine;
    if (!fEngine || !fEngine->init(this)) {
        if (fEngine) {
            fEngine->release();
            fEngine = 0;
        }
        return false;
    }
    if (activateAudioEngine(fEngine) != kIOReturnSuccess) {
        fEngine->release();
        fEngine = 0;
        return false;
    }
    fEngine->release();             // the audio device keeps it; we keep a weak pointer
    LOG("output stream %u, engine active", fStream);
    return true;
}

void RippleVirtioSound::stop(IOService *provider)
{
    if (fIntSrc) {
        fIntSrc->disable();
        IOWorkLoop *wl = getWorkLoop();
        if (wl)
            wl->removeEventSource(fIntSrc);
        fIntSrc->release();
        fIntSrc = 0;
    }
    fEngine = 0;
    reset();
    super::stop(provider);
}

void RippleVirtioSound::free()
{
    reset();
    freeMem(fCtl.mem);
    freeMem(fTx.mem);
    freeMem(fCtlMem);
    freeMem(fTxMem);
    if (fCtlLock)
        IOLockFree(fCtlLock);
    for (unsigned i = 0; i < kNumBars; i++)
        if (fBarMap[i])
            fBarMap[i]->release();
    if (fPCI)
        fPCI->release();
    super::free();
}
