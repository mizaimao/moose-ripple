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
 * segments. Native D3D frames use separate packets and descriptor pairs. */
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
/* Each scene slot holds one SUBMIT_3D packet and, at its end, the reply. */
enum { FrameFirstDesc = QueueEntries - 2*RB2K_D7_FLIGHT_SLOTS, FramePacketBytes = 262144,
       FrameReplyOffset = FramePacketBytes - 64 };
typedef char frame_fits[sizeof(struct virtio_gpu_cmd_submit) + RB2K_D7_FRAME_WORDS*sizeof(ULONG) <= FrameReplyOffset ? 1 : -1];
typedef char check_desc_size[sizeof(DESCRIPTOR) == 16 ? 1 : -1];

typedef struct { ULONG start, bytes; } REGION;

/* QEMU answers a fenced request once it finds that fence retired. It looks
 * after handling each notification of the control queue, and otherwise on
 * a 10 ms timer, so a request whose GPU work is still running when QEMU
 * handles it waits for that timer. A notification with no new request
 * makes QEMU look again, so a waiter rings the doorbell after 0.2 ms and
 * then at doubling intervals up to 2 ms. Counted in 10 us polls. */
enum { NudgeFirst = 20, NudgeLongest = 200 };
typedef struct { ULONG at, step; } NUDGE;

static void nudge_start(NUDGE *n)
{
    n->at = n->step = NudgeFirst;
}

static void nudge(RB2K_GPU *gpu, NUDGE *n, ULONG polls)
{
    if (polls != n->at) return;
    VideoPortWriteRegisterUshort(gpu->doorbell, 0);
    if (n->step < NudgeLongest) n->step = n->step*2 < NudgeLongest ? n->step*2 : NudgeLongest;
    n->at += n->step;
}

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
    gpu->memoryResource = gpu->memoryBack = 0;   /* the host has dropped them, and the scanout with them */
    gpu->memoryVisible = 0;
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
    gpu->index = gpu->usedIndex = gpu->cursorIndex = 0;
    gpu->d7Next = 0;
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

/* These callers have one in-flight request. A timeout quarantines both
 * queues until the transport acknowledges a reset. */
static VP_STATUS submit_chain(RB2K_GPU *gpu, ULONG offset, USHORT number, USHORT entries,
                              PUSHORT index, PUSHORT doorbell, ULONG capacity, PULONG received)
{
    volatile AVAILABLE *available = (AVAILABLE *)(gpu->dma + offset + Avail);
    volatile USED_RING *used = (USED_RING *)(gpu->dma + offset + Used);
    USHORT slot = *index % entries, next = (USHORT)(*index + 1);
    ULONG i;
    NUDGE n;
    available->ring[slot] = 0;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    available->index = next;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    VideoPortWriteRegisterUshort(doorbell, number);
    nudge_start(&n);
    for (i = 0; used->index == *index; ++i) {
        if (i == 1000000) { gpu->ready = FALSE; return ERROR_TIMEOUT; }
        VideoPortStallExecution(10);
        if (doorbell == gpu->doorbell) nudge(gpu, &n, i+1);
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

/* The control queue has one ordinary request (head 0) and at most two
 * pending native scenes. Used entries can arrive in a different order. */
static VP_STATUS control_error(RB2K_GPU *gpu, VP_STATUS status)
{
    gpu->ready = FALSE;
    ++gpu->d7Flight.failed;
    return status;
}

static void publish_control(RB2K_GPU *gpu, USHORT head)
{
    volatile AVAILABLE *available = (AVAILABLE *)(gpu->dma + Avail);
    available->ring[gpu->index % QueueEntries] = head;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    available->index = ++gpu->index;
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    VideoPortWriteRegisterUshort(gpu->doorbell, 0);
}

static VP_STATUS collect_control(RB2K_GPU *gpu, ULONG capacity, PULONG received, BOOLEAN *ordinaryDone)
{
    volatile USED_RING *used = (USED_RING *)(gpu->dma + Used);
    USHORT end = used->index;
    if (end == gpu->usedIndex) return NO_ERROR;
    if ((USHORT)(end-gpu->usedIndex) > (USHORT)(gpu->index-gpu->usedIndex))
        return control_error(gpu, ERROR_INVALID_DATA);
    VideoPortInterlockedExchange(&gpu->barrier, 0);
    while (gpu->usedIndex != end) {
        volatile USED_ENTRY *entry = &used->ring[gpu->usedIndex % QueueEntries];
        ULONG id = entry->id, length = entry->length, slot;
        RB2K_D7_SUBMISSION *job;
        struct virtio_gpu_ctrl_hdr *reply;
        ++gpu->usedIndex;
        if (!id) {
            if (!ordinaryDone || *ordinaryDone || length > capacity) return control_error(gpu, ERROR_INVALID_DATA);
            *received = length;
            *ordinaryDone = TRUE;
            continue;
        }
        if (id < FrameFirstDesc || id >= QueueEntries || ((id-FrameFirstDesc) & 1))
            return control_error(gpu, ERROR_INVALID_DATA);
        slot = (id-FrameFirstDesc)/2;
        job = &gpu->d7Submitted[slot];
        if (!job->image || !gpu->d7Packets[slot].data) return control_error(gpu, ERROR_INVALID_DATA);
        reply = (struct virtio_gpu_ctrl_hdr *)(gpu->d7Packets[slot].data + FrameReplyOffset);
        if (length != sizeof(*reply) || reply->type != VIRTIO_GPU_RESP_OK_NODATA ||
            (job->fence ? !(reply->flags & VIRTIO_GPU_FLAG_FENCE) || reply->fence_id != job->fence
                        : (reply->flags & VIRTIO_GPU_FLAG_FENCE) != 0)) {
            job->image->frameFailed = TRUE;
            return control_error(gpu, ERROR_INVALID_DATA);
        }
        gpu->d7Completed.clears += job->work.clears;
        gpu->d7Completed.triangles += job->work.triangles;
        gpu->hostFills += job->work.clears;
        job->image = NULL;
        --gpu->d7Flight.pending;
        ++gpu->d7Flight.completed;
    }
    return NO_ERROR;
}

VP_STATUS rb_gpu_poll_scenes(RB2K_GPU *gpu)
{
    if (!gpu->d7Flight.pending) return NO_ERROR;
    return gpu->ready ? collect_control(gpu, 0, NULL, NULL) : ERROR_NOT_READY;
}

static VP_STATUS wait_job(RB2K_GPU *gpu, RB2K_D7_SUBMISSION *job, BOOLEAN throttling)
{
    LONGLONG start, elapsed;
    ULONG polls;
    NUDGE n;
    VP_STATUS status;
    if (!job->image) return NO_ERROR;
    status = collect_control(gpu, 0, NULL, NULL);
    if (status || !job->image) return status;
    start = rb_counter_ticks(NULL);
    nudge_start(&n);
    for (polls = 0; job->image; ++polls) {
        if (polls == 1000000) return control_error(gpu, ERROR_TIMEOUT);
        VideoPortStallExecution(10);
        nudge(gpu, &n, polls+1);
        status = collect_control(gpu, 0, NULL, NULL);
        if (status) return status;
    }
    elapsed = rb_counter_ticks(NULL)-start;
    if (throttling) {
        ++gpu->d7Flight.throttleWaits;
        gpu->d7Flight.throttleTicks += elapsed;
    } else {
        ++gpu->d7Flight.accessWaits;
        gpu->d7Flight.accessTicks += elapsed;
    }
    if (gpu->d7Measure) gpu->d7Measure->fenceTicks += elapsed;
    return NO_ERROR;
}

VP_STATUS rb_gpu_wait_image(RB2K_GPU *gpu, const RB2K_IMAGE *image)
{
    ULONG i;
    if (!gpu->d7Flight.pending) return NO_ERROR;
    if (!gpu->ready) return ERROR_NOT_READY;
    for (i = 0; i < RB2K_D7_FLIGHT_SLOTS; ++i) {
        RB2K_D7_SUBMISSION *job = &gpu->d7Submitted[i];
        if (!image || job->image == image) {
            VP_STATUS status = wait_job(gpu, job, FALSE);
            if (status) return status;
        }
    }
    return NO_ERROR;
}

/* Queue the open frame's command words as one SUBMIT_3D without waiting for
 * it. The host runs control requests in queue order. A fenced packet is
 * answered once the host GPU has finished it, which QEMU notices on a 10 ms
 * timer or a notification; an unfenced one as soon as QEMU has read it, which
 * is all a packet slot needs before it is used again. Only a frame's last
 * packet is fenced. */
VP_STATUS rb_gpu_submit_scene(RB2K_GPU *gpu, const ULONG *words, ULONG count, const RB2K_D3D7_COMPLETED *work,
                              BOOLEAN fenced)
{
    ULONG slot = gpu->d7Next, offset = 0;
    RB2K_DMA_SEGMENT *dma = &gpu->d7Packets[slot];   /* contiguous per slot, not for all slots at once */
    ULONG bytes = sizeof(struct virtio_gpu_cmd_submit) + count*sizeof(ULONG);
    USHORT head = (USHORT)(FrameFirstDesc + 2*slot);
    RB2K_D7_SUBMISSION *job = &gpu->d7Submitted[slot];
    struct virtio_gpu_cmd_submit *packet;
    DESCRIPTOR *desc;
    LONGLONG start;
    VP_STATUS status;
    if (!gpu->ready || !gpu->d7Frame.target) return ERROR_NOT_READY;
    if (!count || count > RB2K_D7_FRAME_WORDS) return ERROR_INVALID_PARAMETER;
    if (fenced && gpu->fence == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
    if (!dma->data) {
        dma->data = VideoPortGetCommonBuffer(gpu->extension, FramePacketBytes, 4096,
                                              &dma->bus, &dma->allocated, TRUE);
        if (!dma->data) return ERROR_NOT_ENOUGH_MEMORY;
        if (dma->allocated < FramePacketBytes) {
            VideoPortFreeCommonBuffer(gpu->extension, dma->allocated, dma->data, dma->bus, TRUE);
            dma->data = NULL;
            return ERROR_NOT_ENOUGH_MEMORY;
        }
    }
    start = rb_counter_ticks(NULL);
    /* This is frame N-2's slot. Leave the immediately preceding scene alone. */
    status = rb_gpu_poll_scenes(gpu);
    if (!status) status = wait_job(gpu, job, TRUE);
    if (status) return status;
    packet = (struct virtio_gpu_cmd_submit *)(dma->data + offset);
    VideoPortZeroMemory(packet, sizeof(*packet));
    packet->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    packet->hdr.ctx_id = gpu->d7Frame.target->id;
    packet->hdr.flags = fenced ? VIRTIO_GPU_FLAG_FENCE : 0;
    packet->hdr.fence_id = fenced ? ++gpu->fence : 0;
    packet->size = count*sizeof(ULONG);
    VideoPortMoveMemory(packet+1, (PVOID)words, count*sizeof(ULONG));
    VideoPortZeroMemory(dma->data + offset + FrameReplyOffset, sizeof(struct virtio_gpu_ctrl_hdr));
    desc = (DESCRIPTOR *)(gpu->dma + Desc);
    desc[head].address.QuadPart = dma->bus.QuadPart + offset;
    desc[head].length = bytes;
    desc[head].flags = DescriptorNext;
    desc[head].next = head+1;
    desc[head+1].address.QuadPart = dma->bus.QuadPart + offset + FrameReplyOffset;
    desc[head+1].length = sizeof(struct virtio_gpu_ctrl_hdr);
    desc[head+1].flags = DescriptorWrite;
    desc[head+1].next = 0;
    job->image = gpu->d7Frame.target;
    job->work = *work;
    job->fence = fenced ? gpu->fence : 0;
    gpu->d7Next = (slot+1) % RB2K_D7_FLIGHT_SLOTS;
    ++gpu->d7Flight.queued;
    if (++gpu->d7Flight.pending > gpu->d7Flight.peak) gpu->d7Flight.peak = gpu->d7Flight.pending;
    publish_control(gpu, head);
    if (gpu->d7Measure) {
        ++gpu->d7Measure->submits;
        if (fenced) ++gpu->d7Measure->fences;
        gpu->d7Measure->bytes += bytes;
        gpu->d7Measure->queueTicks += rb_counter_ticks(NULL)-start;
    }
    return NO_ERROR;
}

VP_STATUS rb_gpu_submit_extents(RB2K_GPU *gpu, const void *request, ULONG bytes,
                                const RB2K_GPU_EXTENT *extents, ULONG count,
                                void *reply, ULONG capacity, PULONG received)
{
    DESCRIPTOR *desc;
    ULONG i, polls;
    BOOLEAN done = FALSE;
    VP_STATUS status;
    LONGLONG start = 0;
    *received = 0;
    if (!gpu->ready) return ERROR_NOT_READY;
    if (!bytes || bytes > PacketBytes || !capacity || capacity > ReplyBytes ||
        count > RB2K_GPU_MAX_EXTENTS || (count && !extents))
        return ERROR_INVALID_PARAMETER;
    /* Large scatter/gather requests may need the descriptors that frames
     * normally occupy. Finish only the slots whose descriptors overlap. */
    for (i = 0; i < RB2K_D7_FLIGHT_SLOTS && 1+count >= FrameFirstDesc+2*i; ++i) {
        status = wait_job(gpu, &gpu->d7Submitted[i], FALSE);
        if (status) return status;
    }
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
    if (!gpu->d7Flight.pending) {
        /* Keep the ordinary single-request path when no scene owns the queue. */
        status = submit_chain(gpu, 0, 0, QueueEntries, &gpu->index, gpu->doorbell, capacity, received);
        gpu->usedIndex = gpu->index;
    } else {
        NUDGE n;
        publish_control(gpu, 0);
        nudge_start(&n);
        for (polls = 0; ; ++polls) {
            status = collect_control(gpu, capacity, received, &done);
            if (status || done) break;
            if (polls == 1000000) { status = control_error(gpu, ERROR_TIMEOUT); break; }
            VideoPortStallExecution(10);
            nudge(gpu, &n, polls+1);
        }
    }
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
    gpu->presentCache = 0;
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
    gpu->presentCache = 0;
    rb_gl_finish(gpu);
    cleanup = stop(gpu);
    if (!cleanup) VideoPortZeroMemory(&gpu->glDesktop, sizeof(gpu->glDesktop));
    /* A reset, including a failed attempt, invalidates all guest handles.
     * The pinned buffers survive an unacknowledged reset until a retry. */
    VideoPortZeroMemory(gpu->images, sizeof(gpu->images));
    gpu->imageCount = 0;
    rb_gpu_texture_forget(gpu);
    VideoPortZeroMemory(&gpu->d7Frame, sizeof(gpu->d7Frame));
    VideoPortZeroMemory(gpu->d7Submitted, sizeof(gpu->d7Submitted));
    gpu->d7Flight.pending = 0;
    for (i = 0; !cleanup && i < RB2K_D7_FLIGHT_SLOTS; ++i) {
        RB2K_DMA_SEGMENT *dma = &gpu->d7Packets[i];
        if (!dma->data) continue;
        VideoPortFreeCommonBuffer(gpu->extension, dma->allocated, dma->data, dma->bus, TRUE);
        dma->data = NULL;
    }
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
