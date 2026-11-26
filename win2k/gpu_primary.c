/* The 32-bit desktop in guest memory (RipplePrimaryInMemory, issue 121).
 * QEMU's virtio-vga has 8 MB of VGA memory, too little for a 32-bit desktop
 * at 1920x1080. With the switch on, a 32-bit desktop lives in guest pages
 * instead. They back a host resource that scanout 0 shows, and what GDI and
 * the presents change goes to the host as rectangles: TRANSFER_TO_HOST_3D
 * from the pages, then RESOURCE_FLUSH. The pages are made once, for the
 * largest such mode, and kept; the resource is made for each mode, and again
 * after a transport reset, which drops it (and with it the scanout). */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include "gpu_memory.h"
#pragma warning(push)
#pragma warning(disable:4200) /* flexible capset array in the virtio ABI */
#include <linux/virtio_gpu.h>
#pragma warning(pop)
#include <virgl_hw.h>

enum { PipeTexture2D = 2 };

typedef union {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_resource_create_3d create;
    struct virtio_gpu_resource_attach_backing attach;
    struct virtio_gpu_set_scanout scanout;
    struct virtio_gpu_resource_flush flush;
    struct virtio_gpu_transfer_host_3d transfer;
    struct virtio_gpu_resource_unref unref;
} PRIMARY_COMMAND;

static void begin(PRIMARY_COMMAND *c, ULONG type)
{
    VideoPortZeroMemory(c, sizeof(*c));
    c->hdr.type = type;
}

static VP_STATUS send(RB2K_GPU *gpu, PRIMARY_COMMAND *c, ULONG bytes, const RB2K_GPU_EXTENT *extent)
{
    struct virtio_gpu_ctrl_hdr reply;
    ULONG received = 0;
    VP_STATUS status = rb_gpu_submit_extents(gpu, c, bytes, extent, extent ? 1 : 0,
                                             &reply, sizeof(reply), &received);
    if (status) return status;
    if (received < sizeof(reply)) return ERROR_INVALID_DATA;
    return reply.type == VIRTIO_GPU_RESP_OK_NODATA ? NO_ERROR : ERROR_GEN_FAILURE;
}

static void unref(RB2K_GPU *gpu, ULONG resource)
{
    PRIMARY_COMMAND c;
    begin(&c, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    c.unref.resource_id = resource;
    if (send(gpu, &c, sizeof(c.unref), NULL)) gpu->ready = FALSE;
}

/* The pages' physical runs go in the first staging segment: the largest desktop
 * has 2,250 pages, and a segment holds 4,096 runs. */
static VP_STATUS attach(RB2K_GPU *gpu, ULONG resource)
{
    RB2K_PAGES pages;
    RB2K_GPU_EXTENT extent;
    PRIMARY_COMMAND c;
    ULONG runs;
    VP_STATUS status = rb_gpu_reserve_staging(gpu, RB2K_SEGMENT_BYTES);
    if (status) return status;
    VideoPortZeroMemory(&pages, sizeof(pages));
    pages.mdl = gpu->memoryMdl;
    pages.bytes = gpu->memoryBytes;
    runs = rb_pages_runs(&pages, (RB2K_PAGE_RUN *)gpu->staging[0].data, RB2K_SEGMENT_BYTES / sizeof(RB2K_PAGE_RUN));
    if (!runs) return ERROR_NOT_ENOUGH_MEMORY;
    extent.bus = gpu->staging[0].bus;
    extent.length = runs * sizeof(RB2K_PAGE_RUN);
    begin(&c, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    c.attach.resource_id = resource;
    c.attach.nr_entries = runs;
    return send(gpu, &c, sizeof(c.attach), &extent);
}

/* The resource for the current mode, on scanout 0. */
static VP_STATUS create(RB2K_GPU *gpu)
{
    RB2K_GPU_RESULT result;
    PRIMARY_COMMAND c;
    ULONG id;
    VP_STATUS status;
    if (!gpu->ready) {
        status = rb_gpu_start(gpu, &result);
        if (status) { rb_gpu_stop(gpu); return status; }
    }
    if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
    if (!gpu->nextId) gpu->nextId = 1;   /* resource 1 belongs to the single-pixel diagnostic */
    id = ++gpu->nextId;
    begin(&c, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
    c.create.resource_id = id;
    c.create.target = PipeTexture2D;
    c.create.format = VIRGL_FORMAT_B8G8R8A8_UNORM;   /* as the frame images, which presents copy */
    c.create.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_SCANOUT;
    c.create.width = gpu->memoryWidth;
    c.create.height = gpu->memoryHeight;
    c.create.depth = c.create.array_size = 1;
    status = send(gpu, &c, sizeof(c.create), NULL);
    if (status) return status;
    status = attach(gpu, id);
    if (!status) {
        begin(&c, VIRTIO_GPU_CMD_SET_SCANOUT);
        c.scanout.resource_id = id;
        c.scanout.r.width = gpu->memoryWidth;
        c.scanout.r.height = gpu->memoryHeight;
        status = send(gpu, &c, sizeof(c.scanout), NULL);
    }
    if (status) {
        if (gpu->ready) unref(gpu, id);
        return status;
    }
    gpu->memoryResource = id;
    return NO_ERROR;
}

/* Between the pages and the resource: the pages are its backing, rows of memoryWidth pixels. */
static VP_STATUS transfer(RB2K_GPU *gpu, ULONG type, const RB2K_RECT *r)
{
    PRIMARY_COMMAND c;
    begin(&c, type);
    c.transfer.resource_id = gpu->memoryResource;
    c.transfer.box.x = r->left;
    c.transfer.box.y = r->top;
    c.transfer.box.w = r->right - r->left;
    c.transfer.box.h = r->bottom - r->top;
    c.transfer.box.d = 1;
    c.transfer.offset = ((ULONGLONG)r->top * gpu->memoryWidth + r->left) * sizeof(ULONG);
    c.transfer.stride = gpu->memoryWidth * sizeof(ULONG);
    return send(gpu, &c, sizeof(c.transfer), NULL);
}

static VP_STATUS flush(RB2K_GPU *gpu, const RB2K_RECT *r)
{
    PRIMARY_COMMAND c;
    begin(&c, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    c.flush.resource_id = gpu->memoryResource;
    c.flush.r.x = r->left;
    c.flush.r.y = r->top;
    c.flush.r.width = r->right - r->left;
    c.flush.r.height = r->bottom - r->top;
    return send(gpu, &c, sizeof(c.flush), NULL);
}

/* Send a changed rectangle of the desktop (NULL: all of it) and show it. */
VP_STATUS rb_memory_primary_upload(RB2K_GPU *gpu, const RB2K_RECT *rect)
{
    RB2K_RECT r;
    VP_STATUS status;
    if (!gpu->memoryWidth) return ERROR_NOT_READY;
    if (!gpu->memoryResource || !gpu->ready) {
        gpu->memoryResource = 0;
        status = create(gpu);
        if (status) { ++gpu->memoryFailures; return status; }
        rect = NULL;
    }
    r.left = r.top = 0;
    r.right = gpu->memoryWidth;
    r.bottom = gpu->memoryHeight;
    if (rect) {
        if (rect->left > r.left) r.left = rect->left;
        if (rect->top > r.top) r.top = rect->top;
        if (rect->right < r.right) r.right = rect->right;
        if (rect->bottom < r.bottom) r.bottom = rect->bottom;
    }
    if (r.left >= r.right || r.top >= r.bottom) return NO_ERROR;
    status = transfer(gpu, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, &r);
    if (!status) status = flush(gpu, &r);
    if (status) { ++gpu->memoryFailures; return status; }
    ++gpu->memoryUploads;
    gpu->memoryPixelsSent += (r.right - r.left) * (r.bottom - r.top);
    return NO_ERROR;
}

/* A Direct3D present to the desktop in memory: the host copies the frame's rectangle
 * into the desktop resource, which shows it, and the same pixels come back into the
 * pages for GDI (which reads them for screen copies and window moves). No staging,
 * so no limit below the desktop's own size, and no copy or upload in the guest. */
VP_STATUS rb_memory_primary_present(RB2K_GPU *gpu, ULONG image, ULONG srcX, ULONG srcY, const RB2K_RECT *to)
{
    RB2K_GPU_RESULT result;
    VP_STATUS status;
    if (!gpu->memoryWidth) return ERROR_NOT_READY;
    if (to->left >= to->right || to->top >= to->bottom ||
        to->right > gpu->memoryWidth || to->bottom > gpu->memoryHeight) return ERROR_INVALID_PARAMETER;
    if (!gpu->memoryResource || !gpu->ready) {
        status = rb_memory_primary_upload(gpu, NULL);
        if (status) return status;
    }
    if (!rb_gpu_copy_image_rect(gpu, &result, image, gpu->memoryResource, srcX, srcY, to->left, to->top,
                                to->right - to->left, to->bottom - to->top)) {
        ++gpu->memoryFailures;
        return result.status;
    }
    status = transfer(gpu, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, to);
    if (!status) status = flush(gpu, to);
    if (status) { ++gpu->memoryFailures; return status; }
    ++gpu->memoryPresents;
    return NO_ERROR;
}

/* A mode set to a 32-bit mode with the desktop in memory. */
VP_STATUS rb_memory_primary_show(RB2K_GPU *gpu, ULONG width, ULONG height, BOOLEAN clear)
{
    if (!width || !height || width > RB2K_MEMORY_PRIMARY_BYTES / sizeof(ULONG) / height)
        return ERROR_INVALID_PARAMETER;
    if (!gpu->memoryPixels) {
        gpu->memoryPixels = rb_system_pages(RB2K_MEMORY_PRIMARY_BYTES, &gpu->memoryMdl);
        if (!gpu->memoryPixels) return ERROR_NOT_ENOUGH_MEMORY;
        gpu->memoryBytes = RB2K_MEMORY_PRIMARY_BYTES;
    } else if (clear) {
        VideoPortZeroMemory(gpu->memoryPixels, width * height * sizeof(ULONG));
    }
    if (gpu->memoryResource && (gpu->memoryWidth != width || gpu->memoryHeight != height)) {
        /* The new resource takes scanout 0 before the old one goes. */
        ULONG old = gpu->memoryResource;
        VP_STATUS status;
        gpu->memoryResource = 0;
        gpu->memoryWidth = width;
        gpu->memoryHeight = height;
        status = rb_memory_primary_upload(gpu, NULL);
        if (gpu->ready && old) unref(gpu, old);
        return status;
    }
    gpu->memoryWidth = width;
    gpu->memoryHeight = height;
    return rb_memory_primary_upload(gpu, NULL);
}

/* Leaving the desktop in memory: the resource goes, the pages stay. Scanout
 * returns to VGA only with the transport reset that follows once nothing
 * else holds the transport. */
void rb_memory_primary_hide(RB2K_GPU *gpu)
{
    if (gpu->memoryResource && gpu->ready) {
        PRIMARY_COMMAND c;
        begin(&c, VIRTIO_GPU_CMD_SET_SCANOUT);   /* resource 0: nothing on scanout 0 */
        if (send(gpu, &c, sizeof(c.scanout), NULL)) gpu->ready = FALSE;
        else unref(gpu, gpu->memoryResource);
    }
    gpu->memoryResource = 0;
    gpu->memoryWidth = gpu->memoryHeight = 0;
}
