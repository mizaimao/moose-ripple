/* The miniport side of the RIPPLEVD interface (rb2krvd.h): host contexts,
 * resources, command submission and transfers for Mesa's virgl winsys,
 * as Windows 98's ripplevd.vxd provides them. Every request is one
 * synchronous exchange with the host.
 *
 * A buffer is a host resource with guest pages attached for its whole life
 * (gpu_memory.c): Mesa's command stream moves data between the two on its
 * own, so the pages cannot be lent only for explicit transfers. The pages
 * are mapped into the process that made the buffer, which alone may use or
 * destroy it. Contexts and buffers of a process that has ended are released
 * at the next context or buffer creation.
 *
 * IDs come from the same counter as the DirectDraw surfaces. While a context
 * is open the transport stays up: DirectDraw no longer resets the device. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include "gpu_memory.h"
#include "rb2krvd.h"
#pragma warning(push)
#pragma warning(disable:4200) /* flexible capset array in the virtio ABI */
#include <linux/virtio_gpu.h>
#pragma warning(pop)

enum { MaxContexts = 64, MaxBuffers = 4096 };

typedef struct {
    ULONG id;
    PVOID owner;
} RVD_CONTEXT;

typedef struct {
    ULONG handle, context;
    RB2K_PAGES pages;
} RVD_BUFFER;

typedef union {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_get_capset_info capsetInfo;
    struct virtio_gpu_get_capset capset;
    struct virtio_gpu_ctx_create context;
    struct virtio_gpu_ctx_destroy destroy;
    struct virtio_gpu_resource_create_3d resource;
    struct virtio_gpu_resource_attach_backing attach;
    struct virtio_gpu_ctx_resource ctxResource;
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_cmd_submit submit;
    struct virtio_gpu_transfer_host_3d transfer;
} RVD_COMMAND;

static RVD_CONTEXT contexts[MaxContexts];
static RVD_BUFFER buffers[MaxBuffers];

typedef char rvd_staging_matches[RB2K_RVD_STAGING_SEGMENTS == RB2K_RVD_SEGMENTS &&
                                 RB2K_RVD_SEGMENT_BYTES == RB2K_SEGMENT_BYTES ? 1 : -1];
typedef char rvd_submit_fits_extents[RB2K_RVD_MAX_SUBMIT / RB2K_RVD_SEGMENT_BYTES <=
                                     RB2K_GPU_MAX_EXTENTS ? 1 : -1];
/* The worst case: one run per page of the largest buffer, in staging. */
typedef char rvd_runs_fit_extents[(RB2K_RVD_MAX_BUFFER / 4096) * sizeof(RB2K_PAGE_RUN) <=
                                  RB2K_GPU_MAX_EXTENTS * RB2K_RVD_SEGMENT_BYTES ? 1 : -1];

static void begin(RVD_COMMAND *c, ULONG type, ULONG context)
{
    VideoPortZeroMemory(c, sizeof(*c));
    c->hdr.type = type;
    c->hdr.ctx_id = context;
}

/* One exchange. reply may be NULL when only the header comes back. */
static VP_STATUS send(RB2K_GPU *gpu, RVD_COMMAND *c, ULONG bytes, const RB2K_GPU_EXTENT *extents,
                      ULONG count, BOOLEAN fence, void *reply, ULONG capacity, ULONG expect)
{
    struct virtio_gpu_ctrl_hdr local, *answer;
    ULONG received = 0;
    VP_STATUS status;
    if (fence) {
        if (gpu->fence == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
        c->hdr.flags |= VIRTIO_GPU_FLAG_FENCE;
        c->hdr.fence_id = ++gpu->fence;
    }
    if (!reply) {
        reply = &local;
        capacity = sizeof(local);
    }
    status = rb_gpu_submit_extents(gpu, c, bytes, extents, count, reply, capacity, &received);
    if (status) return status;
    answer = reply;
    if (received < sizeof(*answer)) return ERROR_INVALID_DATA;
    if (answer->type != expect) return ERROR_GEN_FAILURE;
    if (fence && (!(answer->flags & VIRTIO_GPU_FLAG_FENCE) || answer->fence_id != c->hdr.fence_id))
        return ERROR_INVALID_DATA;
    return NO_ERROR;
}

static VP_STATUS simple(RB2K_GPU *gpu, RVD_COMMAND *c, ULONG bytes)
{
    return send(gpu, c, bytes, NULL, 0, FALSE, NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
}

/* Staging segments from the start, covering bytes; 0 if they do not fit. */
static ULONG extents(RB2K_GPU *gpu, ULONG bytes, RB2K_GPU_EXTENT *out, ULONG max)
{
    ULONG i, count = (bytes + RB2K_SEGMENT_BYTES - 1) / RB2K_SEGMENT_BYTES;
    if (!bytes || count > max) return 0;
    for (i = 0; i < count; ++i) {
        if (!gpu->rvdStaging[i].data) return 0;
        out[i].bus = gpu->rvdStaging[i].bus;
        out[i].length = bytes - i * RB2K_SEGMENT_BYTES;
        if (out[i].length > RB2K_SEGMENT_BYTES) out[i].length = RB2K_SEGMENT_BYTES;
    }
    return count;
}

static VP_STATUS start(RB2K_GPU *gpu)
{
    RB2K_GPU_RESULT result;
    VP_STATUS status;
    if (gpu->ready) return NO_ERROR;
    VideoPortZeroMemory(&result, sizeof(result));
    status = rb_gpu_start(gpu, &result);
    if (status) rb_gpu_stop(gpu);
    return status;
}

static void stop_when_idle(RB2K_GPU *gpu)
{
    if (gpu->ready && !gpu->rvdContexts && !gpu->imageCount) rb_gpu_stop(gpu);
}

/* The host lets go of the resource and its pages before they are freed. */
static void release_buffer(RB2K_GPU *gpu, RVD_BUFFER *b)
{
    RVD_COMMAND c;
    if (gpu->ready) {
        begin(&c, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, b->context);
        c.ctxResource.resource_id = b->handle;
        simple(gpu, &c, sizeof(c.ctxResource));
        begin(&c, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
        c.unref.resource_id = b->handle;
        simple(gpu, &c, sizeof(c.unref));
    }
    rb_pages_free(&b->pages);
    VideoPortZeroMemory(b, sizeof(*b));
}

static void release_context(RB2K_GPU *gpu, RVD_CONTEXT *x)
{
    RVD_COMMAND c;
    ULONG i;
    for (i = 0; i < MaxBuffers; ++i)
        if (buffers[i].handle && buffers[i].context == x->id) release_buffer(gpu, &buffers[i]);
    if (gpu->ready) {
        begin(&c, VIRTIO_GPU_CMD_CTX_DESTROY, x->id);
        simple(gpu, &c, sizeof(c.destroy));
    }
    rb_process_deref(x->owner);
    VideoPortZeroMemory(x, sizeof(*x));
    if (gpu->rvdContexts) --gpu->rvdContexts;
}

/* Contexts of processes that ended without destroying them. */
static void sweep(RB2K_GPU *gpu)
{
    ULONG i;
    for (i = 0; i < MaxContexts; ++i)
        if (contexts[i].id && rb_process_gone(contexts[i].owner)) release_context(gpu, &contexts[i]);
}

/* For other users of the transport, with gpu->busy held: Mesa does not always
 * destroy its context when a program ends. */
void rb_gpu_rvd_sweep(RB2K_GPU *gpu)
{
    sweep(gpu);
}

static RVD_CONTEXT *own_context(ULONG id)
{
    ULONG i;
    if (id) {
        for (i = 0; i < MaxContexts; ++i)
            if (contexts[i].id == id && rb_process_current(contexts[i].owner)) return &contexts[i];
    }
    return NULL;
}

static RVD_BUFFER *own_buffer(ULONG handle)
{
    ULONG i;
    if (handle) {
        for (i = 0; i < MaxBuffers; ++i)
            if (buffers[i].handle == handle && rb_process_current(buffers[i].pages.owner))
                return &buffers[i];
    }
    return NULL;
}

static VP_STATUS staging(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    RB2K_RVD_STAGING_INFO *info = packet->OutputBuffer;
    ULONG i;
    if (packet->OutputBufferLength < sizeof(*info)) return ERROR_INSUFFICIENT_BUFFER;
    for (i = 0; i < RB2K_RVD_SEGMENTS; ++i) {
        RB2K_DMA_SEGMENT *s = &gpu->rvdStaging[i];
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
        info->data[i] = s->data;
    }
    info->count = RB2K_RVD_SEGMENTS;
    info->segmentBytes = RB2K_SEGMENT_BYTES;
    packet->StatusBlock->Information = sizeof(*info);
    return NO_ERROR;
}

static VP_STATUS info(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    struct rvd_info *out = packet->OutputBuffer;
    struct virtio_gpu_resp_capset_info answer;
    RVD_COMMAND c;
    VP_STATUS status;
    ULONG i;
    if (packet->OutputBufferLength < sizeof(*out)) return ERROR_INSUFFICIENT_BUFFER;
    status = start(gpu);
    if (status) return status;
    VideoPortZeroMemory(out, sizeof(*out));
    out->version = RVD_INTERFACE_VERSION;
    out->virgl = 1;
    out->max_submit = RB2K_RVD_MAX_SUBMIT;
    out->max_buffer = RB2K_RVD_MAX_BUFFER;
    for (i = 0; i < RVD_MAX_CAPS; ++i) {
        begin(&c, VIRTIO_GPU_CMD_GET_CAPSET_INFO, 0);
        c.capsetInfo.capset_index = i;
        status = send(gpu, &c, sizeof(c.capsetInfo), NULL, 0, FALSE, &answer, sizeof(answer),
                      VIRTIO_GPU_RESP_OK_CAPSET_INFO);
        /* QEMU answers an index past its last capability set with ID 0. */
        if (status || !answer.capset_id) break;
        out->caps_ids[i] = answer.capset_id;
        out->caps_versions[i] = answer.capset_max_version;
        out->caps_sizes[i] = answer.capset_max_size;
        out->num_caps = i + 1;
    }
    if (!out->num_caps) return status ? status : ERROR_NOT_SUPPORTED;
    packet->StatusBlock->Information = sizeof(*out);
    return NO_ERROR;
}

static VP_STATUS caps(RB2K_GPU *gpu, const RB2K_RVD_REQUEST *r, PVIDEO_REQUEST_PACKET packet)
{
    struct virtio_gpu_ctrl_hdr *answer;
    RVD_COMMAND c;
    VP_STATUS status;
    ULONG want = r->bytes;
    if (!gpu->ready || !gpu->rvdStaging[0].data) return ERROR_NOT_READY;
    if (!want || want > 4096 - sizeof(*answer) || packet->OutputBufferLength < want)
        return ERROR_INVALID_PARAMETER;
    answer = (struct virtio_gpu_ctrl_hdr *)gpu->rvdStaging[0].data;
    begin(&c, VIRTIO_GPU_CMD_GET_CAPSET, 0);
    c.capset.capset_id = r->a;
    c.capset.capset_version = r->b;
    status = send(gpu, &c, sizeof(c.capset), NULL, 0, FALSE, answer, sizeof(*answer) + want,
                  VIRTIO_GPU_RESP_OK_CAPSET);
    if (status) return status;
    VideoPortMoveMemory(packet->OutputBuffer, answer + 1, want);
    packet->StatusBlock->Information = want;
    return NO_ERROR;
}

static VP_STATUS context_create(RB2K_GPU *gpu, const RB2K_RVD_REQUEST *r,
                                PVIDEO_REQUEST_PACKET packet)
{
    RVD_COMMAND c;
    VP_STATUS status;
    ULONG id, slot;
    if (packet->OutputBufferLength < sizeof(ULONG) || r->bytes > RB2K_RVD_MAX_NAME)
        return ERROR_INVALID_PARAMETER;
    sweep(gpu);
    for (slot = 0; slot < MaxContexts; ++slot) if (!contexts[slot].id) break;
    if (slot == MaxContexts) return ERROR_NOT_ENOUGH_MEMORY;
    status = start(gpu);
    if (status) return status;
    if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
    id = ++gpu->nextId;
    begin(&c, VIRTIO_GPU_CMD_CTX_CREATE, id);
    c.context.nlen = r->bytes;
    VideoPortMoveMemory(c.context.debug_name, (PVOID)r->u.name, r->bytes);
    status = simple(gpu, &c, sizeof(c.context));
    if (status) {
        stop_when_idle(gpu);
        return status;
    }
    contexts[slot].id = id;
    contexts[slot].owner = rb_process_ref();
    ++gpu->rvdContexts;
    *(PULONG)packet->OutputBuffer = id;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

static VP_STATUS context_destroy(RB2K_GPU *gpu, const RB2K_RVD_REQUEST *r)
{
    RVD_CONTEXT *x = own_context(r->context);
    if (!x) return ERROR_INVALID_PARAMETER;
    release_context(gpu, x);
    stop_when_idle(gpu);
    return NO_ERROR;
}

static VP_STATUS buffer_create(RB2K_GPU *gpu, const RB2K_RVD_REQUEST *r,
                               PVIDEO_REQUEST_PACKET packet)
{
    const struct rvd_buffer_create *b = &r->u.create;
    struct rvd_buffer *out = packet->OutputBuffer;
    RB2K_GPU_EXTENT data[RB2K_GPU_MAX_EXTENTS];
    RVD_BUFFER *buffer;
    RVD_COMMAND c;
    VP_STATUS status;
    ULONG slot, runs, count, bytes = (b->size + 4095) & ~4095UL;
    if (packet->OutputBufferLength < sizeof(*out) || !bytes || b->size > bytes ||
        bytes > RB2K_RVD_MAX_BUFFER)
        return ERROR_INVALID_PARAMETER;
    sweep(gpu);
    if (!gpu->ready || !gpu->rvdStaging[0].data) return ERROR_NOT_READY;
    if (!own_context(b->context)) return ERROR_INVALID_PARAMETER;
    for (slot = 0; slot < MaxBuffers; ++slot) if (!buffers[slot].handle) break;
    if (slot == MaxBuffers || gpu->nextId == 0xffffffffUL) return ERROR_NOT_ENOUGH_MEMORY;
    buffer = &buffers[slot];
    if (!rb_pages_alloc(&buffer->pages, bytes)) return ERROR_NOT_ENOUGH_MEMORY;
    buffer->handle = ++gpu->nextId;
    buffer->context = b->context;

    begin(&c, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, b->context);
    c.resource.resource_id = buffer->handle;
    c.resource.target = b->target;
    c.resource.format = b->format;
    c.resource.bind = b->bind;
    c.resource.width = b->width;
    c.resource.height = b->height;
    c.resource.depth = b->depth;
    c.resource.array_size = b->array_size;
    c.resource.last_level = b->last_level;
    c.resource.nr_samples = b->nr_samples;
    c.resource.flags = b->flags;
    status = simple(gpu, &c, sizeof(c.resource));
    if (status) {
        rb_pages_free(&buffer->pages);
        VideoPortZeroMemory(buffer, sizeof(*buffer));
        return status;
    }
    /* The page list goes in staging, after the request. */
    runs = rb_pages_runs(&buffer->pages, (RB2K_PAGE_RUN *)gpu->rvdStaging[0].data,
                         RB2K_SEGMENT_BYTES / sizeof(RB2K_PAGE_RUN));
    count = runs ? extents(gpu, runs * sizeof(RB2K_PAGE_RUN), data, RB2K_GPU_MAX_EXTENTS) : 0;
    if (!count) {
        status = ERROR_NOT_ENOUGH_MEMORY;
    } else {
        begin(&c, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, b->context);
        c.attach.resource_id = buffer->handle;
        c.attach.nr_entries = runs;
        status = send(gpu, &c, sizeof(c.attach), data, count, FALSE, NULL, 0,
                      VIRTIO_GPU_RESP_OK_NODATA);
    }
    if (!status) {
        begin(&c, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, b->context);
        c.ctxResource.resource_id = buffer->handle;
        status = simple(gpu, &c, sizeof(c.ctxResource));
    }
    if (status) {
        release_buffer(gpu, buffer);
        return status;
    }
    out->handle = buffer->handle;
    out->size = bytes;
    out->address = buffer->pages.user;
    packet->StatusBlock->Information = sizeof(*out);
    return NO_ERROR;
}

static VP_STATUS buffer_destroy(RB2K_GPU *gpu, const RB2K_RVD_REQUEST *r)
{
    RVD_BUFFER *b = own_buffer(r->a);
    if (!b) return ERROR_INVALID_PARAMETER;
    release_buffer(gpu, b);
    return NO_ERROR;
}

/* What programs hold now, after freeing what ended programs left. */
static VP_STATUS held(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    PULONG out = packet->OutputBuffer;
    ULONG i;
    if (packet->OutputBufferLength < 3 * sizeof(ULONG)) return ERROR_INSUFFICIENT_BUFFER;
    sweep(gpu);
    out[0] = out[1] = out[2] = 0;
    for (i = 0; i < MaxContexts; ++i) if (contexts[i].id) ++out[0];
    for (i = 0; i < MaxBuffers; ++i) {
        if (!buffers[i].handle) continue;
        ++out[1];
        out[2] += buffers[i].pages.bytes / 4096;
    }
    packet->StatusBlock->Information = 3 * sizeof(ULONG);
    return NO_ERROR;
}

static VP_STATUS submit(RB2K_GPU *gpu, const RB2K_RVD_REQUEST *r)
{
    RB2K_GPU_EXTENT data[RB2K_GPU_MAX_EXTENTS];
    RVD_COMMAND c;
    ULONG count;
    if (!gpu->ready) return ERROR_NOT_READY;
    if (!own_context(r->context) || r->bytes > RB2K_RVD_MAX_SUBMIT || (r->bytes & 3))
        return ERROR_INVALID_PARAMETER;
    if (!r->bytes) return NO_ERROR;
    count = extents(gpu, r->bytes, data, RB2K_GPU_MAX_EXTENTS);
    if (!count) return ERROR_INVALID_PARAMETER;
    begin(&c, VIRTIO_GPU_CMD_SUBMIT_3D, r->context);
    c.submit.size = r->bytes;
    return send(gpu, &c, sizeof(c.submit), data, count, FALSE, NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
}

/* Between the resource and its attached pages, at the winsys's offset; the
 * host works out the layout (stride and layer stride 0), as for the DRM
 * winsys's resources without a blob. */
static VP_STATUS transfer(RB2K_GPU *gpu, const RB2K_RVD_REQUEST *r)
{
    const struct rvd_transfer *t = &r->u.transfer;
    RVD_BUFFER *b = own_buffer(t->buffer);
    RVD_COMMAND c;
    BOOLEAN from = t->direction == RVD_FROM_HOST;
    if (!gpu->ready) return ERROR_NOT_READY;
    if (!b || !own_context(t->context) || t->offset >= b->pages.bytes ||
        (t->direction != RVD_TO_HOST && !from))
        return ERROR_INVALID_PARAMETER;
    begin(&c, from ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
          t->context);
    c.transfer.resource_id = t->buffer;
    c.transfer.level = t->level;
    c.transfer.offset = t->offset;
    c.transfer.stride = t->stride;
    c.transfer.layer_stride = t->layer_stride;
    c.transfer.box.x = t->x;
    c.transfer.box.y = t->y;
    c.transfer.box.z = t->z;
    c.transfer.box.w = t->width;
    c.transfer.box.h = t->height;
    c.transfer.box.d = t->depth;
    /* A readback is complete before the program reads the pages. */
    return send(gpu, &c, sizeof(c.transfer), NULL, 0, from, NULL, 0, VIRTIO_GPU_RESP_OK_NODATA);
}

VP_STATUS rb_gpu_rvd_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    const RB2K_RVD_REQUEST *r = packet->InputBuffer;
    VP_STATUS status;
    if (packet->InputBufferLength != sizeof(*r)) return ERROR_INVALID_PARAMETER;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    switch (r->op) {
    case RB2K_RVD_OP_STAGING: status = staging(gpu, packet); break;
    case RB2K_RVD_OP_INFO: status = info(gpu, packet); break;
    case RB2K_RVD_OP_CAPS: status = caps(gpu, r, packet); break;
    case RB2K_RVD_OP_CONTEXT_CREATE: status = context_create(gpu, r, packet); break;
    case RB2K_RVD_OP_CONTEXT_DESTROY: status = context_destroy(gpu, r); break;
    case RB2K_RVD_OP_BUFFER_CREATE: status = buffer_create(gpu, r, packet); break;
    case RB2K_RVD_OP_BUFFER_DESTROY: status = buffer_destroy(gpu, r); break;
    case RB2K_RVD_OP_SUBMIT: status = submit(gpu, r); break;
    case RB2K_RVD_OP_TRANSFER: status = transfer(gpu, r); break;
    case RB2K_RVD_OP_HELD: status = held(gpu, packet); break;
    default: status = ERROR_INVALID_FUNCTION; break;
    }
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status;
}
