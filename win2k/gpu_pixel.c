/* Wire commands shared by the fixed pixel diagnostic and offscreen
 * DirectDraw surfaces. None of these operations changes the scanout. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "rb2kprobe.h"
#include "gpu_transport.h"
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
} PIXEL_REQUEST;

typedef struct {
    struct virtio_gpu_resource_attach_backing attach;
    struct virtio_gpu_mem_entry entries[RB2K_MAX_SEGMENTS];
} BACKING_REQUEST;

/* The combined submit may have compiler-inserted tail padding. Only the
 * byte count in its wire header and the command words are transmitted. */
typedef char gpu_header_must_be_24[sizeof(struct virtio_gpu_ctrl_hdr) == 24 ? 1 : -1];
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
                            const RB2K_IMAGE *image, const char *name, ULONG nameBytes)
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
    request.resource.bind = VIRGL_BIND_RENDER_TARGET;
    request.resource.width = image->width;
    request.resource.height = image->height;
    request.resource.depth = request.resource.array_size = 1;
    if (!send_request(gpu, result, &request, sizeof(request.resource), RB2K_STAGE_RESOURCE)) return FALSE;

    return context_resource(gpu, result, image->id, image->id, TRUE, RB2K_STAGE_ATTACH);
}

/* IEEE single-precision encoding of channel/255 rounded to the nearest
 * representable value, without using the kernel thread's floating-point state. */
static ULONG channel_bits(ULONG channel)
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
                      PIXEL_REQUEST *request, ULONG bytes, ULONG stage)
{
    /* The renderer uses a 32-bit fence timeline. Reset at exhaustion before
     * an old completion could be mistaken for a new fence. */
    if (gpu->fence == 0xffffffffUL) {
        result->status = ERROR_NO_MORE_ITEMS;
        result->stage = stage;
        return FALSE;
    }
    request->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    request->hdr.fence_id = ++gpu->fence;
    return send_request(gpu, result, request, bytes, stage);
}

BOOLEAN rb_gpu_draw_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id,
                          ULONG color, BOOLEAN createSurface)
{
    PIXEL_REQUEST request;
    ULONG skip = createSurface ? 0 : CreateWords;
    ULONG bytes = sizeof(clear_commands) - skip * sizeof(ULONG);
    ULONG *words = request.draw.words;
    begin_request(&request, VIRTIO_GPU_CMD_SUBMIT_3D, id);
    request.draw.submit.size = bytes;
    VideoPortMoveMemory(words, (PVOID)(clear_commands + skip), bytes);
    if (createSurface) words[2] = id;
    words[12 - skip] = channel_bits((color >> 16) & 255);
    words[13 - skip] = channel_bits((color >> 8) & 255);
    words[14 - skip] = channel_bits(color & 255);
    words[15 - skip] = channel_bits(color >> 24);
    return fenced(gpu, result, &request, sizeof(request.draw.submit) + bytes, RB2K_STAGE_CLEAR);
}

/* Use a packed DMA buffer even when the source box is inside a larger image. */
BOOLEAN rb_gpu_transfer_box(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG resource,
                            ULONG x, ULONG y, ULONG width, ULONG height, PVOID upload)
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
        if (upload) VideoPortMoveMemory(gpu->staging[i].data, (PUCHAR)upload + offset, length);
        else VideoPortZeroMemory(gpu->staging[i].data, length);
    }
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
    if (!fenced(gpu, result, &request, sizeof(request.transfer), RB2K_STAGE_TRANSFER)) return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING, 0);
    request.detach.resource_id = resource;
    return send_request(gpu, result, &request, sizeof(request.detach), RB2K_STAGE_BACKING);
}

BOOLEAN rb_gpu_transfer_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                              const RB2K_IMAGE *image, PVOID pixels, BOOLEAN readback)
{
    ULONG bytes = image->width * image->height * sizeof(ULONG);
    ULONG offset;
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

BOOLEAN rb_gpu_copy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                          const RB2K_IMAGE *source, const RB2K_IMAGE *destination)
{
    PIXEL_REQUEST request;
    ULONG *words = request.draw.words;
    ULONG bytes = (1 + VIRGL_CMD_RESOURCE_COPY_REGION_SIZE) * sizeof(ULONG);
    if (!context_resource(gpu, result, destination->id, source->id, TRUE, RB2K_STAGE_ATTACH))
        return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_SUBMIT_3D, destination->id);
    request.draw.submit.size = bytes;
    words[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    words[VIRGL_CMD_RCR_DST_RES_HANDLE] = destination->id;
    words[VIRGL_CMD_RCR_SRC_RES_HANDLE] = source->id;
    words[VIRGL_CMD_RCR_SRC_W] = source->width;
    words[VIRGL_CMD_RCR_SRC_H] = source->height;
    words[VIRGL_CMD_RCR_SRC_D] = 1;
    if (!fenced(gpu, result, &request, sizeof(request.draw.submit) + bytes, RB2K_STAGE_TRANSFER))
        return FALSE;
    return context_resource(gpu, result, destination->id, source->id, FALSE, RB2K_STAGE_CLEANUP);
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
    if (!rb_gpu_create_image(gpu, result, &image, name, sizeof(name) - 1)) return;
    result->observed = ~result->expected;
    if (!rb_gpu_draw_image(gpu, result, image.id, result->expected, TRUE) ||
        !rb_gpu_transfer_image(gpu, result, &image, &result->observed, TRUE)) return;
    if (!rb_gpu_destroy_image(gpu, result, image.id)) return;
    if (result->observed != result->expected) {
        result->stage = RB2K_STAGE_COMPARE;
        result->status = ERROR_INVALID_DATA;
        return;
    }
    result->stage = RB2K_STAGE_DONE;
}
