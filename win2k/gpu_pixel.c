/* Fixed diagnostic: render one orange pixel on the host, then read it
 * back into private DMA backing. The resource is never set as a scanout. */
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
    PixelContext = 1, PixelResource = 1, PixelSurface = 1,
    PipeTexture2D = 2, PipeClearColor0 = 1 << 2
};

/* Float bit patterns keep the miniport free of floating-point operations. */
static const ULONG clear_commands[] = {
    VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE),
    PixelSurface, PixelResource, VIRGL_FORMAT_B8G8R8A8_UNORM, 0, 0,
    VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)),
    1, 0, PixelSurface,
    VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE),
    PipeClearColor0, 0x3f800000, 0x3f000000, 0, 0x3f800000, 0, 0, 0
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

/* The caller owns a freshly negotiated device for this diagnostic and resets
 * it even on failure, before the queue, backing memory or IDs can be reused.
 * Return immediately with the failing stage if a command is unacknowledged. */
void rb_gpu_pixel(RB2K_GPU *gpu, RB2K_GPU_RESULT *result)
{
    static const char name[] = "rb2k-pixel";
    PIXEL_REQUEST request;
    PHYSICAL_ADDRESS bus;
    volatile ULONG *pixel = rb_gpu_backing(gpu, &bus);
    result->expected = 0xffff8000UL;
    if (!pixel) {
        result->stage = RB2K_STAGE_BACKING;
        result->status = ERROR_NOT_ENOUGH_MEMORY;
        return;
    }
    *pixel = ~result->expected;

    begin_request(&request, VIRTIO_GPU_CMD_CTX_CREATE, PixelContext);
    request.context.nlen = sizeof(name) - 1;
    VideoPortMoveMemory(request.context.debug_name, (PVOID)name, sizeof(name));
    if (!send_request(gpu, result, &request, sizeof(request.context), RB2K_STAGE_CONTEXT)) return;

    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, PixelContext);
    request.resource.resource_id = PixelResource;
    request.resource.target = PipeTexture2D;
    request.resource.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    request.resource.bind = VIRGL_BIND_RENDER_TARGET;
    request.resource.width = request.resource.height = 1;
    request.resource.depth = request.resource.array_size = 1;
    if (!send_request(gpu, result, &request, sizeof(request.resource), RB2K_STAGE_RESOURCE)) return;

    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, 0);
    request.backing.attach.resource_id = PixelResource;
    request.backing.attach.nr_entries = 1;
    request.backing.entry.addr = bus.QuadPart;
    request.backing.entry.length = sizeof(*pixel);
    if (!send_request(gpu, result, &request, sizeof(request.backing), RB2K_STAGE_BACKING)) return;

    begin_request(&request, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, PixelContext);
    request.ctx_resource.resource_id = PixelResource;
    if (!send_request(gpu, result, &request, sizeof(request.ctx_resource), RB2K_STAGE_ATTACH)) return;

    begin_request(&request, VIRTIO_GPU_CMD_SUBMIT_3D, PixelContext);
    request.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    request.hdr.fence_id = 1;
    request.draw.submit.size = sizeof(clear_commands);
    VideoPortMoveMemory(request.draw.words, (PVOID)clear_commands, sizeof(clear_commands));
    if (!send_request(gpu, result, &request, sizeof(request.draw.submit) + sizeof(clear_commands),
                      RB2K_STAGE_CLEAR)) return;

    begin_request(&request, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, PixelContext);
    request.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    request.hdr.fence_id = 2;
    request.transfer.resource_id = PixelResource;
    request.transfer.box.w = request.transfer.box.h = request.transfer.box.d = 1;
    if (!send_request(gpu, result, &request, sizeof(request.transfer), RB2K_STAGE_TRANSFER)) return;
    result->observed = *pixel;

    begin_request(&request, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, PixelContext);
    request.ctx_resource.resource_id = PixelResource;
    if (!send_request(gpu, result, &request, sizeof(request.ctx_resource), RB2K_STAGE_CLEANUP)) return;
    begin_request(&request, VIRTIO_GPU_CMD_CTX_DESTROY, PixelContext);
    if (!send_request(gpu, result, &request, sizeof(request.hdr), RB2K_STAGE_CLEANUP)) return;
    begin_request(&request, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
    request.unref.resource_id = PixelResource;
    if (!send_request(gpu, result, &request, sizeof(request.unref), RB2K_STAGE_CLEANUP)) return;

    if (result->observed != result->expected) {
        result->stage = RB2K_STAGE_COMPARE;
        result->status = ERROR_INVALID_DATA;
        return;
    }
    result->stage = RB2K_STAGE_DONE;
}
