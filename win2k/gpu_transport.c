/* Windows 2000 x86 modern virtio PCI transport for offscreen rendering. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"

enum {
    DeviceFeatureSelect = 0, DeviceFeature = 4, DriverFeatureSelect = 8,
    DriverFeature = 12, DeviceStatus = 20, QueueSelect = 22, QueueSize = 24,
    QueueEnable = 28, QueueNotifyOffset = 30, QueueDesc = 32,
    QueueDriver = 40, QueueDevice = 48, CommonBytes = 56
};
enum {
    Ack = 1, Driver = 2, DriverOk = 4, FeaturesOk = 8, Virgl = 1,
    Version1High = 1, DescriptorNext = 1, DescriptorWrite = 2,
    NoInterrupt = 1, QueueEntries = 2
};
/* The queue needs one page. CPU transfers allocate separate DMA staging,
 * shared under the same lock and attached to one host texture at a time. */
enum {
    Desc = 0, Avail = 32, Used = 48, Request = 128, Reply = 1152,
    PacketBytes = 1024, DmaBytes = 4096
};
typedef struct { PHYSICAL_ADDRESS address; ULONG length; USHORT flags, next; } DESCRIPTOR;
typedef struct { USHORT flags, index, ring[QueueEntries]; } AVAILABLE;
typedef struct { ULONG id, length; } USED_ENTRY;
typedef struct { USHORT flags, index; USED_ENTRY ring[QueueEntries]; } USED_RING;
typedef char check_desc_size[sizeof(DESCRIPTOR) == 16 ? 1 : -1];

typedef struct { ULONG start, bytes; } REGION;

void rb_gpu_find(RB2K_GPU *gpu, PVOID extension, ULONG slot,
                 PVIDEO_ACCESS_RANGE registers, PVIDEO_PORT_CONFIG_INFO config)
{
    ULONG data[64], pos, visited, kind, offset, length;
    PUCHAR pci = (PUCHAR)data;
    REGION common = {0, 0}, notify = {0, 0};
    PHYSICAL_ADDRESS address;
    if (VideoPortGetBusData(extension, PCIConfiguration, slot, pci, 0, sizeof(data))
            != sizeof(data) || !(pci[6] & 0x10)) return;
    pos = pci[0x34] & 0xfc;
    for (visited = 0; pos && visited < 48; ++visited, pos = pci[pos+1] & 0xfc) {
        if (pos < 0x40) return;
        if (pci[pos] != 9) continue;
        if (pos > 240 || pci[pos+2] < 16 || pci[pos+2] > 256 - pos) return;
        kind = pci[pos+3];
        if (kind != 1 && kind != 2) continue;
        /* QEMU's virtio-vga keeps these capabilities in the assigned BAR2
         * alongside (and separate from) the compatibility VBE registers. */
        /* A second notification capability can advertise a port I/O BAR. */
        if (pci[pos+4] != 2) continue;
        offset = data[(pos+8)/4];
        length = data[(pos+12)/4];
        if (offset > registers->RangeLength || length > registers->RangeLength - offset)
            return;
        if (kind == 1) {
            if ((offset & 3) || length < CommonBytes) return;
            common.start = offset;
            common.bytes = CommonBytes;
        } else {
            if ((offset & 1) || length < 2 || pci[pos+2] < 20) return;
            notify.start = offset;
            notify.bytes = length;
            gpu->multiplier = data[(pos+16)/4];
        }
    }
    if (pos || !common.bytes || !notify.bytes) return;
    address.QuadPart = registers->RangeStart.QuadPart + common.start;
    gpu->common = VideoPortGetDeviceBase(extension, address, common.bytes, 0);
    if (!gpu->common) return;
    address.QuadPart = registers->RangeStart.QuadPart + notify.start;
    gpu->notifyBase = VideoPortGetDeviceBase(extension, address, notify.bytes, 0);
    if (!gpu->notifyBase) {
        VideoPortFreeDeviceBase(extension, gpu->common);
        gpu->common = NULL;
        return;
    }
    gpu->extension = extension;
    gpu->notifySize = notify.bytes;
    /* The Windows 2000 VideoPort DMA interface distinguishes DMA use from
     * an idle channel/port pair. This is a PCI bus master. */
    config->DmaChannel = 1;
    config->Master = TRUE;
    config->DmaWidth = Width32Bits;
    config->MaximumTransferLength = RB2K_SEGMENT_BYTES;
}

static void write32(RB2K_GPU *gpu, ULONG reg, ULONG value)
{
    VideoPortWriteRegisterUlong((PULONG)(gpu->common + reg), value);
}

static void write_address(RB2K_GPU *gpu, ULONG reg, ULONG offset)
{
    PHYSICAL_ADDRESS address;
    address.QuadPart = gpu->bus.QuadPart + offset;
    write32(gpu, reg, address.LowPart);
    write32(gpu, reg + 4, (ULONG)address.HighPart);
}

/* Only retire DMA storage after status zero acknowledges reset. */
static VP_STATUS stop(RB2K_GPU *gpu)
{
    ULONG i;
    gpu->ready = FALSE;
    if (gpu->owned) {
        VideoPortWriteRegisterUchar(gpu->common + DeviceStatus, 0);
        for (i = 0; VideoPortReadRegisterUchar(gpu->common + DeviceStatus); ++i) {
            if (i == 10000) return ERROR_TIMEOUT;
            VideoPortStallExecution(10);
        }
        gpu->owned = FALSE;
    }
    return NO_ERROR;
}

/* NT5's PCI miniport interface takes a slot of zero for this adapter.
 * Change only the two-byte command word: the adjacent status word has
 * write-one-to-clear bits. Keep the pre- and post-write values for a
 * transport-stage error report. */
static VP_STATUS bus_master(RB2K_GPU *gpu, BOOLEAN enable, PULONG observed)
{
    PCI_COMMON_CONFIG pci;
    USHORT command, desired;
    ULONG done;
    done = VideoPortGetBusData(gpu->extension, PCIConfiguration, 0, &pci, 0,
                               PCI_COMMON_HDR_LENGTH);
    if (done != PCI_COMMON_HDR_LENGTH) return ERROR_READ_FAULT;
    if (pci.VendorID != 0x1af4 || pci.DeviceID != 0x1050) return ERROR_DEV_NOT_EXIST;
    command = pci.Command;
    *observed = command;
    if (enable && !(command & PCI_ENABLE_MEMORY_SPACE)) return ERROR_NOT_READY;
    desired = enable ? (USHORT)(command | PCI_ENABLE_BUS_MASTER) :
                       (USHORT)(command & ~PCI_ENABLE_BUS_MASTER);
    if (desired != command) {
        if (enable) gpu->enabledMaster = TRUE;
        done = VideoPortSetBusData(gpu->extension, PCIConfiguration, 0, &desired,
                                   FIELD_OFFSET(PCI_COMMON_CONFIG, Command), sizeof(desired));
        if (done != sizeof(desired)) return ERROR_WRITE_FAULT;
        done = VideoPortGetBusData(gpu->extension, PCIConfiguration, 0, &pci, 0,
                                   PCI_COMMON_HDR_LENGTH);
        if (done != PCI_COMMON_HDR_LENGTH) return ERROR_READ_FAULT;
    }
    *observed |= (ULONG)pci.Command << 16;
    if ((pci.Command ^ desired) & PCI_ENABLE_BUS_MASTER) return ERROR_ACCESS_DENIED;
    if (!enable) gpu->enabledMaster = FALSE;
    return NO_ERROR;
}

VP_STATUS rb_gpu_start(RB2K_GPU *gpu, RB2K_GPU_RESULT *result)
{
    PUCHAR common = gpu->common;
    USHORT notifyOffset;
    ULONG high;
    VP_STATUS status;
    if (!common) return ERROR_NOT_SUPPORTED;
    status = bus_master(gpu, TRUE, &result->response);
    if (status) return status;
    /* The miniport owns the modern function and keeps scanout on VGA.
     * Reset acknowledges completion of any previously timed-out request. */
    gpu->owned = TRUE;
    status = stop(gpu);
    if (status) return status;
    gpu->owned = TRUE;
    VideoPortWriteRegisterUchar(common + DeviceStatus, Ack);
    VideoPortWriteRegisterUchar(common + DeviceStatus, Ack | Driver);
    write32(gpu, DeviceFeatureSelect, 0);
    result->features = VideoPortReadRegisterUlong((PULONG)(common + DeviceFeature));
    write32(gpu, DeviceFeatureSelect, 1);
    high = VideoPortReadRegisterUlong((PULONG)(common + DeviceFeature));
    if (!(result->features & Virgl) || !(high & Version1High)) return ERROR_NOT_SUPPORTED;
    write32(gpu, DriverFeatureSelect, 0);
    write32(gpu, DriverFeature, Virgl);
    write32(gpu, DriverFeatureSelect, 1);
    write32(gpu, DriverFeature, Version1High);
    VideoPortWriteRegisterUchar(common + DeviceStatus, Ack | Driver | FeaturesOk);
    if (!(VideoPortReadRegisterUchar(common + DeviceStatus) & FeaturesOk))
        return ERROR_NOT_SUPPORTED;
    VideoPortWriteRegisterUshort((PUSHORT)(common + QueueSelect), 0);
    if (VideoPortReadRegisterUshort((PUSHORT)(common + QueueSize)) < QueueEntries ||
        VideoPortReadRegisterUshort((PUSHORT)(common + QueueEnable))) return ERROR_INVALID_DATA;
    notifyOffset = VideoPortReadRegisterUshort((PUSHORT)(common + QueueNotifyOffset));
    if (notifyOffset && gpu->multiplier > (gpu->notifySize - 2) / notifyOffset)
        return ERROR_INVALID_DATA;
    if ((notifyOffset * gpu->multiplier) & 1) return ERROR_INVALID_DATA;
    gpu->doorbell = (PUSHORT)(gpu->notifyBase + notifyOffset * gpu->multiplier);

    if (!gpu->dma)
        gpu->dma = VideoPortGetCommonBuffer(gpu->extension, DmaBytes, 4096, &gpu->bus,
                                            &gpu->allocated, TRUE);
    if (!gpu->dma || gpu->allocated < DmaBytes) return ERROR_NOT_ENOUGH_MEMORY;
    if (gpu->bus.LowPart & 15) return ERROR_INVALID_DATA;
    VideoPortZeroMemory(gpu->dma, DmaBytes);
    ((AVAILABLE *)(gpu->dma + Avail))->flags = NoInterrupt;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    gpu->index = 0;
    gpu->fence = 0;
    VideoPortWriteRegisterUshort((PUSHORT)(common + QueueSize), QueueEntries);
    write_address(gpu, QueueDesc, Desc);
    write_address(gpu, QueueDriver, Avail);
    write_address(gpu, QueueDevice, Used);
    VideoPortWriteRegisterUshort((PUSHORT)(common + QueueEnable), 1);
    VideoPortWriteRegisterUchar(common + DeviceStatus, Ack | Driver | FeaturesOk | DriverOk);
    if (VideoPortReadRegisterUchar(common + DeviceStatus) != (Ack | Driver | FeaturesOk | DriverOk))
        return ERROR_NOT_READY;
    gpu->ready = TRUE;
    return NO_ERROR;
}

VP_STATUS rb_gpu_submit(RB2K_GPU *gpu, const void *request, ULONG bytes,
                        void *reply, ULONG capacity, PULONG received)
{
    DESCRIPTOR *desc;
    volatile AVAILABLE *available;
    volatile USED_RING *used;
    USHORT next;
    ULONG slot, i, length;
    *received = 0;
    if (!gpu->ready) return ERROR_NOT_READY;
    if (!bytes || bytes > PacketBytes || !capacity || capacity > PacketBytes)
        return ERROR_INVALID_PARAMETER;
    desc = (DESCRIPTOR *)(gpu->dma + Desc);
    available = (AVAILABLE *)(gpu->dma + Avail);
    used = (USED_RING *)(gpu->dma + Used);
    VideoPortMoveMemory(gpu->dma + Request, (PVOID)request, bytes);
    VideoPortZeroMemory(gpu->dma + Reply, capacity);
    desc[0].address.QuadPart = gpu->bus.QuadPart + Request;
    desc[0].length = bytes;
    desc[0].flags = DescriptorNext;
    desc[0].next = 1;
    desc[1].address.QuadPart = gpu->bus.QuadPart + Reply;
    desc[1].length = capacity;
    desc[1].flags = DescriptorWrite;
    desc[1].next = 0;
    slot = gpu->index % QueueEntries;
    next = (USHORT)(gpu->index + 1);
    available->ring[slot] = 0;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    available->index = next;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    VideoPortWriteRegisterUshort(gpu->doorbell, 0);
    /* StartIO is synchronous. Bound a missing reply; no further command
     * can reuse these descriptors until the transport has been reset. */
    for (i = 0; used->index == gpu->index; ++i) {
        if (i == 100000) {
            gpu->ready = FALSE;
            return ERROR_TIMEOUT;
        }
        VideoPortStallExecution(10);
    }
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    length = used->ring[slot].length;
    if (used->index != next || used->ring[slot].id != 0 || length > capacity) {
        gpu->ready = FALSE;
        return ERROR_INVALID_DATA;
    }
    gpu->index = next;
    VideoPortMoveMemory(reply, gpu->dma + Reply, length);
    *received = length;
    return NO_ERROR;
}

VP_STATUS rb_gpu_reserve_staging(RB2K_GPU *gpu, ULONG bytes)
{
    ULONG i;
    if (!gpu->ready) return ERROR_NOT_READY;
    if (!bytes || bytes > RB2K_DD_MAX_BYTES) return ERROR_INVALID_PARAMETER;
    for (i = 0; i * RB2K_SEGMENT_BYTES < bytes; ++i) {
        RB2K_DMA_SEGMENT *s = &gpu->staging[i];
        if (!s->data) {
            s->data = VideoPortGetCommonBuffer(gpu->extension, RB2K_SEGMENT_BYTES, 4096,
                                                &s->bus, &s->allocated, TRUE);
            if (!s->data) return ERROR_NOT_ENOUGH_MEMORY;
            if (s->allocated < RB2K_SEGMENT_BYTES) {
                VideoPortFreeCommonBuffer(gpu->extension, s->allocated, s->data, s->bus, TRUE);
                s->data = NULL;
                return ERROR_NOT_ENOUGH_MEMORY;
            }
        }
    }
    return NO_ERROR;
}

VP_STATUS rb_gpu_stop(RB2K_GPU *gpu)
{
    ULONG pciCommand, i;
    VP_STATUS cleanup = stop(gpu);
    /* A reset, including a failed attempt, invalidates all guest handles.
     * The pinned buffers survive an unacknowledged reset until a retry. */
    VideoPortZeroMemory(gpu->images, sizeof(gpu->images));
    gpu->imageCount = 0;
    if (!cleanup && gpu->enabledMaster) cleanup = bus_master(gpu, FALSE, &pciCommand);
    if (!cleanup) {
        for (i = 0; i < RB2K_MAX_SEGMENTS; ++i) {
            RB2K_DMA_SEGMENT *s = &gpu->staging[i];
            if (!s->data) break;
            VideoPortFreeCommonBuffer(gpu->extension, s->allocated, s->data, s->bus, TRUE);
            s->data = NULL;
        }
    }
    if (!cleanup && gpu->dma) {
        VideoPortFreeCommonBuffer(gpu->extension, gpu->allocated, gpu->dma, gpu->bus, TRUE);
        gpu->dma = NULL;
    }
    return cleanup;
}

void rb_gpu_probe(RB2K_GPU *gpu, RB2K_GPU_RESULT *result)
{
    VP_STATUS cleanup;
    VideoPortZeroMemory(result, sizeof(*result));
    result->version = RB2K_GPU_VERSION;
    result->stage = RB2K_STAGE_TRANSPORT;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) {
        result->status = ERROR_BUSY;
        return;
    }
    if (gpu->imageCount) {
        result->status = ERROR_BUSY;
        VideoPortInterlockedExchange(&gpu->busy, 0);
        return;
    }
    result->status = rb_gpu_start(gpu, result);
    if (!result->status) rb_gpu_pixel(gpu, result);
    cleanup = rb_gpu_stop(gpu);
    if (cleanup && !result->status) {
        result->stage = RB2K_STAGE_CLEANUP;
        result->status = cleanup;
    }
    VideoPortInterlockedExchange(&gpu->busy, 0);
}
