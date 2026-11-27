/* Virtio commands for the pixel diagnostic, DirectDraw textures and the
 * optional OpenGL desktop. Only rb_gpu_show_image selects a scanout. */
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
    PipeTexture2D = 2, PipeTexture3D = 3, PipeTextureCube = 4, PipeClearColor0 = 1 << 2
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
typedef char backing_request_fits_queue[sizeof(BACKING_REQUEST) <= RB2K_INLINE_PACKET_BYTES ? 1 : -1];

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
    RB2K_GPU_RESULT cleanup;
    begin_request(&request, VIRTIO_GPU_CMD_CTX_CREATE, image->id);
    request.context.nlen = nameBytes;
    VideoPortMoveMemory(request.context.debug_name, (PVOID)name, nameBytes);
    if (!send_request(gpu, result, &request, sizeof(request.context), RB2K_STAGE_CONTEXT)) return FALSE;

    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, image->id);
    request.resource.resource_id = image->id;
    request.resource.target = PipeTexture2D;
    request.resource.format = image->stencil8 ? VIRGL_FORMAT_S8_UINT_Z24_UNORM :
                              image->depth16 ? VIRGL_FORMAT_Z16_UNORM : VIRGL_FORMAT_B8G8R8A8_UNORM;
    /* Colour images may be render target textures, sampled by other targets. */
    request.resource.bind = image->depth16 ? VIRGL_BIND_DEPTH_STENCIL : VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW;
    if (scanout) request.resource.bind |= VIRGL_BIND_SCANOUT;
    request.resource.width = image->width;
    request.resource.height = image->height;
    request.resource.depth = request.resource.array_size = 1;
    if (send_request(gpu, result, &request, sizeof(request.resource), RB2K_STAGE_RESOURCE)) {
        if (context_resource(gpu, result, image->id, image->id, TRUE, RB2K_STAGE_ATTACH)) return TRUE;
        if (gpu->ready) {
            begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
            request.unref.resource_id = image->id;
            if (!send_request(gpu, &cleanup, &request, sizeof(request.unref), RB2K_STAGE_CLEANUP))
                gpu->ready = FALSE;
        }
    }
    /* A renderer may reject a format after its private context is created.
     * Undo the successful steps and preserve the caller's original error. */
    if (gpu->ready) {
        begin_request(&request, VIRTIO_GPU_CMD_CTX_DESTROY, image->id);
        if (!send_request(gpu, &cleanup, &request, sizeof(request.hdr), RB2K_STAGE_CLEANUP))
            gpu->ready = FALSE;
    }
    return FALSE;
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

/* Every packet of a frame is queued without waiting; the host runs them
 * in order. The last one carries the frame's work counts, and with
 * RippleD3D7WaitCurrent set the frame is waited for. */
static BOOLEAN submit_frame(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, BOOLEAN last)
{
    static const RB2K_D3D7_COMPLETED none;
    RB2K_IMAGE *target = gpu->d7Frame.target;
    BOOLEAN ok;
    if (!gpu->d7Frame.count) return TRUE;
    r->stage = RB2K_STAGE_TRANSFER;
    r->status = rb_gpu_submit_scene(gpu, gpu->d7Frame.words, gpu->d7Frame.count,
                                    last ? &gpu->d7Frame.pending : &none, last);
    ok = r->status == NO_ERROR;
    if (ok && last && gpu->d7WaitCurrent) {
        r->status = rb_gpu_wait_image(gpu, target);
        ok = r->status == NO_ERROR;
    }
    gpu->d7Frame.count = 0;
    if (last || !ok) {
        if (!ok) target->frameFailed = TRUE;
        gpu->d7Frame.target = NULL;
        gpu->d7Frame.pending.clears = gpu->d7Frame.pending.triangles = 0;
    }
    return ok;
}

BOOLEAN rb_gpu_frame_flush(RB2K_GPU *gpu, RB2K_GPU_RESULT *r)
{
    return submit_frame(gpu, r, TRUE);
}

/* Room for count words at the end of the image's open frame. The caller
 * writes all of them before its next frame request. */
ULONG *rb_gpu_frame_reserve(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image, ULONG count)
{
    const ULONG capacity = sizeof(gpu->d7Frame.words)/sizeof(ULONG);
    ULONG *words;
    if (!count || count > capacity || image->frameFailed) {
        r->status = ERROR_INVALID_DATA;
        r->stage = RB2K_STAGE_TRANSFER;
        return NULL;
    }
    if (gpu->d7Frame.target != image && !rb_gpu_frame_flush(gpu, r)) return NULL;
    if (count > capacity-gpu->d7Frame.count && !submit_frame(gpu, r, FALSE)) return NULL;
    words = gpu->d7Frame.words + gpu->d7Frame.count;
    gpu->d7Frame.count += count;
    gpu->d7Frame.target = image;
    return words;
}

BOOLEAN rb_gpu_frame_words(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                           const ULONG *words, ULONG count)
{
    ULONG *room = rb_gpu_frame_reserve(gpu, r, image, count);
    if (!room) return FALSE;
    VideoPortMoveMemory(room, (PVOID)words, count*sizeof(*words));
    return TRUE;
}

BOOLEAN rb_gpu_draw_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id,
                          ULONG color, BOOLEAN createSurface, RB2K_IMAGE *frameTarget, ULONG depthView)
{
    PIXEL_REQUEST request;
    ULONG skip = createSurface ? 0 : CreateWords;
    ULONG bytes = sizeof(clear_commands) - skip * sizeof(ULONG);
    ULONG *words = request.draw.words;
    begin_request(&request, VIRTIO_GPU_CMD_SUBMIT_3D, id);
    request.draw.submit.size = bytes;
    VideoPortMoveMemory(words, (PVOID)(clear_commands + skip), bytes);
    if (createSurface) words[2] = id;
    words[8 - skip] = depthView;
    words[12 - skip] = rb_gpu_channel((color >> 16) & 255);
    words[13 - skip] = rb_gpu_channel((color >> 8) & 255);
    words[14 - skip] = rb_gpu_channel(color & 255);
    words[15 - skip] = rb_gpu_channel(color >> 24);
    if (frameTarget) return rb_gpu_frame_words(gpu, result, frameTarget, words, bytes / sizeof(*words));
    return fenced(gpu, result, &request.hdr, sizeof(request.draw.submit) + bytes, RB2K_STAGE_CLEAR);
}

/* Depth writes run in the attached colour target's context. A CPU observer
 * must wait on that target even when locking just the depth surface. */
BOOLEAN rb_gpu_sync_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image)
{
    RB2K_IMAGE *writer = image->depthOwner ? image->depthOwner : image;
    if (gpu->d7Frame.target == writer && !rb_gpu_frame_flush(gpu, r)) return FALSE;
    r->stage = RB2K_STAGE_TRANSFER;
    r->status = rb_gpu_wait_image(gpu, writer);
    return r->status == NO_ERROR;
}

/* Queue the image's open frame without waiting for it. A request in the
 * image's own context that follows in the queue sees all its drawing. */
BOOLEAN rb_gpu_order_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image)
{
    RB2K_IMAGE *writer = image->depthOwner ? image->depthOwner : image;
    return gpu->d7Frame.target != writer || rb_gpu_frame_flush(gpu, r);
}

/* One view belongs to one colour context. Rebinding retires the old view
 * before reusing its handle or attaching its resource in a different context.
 * Repeated frames using the same pair add neither a submit nor a wait. */
BOOLEAN rb_gpu_depth_target(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *color, RB2K_IMAGE *depth)
{
    static const ULONG release[] = {
        VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)),
        1, 0, RB2K_D7_COLOR_VIEW,
        VIRGL_CMD0(VIRGL_CCMD_DESTROY_OBJECT, VIRGL_OBJECT_SURFACE, 1), RB2K_D7_DEPTH_VIEW
    };
    ULONG attach[] = {
        VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE),
        RB2K_D7_DEPTH_VIEW, 0, VIRGL_FORMAT_Z16_UNORM, 0, 0,
        VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)),
        1, RB2K_D7_DEPTH_VIEW, RB2K_D7_COLOR_VIEW,
        /* An untouched depth surface is logically zero, including after a first partial draw. */
        VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE), 1, 0, 0, 0, 0, 0, 0, 0
    };
    if (depth && depth->stencil8) { attach[3] = VIRGL_FORMAT_S8_UINT_Z24_UNORM; attach[11] = 3; }   /* and stencil */
    RB2K_IMAGE *old = color->depthBuffer;
    ULONG count = sizeof(attach)/sizeof(*attach);
    if (color->depth16 || (depth && (!depth->depth16 || depth->frameFailed ||
        color->width > depth->width || color->height > depth->height))) {
        r->status = ERROR_INVALID_PARAMETER;
        r->stage = RB2K_STAGE_ATTACH;
        return FALSE;
    }
    if (old == depth) return TRUE;
    if (old) {
        if (!rb_gpu_frame_words(gpu, r, color, release, sizeof(release)/sizeof(*release)) ||
            !rb_gpu_sync_image(gpu, r, color) ||
            !context_resource(gpu, r, color->id, old->id, FALSE, RB2K_STAGE_CLEANUP)) {
            gpu->ready = FALSE;
            return FALSE;
        }
        old->depthOwner = NULL;
        color->depthBuffer = NULL;
    }
    if (!depth) return TRUE;
    if (depth->depthOwner && !rb_gpu_depth_target(gpu, r, depth->depthOwner, NULL)) return FALSE;
    if (!rb_gpu_prepare_target(gpu, r, color) ||
        !context_resource(gpu, r, color->id, depth->id, TRUE, RB2K_STAGE_ATTACH)) return FALSE;
    attach[2] = depth->id;
    if (depth->defined) count -= 1 + VIRGL_OBJ_CLEAR_SIZE;
    if (!rb_gpu_frame_words(gpu, r, color, attach, count)) {
        RB2K_GPU_RESULT cleanup;
        if (gpu->ready && !context_resource(gpu, &cleanup, color->id, depth->id, FALSE, RB2K_STAGE_CLEANUP))
            gpu->ready = FALSE;
        return FALSE;
    }
    color->depthBuffer = depth;
    depth->depthOwner = color;
    depth->defined = TRUE;
    return TRUE;
}

/* Direct3D supplies a float in [0,1]. Gallium clears use a double. Expand
 * the IEEE bits exactly, including subnormals, without using kernel FP state. */
static void clear_depth_words(ULONG value, ULONG *lo, ULONG *hi)
{
    ULONG fraction = value & 0x7fffff;
    ULONG exponent = (value >> 23) & 255;
    if (!(value & 0x7fffffff)) { *lo = *hi = 0; return; }
    if (exponent) exponent += 896;
    else {
        exponent = 897;
        while (!(fraction & 0x800000)) { fraction <<= 1; --exponent; }
        fraction &= 0x7fffff;
    }
    *lo = fraction << 29;
    *hi = (exponent << 20) | (fraction >> 3);
}

BOOLEAN rb_gpu_clear_buffers(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                             ULONG flags, ULONG color, ULONG depthBits, ULONG stencil)
{
    ULONG words[1 + VIRGL_OBJ_CLEAR_SIZE] = { VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE) };
    if (flags & RB2K_CLEAR_DEPTH) {
        if (!image->depthBuffer || (depthBits > 0x3f800000 && depthBits != 0x80000000)) {
            r->stage = RB2K_STAGE_CLEAR;
            r->status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        words[VIRGL_OBJ_CLEAR_BUFFERS] |= 1; /* PIPE_CLEAR_DEPTH */
        clear_depth_words(depthBits, words + VIRGL_OBJ_CLEAR_DEPTH_0, words + VIRGL_OBJ_CLEAR_DEPTH_1);
    }
    if (flags & RB2K_CLEAR_STENCIL) {
        if (!image->depthBuffer || !image->depthBuffer->stencil8 || stencil > 255) {
            r->stage = RB2K_STAGE_CLEAR;
            r->status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        words[VIRGL_OBJ_CLEAR_BUFFERS] |= 2; /* PIPE_CLEAR_STENCIL */
        words[VIRGL_OBJ_CLEAR_STENCIL] = stencil;
    }
    if (flags & RB2K_CLEAR_COLOR) {
        words[VIRGL_OBJ_CLEAR_BUFFERS] |= PipeClearColor0;
        words[VIRGL_OBJ_CLEAR_COLOR_0] = rb_gpu_channel((color >> 16) & 255);
        words[VIRGL_OBJ_CLEAR_COLOR_1] = rb_gpu_channel((color >> 8) & 255);
        words[VIRGL_OBJ_CLEAR_COLOR_2] = rb_gpu_channel(color & 255);
        words[VIRGL_OBJ_CLEAR_COLOR_3] = rb_gpu_channel(color >> 24);
    }
    if (!rb_gpu_frame_words(gpu, r, image, words, sizeof(words)/sizeof(*words))) return FALSE;
    if (flags & RB2K_CLEAR_COLOR) {
        image->defined = image->uniform = TRUE;
        image->color = color;
    }
    return TRUE;
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

/* An ordered read is not fenced: the renderer copies the pixels before it
 * answers, after everything queued earlier in that context. */
/* dataRows: rows of rowBytes in the upload; height texel rows, which differ
 * for 4x4 blocks. */
static BOOLEAN transfer_rows(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG context, ULONG resource,
                             ULONG x, ULONG y, ULONG width, ULONG height, PVOID upload, ULONG pitch,
                             ULONG rowBytes, BOOLEAN ordered, ULONG level, ULONG dataRows, ULONG layer)
{
    PIXEL_REQUEST request;
    BACKING_REQUEST backing;
    ULONG bytes = rowBytes * dataRows;
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
        else if (pitch == rowBytes)
            VideoPortMoveMemory(gpu->staging[i].data, (PUCHAR)upload + offset, length);
    }
    if (upload && pitch != rowBytes)
        copy_rows(gpu, upload, pitch, rowBytes, dataRows, FALSE);
    if (!send_request(gpu, result, &backing, used, RB2K_STAGE_BACKING)) return FALSE;
    begin_request(&request, upload ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D :
        VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, context);
    request.transfer.resource_id = resource;
    request.transfer.level = level;
    request.transfer.box.x = x;
    request.transfer.box.y = y;
    request.transfer.box.w = width;
    request.transfer.box.h = height;
    request.transfer.box.z = layer;
    request.transfer.box.d = 1;
    request.transfer.stride = rowBytes;
    if (ordered && !upload ? !send_request(gpu, result, &request.hdr, sizeof(request.transfer), RB2K_STAGE_TRANSFER) :
        !fenced(gpu, result, &request.hdr, sizeof(request.transfer), RB2K_STAGE_TRANSFER)) return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING, 0);
    request.detach.resource_id = resource;
    return send_request(gpu, result, &request, sizeof(request.detach), RB2K_STAGE_BACKING);
}

static BOOLEAN transfer_box(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG context, ULONG resource,
                            ULONG x, ULONG y, ULONG width, ULONG height, PVOID upload, ULONG pitch,
                            ULONG rowBytes, BOOLEAN ordered, ULONG level)
{
    return transfer_rows(gpu, result, context, resource, x, y, width, height, upload, pitch, rowBytes, ordered,
                         level, height, 0);
}

BOOLEAN rb_gpu_transfer_box(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG resource,
                            ULONG x, ULONG y, ULONG width, ULONG height, PVOID upload)
{
    ULONG rowBytes = width * sizeof(ULONG);
    return transfer_box(gpu, result, resource, resource, x, y, width, height, upload, rowBytes, rowBytes, TRUE, 0);
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
                          const ULONG *pixels, ULONG pitchPixels, ULONG level)
{
    ULONG row, count = VIRGL_RESOURCE_IW_DATA_START + w*h;
    VideoPortZeroMemory(words, VIRGL_RESOURCE_IW_DATA_START * sizeof(*words));
    words[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, count-1);
    words[VIRGL_RESOURCE_IW_RES_HANDLE] = id;
    words[VIRGL_RESOURCE_IW_LEVEL] = level;
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
                              primary->pixels + r->top*primary->pitchPixels + r->left, primary->pitchPixels, 0);
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
    if (!transfer_box(gpu, result, gpu->glDesktop.image.id, gpu->glDesktop.image.id, rect->left, rect->top,
                      width, height, readback ? NULL : first, pitch, width * sizeof(ULONG), FALSE, 0))
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

/* Put a resource on scanout 0 (select) and show its latest contents. */
BOOLEAN rb_gpu_show_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG resource, ULONG width, ULONG height,
                          BOOLEAN select)
{
    PIXEL_REQUEST request;
    LONGLONG start = rb_counter_ticks(NULL);
    BOOLEAN ok;
    if (select) {
        begin_request(&request, VIRTIO_GPU_CMD_SET_SCANOUT, 0);
        request.scanout.resource_id = resource;
        request.scanout.r.width = width;
        request.scanout.r.height = height;
        if (!send_request(gpu, result, &request, sizeof(request.scanout), RB2K_STAGE_TRANSFER))
            return rb_gl_record_time(gpu, RB2K_GL_FLUSH, start, FALSE);
    }
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_FLUSH, 0);
    request.flush.resource_id = resource;
    request.flush.r.width = width;
    request.flush.r.height = height;
    ok = send_request(gpu, result, &request, sizeof(request.flush), RB2K_STAGE_TRANSFER);
    return rb_gl_record_time(gpu, RB2K_GL_FLUSH, start, ok);
}

BOOLEAN rb_gpu_show_desktop(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, BOOLEAN select)
{
    RB2K_IMAGE *image = &gpu->glDesktop.image;
    if (!rb_gpu_show_image(gpu, result, image->id, image->width, image->height, select)) return FALSE;
    if (select) gpu->glDesktop.visible = TRUE;
    return TRUE;
}

BOOLEAN rb_gpu_transfer_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                              const RB2K_IMAGE *image, PVOID pixels, BOOLEAN readback)
{
    ULONG rowBytes = image->width * (image->depth16 ? sizeof(USHORT) : sizeof(ULONG));
    ULONG bytes = rowBytes * image->height;
    ULONG offset;
    if (!transfer_box(gpu, result, image->id, image->id, 0, 0, image->width, image->height,
                      readback ? NULL : pixels, rowBytes, rowBytes, FALSE, 0)) return FALSE;
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

/* A square colour image into layer (a cube face) of a host texture, in the image's own context, to
 * which the texture is attached. */
BOOLEAN rb_gpu_copy_layer(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG image, ULONG texture, ULONG layer, ULONG side)
{
    COPY_PACKET *p = (COPY_PACKET *)gpu->copyPacket.bytes;
    ULONG *w = p->command[0];
    VideoPortZeroMemory(p, sizeof(p->submit) + sizeof(p->command[0]));
    p->submit.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    p->submit.hdr.ctx_id = image;
    p->submit.size = sizeof(p->command[0]);
    w[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    w[VIRGL_CMD_RCR_DST_RES_HANDLE] = texture;
    w[VIRGL_CMD_RCR_DST_Z] = layer;
    w[VIRGL_CMD_RCR_SRC_RES_HANDLE] = image;
    w[VIRGL_CMD_RCR_SRC_W] = side;
    w[VIRGL_CMD_RCR_SRC_H] = side;
    w[VIRGL_CMD_RCR_SRC_D] = 1;
    return fenced(gpu, result, &p->submit.hdr, sizeof(p->submit) + sizeof(p->command[0]), RB2K_STAGE_TRANSFER);
}

/* A rectangle of an image into another resource (the desktop in guest memory,
 * gpu_primary.c), in the image's own context, to which the destination is
 * attached for the copy. wait: until the host GPU has done it (fenced); otherwise
 * the host has only queued it, after the image's own drawing. */
BOOLEAN rb_gpu_copy_image_rect(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG image, ULONG destination,
                               ULONG srcX, ULONG srcY, ULONG dstX, ULONG dstY, ULONG width, ULONG height,
                               BOOLEAN wait)
{
    COPY_PACKET *p = (COPY_PACKET *)gpu->copyPacket.bytes;
    RB2K_GPU_RESULT cleanup;
    ULONG *w = p->command[0];
    VideoPortZeroMemory(p, sizeof(p->submit) + sizeof(p->command[0]));
    p->submit.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    p->submit.hdr.ctx_id = image;
    p->submit.size = sizeof(p->command[0]);
    w[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    w[VIRGL_CMD_RCR_DST_RES_HANDLE] = destination;
    w[VIRGL_CMD_RCR_DST_X] = dstX;
    w[VIRGL_CMD_RCR_DST_Y] = dstY;
    w[VIRGL_CMD_RCR_SRC_RES_HANDLE] = image;
    w[VIRGL_CMD_RCR_SRC_X] = srcX;
    w[VIRGL_CMD_RCR_SRC_Y] = srcY;
    w[VIRGL_CMD_RCR_SRC_W] = width;
    w[VIRGL_CMD_RCR_SRC_H] = height;
    w[VIRGL_CMD_RCR_SRC_D] = 1;
    if (!context_resource(gpu, result, image, destination, TRUE, RB2K_STAGE_ATTACH)) return FALSE;
    if (wait ? !fenced(gpu, result, &p->submit.hdr, sizeof(p->submit) + sizeof(p->command[0]), RB2K_STAGE_TRANSFER) :
               !send_request(gpu, result, p, sizeof(p->submit) + sizeof(p->command[0]), RB2K_STAGE_TRANSFER)) {
        if (gpu->ready) context_resource(gpu, &cleanup, image, destination, FALSE, RB2K_STAGE_CLEANUP);
        return FALSE;
    }
    return context_resource(gpu, result, image, destination, FALSE, RB2K_STAGE_CLEANUP);
}

enum { BlitWords = 1 + VIRGL_CMD_BLIT_SIZE };
typedef struct {
    struct virtio_gpu_cmd_submit submit;
    ULONG command[BlitWords];
} BLIT_PACKET;
typedef char blit_fits_inline[sizeof(BLIT_PACKET) <= RB2K_INLINE_PACKET_BYTES ? 1 : -1];

/* A scaled copy of one rectangle of a colour image into a rectangle of another, both already
 * inside their images, point-sampled or linear: the host blits it in the destination's context. */
BOOLEAN rb_gpu_blit_rect(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG destination, ULONG source,
                         const RB2K_RECT *to, const RB2K_RECT *from, BOOLEAN linear)
{
    BLIT_PACKET *p = (BLIT_PACKET *)gpu->copyPacket.bytes;
    RB2K_GPU_RESULT cleanup;
    ULONG *w = p->command;
    BOOLEAN ok;
    VideoPortZeroMemory(p, sizeof(*p));
    p->submit.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    p->submit.hdr.ctx_id = destination;
    p->submit.size = sizeof(p->command);
    w[0] = VIRGL_CMD0(VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE);
    /* All four channels; filter 1 is linear, 0 nearest (Gallium's PIPE_TEX_FILTER_*). */
    w[VIRGL_CMD_BLIT_S0] = VIRGL_CMD_BLIT_S0_MASK(0xf) | VIRGL_CMD_BLIT_S0_FILTER(linear ? 1 : 0);
    w[VIRGL_CMD_BLIT_DST_RES_HANDLE] = destination;
    w[VIRGL_CMD_BLIT_DST_FORMAT] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    w[VIRGL_CMD_BLIT_DST_X] = to->left;
    w[VIRGL_CMD_BLIT_DST_Y] = to->top;
    w[VIRGL_CMD_BLIT_DST_W] = to->right - to->left;
    w[VIRGL_CMD_BLIT_DST_H] = to->bottom - to->top;
    w[VIRGL_CMD_BLIT_DST_D] = 1;
    w[VIRGL_CMD_BLIT_SRC_RES_HANDLE] = source;
    w[VIRGL_CMD_BLIT_SRC_FORMAT] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    w[VIRGL_CMD_BLIT_SRC_X] = from->left;
    w[VIRGL_CMD_BLIT_SRC_Y] = from->top;
    w[VIRGL_CMD_BLIT_SRC_W] = from->right - from->left;
    w[VIRGL_CMD_BLIT_SRC_H] = from->bottom - from->top;
    w[VIRGL_CMD_BLIT_SRC_D] = 1;
    if (!context_resource(gpu, result, destination, source, TRUE, RB2K_STAGE_ATTACH)) return FALSE;
    ok = fenced(gpu, result, &p->submit.hdr, sizeof(*p), RB2K_STAGE_TRANSFER);
    if (!ok) {
        if (gpu->ready) context_resource(gpu, &cleanup, destination, source, FALSE, RB2K_STAGE_CLEANUP);
        return FALSE;
    }
    return context_resource(gpu, result, destination, source, FALSE, RB2K_STAGE_CLEANUP);
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
    static const RB2K_IMAGE image = {1, 1, 1, FALSE, FALSE, FALSE, 0};   /* static: an image is larger than a page of stack */
    result->expected = 0xffff8000UL;
    if (!rb_gpu_create_image(gpu, result, &image, name, sizeof(name) - 1, FALSE)) return;
    result->observed = ~result->expected;
    if (!rb_gpu_draw_image(gpu, result, image.id, result->expected, TRUE, NULL, 0) ||
        !rb_gpu_transfer_image(gpu, result, &image, &result->observed, TRUE)) return;
    if (!rb_gpu_destroy_image(gpu, result, image.id)) return;
    if (result->observed != result->expected) {
        result->stage = RB2K_STAGE_COMPARE;
        result->status = ERROR_INVALID_DATA;
        return;
    }
    result->stage = RB2K_STAGE_DONE;
}

/* ---- Direct3D textures: host resources without a context of their own ---- */

RB2K_TEXTURE *rb_gpu_texture(RB2K_GPU *gpu, ULONG id)
{
    ULONG i;
    for (i = 0; id && i < RB2K_D7_TEXTURES; ++i)
        if (gpu->textures[i].id == id) return &gpu->textures[i];
    return NULL;
}

/* The upload formats the host samples, from its virgl capability set (bit n
 * for upload format n: RB2K_D7_BLOCKS_DXT*, RB2K_D7_TEXELS_*), asked once. */
ULONG rb_gpu_host_blocks(RB2K_GPU *gpu)
{
    struct virtio_gpu_get_capset request;
    struct { struct virtio_gpu_ctrl_hdr hdr; ULONG maxVersion, sampler[16], rest[110]; } reply;
    static const ULONG formats[RB2K_D7_UPLOAD_FORMATS] = { 0, VIRGL_FORMAT_DXT1_RGBA, VIRGL_FORMAT_DXT3_RGBA,
        VIRGL_FORMAT_DXT5_RGBA, VIRGL_FORMAT_R8G8_SNORM, VIRGL_FORMAT_R8G8B8A8_SNORM };
    ULONG received = 0, k;
    if (gpu->hostBlocksKnown || !gpu->ready) return gpu->hostBlocks;
    gpu->hostBlocksKnown = TRUE;
    VideoPortZeroMemory(&request, sizeof(request));
    request.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    request.capset_id = VIRTIO_GPU_CAPSET_VIRGL;
    request.capset_version = 1;
    if (rb_gpu_submit(gpu, &request, sizeof(request), &reply, sizeof(reply), &received) ||
        received < sizeof(reply.hdr) + sizeof(ULONG) * 17 || reply.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET)
        return 0;
    for (k = 1; k < RB2K_D7_UPLOAD_FORMATS; ++k)
        if ((reply.sampler[formats[k] / 32] >> (formats[k] % 32)) & 1) gpu->hostBlocks |= 1UL << k;
    return gpu->hostBlocks;
}

BOOLEAN rb_gpu_texture_create(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture)
{
    PIXEL_REQUEST request;
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, 0);
    request.resource.resource_id = texture->id;
    request.resource.target = texture->depth > 1 ? PipeTexture3D : texture->faces == 6 ? PipeTextureCube : PipeTexture2D;
    request.resource.format = texture->format ? texture->format : VIRGL_FORMAT_B8G8R8A8_UNORM;
    request.resource.bind = VIRGL_BIND_SAMPLER_VIEW;
    request.resource.width = texture->width;
    request.resource.height = texture->height;
    request.resource.depth = texture->depth > 1 ? texture->depth : 1;
    request.resource.array_size = texture->faces == 6 ? 6 : 1;
    request.resource.last_level = texture->levels - 1;
    return send_request(gpu, result, &request, sizeof(request.resource), RB2K_STAGE_RESOURCE);
}

/* The image's slot in gpu->images, or RB2K_DD_SLOTS for any other image. */
static ULONG image_slot(const RB2K_GPU *gpu, const RB2K_IMAGE *image)
{
    return image >= gpu->images && image < gpu->images + RB2K_DD_SLOTS ? (ULONG)(image - gpu->images) : RB2K_DD_SLOTS;
}

/* Attach a resource to the target's context once; bits (a bit per image
 * slot) remembers the contexts it is attached to. */
static BOOLEAN attach_once(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, PULONG bits, ULONG resource,
                           const RB2K_IMAGE *target)
{
    ULONG slot = image_slot(gpu, target), bit = 1UL << (slot & 31);
    if (slot == RB2K_DD_SLOTS) {
        result->status = ERROR_INVALID_PARAMETER;
        result->stage = RB2K_STAGE_ATTACH;
        return FALSE;
    }
    if (bits[slot / 32] & bit) return TRUE;
    if (!context_resource(gpu, result, target->id, resource, TRUE, RB2K_STAGE_ATTACH)) return FALSE;
    bits[slot / 32] |= bit;
    return TRUE;
}

/* A texture is attached to each colour context that samples it. */
BOOLEAN rb_gpu_texture_attach(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture,
                              const RB2K_IMAGE *image)
{
    return attach_once(gpu, result, texture->attached, texture->id, image);
}

/* An image sampled by another target (a render target texture) is attached
 * to that target's context too, like a texture. */
BOOLEAN rb_gpu_image_attach(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *source, const RB2K_IMAGE *target)
{
    return attach_once(gpu, result, source->sampledBy, source->id, target);
}

/* The image is going: the next image in its slot has a new context. */
void rb_gpu_slot_forget(RB2K_GPU *gpu, const RB2K_IMAGE *image)
{
    ULONG slot = image_slot(gpu, image), i, bit = ~(1UL << (slot & 31));
    if (slot == RB2K_DD_SLOTS) return;
    for (i = 0; i < RB2K_D7_TEXTURES; ++i) gpu->textures[i].attached[slot / 32] &= bit;
    for (i = 0; i < RB2K_DD_SLOTS; ++i) gpu->images[i].sampledBy[slot / 32] &= bit;
}

/* Earlier draws in the open frame must reach the host before new texels. */
BOOLEAN rb_gpu_texture_upload(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, const RB2K_TEXTURE *texture,
                              const RB2K_IMAGE *image, ULONG level, ULONG face, ULONG y, ULONG rows, PVOID texels)
{
    ULONG width = texture->width >> level ? texture->width >> level : 1;
    ULONG rowBytes = width * sizeof(ULONG), bytes = rowBytes * rows;
    if (gpu->d7Frame.target && gpu->d7Frame.count && !submit_frame(gpu, result, FALSE)) return FALSE;
    if (texture->format) {
        /* Rows of 4x4 DXT blocks (eight bytes each for DXT1, sixteen for the
         * others), or rows of signed texels of two or four bytes. */
        ULONG side = texture->format == VIRGL_FORMAT_R8G8_SNORM || texture->format == VIRGL_FORMAT_R8G8B8A8_SNORM ? 1 : 4;
        ULONG block = texture->format == VIRGL_FORMAT_DXT1_RGBA ? 8 : side == 4 ? 16 :
                      texture->format == VIRGL_FORMAT_R8G8_SNORM ? 2 : 4;
        rowBytes = (width + side - 1) / side * block;
        return transfer_rows(gpu, result, image->id, texture->id, 0, y, width, rows, texels, rowBytes, rowBytes,
                             FALSE, level, (rows + side - 1) / side, face);
    }
    if (bytes <= 16384) {
        ULONG count;
        result->stage = RB2K_STAGE_BACKING;
        result->status = rb_gpu_reserve_staging(gpu, bytes + VIRGL_RESOURCE_IW_DATA_START*sizeof(ULONG));
        if (result->status) return FALSE;
        count = inline_write((PULONG)gpu->staging[0].data, texture->id, 0, y, width, rows, texels, width, level);
        ((PULONG)gpu->staging[0].data)[VIRGL_RESOURCE_IW_Z] = face;   /* the cube face */
        result->stage = RB2K_STAGE_TRANSFER;
        result->status = submit_inline(gpu, image->id, count);
        return result->status == NO_ERROR;
    }
    return transfer_rows(gpu, result, image->id, texture->id, 0, y, width, rows,
                        texels, rowBytes, rowBytes, FALSE, level, rows, face);
}

/* flush: draws of the open frame may sample the texture (its view is made by commands that have
 * not gone to the host yet): they go first. Unreferencing takes effect at once, and a view made
 * of a resource that is gone is an error that makes the host drop the rest of that packet. */
BOOLEAN rb_gpu_texture_free(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture, BOOLEAN flush)
{
    PIXEL_REQUEST request;
    if (flush && gpu->d7Frame.target && gpu->d7Frame.count && !submit_frame(gpu, result, FALSE)) return FALSE;
    /* Unreferencing detaches the resource from every context. A bound view
     * keeps the host storage alive until that context replaces it. */
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
    request.unref.resource_id = texture->id;
    if (!send_request(gpu, result, &request, sizeof(request.unref), RB2K_STAGE_CLEANUP)) return FALSE;
    gpu->textureBytes -= texture->bytes;
    --gpu->textureCount;
    VideoPortZeroMemory(texture, sizeof(*texture));
    return TRUE;
}

/* The transport is going away or every context was destroyed. */
void rb_gpu_texture_forget(RB2K_GPU *gpu)
{
    VideoPortZeroMemory(gpu->textures, sizeof(gpu->textures));
    gpu->textureCount = gpu->textureBytes = 0;
}
