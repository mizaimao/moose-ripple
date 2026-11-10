/*
 * VirtioGPUDevice: QEMU's virtio-gpu (PCI 1af4:1050) over the modern virtio 1.0 PCI transport.
 * Owns the virtqueues and sends commands; no IOKit service of its own. The framebuffer (and,
 * from M3, the accelerator) use it. Commands are synchronous and polled for now.
 */
#ifndef RIPPLE_VIRTIO_GPU_DEVICE_H
#define RIPPLE_VIRTIO_GPU_DEVICE_H

#include <libkern/c++/OSObject.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "virtqueue.h"

class VirtioGPUDevice : public OSObject
{
    OSDeclareDefaultStructors(VirtioGPUDevice)

public:
    enum { kMaxScanouts = 16 };
    struct Rect { uint32_t x, y, width, height; };
    struct DisplayInfo { Rect r; uint32_t enabled; };

    static VirtioGPUDevice *withPCIDevice(IOPCIDevice *pci);

    uint32_t features() const { return fFeatures; }
    uint32_t numScanouts() const { return fNumScanouts; }
    uint32_t numCapsets() const { return fNumCapsets; }

    void reset();                  // device reset: drops all host resources

    // Control queue commands. Each returns true on an OK response.
    bool getDisplayInfo(DisplayInfo *out, unsigned count);
    bool resourceCreate2D(uint32_t id, uint32_t format, uint32_t width, uint32_t height);
    bool resourceUnref(uint32_t id);
    bool attachBacking(uint32_t id, IOMemoryDescriptor *md, IOByteCount length);
    bool detachBacking(uint32_t id);
    bool setScanout(uint32_t scanout, uint32_t id, const Rect &r);
    bool transferToHost2D(uint32_t id, const Rect &r, uint64_t offset);
    bool resourceFlush(uint32_t id, const Rect &r);

    // Cursor queue: no response; the request buffer is reclaimed later.
    bool updateCursor(uint32_t scanout, uint32_t id, int32_t x, int32_t y,
                      uint32_t hotX, uint32_t hotY);
    bool moveCursor(uint32_t scanout, int32_t x, int32_t y);

protected:
    virtual bool init(IOPCIDevice *pci);
    virtual void free();

private:
    enum { kNumBars = 6, kCtrlQueueSize = 64, kCmdBufSize = 8192, kCursorSlots = 16 };

    struct Queue {
        ripple_vq                 vq;
        IOBufferMemoryDescriptor *mem;
        volatile uint16_t        *notify;
        uint16_t                  index;
    };

    IOPCIDevice *fPCI;
    IOMemoryMap *fBarMap[kNumBars];
    volatile uint8_t *fCommon, *fNotify, *fISR, *fDevCfg;
    uint32_t fNotifyMult;
    uint32_t fFeatures, fNumScanouts, fNumCapsets;
    Queue fQueue[2];               // 0 control, 1 cursor

    IOLock *fCmdLock, *fCursorLock;
    IOBufferMemoryDescriptor *fCmdBuf;     // request | response for one control command
    uint64_t fCmdBufPA;
    IOBufferMemoryDescriptor *fCursorBuf;  // kCursorSlots cursor requests
    uint64_t fCursorBufPA;
    unsigned fCursorNext;

    bool mapCapabilities();
    bool setupQueue(unsigned index);
    bool negotiate();
    uint8_t status();
    void setStatus(uint8_t s);
    bool simple(const void *req, uint32_t reqLen,
                uint64_t extraPA = 0, uint32_t extraLen = 0);
    uint32_t command(const void *req, uint32_t reqLen, void *resp, uint32_t respLen,
                     uint64_t extraPA, uint32_t extraLen);
    bool cursorCommand(const void *req, uint32_t len);
};

#endif
