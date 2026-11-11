/*
 * VirtioGPUDevice: QEMU's virtio-gpu (PCI 1af4:1050) over the modern virtio 1.0 PCI transport.
 * Owns the virtqueues and sends commands; no IOKit service of its own. The framebuffer (and,
 * from M3, the accelerator) use it. Commands are synchronous and polled for now.
 */
#ifndef RIPPLE_VIRTIO_GPU_DEVICE_H
#define RIPPLE_VIRTIO_GPU_DEVICE_H

#include <libkern/c++/OSObject.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOFilterInterruptEventSource.h>
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
    bool hasVirgl() const { return fVirgl; }

    // Capability sets (virgl: id 1 and 2). getCapset fills up to `size` bytes, returns the length.
    bool getCapsetInfo(uint32_t index, uint32_t *id, uint32_t *maxVersion, uint32_t *maxSize);
    uint32_t getCapset(uint32_t id, uint32_t version, void *buf, uint32_t size);

    // Slowest control command since the last call (for diagnostics), in microseconds.
    void takeSlowest(uint32_t *us, uint32_t *type, uint32_t *over100ms);
    // Completion counts: interrupts taken, commands completed from the interrupt, by polling.
    void interruptStats(uint32_t *ints, uint32_t *byInt, uint32_t *byPoll) const
        { *ints = fIntCount; *byInt = fIntDone; *byPoll = fPollDone; }

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

    // Device-wide ids (resources are shared by the framebuffer and 3D clients).
    uint32_t allocResourceId() { return (uint32_t)OSIncrementAtomic(&fNextResource) + 1; }
    uint32_t allocContextId()  { return (uint32_t)OSIncrementAtomic(&fNextContext) + 1; }

    // 3D (virgl). Submits and transfers return once QEMU has processed them (decoded and
    // queued on the host GPU; transfers copied); *fence gets a number, in queue order, that
    // signalled() passes once the host GPU has finished it. sync() makes that happen: an
    // empty fenced submit whose reply QEMU holds until the GPU has done everything before it
    // (performance stage, 2026-11-11: fencing every submit made each flush wait for the GPU).
    bool ctxCreate(uint32_t ctx, const char *name);
    bool ctxDestroy(uint32_t ctx);
    bool ctxAttachResource(uint32_t ctx, uint32_t id);
    bool ctxDetachResource(uint32_t ctx, uint32_t id);
    bool resourceCreate3D(uint32_t id, uint32_t target, uint32_t format, uint32_t bind,
                          uint32_t width, uint32_t height, uint32_t depth, uint32_t arraySize,
                          uint32_t lastLevel, uint32_t nrSamples, uint32_t flags);
    bool submit3D(uint32_t ctx, uint64_t pa, uint32_t size, uint64_t *fence);
    bool transfer3D(bool toHost, uint32_t ctx, uint32_t id, uint32_t level, const uint32_t box[6],
                    uint32_t stride, uint32_t layerStride, uint64_t offset, uint64_t *fence);
    bool sync(uint32_t ctx);              // ctx 0: the context of the last submit
    bool syncAsync(uint32_t ctx);         // the same, without waiting for the reply
    uint64_t signalled() const { return (uint64_t)fSignalled; }

    // Cursor queue: no response; the request buffer is reclaimed later.
    bool updateCursor(uint32_t scanout, uint32_t id, int32_t x, int32_t y,
                      uint32_t hotX, uint32_t hotY);
    bool moveCursor(uint32_t scanout, int32_t x, int32_t y);

protected:
    virtual bool init(IOPCIDevice *pci);
    virtual void free();

private:
    enum { kNumBars = 6, kCtrlQueueSize = 64, kCursorSlots = 16,
           kSlots = 16, kSlotReq = 4096, kSlotResp = 4096 };   // in-flight control commands

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
    bool     fVirgl;                      // VIRTIO_GPU_F_VIRGL negotiated
    volatile SInt32 fNextResource, fNextContext;
    volatile SInt64 fNextFence;           // last number handed out (under fCmdLock)
    volatile SInt64 fSignalled;           // every number up to this one is done on the GPU
    volatile UInt32 fLastCtx;             // context of the last submit (for sync(0))
    uint64_t fSlowestAbs;               // slowest command (absolute time) and its type
    uint32_t fSlowestType, fOver100ms;
    Queue fQueue[2];               // 0 control, 1 cursor

    IOLock *fCmdLock, *fCursorLock;        // fCmdLock: control ring and slots
    IOBufferMemoryDescriptor *fSlotMem;    // kSlots x (request | response)
    uint64_t fSlotPA;
    struct Slot { bool busy; bool done; bool lost; bool async; uint64_t fence; } fSlot[kSlots];
    int8_t   fHeadSlot[kCtrlQueueSize];   // descriptor chain head -> slot

    // Interrupts: a filter on the PCI interrupt reads the ISR; completions are drained on our
    // own work loop (not the framebuffer's, whose timer waits for commands). Waiters also
    // poll, so a missing interrupt only costs latency.
    IOWorkLoop                    *fWorkLoop;
    IOFilterInterruptEventSource  *fIntSrc;
    bool                           fIntEnabled;
    uint32_t                       fIntCount, fIntDone, fPollDone;
    IOBufferMemoryDescriptor *fCursorBuf;  // kCursorSlots cursor requests
    uint64_t fCursorBufPA;
    unsigned fCursorNext;

    bool mapCapabilities();
    bool setupQueue(unsigned index);
    bool negotiate();
    uint8_t status();
    void setStatus(uint8_t s);
    bool simple(const void *req, uint32_t reqLen,
                uint64_t extraPA = 0, uint32_t extraLen = 0, uint64_t *fence = 0);
    // fence: give the request the next fence number as it is queued (queue order = number order)
    uint32_t command(const void *req, uint32_t reqLen, void *resp, uint32_t respLen,
                     uint64_t extraPA, uint32_t extraLen, uint64_t *fence = 0,
                     bool async = false);   // async: queue and return; the drain frees it
    bool cursorCommand(const void *req, uint32_t len);
    void drainControlLocked(bool fromInterrupt);
    bool setupInterrupts();
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
};

#endif
