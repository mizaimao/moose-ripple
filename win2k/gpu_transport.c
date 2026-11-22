/* Windows 2000 x86 modern virtio PCI transport for offscreen rendering. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include "gpu_memory.h"
#pragma warning(push)
#pragma warning(disable:4200)
#include <linux/virtio_gpu.h>
#pragma warning(pop)

enum {
    DeviceFeatureSelect = 0, DeviceFeature = 4, DriverFeatureSelect = 8,
    DriverFeature = 12, DeviceStatus = 20, QueueSelect = 22, QueueSize = 24,
    QueueEnable = 28, QueueNotifyOffset = 30, QueueDesc = 32,
    QueueDriver = 40, QueueDevice = 48, CommonBytes = 56
};
enum {
    Ack = 1, Driver = 2, DriverOk = 4, FeaturesOk = 8, Virgl = 1,
    Version1High = 1, DescriptorNext = 1, DescriptorWrite = 2,
    NoInterrupt = 1, QueueEntries = 16
};
/* The queue and one request take the first page, the reply the second.
 * Larger command data follows the request in separate DMA staging
 * segments, chained as further descriptors. One request is in flight. */
enum {
    Desc = 0, Avail = 256, Used = 512, Request = 1024, Reply = 4096,
    PacketBytes = RB2K_INLINE_PACKET_BYTES, ReplyBytes = 4096, DmaBytes = 8192,
    CursorQueue = 8192, CursorEntries = 2, CursorDmaBytes = RB2K_CURSOR_DMA_PIXELS + RB2K_CURSOR_PIXELS * 4
};
typedef char request_fits_before_reply[Request + PacketBytes <= Reply ? 1 : -1];
typedef char queue_fits_before_request[Used + 4 + 8 * QueueEntries + 2 <= Request ? 1 : -1];
typedef char avail_fits_before_used[Avail + 4 + 2 * QueueEntries + 2 <= Used ? 1 : -1];
typedef char extents_fit_queue[RB2K_GPU_MAX_EXTENTS + 2 <= QueueEntries ? 1 : -1];
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
    gpu->cursorResource = 0;
    gpu->cursorDoorbell = NULL;
    gpu->cursorVisible = FALSE;
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

static PUSHORT configure_queue(RB2K_GPU *gpu, USHORT number, USHORT entries, ULONG offset)
{
    PUCHAR common = gpu->common;
    USHORT notify;
    VideoPortWriteRegisterUshort((PUSHORT)(common + QueueSelect), number);
    if (VideoPortReadRegisterUshort((PUSHORT)(common + QueueSize)) < entries ||
        VideoPortReadRegisterUshort((PUSHORT)(common + QueueEnable))) return NULL;
    notify = VideoPortReadRegisterUshort((PUSHORT)(common + QueueNotifyOffset));
    if (notify && gpu->multiplier > (gpu->notifySize - 2) / notify) return NULL;
    if ((notify * gpu->multiplier) & 1) return NULL;
    ((AVAILABLE *)(gpu->dma + offset + Avail))->flags = NoInterrupt;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    VideoPortWriteRegisterUshort((PUSHORT)(common + QueueSize), entries);
    write_address(gpu, QueueDesc, offset + Desc);
    write_address(gpu, QueueDriver, offset + Avail);
    write_address(gpu, QueueDevice, offset + Used);
    VideoPortWriteRegisterUshort((PUSHORT)(common + QueueEnable), 1);
    return (PUSHORT)(gpu->notifyBase + notify * gpu->multiplier);
}

VP_STATUS rb_gpu_start(RB2K_GPU *gpu, RB2K_GPU_RESULT *result)
{
    PUCHAR common = gpu->common;
    ULONG high, bytes = gpu->cursorEnabled ? CursorDmaBytes : DmaBytes;
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
    gpu->features = result->features;
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
    if (!gpu->dma && gpu->cursorEnabled)
        gpu->dma = VideoPortGetCommonBuffer(gpu->extension, bytes, 4096, &gpu->bus,
                                            &gpu->allocated, TRUE);
    if (!gpu->dma)
        gpu->dma = VideoPortGetCommonBuffer(gpu->extension, DmaBytes, 4096, &gpu->bus,
                                            &gpu->allocated, TRUE);
    if (!gpu->dma || gpu->allocated < DmaBytes) return ERROR_NOT_ENOUGH_MEMORY;
    if (gpu->bus.LowPart & 15) return ERROR_INVALID_DATA;
    VideoPortZeroMemory(gpu->dma, gpu->allocated < bytes ? DmaBytes : bytes);
    gpu->index = gpu->cursorIndex = 0;
    gpu->fence = 0;
    gpu->doorbell = configure_queue(gpu, 0, QueueEntries, 0);
    if (!gpu->doorbell) return ERROR_INVALID_DATA;
    if (gpu->cursorEnabled && gpu->allocated >= CursorDmaBytes)
        gpu->cursorDoorbell = configure_queue(gpu, 1, CursorEntries, CursorQueue);
    VideoPortWriteRegisterUchar(common + DeviceStatus, Ack | Driver | FeaturesOk | DriverOk);
    if (VideoPortReadRegisterUchar(common + DeviceStatus) != (Ack | Driver | FeaturesOk | DriverOk))
        return ERROR_NOT_READY;
    gpu->ready = TRUE;
    return NO_ERROR;
}

VP_STATUS rb_gpu_submit(RB2K_GPU *gpu, const void *request, ULONG bytes,
                        void *reply, ULONG capacity, PULONG received)
{
    return rb_gpu_submit_extents(gpu, request, bytes, NULL, 0, reply, capacity, received);
}

/* Each queue allows one in-flight request. A timeout quarantines both
 * queues until the transport acknowledges a reset. */
static VP_STATUS submit_chain(RB2K_GPU *gpu, ULONG offset, USHORT number, USHORT entries,
                              PUSHORT index, PUSHORT doorbell, ULONG capacity, PULONG received)
{
    volatile AVAILABLE *available = (AVAILABLE *)(gpu->dma + offset + Avail);
    volatile USED_RING *used = (USED_RING *)(gpu->dma + offset + Used);
    USHORT slot = *index % entries, next = (USHORT)(*index + 1);
    ULONG i;
    available->ring[slot] = 0;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    available->index = next;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    VideoPortWriteRegisterUshort(doorbell, number);
    for (i = 0; used->index == *index; ++i) {
        if (i == 1000000) { gpu->ready = FALSE; return ERROR_TIMEOUT; }
        VideoPortStallExecution(10);
    }
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    if (used->index != next || used->ring[slot].id != 0 || used->ring[slot].length > capacity) {
        gpu->ready = FALSE;
        return ERROR_INVALID_DATA;
    }
    *index = next;
    *received = used->ring[slot].length;
    return NO_ERROR;
}

VP_STATUS rb_gpu_cursor_command(RB2K_GPU *gpu, const void *request, ULONG bytes)
{
    DESCRIPTOR *desc;
    ULONG received;
    if (!gpu->ready || !gpu->cursorDoorbell) return ERROR_NOT_READY;
    if (!bytes || bytes > RB2K_CURSOR_DMA_PIXELS - CursorQueue - Request) return ERROR_INVALID_PARAMETER;
    desc = (DESCRIPTOR *)(gpu->dma + CursorQueue + Desc);
    VideoPortMoveMemory(gpu->dma + CursorQueue + Request, (PVOID)request, bytes);
    desc->address.QuadPart = gpu->bus.QuadPart + CursorQueue + Request;
    desc->length = bytes;
    desc->flags = desc->next = 0;
    return submit_chain(gpu, CursorQueue, 1, CursorEntries, &gpu->cursorIndex, gpu->cursorDoorbell, 0, &received);
}

VP_STATUS rb_gpu_submit_extents(RB2K_GPU *gpu, const void *request, ULONG bytes,
                                const RB2K_GPU_EXTENT *extents, ULONG count,
                                void *reply, ULONG capacity, PULONG received)
{
    DESCRIPTOR *desc;
    ULONG i;
    VP_STATUS status;
    LONGLONG start = 0;
    *received = 0;
    if (!gpu->ready) return ERROR_NOT_READY;
    if (!bytes || bytes > PacketBytes || !capacity || capacity > ReplyBytes ||
        count > RB2K_GPU_MAX_EXTENTS || (count && !extents))
        return ERROR_INVALID_PARAMETER;
    desc = (DESCRIPTOR *)(gpu->dma + Desc);
    VideoPortMoveMemory(gpu->dma + Request, (PVOID)request, bytes);
    VideoPortZeroMemory(gpu->dma + Reply, capacity);
    desc[0].address.QuadPart = gpu->bus.QuadPart + Request;
    desc[0].length = bytes;
    desc[0].flags = DescriptorNext;
    desc[0].next = 1;
    /* Device-readable data that continues the request, in order. */
    for (i = 0; i < count; ++i) {
        desc[1 + i].address = extents[i].bus;
        desc[1 + i].length = extents[i].length;
        desc[1 + i].flags = DescriptorNext;
        desc[1 + i].next = (USHORT)(2 + i);
    }
    desc[1 + count].address.QuadPart = gpu->bus.QuadPart + Reply;
    desc[1 + count].length = capacity;
    desc[1 + count].flags = DescriptorWrite;
    desc[1 + count].next = 0;
    if (gpu->d7Measure) start = rb_counter_ticks(NULL);
    status = submit_chain(gpu, 0, 0, QueueEntries, &gpu->index, gpu->doorbell, capacity, received);
    if (gpu->d7Measure) {
        const struct virtio_gpu_ctrl_hdr *header = request;
        RB2K_D7_TIME *t = gpu->d7Measure;
        LONGLONG waited = rb_counter_ticks(NULL)-start;
        t->queueTicks += waited;
        ++t->submits;
        t->bytes += bytes;
        for (i = 0; i < count; ++i) t->bytes += extents[i].length;
        if (bytes >= sizeof(*header) && (header->flags & VIRTIO_GPU_FLAG_FENCE)) {
            t->fenceTicks += waited;
            ++t->fences;
        }
    }
    if (!status) VideoPortMoveMemory(reply, gpu->dma + Reply, *received);
    return status;
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
    VP_STATUS cleanup;
    rb_gl_finish(gpu);
    cleanup = stop(gpu);
    if (!cleanup) VideoPortZeroMemory(&gpu->glDesktop, sizeof(gpu->glDesktop));
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
    if (!cleanup && gpu->cursorValid && gpu->cursorShape.info.x >= 0) {
        if (rb_cursor_restore(gpu)) ++gpu->cursorStats.failed;
        else ++gpu->cursorStats.restores;
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
    rb_gpu_rvd_sweep(gpu);
    if (gpu->imageCount || gpu->rvdContexts) {
        result->status = ERROR_BUSY;
        VideoPortInterlockedExchange(&gpu->busy, 0);
        return;
    }
    if (gpu->ready) result->features = gpu->features;
    else result->status = rb_gpu_start(gpu, result);
    if (!result->status) rb_gpu_pixel(gpu, result);
    cleanup = rb_gpu_stop(gpu);
    if (cleanup && !result->status) {
        result->stage = RB2K_STAGE_CLEANUP;
        result->status = cleanup;
    }
    VideoPortInterlockedExchange(&gpu->busy, 0);
}
