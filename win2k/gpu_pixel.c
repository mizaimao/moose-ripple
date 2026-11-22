/* Virtio commands for the pixel diagnostic, DirectDraw textures and the
 * optional OpenGL desktop. Only rb_gpu_show_desktop selects a scanout. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "rb2kprobe.h"
#include "gpu_transport.h"
#include "gpu_memory.h"
#pragma warning(push)
#pragma warning(disable:4200) /* flexible capset array in the virtio ABI */
#include <linux/virtio_gpu.h>
#include <virgl_hw.h>
#include <virgl_protocol.h>
#pragma warning(pop)

enum {
    PixelSurface = 1, CreateWords = 1 + VIRGL_OBJ_SURFACE_SIZE,
    PipeTexture2D = 2, PipeClearColor0 = 1 << 2
};

/* Each texture has a private context, so its surface object is always 1. */
static const ULONG clear_commands[] = {
    VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE),
    PixelSurface, 0, VIRGL_FORMAT_B8G8R8A8_UNORM, 0, 0,
    VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)),
    1, 0, PixelSurface,
    VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE),
    PipeClearColor0, 0, 0, 0, 0, 0, 0, 0
};

typedef union {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_ctx_create context;
    struct virtio_gpu_resource_create_3d resource;
    struct virtio_gpu_resource_detach_backing detach;
    struct virtio_gpu_ctx_resource ctx_resource;
    struct {
        struct virtio_gpu_cmd_submit submit;
        ULONG words[sizeof(clear_commands) / sizeof(ULONG)];
    } draw;
    struct virtio_gpu_transfer_host_3d transfer;
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_set_scanout scanout;
    struct virtio_gpu_resource_flush flush;
} PIXEL_REQUEST;

typedef struct {
    struct virtio_gpu_resource_attach_backing attach;
    struct virtio_gpu_mem_entry entries[RB2K_MAX_SEGMENTS];
} BACKING_REQUEST;

/* The combined submit may have compiler-inserted tail padding. Only the
 * byte count in its wire header and the command words are transmitted. */
typedef char gpu_header_must_be_24[sizeof(struct virtio_gpu_ctrl_hdr) == 24 ? 1 : -1];
typedef char frame_header_is_32[sizeof(struct virtio_gpu_cmd_submit) == 32 ? 1 : -1];
typedef char gpu_entry_must_be_16[sizeof(struct virtio_gpu_mem_entry) == 16 ? 1 : -1];
typedef char backing_request_fits_queue[sizeof(BACKING_REQUEST) <= 1024 ? 1 : -1];

static void begin_request(PIXEL_REQUEST *request, ULONG type, ULONG context)
{
    VideoPortZeroMemory(request, sizeof(*request));
    request->hdr.type = type;
    request->hdr.ctx_id = context;
}

static BOOLEAN send_request(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                            const void *request, ULONG bytes, ULONG stage)
{
    const struct virtio_gpu_ctrl_hdr *header = request;
    struct virtio_gpu_ctrl_hdr reply;
    ULONG received = 0;
    result->stage = stage;
    result->response = 0;
    result->status = rb_gpu_submit(gpu, request, bytes, &reply, sizeof(reply), &received);
    if (result->status != NO_ERROR) return FALSE;
    if (received != sizeof(reply)) {
        result->status = ERROR_INVALID_DATA;
        return FALSE;
    }
    result->response = reply.type;
    if (reply.type != VIRTIO_GPU_RESP_OK_NODATA) {
        result->status = ERROR_GEN_FAILURE;
        return FALSE;
    }
    if ((header->flags & VIRTIO_GPU_FLAG_FENCE) &&
        (!(reply.flags & VIRTIO_GPU_FLAG_FENCE) || reply.fence_id != header->fence_id)) {
        result->status = ERROR_INVALID_DATA;
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN context_resource(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG context,
                                ULONG resource, BOOLEAN attach, ULONG stage)
{
    PIXEL_REQUEST request;
    begin_request(&request, attach ? VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE :
                  VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, context);
    request.ctx_resource.resource_id = resource;
    return send_request(gpu, result, &request, sizeof(request.ctx_resource), stage);
}

BOOLEAN rb_gpu_create_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                            const RB2K_IMAGE *image, const char *name, ULONG nameBytes, BOOLEAN scanout)
{
    PIXEL_REQUEST request;
    begin_request(&request, VIRTIO_GPU_CMD_CTX_CREATE, image->id);
    request.context.nlen = nameBytes;
    VideoPortMoveMemory(request.context.debug_name, (PVOID)name, nameBytes);
    if (!send_request(gpu, result, &request, sizeof(request.context), RB2K_STAGE_CONTEXT)) return FALSE;

    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, image->id);
    request.resource.resource_id = image->id;
    request.resource.target = PipeTexture2D;
    request.resource.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    request.resource.bind = VIRGL_BIND_RENDER_TARGET | (scanout ? VIRGL_BIND_SCANOUT : 0);
    if (image->sampled) request.resource.bind |= VIRGL_BIND_SAMPLER_VIEW;
    request.resource.width = image->width;
    request.resource.height = image->height;
    request.resource.depth = request.resource.array_size = 1;
    if (!send_request(gpu, result, &request, sizeof(request.resource), RB2K_STAGE_RESOURCE)) return FALSE;

    return context_resource(gpu, result, image->id, image->id, TRUE, RB2K_STAGE_ATTACH);
}

/* IEEE single-precision encoding of channel/255 rounded to the nearest
 * representable value, without using the kernel thread's floating-point state. */
ULONG rb_gpu_channel(ULONG channel)
{
    ULONG exponent = 127;
    if (!channel) return 0;
    while (channel < 255) {
        channel <<= 1;
        --exponent;
    }
    return (exponent << 23) | (((channel - 255) << 23) + 127) / 255;
}

static BOOLEAN fenced(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                      struct virtio_gpu_ctrl_hdr *header, ULONG bytes, ULONG stage)
{
    /* The renderer uses a 32-bit fence timeline. Reset at exhaustion before
     * an old completion could be mistaken for a new fence. */
    if (gpu->fence == 0xffffffffUL) {
        result->status = ERROR_NO_MORE_ITEMS;
        result->stage = stage;
        return FALSE;
    }
    header->flags = VIRTIO_GPU_FLAG_FENCE;
    header->fence_id = ++gpu->fence;
    return send_request(gpu, result, header, bytes, stage);
}

/* Command data is inline in the control descriptor. After an unfenced
 * descriptor is returned the scratch packet is reusable. The fence on the
 * last packet completes all earlier drawing in this image's virgl context. */
static BOOLEAN submit_frame(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, BOOLEAN last)
{
    struct virtio_gpu_cmd_submit *p = (struct virtio_gpu_cmd_submit *)gpu->copyPacket.bytes;
    ULONG bytes = gpu->d7Frame.count * sizeof(ULONG);
    BOOLEAN ok, asynchronous = last && !gpu->d7WaitCurrent;
    if (!bytes) return TRUE;
    VideoPortZeroMemory(p, sizeof(*p));
    p->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    p->hdr.ctx_id = gpu->d7Frame.target->id;
    p->size = bytes;
    VideoPortMoveMemory(p+1, gpu->d7Frame.words, bytes);
    bytes += sizeof(*p);
    if (asynchronous) {
        r->stage = RB2K_STAGE_TRANSFER;
        r->status = rb_gpu_submit_scene(gpu, p, bytes);
        ok = r->status == NO_ERROR;
    } else if (last) ok = fenced(gpu, r, &p->hdr, bytes, RB2K_STAGE_TRANSFER);
    else ok = send_request(gpu, r, p, bytes, RB2K_STAGE_TRANSFER);
    gpu->d7Frame.count = 0;
    if (last || !ok) {
        if (ok && !asynchronous) {
            gpu->d7Completed.clears += gpu->d7Frame.pending.clears;
            gpu->d7Completed.triangles += gpu->d7Frame.pending.triangles;
            gpu->hostFills += gpu->d7Frame.pending.clears;
        } else if (!ok) gpu->d7Frame.target->frameFailed = TRUE;
        gpu->d7Frame.target = NULL;
        gpu->d7Frame.pending.clears = gpu->d7Frame.pending.triangles = 0;
    }
    return ok;
}

BOOLEAN rb_gpu_frame_flush(RB2K_GPU *gpu, RB2K_GPU_RESULT *r)
{
    return submit_frame(gpu, r, TRUE);
}

BOOLEAN rb_gpu_frame_words(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                           const ULONG *words, ULONG count)
{
    const ULONG capacity = sizeof(gpu->d7Frame.words)/sizeof(ULONG);
    if (!count || count > capacity || image->frameFailed) {
        r->status = ERROR_INVALID_DATA;
        r->stage = RB2K_STAGE_TRANSFER;
        return FALSE;
    }
    if (gpu->d7Frame.target != image && !rb_gpu_frame_flush(gpu, r)) return FALSE;
    if (count > capacity-gpu->d7Frame.count && !submit_frame(gpu, r, FALSE)) return FALSE;
    VideoPortMoveMemory(gpu->d7Frame.words + gpu->d7Frame.count, (PVOID)words, count*sizeof(*words));
    gpu->d7Frame.count += count;
    gpu->d7Frame.target = image;
    return TRUE;
}

BOOLEAN rb_gpu_draw_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id,
                          ULONG color, BOOLEAN createSurface, RB2K_IMAGE *frameTarget)
{
    PIXEL_REQUEST request;
    ULONG skip = createSurface ? 0 : CreateWords;
    ULONG bytes = sizeof(clear_commands) - skip * sizeof(ULONG);
    ULONG *words = request.draw.words;
    begin_request(&request, VIRTIO_GPU_CMD_SUBMIT_3D, id);
    request.draw.submit.size = bytes;
    VideoPortMoveMemory(words, (PVOID)(clear_commands + skip), bytes);
    if (createSurface) words[2] = id;
    words[12 - skip] = rb_gpu_channel((color >> 16) & 255);
    words[13 - skip] = rb_gpu_channel((color >> 8) & 255);
    words[14 - skip] = rb_gpu_channel(color & 255);
    words[15 - skip] = rb_gpu_channel(color >> 24);
    if (frameTarget) return rb_gpu_frame_words(gpu, result, frameTarget, words, bytes / sizeof(*words));
    return fenced(gpu, result, &request.hdr, sizeof(request.draw.submit) + bytes, RB2K_STAGE_CLEAR);
}

VP_STATUS rb_gpu_cursor_pixels(RB2K_GPU *gpu)
{
    struct virtio_gpu_resource_create_2d create;
    struct { struct virtio_gpu_resource_attach_backing header; struct virtio_gpu_mem_entry entry; } backing;
    struct virtio_gpu_transfer_to_host_2d transfer;
    RB2K_GPU_RESULT result;
    if (!gpu->ready || !gpu->cursorDoorbell) return ERROR_NOT_SUPPORTED;
    if (!gpu->cursorResource) {
        ULONG id;
        if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
        if (!gpu->nextId) gpu->nextId = 1; /* resource 1 belongs to the single-pixel diagnostic */
        id = ++gpu->nextId;
        VideoPortZeroMemory(&create, sizeof(create));
        create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        create.resource_id = id;
        create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        create.width = create.height = RB2K_CURSOR_EDGE;
        if (!fenced(gpu, &result, &create.hdr, sizeof(create), RB2K_STAGE_RESOURCE)) return result.status;
        VideoPortZeroMemory(&backing, sizeof(backing));
        backing.header.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        backing.header.resource_id = id;
        backing.header.nr_entries = 1;
        backing.entry.addr = gpu->bus.QuadPart + RB2K_CURSOR_DMA_PIXELS;
        backing.entry.length = sizeof(gpu->cursorShape.pixels);
        if (!fenced(gpu, &result, &backing.header.hdr, sizeof(backing), RB2K_STAGE_BACKING)) {
            PIXEL_REQUEST unref;
            RB2K_GPU_RESULT cleanup;
            begin_request(&unref, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
            unref.unref.resource_id = id;
            if (gpu->ready) send_request(gpu, &cleanup, &unref, sizeof(unref.unref), RB2K_STAGE_CLEANUP);
            return result.status;
        }
        gpu->cursorResource = id;
    }
    VideoPortMoveMemory(gpu->dma + RB2K_CURSOR_DMA_PIXELS, gpu->cursorShape.pixels,
                        sizeof(gpu->cursorShape.pixels));
    VideoPortZeroMemory(&transfer, sizeof(transfer));
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r.width = transfer.r.height = RB2K_CURSOR_EDGE;
    transfer.resource_id = gpu->cursorResource;
    if (!fenced(gpu, &result, &transfer.hdr, sizeof(transfer), RB2K_STAGE_TRANSFER)) return result.status;
    ++gpu->cursorStats.transfers;
    return NO_ERROR;
}

/* The DMA pages hold packed rows. The VGA primary may have a wider stride. */
static void copy_rows(RB2K_GPU *gpu, PUCHAR pixels, ULONG pitch, ULONG rowBytes,
                      ULONG rows, BOOLEAN toPrimary)
{
    ULONG row, offset = 0;
    for (row = 0; row < rows; ++row, pixels += pitch) {
        ULONG done = 0;
        while (done < rowBytes) {
            ULONG inPage = offset % RB2K_SEGMENT_BYTES;
            ULONG bytes = RB2K_SEGMENT_BYTES - inPage;
            PUCHAR page = gpu->staging[offset / RB2K_SEGMENT_BYTES].data + inPage;
            if (bytes > rowBytes - done) bytes = rowBytes - done;
            if (toPrimary) VideoPortMoveMemory(pixels + done, page, bytes);
            else VideoPortMoveMemory(page, pixels + done, bytes);
            done += bytes;
            offset += bytes;
        }
    }
}

static BOOLEAN transfer_box(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG resource,
                            ULONG x, ULONG y, ULONG width, ULONG height, PVOID upload, ULONG pitch)
{
    PIXEL_REQUEST request;
    BACKING_REQUEST backing;
    ULONG bytes = width * height * sizeof(ULONG);
    ULONG count = (bytes + RB2K_SEGMENT_BYTES - 1) / RB2K_SEGMENT_BYTES;
    ULONG i, used = sizeof(backing.attach) + count * sizeof(backing.entries[0]);
    result->stage = RB2K_STAGE_BACKING;
    result->status = rb_gpu_reserve_staging(gpu, bytes);
    if (result->status) return FALSE;
    VideoPortZeroMemory(&backing, used);
    backing.attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    backing.attach.resource_id = resource;
    backing.attach.nr_entries = count;
    for (i = 0; i < count; ++i) {
        ULONG offset = i * RB2K_SEGMENT_BYTES;
        ULONG length = bytes - offset;
        if (length > RB2K_SEGMENT_BYTES) length = RB2K_SEGMENT_BYTES;
        backing.entries[i].addr = gpu->staging[i].bus.QuadPart;
        backing.entries[i].length = length;
        /* A failed or short renderer write must never return another surface's bytes. */
        if (!upload) VideoPortZeroMemory(gpu->staging[i].data, length);
        else if (pitch == width * sizeof(ULONG))
            VideoPortMoveMemory(gpu->staging[i].data, (PUCHAR)upload + offset, length);
    }
    if (upload && pitch != width * sizeof(ULONG))
        copy_rows(gpu, upload, pitch, width * sizeof(ULONG), height, FALSE);
    if (!send_request(gpu, result, &backing, used, RB2K_STAGE_BACKING)) return FALSE;
    begin_request(&request, upload ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D :
        VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, resource);
    request.transfer.resource_id = resource;
    request.transfer.box.x = x;
    request.transfer.box.y = y;
    request.transfer.box.w = width;
    request.transfer.box.h = height;
    request.transfer.box.d = 1;
    request.transfer.stride = width * sizeof(ULONG);
    if (!fenced(gpu, result, &request.hdr, sizeof(request.transfer), RB2K_STAGE_TRANSFER)) return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING, 0);
    request.detach.resource_id = resource;
    return send_request(gpu, result, &request, sizeof(request.detach), RB2K_STAGE_BACKING);
}

BOOLEAN rb_gpu_transfer_box(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG resource,
                            ULONG x, ULONG y, ULONG width, ULONG height, PVOID upload)
{
    return transfer_box(gpu, result, resource, x, y, width, height, upload, width * sizeof(ULONG));
}

BOOLEAN rb_gl_record_time(RB2K_GPU *gpu, ULONG operation, LONGLONG start, BOOLEAN ok)
{
    RB2K_GL_TIME *time = &gpu->glTiming.operations[operation];
    time->ticks += rb_counter_ticks(NULL) - start;
    ++time->calls;
    if (!ok) ++time->failed;
    return ok;
}

/* Keep pixels inside the virgl command. Returning an unfenced descriptor
 * means the command stream owns its data and staging may be reused. */
static ULONG inline_write(PULONG words, ULONG id, ULONG x, ULONG y, ULONG w, ULONG h,
                          const ULONG *pixels, ULONG pitchPixels)
{
    ULONG row, count = VIRGL_RESOURCE_IW_DATA_START + w*h;
    VideoPortZeroMemory(words, VIRGL_RESOURCE_IW_DATA_START * sizeof(*words));
    words[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, count-1);
    words[VIRGL_RESOURCE_IW_RES_HANDLE] = id;
    words[VIRGL_RESOURCE_IW_USAGE] = 2; /* PIPE_MAP_WRITE */
    words[VIRGL_RESOURCE_IW_STRIDE] = w*sizeof(*words);
    words[VIRGL_RESOURCE_IW_LAYER_STRIDE] = w*h*sizeof(*words);
    words[VIRGL_RESOURCE_IW_X] = x;
    words[VIRGL_RESOURCE_IW_Y] = y;
    words[VIRGL_RESOURCE_IW_W] = w;
    words[VIRGL_RESOURCE_IW_H] = h;
    words[VIRGL_RESOURCE_IW_D] = 1;
    for (row = 0; row < h; ++row)
        VideoPortMoveMemory(words + VIRGL_RESOURCE_IW_DATA_START + row*w,
                            (PVOID)(pixels + row*pitchPixels), w*sizeof(*words));
    return count;
}

static VP_STATUS submit_inline(RB2K_GPU *gpu, ULONG id, ULONG words)
{
    struct virtio_gpu_cmd_submit submit;
    struct virtio_gpu_ctrl_hdr reply;
    RB2K_GPU_EXTENT data;
    ULONG received;
    VP_STATUS status;
    VideoPortZeroMemory(&submit, sizeof(submit));
    submit.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    submit.hdr.ctx_id = id;
    submit.size = words*sizeof(ULONG);
    data.bus = gpu->staging[0].bus;
    data.length = submit.size;
    status = rb_gpu_submit_extents(gpu, &submit, sizeof(submit), &data, 1,
                                   &reply, sizeof(reply), &received);
    if (!status && (received != sizeof(reply) || reply.type != VIRTIO_GPU_RESP_OK_NODATA))
        status = ERROR_INVALID_DATA;
    return status;
}

VP_STATUS rb_gpu_inline_uploads(RB2K_GPU *gpu, const RB2K_RECT *rects, ULONG count)
{
    const RB2K_PRIMARY *primary = &gpu->primary;
    PULONG words;
    ULONG i, total = 0;
    VP_STATUS status;
    LONGLONG start = rb_counter_ticks(NULL);
    if (!count || count > sizeof(gpu->glDesktop.gdiWrites) / sizeof(*rects) ||
        !primary->pixels || primary->bitsPerPixel != 32 || !primary->height ||
        primary->width != gpu->glDesktop.image.width || primary->height != gpu->glDesktop.image.height ||
        primary->pitchPixels < primary->width ||
        primary->pitchPixels > primary->bytes / sizeof(ULONG) / primary->height)
        return ERROR_INVALID_PARAMETER;
    status = rb_gpu_reserve_staging(gpu, RB2K_SEGMENT_BYTES);
    if (status) return status;
    words = (PULONG)gpu->staging[0].data;
    for (i = 0; i < count; ++i) {
        const RB2K_RECT *r = &rects[i];
        ULONG w = r->right - r->left, h = r->bottom - r->top;
        ULONG size = VIRGL_RESOURCE_IW_DATA_START + w * h;
        if (!w || !h || w > 64 || h > 64 || r->left >= r->right || r->top >= r->bottom ||
            r->right > primary->width || r->bottom > primary->height ||
            size > RB2K_SEGMENT_BYTES / sizeof(*words) - total) return ERROR_INVALID_PARAMETER;
        total += inline_write(words+total, gpu->glDesktop.image.id, r->left, r->top, w, h,
                              primary->pixels + r->top*primary->pitchPixels + r->left, primary->pitchPixels);
    }
    status = submit_inline(gpu, gpu->glDesktop.image.id, total);
    rb_gl_record_time(gpu, RB2K_GL_UPLOAD, start, (BOOLEAN)!status);
    if (!status) {
        gpu->glTiming.operations[RB2K_GL_UPLOAD].calls += count - 1;
        gpu->glStats.uploads += count;
        ++gpu->glProfile.inlineBatches;
        gpu->glProfile.inlineRects += count;
    }
    return status;
}

BOOLEAN rb_gpu_primary_transfer(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                                const RB2K_RECT *rect, BOOLEAN readback)
{
    ULONG width = rect->right - rect->left, height = rect->bottom - rect->top;
    ULONG pitch = gpu->primary.pitchPixels * sizeof(ULONG);
    ULONG operation = RB2K_GL_UPLOAD;
    LONGLONG start;
    PULONG first;
    if (!gpu->primary.pixels || gpu->primary.bitsPerPixel != 32 ||
        gpu->primary.width != gpu->glDesktop.image.width || gpu->primary.height != gpu->glDesktop.image.height ||
        !width || !height || rect->right > gpu->primary.width || rect->bottom > gpu->primary.height ||
        rect->left >= rect->right || rect->top >= rect->bottom ||
        gpu->primary.pitchPixels < gpu->primary.width ||
        gpu->primary.pitchPixels > gpu->primary.bytes / sizeof(ULONG) / gpu->primary.height) {
        result->status = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    if (readback) {
        const RB2K_RECT *owned = &gpu->glDesktop.gpuPixels;
        operation = rect->left == owned->left && rect->top == owned->top &&
            rect->right == owned->right && rect->bottom == owned->bottom ?
            RB2K_GL_READ_FRAME : RB2K_GL_READ_PART;
    }
    first = gpu->primary.pixels + rect->top * gpu->primary.pitchPixels + rect->left;
    start = rb_counter_ticks(NULL);
    if (!transfer_box(gpu, result, gpu->glDesktop.image.id, rect->left, rect->top,
                      width, height, readback ? NULL : first, pitch))
        return rb_gl_record_time(gpu, operation, start, FALSE);
    if (readback) copy_rows(gpu, (PUCHAR)first, pitch, width * sizeof(ULONG), height, TRUE);
    if (readback) {
        ++gpu->glStats.readbacks;
        gpu->glStats.readbackPixels += width * height;
        gpu->glStats.lastReadWidth = width;
        gpu->glStats.lastReadHeight = height;
        if (operation == RB2K_GL_READ_FRAME) ++gpu->glStats.fullFrameReads;
    }
    else ++gpu->glStats.uploads;
    return rb_gl_record_time(gpu, operation, start, TRUE);
}

BOOLEAN rb_gpu_show_desktop(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, BOOLEAN select)
{
    PIXEL_REQUEST request;
    RB2K_IMAGE *image = &gpu->glDesktop.image;
    LONGLONG start = rb_counter_ticks(NULL);
    BOOLEAN ok;
    if (select) {
        begin_request(&request, VIRTIO_GPU_CMD_SET_SCANOUT, 0);
        request.scanout.resource_id = image->id;
        request.scanout.r.width = image->width;
        request.scanout.r.height = image->height;
        if (!send_request(gpu, result, &request, sizeof(request.scanout), RB2K_STAGE_TRANSFER))
            return rb_gl_record_time(gpu, RB2K_GL_FLUSH, start, FALSE);
        gpu->glDesktop.visible = TRUE;
    }
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_FLUSH, 0);
    request.flush.resource_id = image->id;
    request.flush.r.width = image->width;
    request.flush.r.height = image->height;
    ok = send_request(gpu, result, &request, sizeof(request.flush), RB2K_STAGE_TRANSFER);
    return rb_gl_record_time(gpu, RB2K_GL_FLUSH, start, ok);
}

BOOLEAN rb_gpu_transfer_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                              const RB2K_IMAGE *image, PVOID pixels, BOOLEAN readback)
{
    ULONG bytes = image->width * image->height * sizeof(ULONG);
    ULONG offset;
    if (image->sampled && !readback && image->width <= 64 && image->height <= 64) {
        ULONG count;
        result->stage = RB2K_STAGE_BACKING;
        result->status = rb_gpu_reserve_staging(gpu, bytes + VIRGL_RESOURCE_IW_DATA_START*sizeof(ULONG));
        if (result->status) return FALSE;
        count = inline_write((PULONG)gpu->staging[0].data, image->id, 0, 0, image->width,
                             image->height, pixels, image->width);
        result->stage = RB2K_STAGE_TRANSFER;
        result->status = submit_inline(gpu, image->id, count);
        return result->status == NO_ERROR;
    }
    if (!rb_gpu_transfer_box(gpu, result, image->id, 0, 0, image->width, image->height,
                             readback ? NULL : pixels)) return FALSE;
    if (readback) {
        for (offset = 0; offset < bytes; offset += RB2K_SEGMENT_BYTES) {
            ULONG length = bytes - offset;
            if (length > RB2K_SEGMENT_BYTES) length = RB2K_SEGMENT_BYTES;
            VideoPortMoveMemory((PUCHAR)pixels + offset,
                                gpu->staging[offset / RB2K_SEGMENT_BYTES].data, length);
        }
    }
    return TRUE;
}

enum { CopyWords = 1 + VIRGL_CMD_RESOURCE_COPY_REGION_SIZE };
typedef struct {
    struct virtio_gpu_cmd_submit submit;
    ULONG command[RB2K_GL_MAX_RECTS][CopyWords];
} COPY_PACKET;
typedef char all_copies_fit_inline[sizeof(COPY_PACKET) <= RB2K_INLINE_PACKET_BYTES ? 1 : -1];

/* All rectangles have already been clipped against both resources. The
 * host sees a single ordered submission and the source stays attached until
 * every copy in that submission completes. */
BOOLEAN rb_gpu_copy_rects(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG destination,
                          ULONG source, const RB2K_RECT *rects, ULONG count,
                          LONG originX, LONG originY, RB2K_GL_PROFILE *profile)
{
    COPY_PACKET *p = (COPY_PACKET *)gpu->copyPacket.bytes;
    RB2K_GPU_RESULT cleanup;
    ULONG i, bytes;
    LONGLONG start = 0, attached = 0, completed = 0;
    BOOLEAN ok;
    if (!count || count > RB2K_GL_MAX_RECTS) { result->status = ERROR_INVALID_PARAMETER; return FALSE; }
    bytes = count * sizeof(p->command[0]);
    VideoPortZeroMemory(p, sizeof(p->submit) + bytes);
    p->submit.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    p->submit.hdr.ctx_id = destination;
    p->submit.size = bytes;
    for (i = 0; i < count; ++i) {
        const RB2K_RECT *r = &rects[i];
        ULONG *w = p->command[i];
        w[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
        w[VIRGL_CMD_RCR_DST_RES_HANDLE] = destination;
        w[VIRGL_CMD_RCR_DST_X] = r->left;
        w[VIRGL_CMD_RCR_DST_Y] = r->top;
        w[VIRGL_CMD_RCR_SRC_RES_HANDLE] = source;
        w[VIRGL_CMD_RCR_SRC_X] = (ULONG)((__int64)r->left - originX);
        w[VIRGL_CMD_RCR_SRC_Y] = (ULONG)((__int64)r->top - originY);
        w[VIRGL_CMD_RCR_SRC_W] = r->right - r->left;
        w[VIRGL_CMD_RCR_SRC_H] = r->bottom - r->top;
        w[VIRGL_CMD_RCR_SRC_D] = 1;
    }
    if (profile) start = rb_counter_ticks(NULL);
    if (!context_resource(gpu, result, destination, source, TRUE, RB2K_STAGE_ATTACH)) return FALSE;
    if (profile) attached = rb_counter_ticks(NULL);
    ok = fenced(gpu, result, &p->submit.hdr, sizeof(p->submit) + bytes, RB2K_STAGE_TRANSFER);
    if (profile) completed = rb_counter_ticks(NULL);
    if (!ok) {
        if (gpu->ready) context_resource(gpu, &cleanup, destination, source, FALSE, RB2K_STAGE_CLEANUP);
        return FALSE;
    }
    ok = context_resource(gpu, result, destination, source, FALSE, RB2K_STAGE_CLEANUP);
    if (ok && profile) {
        profile->rectangles += count;
        ++profile->fences;
        profile->attachTicks += attached - start;
        profile->fenceTicks += completed - attached;
        profile->detachTicks += rb_counter_ticks(NULL) - completed;
    }
    return ok;
}

BOOLEAN rb_gpu_destroy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id)
{
    PIXEL_REQUEST request;
    if (!context_resource(gpu, result, id, id, FALSE, RB2K_STAGE_CLEANUP)) return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_CTX_DESTROY, id);
    if (!send_request(gpu, result, &request, sizeof(request.hdr), RB2K_STAGE_CLEANUP)) return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
    request.unref.resource_id = id;
    return send_request(gpu, result, &request, sizeof(request.unref), RB2K_STAGE_CLEANUP);
}

void rb_gpu_pixel(RB2K_GPU *gpu, RB2K_GPU_RESULT *result)
{
    static const char name[] = "rb2k-pixel";
    const RB2K_IMAGE image = {1, 1, 1, FALSE, FALSE, FALSE, 0};
    result->expected = 0xffff8000UL;
    if (!rb_gpu_create_image(gpu, result, &image, name, sizeof(name) - 1, FALSE)) return;
    result->observed = ~result->expected;
    if (!rb_gpu_draw_image(gpu, result, image.id, result->expected, TRUE, NULL) ||
        !rb_gpu_transfer_image(gpu, result, &image, &result->observed, TRUE)) return;
    if (!rb_gpu_destroy_image(gpu, result, image.id)) return;
    if (result->observed != result->expected) {
        result->stage = RB2K_STAGE_COMPARE;
        result->status = ERROR_INVALID_DATA;
        return;
    }
    result->stage = RB2K_STAGE_DONE;
}
