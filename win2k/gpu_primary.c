/* The 32-bit desktop in guest memory (RipplePrimaryInMemory, issue 121).
 * QEMU's virtio-vga has 8 MB of VGA memory, too little for a 32-bit desktop
 * at 1920x1080. With the switch on, a 32-bit desktop lives in guest pages
 * instead. They back a host resource that scanout 0 shows, and what GDI and
 * the presents change goes to the host as rectangles: TRANSFER_TO_HOST_3D
 * from the pages, then RESOURCE_FLUSH. The pages are made once, for two pages
 * (a flip chain's) of the largest such mode, and kept. Each page has its own
 * resource, made for each mode and again after a transport reset, which drops
 * them (and with them the scanout); a flip puts the other one on scanout 0. */
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

static ULONG page_bytes(const RB2K_GPU *gpu)
{
    return gpu->memoryWidth * gpu->memoryHeight * sizeof(ULONG);
}

/* Page 0 is the desktop GDI draws; page 1 a flip chain's other page. */
static PULONG page_resource(RB2K_GPU *gpu, ULONG page)
{
    return page ? &gpu->memoryBack : &gpu->memoryResource;
}

static VP_STATUS show(RB2K_GPU *gpu, ULONG resource)
{
    PRIMARY_COMMAND c;
    begin(&c, VIRTIO_GPU_CMD_SET_SCANOUT);
    c.scanout.resource_id = resource;
    c.scanout.r.width = resource ? gpu->memoryWidth : 0;
    c.scanout.r.height = resource ? gpu->memoryHeight : 0;
    return send(gpu, &c, sizeof(c.scanout), NULL);
}

/* A page's physical runs go in the first staging segment: a page of the largest
 * desktop spans at most 2,251 memory pages, and a segment holds 4,096 runs. */
static VP_STATUS attach(RB2K_GPU *gpu, ULONG resource, ULONG page)
{
    RB2K_GPU_EXTENT extent;
    PRIMARY_COMMAND c;
    ULONG runs;
    VP_STATUS status = rb_gpu_reserve_staging(gpu, RB2K_SEGMENT_BYTES);
    if (status) return status;
    runs = rb_mdl_runs(gpu->memoryMdl, page * page_bytes(gpu), page_bytes(gpu),
                       (RB2K_PAGE_RUN *)gpu->staging[0].data, RB2K_SEGMENT_BYTES / sizeof(RB2K_PAGE_RUN));
    if (!runs) return ERROR_NOT_ENOUGH_MEMORY;
    extent.bus = gpu->staging[0].bus;
    extent.length = runs * sizeof(RB2K_PAGE_RUN);
    begin(&c, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    c.attach.resource_id = resource;
    c.attach.nr_entries = runs;
    return send(gpu, &c, sizeof(c.attach), &extent);
}

/* A page's resource, on scanout 0 if that page is the visible one. */
static VP_STATUS create(RB2K_GPU *gpu, ULONG page)
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
    status = attach(gpu, id, page);
    if (!status && page == gpu->memoryVisible) status = show(gpu, id);
    if (status) {
        if (gpu->ready) unref(gpu, id);
        return status;
    }
    *page_resource(gpu, page) = id;
    return NO_ERROR;
}

/* Between a page and its resource: the page is its backing, rows of memoryWidth pixels. */
static VP_STATUS transfer(RB2K_GPU *gpu, ULONG type, ULONG page, const RB2K_RECT *r)
{
    PRIMARY_COMMAND c;
    begin(&c, type);
    c.transfer.resource_id = *page_resource(gpu, page);
    c.transfer.box.x = r->left;
    c.transfer.box.y = r->top;
    c.transfer.box.w = r->right - r->left;
    c.transfer.box.h = r->bottom - r->top;
    c.transfer.box.d = 1;
    /* The resource's backing starts at its page. */
    c.transfer.offset = ((ULONGLONG)r->top * gpu->memoryWidth + r->left) * sizeof(ULONG);
    c.transfer.stride = gpu->memoryWidth * sizeof(ULONG);
    return send(gpu, &c, sizeof(c.transfer), NULL);
}

static VP_STATUS flush(RB2K_GPU *gpu, ULONG page, const RB2K_RECT *r)
{
    PRIMARY_COMMAND c;
    begin(&c, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    c.flush.resource_id = *page_resource(gpu, page);
    c.flush.r.x = r->left;
    c.flush.r.y = r->top;
    c.flush.r.width = r->right - r->left;
    c.flush.r.height = r->bottom - r->top;
    return send(gpu, &c, sizeof(c.flush), NULL);
}

/* The page's resource, made (with all of the page sent) if the host has none. A
 * transport reset dropped both, and with them the scanout. */
static VP_STATUS ensure(RB2K_GPU *gpu, ULONG page)
{
    RB2K_RECT all;
    VP_STATUS status;
    if (!gpu->memoryResource || !gpu->ready) {
        gpu->memoryResource = gpu->memoryBack = 0;
        gpu->memoryVisible = 0;
    }
    if (*page_resource(gpu, page)) return NO_ERROR;
    status = create(gpu, page);
    if (status) return status;
    all.left = all.top = 0;
    all.right = gpu->memoryWidth;
    all.bottom = gpu->memoryHeight;
    status = transfer(gpu, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, page, &all);
    return status ? status : flush(gpu, page, &all);
}

/* Send a changed rectangle of a page (NULL: all of it) and show it. */
VP_STATUS rb_memory_primary_upload(RB2K_GPU *gpu, ULONG page, const RB2K_RECT *rect)
{
    RB2K_RECT r;
    VP_STATUS status;
    BOOLEAN made;
    if (!gpu->memoryWidth) return ERROR_NOT_READY;
    if (page > 1) return ERROR_INVALID_PARAMETER;
    made = !*page_resource(gpu, page) || !gpu->memoryResource || !gpu->ready;
    status = ensure(gpu, page);
    if (status) { ++gpu->memoryFailures; return status; }
    if (made) return NO_ERROR;   /* all of the page went with the new resource */
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
    status = transfer(gpu, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, page, &r);
    if (!status) status = flush(gpu, page, &r);
    if (status) { ++gpu->memoryFailures; return status; }
    ++gpu->memoryUploads;
    gpu->memoryPixelsSent += (r.right - r.left) * (r.bottom - r.top);
    return NO_ERROR;
}

/* A Direct3D present to the desktop in memory: the host copies the frame's rectangle
 * into the page's resource, which shows it, and the same pixels come back into the
 * page for GDI (which reads them for screen copies and window moves) and for locks.
 * No staging, so no limit below the desktop's own size, and no copy or upload in the
 * guest. to: in the pages' rows, the second page below the first. */
VP_STATUS rb_memory_primary_present(RB2K_GPU *gpu, ULONG image, ULONG srcX, ULONG srcY, const RB2K_RECT *to)
{
    RB2K_GPU_RESULT result;
    RB2K_RECT r = *to;
    ULONG page;
    VP_STATUS status;
    if (!gpu->memoryWidth) return ERROR_NOT_READY;
    page = r.top >= gpu->memoryHeight ? 1 : 0;
    r.top -= page * gpu->memoryHeight;
    r.bottom -= page * gpu->memoryHeight;
    if (r.left >= r.right || r.top >= r.bottom || r.right > gpu->memoryWidth || r.bottom > gpu->memoryHeight)
        return ERROR_INVALID_PARAMETER;
    status = ensure(gpu, page);
    if (status) return status;
    if (!rb_gpu_copy_image_rect(gpu, &result, image, *page_resource(gpu, page), srcX, srcY, r.left, r.top,
                                r.right - r.left, r.bottom - r.top)) {
        ++gpu->memoryFailures;
        return result.status;
    }
    status = transfer(gpu, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, page, &r);
    if (!status) status = flush(gpu, page, &r);
    if (status) { ++gpu->memoryFailures; return status; }
    ++gpu->memoryPresents;
    return NO_ERROR;
}

/* A flip: the page's resource on scanout 0. */
VP_STATUS rb_memory_primary_flip(RB2K_GPU *gpu, ULONG page)
{
    RB2K_RECT all;
    VP_STATUS status;
    if (!gpu->memoryWidth) return ERROR_NOT_READY;
    if (page > 1) return ERROR_INVALID_PARAMETER;
    status = ensure(gpu, page);
    if (status) return status;
    if (page == gpu->memoryVisible) return NO_ERROR;
    status = show(gpu, *page_resource(gpu, page));
    all.left = all.top = 0;
    all.right = gpu->memoryWidth;
    all.bottom = gpu->memoryHeight;
    if (!status) status = flush(gpu, page, &all);
    if (status) { ++gpu->memoryFailures; return status; }
    gpu->memoryVisible = page;
    return NO_ERROR;
}

/* A flip chain's new back page starts black; page 1 is not visible here. */
void rb_memory_primary_clear_back(RB2K_GPU *gpu)
{
    if (!gpu->memoryWidth || gpu->memoryVisible) return;
    VideoPortZeroMemory((PUCHAR)gpu->memoryPixels + page_bytes(gpu), page_bytes(gpu));
    if (gpu->memoryBack && gpu->ready) unref(gpu, gpu->memoryBack);
    gpu->memoryBack = 0;   /* made again, with all of the page, when it is used */
}

/* A mode set to a 32-bit mode with the desktop in memory. */
VP_STATUS rb_memory_primary_show(RB2K_GPU *gpu, ULONG width, ULONG height, BOOLEAN clear)
{
    ULONG old = 0, oldBack = 0;
    VP_STATUS status;
    if (!width || !height || width > RB2K_MEMORY_PRIMARY_BYTES / 2 / sizeof(ULONG) / height)
        return ERROR_INVALID_PARAMETER;
    if (!gpu->memoryPixels) {
        gpu->memoryPixels = rb_system_pages(RB2K_MEMORY_PRIMARY_BYTES, &gpu->memoryMdl);
        if (!gpu->memoryPixels) return ERROR_NOT_ENOUGH_MEMORY;
        gpu->memoryBytes = RB2K_MEMORY_PRIMARY_BYTES;
    } else if (clear) {
        VideoPortZeroMemory(gpu->memoryPixels, 2 * width * height * sizeof(ULONG));
    }
    if (gpu->ready) {
        /* New resources take scanout 0 before the old ones go. */
        old = gpu->memoryResource;
        oldBack = gpu->memoryBack;
    }
    gpu->memoryResource = gpu->memoryBack = 0;
    gpu->memoryVisible = 0;
    gpu->memoryWidth = width;
    gpu->memoryHeight = height;
    status = rb_memory_primary_upload(gpu, 0, NULL);
    if (gpu->ready && old) unref(gpu, old);
    if (gpu->ready && oldBack) unref(gpu, oldBack);
    return status;
}

/* Leaving the desktop in memory: the resources go, the pages stay. Scanout
 * returns to VGA only with the transport reset that follows once nothing
 * else holds the transport. */
void rb_memory_primary_hide(RB2K_GPU *gpu)
{
    if (gpu->memoryResource && gpu->ready) {
        if (show(gpu, 0)) gpu->ready = FALSE;   /* resource 0: nothing on scanout 0 */
        else unref(gpu, gpu->memoryResource);
    }
    if (gpu->memoryBack && gpu->ready) unref(gpu, gpu->memoryBack);
    gpu->memoryResource = gpu->memoryBack = 0;
    gpu->memoryVisible = 0;
    gpu->memoryWidth = gpu->memoryHeight = 0;
}
