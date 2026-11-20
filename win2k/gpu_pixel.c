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
    struct {
        struct virtio_gpu_resource_attach_backing attach;
        struct virtio_gpu_mem_entry entry;
    } backing;
    struct virtio_gpu_ctx_resource ctx_resource;
    struct {
        struct virtio_gpu_cmd_submit submit;
        ULONG words[sizeof(clear_commands) / sizeof(ULONG)];
    } draw;
    struct virtio_gpu_transfer_host_3d transfer;
    struct virtio_gpu_resource_unref unref;
} PIXEL_REQUEST;

/* The combined submit may have compiler-inserted tail padding. Only the
 * byte count in its wire header and the command words are transmitted. */
typedef char gpu_header_must_be_24[sizeof(struct virtio_gpu_ctrl_hdr) == 24 ? 1 : -1];
typedef char gpu_entry_must_be_16[sizeof(struct virtio_gpu_mem_entry) == 16 ? 1 : -1];

static void begin_request(PIXEL_REQUEST *request, ULONG type, ULONG context)
{
    VideoPortZeroMemory(request, sizeof(*request));
    request->hdr.type = type;
    request->hdr.ctx_id = context;
}

static BOOLEAN send_request(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                            PIXEL_REQUEST *request, ULONG bytes, ULONG stage)
{
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
    if ((request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) &&
        (!(reply.flags & VIRTIO_GPU_FLAG_FENCE) || reply.fence_id != request->hdr.fence_id)) {
        result->status = ERROR_INVALID_DATA;
        return FALSE;
    }
    return TRUE;
}

BOOLEAN rb_gpu_create_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                            const RB2K_IMAGE *image, ULONG slot, const char *name, ULONG nameBytes)
{
    PIXEL_REQUEST request;
    PHYSICAL_ADDRESS bus;
    ULONG *backing = rb_gpu_backing(gpu, slot, &bus);
    ULONG bytes = image->width * image->height * sizeof(ULONG);
    if (!backing) {
        result->stage = RB2K_STAGE_BACKING;
        result->status = ERROR_NOT_ENOUGH_MEMORY;
        return FALSE;
    }
    /* A retired slot can be reused while another surface keeps the device
     * active. Clear its exposed backing extent before attaching it again. */
    VideoPortZeroMemory(backing, bytes);
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

    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, 0);
    request.backing.attach.resource_id = image->id;
    request.backing.attach.nr_entries = 1;
    request.backing.entry.addr = bus.QuadPart;
    request.backing.entry.length = bytes;
    if (!send_request(gpu, result, &request, sizeof(request.backing), RB2K_STAGE_BACKING)) return FALSE;

    begin_request(&request, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, image->id);
    request.ctx_resource.resource_id = image->id;
    return send_request(gpu, result, &request, sizeof(request.ctx_resource), RB2K_STAGE_ATTACH);
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

BOOLEAN rb_gpu_transfer_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                              const RB2K_IMAGE *image, BOOLEAN readback)
{
    PIXEL_REQUEST request;
    begin_request(&request, readback ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D :
        VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, image->id);
    request.transfer.resource_id = image->id;
    request.transfer.box.w = image->width;
    request.transfer.box.h = image->height;
    request.transfer.box.d = 1;
    return fenced(gpu, result, &request, sizeof(request.transfer), RB2K_STAGE_TRANSFER);
}

BOOLEAN rb_gpu_destroy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id)
{
    PIXEL_REQUEST request;
    begin_request(&request, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, id);
    request.ctx_resource.resource_id = id;
    if (!send_request(gpu, result, &request, sizeof(request.ctx_resource), RB2K_STAGE_CLEANUP)) return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_CTX_DESTROY, id);
    if (!send_request(gpu, result, &request, sizeof(request.hdr), RB2K_STAGE_CLEANUP)) return FALSE;
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
    request.unref.resource_id = id;
    return send_request(gpu, result, &request, sizeof(request.unref), RB2K_STAGE_CLEANUP);
}

void rb_gpu_pixel(RB2K_GPU *gpu, RB2K_GPU_RESULT *result)
{
    static const char name[] = "rb2k-pixel";
    const RB2K_IMAGE image = {1, 1, 1, FALSE, FALSE};
    PHYSICAL_ADDRESS bus;
    volatile ULONG *pixel;
    result->expected = 0xffff8000UL;
    if (!rb_gpu_create_image(gpu, result, &image, 0, name, sizeof(name) - 1)) return;
    pixel = rb_gpu_backing(gpu, 0, &bus);
    *pixel = ~result->expected;
    if (!rb_gpu_draw_image(gpu, result, image.id, result->expected, TRUE) ||
        !rb_gpu_transfer_image(gpu, result, &image, TRUE)) return;
    result->observed = *pixel;
    if (!rb_gpu_destroy_image(gpu, result, image.id)) return;
    if (result->observed != result->expected) {
        result->stage = RB2K_STAGE_COMPARE;
        result->status = ERROR_INVALID_DATA;
        return;
    }
    result->stage = RB2K_STAGE_DONE;
}
