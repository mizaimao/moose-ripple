/*
 * RippleVirtioGPU: driver for QEMU's virtio-gpu (PCI 1af4:1050), modern virtio 1.0 transport.
 * M2 step 1: transport, control queue, GET_DISPLAY_INFO and a 2D test pattern. The
 * IOFramebuffer comes in step 2.
 */
#ifndef RIPPLE_VIRTIO_GPU_H
#define RIPPLE_VIRTIO_GPU_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "virtqueue.h"

class RippleVirtioGPU : public IOService
{
    OSDeclareDefaultStructors(RippleVirtioGPU)

    enum { kNumBars = 6, kQueueSize = 64, kCmdBufSize = 4096 };

    IOPCIDevice *fPCI;
    IOMemoryMap *fBarMap[kNumBars];

    volatile uint8_t *fCommon;     // struct virtio_pci_common_cfg
    volatile uint8_t *fNotify;     // notify area base
    uint32_t          fNotifyMult; // notify_off_multiplier
    volatile uint8_t *fISR;
    volatile uint8_t *fDevCfg;     // struct virtio_gpu_config

    struct Queue {
        ripple_vq                 vq;
        IOBufferMemoryDescriptor *mem;
        volatile uint16_t        *notify;
    } fQueue[2];                   // 0 control, 1 cursor

    IOLock                   *fCmdLock;
    IOBufferMemoryDescriptor *fCmdBuf;     // request + response for one command
    uint64_t                  fCmdBufPA;

    // Test pattern
    IOBufferMemoryDescriptor *fFB;
    IOBufferMemoryDescriptor *fEntries;    // virtio_gpu_mem_entry list for ATTACH_BACKING
    uint32_t fWidth, fHeight;

    bool     mapCapabilities();
    bool     setupQueue(unsigned index);
    void     resetDevice();
    uint8_t  status();
    void     setStatus(uint8_t s);

    // Sends `req` (reqLen bytes, plus an optional extra device-readable segment) and waits
    // for the response (respLen bytes) by polling. Returns the response type, or 0 on timeout.
    uint32_t command(const void *req, uint32_t reqLen, void *resp, uint32_t respLen,
                     uint64_t extraPA = 0, uint32_t extraLen = 0);

    bool     drawTestPattern(uint32_t width, uint32_t height);

public:
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
};

#endif
