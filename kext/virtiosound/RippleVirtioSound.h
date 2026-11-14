/*
 * RippleVirtioSound: a Mac OS X 10.6 audio driver for QEMU's virtio-sound-pci (virtio 1.2,
 * section 5.14). The device (this class, an IOAudioDevice) does the virtio PCI transport, the
 * control queue and the TX queue; RippleVirtioSoundEngine (an IOAudioEngine) is Core Audio's
 * side: a ring of samples that Core Audio mixes into, which the engine hands to the device a
 * period at a time. Output only for now.
 *
 * The virtio transport is a trimmed copy of RippleVirtioGPU's (kext/virtiogpu), and so is the
 * interrupt handling: the filter quiets the device itself and never leaves our line masked,
 * because Q35 mirrors every PCI line onto legacy interrupt 10 or 11 (#22, 14f325e).
 */
#ifndef RIPPLE_VIRTIO_SOUND_H
#define RIPPLE_VIRTIO_SOUND_H

#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLocks.h>

#include "virtqueue.h"

class RippleVirtioSoundEngine;

class RippleVirtioSound : public IOAudioDevice {
    OSDeclareDefaultStructors(RippleVirtioSound)

public:
    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();

    // TX slots: a fixed set of buffers, each [xfer header][PCM data][status]. The engine fills
    // a slot's data and submits it; the device returns it when QEMU has taken the data.
    enum { kTxSlots = 8, kMaxPeriodBytes = 8192 };
    void    *txData(unsigned slot);
    bool     txSubmit(unsigned slot, uint32_t bytes);
    unsigned txInFlight() const { return fTxInFlight; }

    // PCM control requests for the engine's stream (VIRTIO_SND_R_PCM_*); true when the device
    // answered VIRTIO_SND_S_OK.
    bool pcmSetParams(uint32_t bufferBytes, uint32_t periodBytes, uint8_t channels,
                      uint8_t format, uint8_t rate);
    bool pcmCommand(uint32_t code);
    // Take completed TX slots off the used ring now (polling, e.g. while stopping).
    void drainTx();

private:
    enum { kNumBars = 6, kQueueSize = 64, kCtlBytes = 512 };
    struct Queue {
        IOBufferMemoryDescriptor *mem;
        struct ripple_vq          vq;
        volatile uint16_t        *notify;
        uint16_t                  index;
    };

    IOPCIDevice       *fPCI;
    IOMemoryMap       *fBarMap[kNumBars];
    volatile uint8_t  *fCommon, *fNotify, *fISR, *fDevCfg;
    uint32_t           fNotifyMult;
    Queue              fCtl, fTx;
    IOLock            *fCtlLock;
    IOBufferMemoryDescriptor *fCtlMem;      // request + response of one control command
    IOBufferMemoryDescriptor *fTxMem;       // kTxSlots slots
    uint32_t           fTxSlotBytes;
    int8_t             fTxHeadSlot[kQueueSize];   // descriptor head -> slot
    unsigned           fTxInFlight;
    IOFilterInterruptEventSource *fIntSrc;
    uint32_t           fStream;             // the output stream we use
    RippleVirtioSoundEngine *fEngine;
    uint32_t           fIntCount;

    bool     mapCapabilities();
    bool     setupQueue(Queue &q, unsigned index);
    bool     negotiate();
    void     reset();
    uint8_t  status();
    void     setStatus(uint8_t s);
    uint32_t control(const void *req, uint32_t reqLen, void *resp, uint32_t respLen);
    bool     findOutputStream();
    bool     setupInterrupts();
    void     txCompleted();
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
};

#endif
