/* The miniport serializes this bounded resource table with the GPU probe and
 * mode changes. Applications reach it through the display DLL's DirectDraw
 * callbacks. Every byte returned by READ belongs to the requested surface. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include "gpu_memory.h"

static VP_STATUS query(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    RB2K_DD_CAPS *caps = packet->OutputBuffer;
    RB2K_GPU_RESULT result;
    VP_STATUS status, cleanup;
    if (packet->InputBufferLength || packet->OutputBufferLength < sizeof(*caps))
        return ERROR_INVALID_PARAMETER;
    if (!gpu->ready && !gpu->tested) {
        status = rb_gpu_start(gpu, &result);
        cleanup = rb_gpu_stop(gpu);
        if (status) return status;
        if (cleanup) return cleanup;
        gpu->tested = TRUE;
    }
    caps->maxBytes = RB2K_DD_MAX_BYTES;
    caps->slots = RB2K_DD_SLOTS;
    caps->hostFills = gpu->hostFills;
    caps->readbacks = gpu->readbacks;
    caps->uploads = gpu->uploads;
    caps->hostCopies = gpu->hostCopies;
    caps->presents = gpu->presents;
    caps->presentReadbacks = gpu->presentReadbacks;
    caps->presentBytes = gpu->presentBytes;
    /* Optional host textures and their primary-blit path use 32-bit pixels. */
    caps->gpuSurfaces = 0;
    if (gpu->ddGpuSurfaces && gpu->primary.bitsPerPixel == 32) {
        caps->gpuSurfaces = RB2K_DD_HOST_SURFACES;
        if (gpu->nativeD3D7) caps->gpuSurfaces |= RB2K_DD_NATIVE_D3D7;
    }
    packet->StatusBlock->Information = sizeof(*caps);
    return NO_ERROR;
}

static VP_STATUS create(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    static const char name[] = "rb2k-ddraw";
    RB2K_DD_CREATE *size = packet->InputBuffer;
    RB2K_GPU_RESULT result;
    RB2K_IMAGE *image;
    VP_STATUS status;
    ULONG slot;
    BOOLEAN texture = packet->IoControlCode == IOCTL_RB2K_DD_CREATE_TEXTURE;
    if (packet->InputBufferLength != sizeof(*size) || packet->OutputBufferLength < sizeof(ULONG))
        return ERROR_INVALID_PARAMETER;
    if (gpu->primary.bitsPerPixel != 32 || (texture && (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces)))
        return ERROR_NOT_SUPPORTED;
    if (!size->width || size->width > RB2K_DD_MAX_WIDTH ||
        !size->height || size->height > RB2K_DD_MAX_HEIGHT)
        return ERROR_INVALID_PARAMETER;
    if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
    for (slot = 0; slot < RB2K_DD_SLOTS; ++slot) if (!gpu->images[slot].id) break;
    if (slot == RB2K_DD_SLOTS) return ERROR_NOT_ENOUGH_MEMORY;
    /* OpenGL contexts (gpu_rvd.c) may already hold the transport. */
    if (!gpu->ready) {
        status = rb_gpu_start(gpu, &result);
        if (status) {
            rb_gpu_stop(gpu);
            return status;
        }
    }
    image = &gpu->images[slot];
    image->id = ++gpu->nextId;
    image->width = size->width;
    image->height = size->height;
    image->sampled = texture;
    if (!rb_gpu_create_image(gpu, &result, image, name, sizeof(name) - 1, FALSE)) {
        if (!gpu->rvdContexts || !gpu->ready) rb_gpu_stop(gpu);
        return result.status;
    }
    ++gpu->imageCount;
    *(PULONG)packet->OutputBuffer = image->id;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

static RB2K_IMAGE *find_image(RB2K_GPU *gpu, ULONG id)
{
    ULONG i;
    if (id) {
        for (i = 0; i < RB2K_DD_SLOTS; ++i)
            if (gpu->images[i].id == id) return &gpu->images[i];
    }
    return NULL;
}

static VP_STATUS command_error(RB2K_GPU *gpu, const RB2K_GPU_RESULT *result)
{
    /* Staging allocation precedes submission and leaves other images usable. */
    /* A late completion must not touch recycled storage. A host error
     * reply leaves no command outstanding, so OpenGL contexts may stay. */
    if ((result->stage != RB2K_STAGE_BACKING || result->status != ERROR_NOT_ENOUGH_MEMORY) &&
        (!gpu->rvdContexts || !gpu->ready))
        rb_gpu_stop(gpu);
    return result->status;
}

static VP_STATUS present(RB2K_GPU *gpu, const RB2K_IMAGE *image, const RB2K_DD_PRESENT *p)
{
    RB2K_PRIMARY *primary = &gpu->primary;
    RB2K_GPU_RESULT result;
    BOOLEAN solid = !image->defined || image->uniform;
    ULONG span[64], offset = 0, y;
    const ULONG segmentPixels = RB2K_SEGMENT_BYTES / sizeof(ULONG);
    if (primary->bitsPerPixel != 32 || !primary->pixels || !primary->height || !primary->width ||
        primary->pitchPixels < primary->width ||
        primary->pitchPixels > primary->bytes / sizeof(ULONG) / primary->height)
        return ERROR_NOT_READY;
    if (!p->width || !p->height || p->width > image->width || p->height > image->height ||
        p->srcX > image->width - p->width || p->srcY > image->height - p->height ||
        p->width > primary->width || p->height > primary->height ||
        p->dstX > primary->width - p->width || p->dstY > primary->height - p->height)
        return ERROR_INVALID_PARAMETER;
    if (solid) {
        ULONG i, color = image->defined ? image->color : 0;
        for (i = 0; i < sizeof(span) / sizeof(span[0]); ++i) span[i] = color;
    } else {
        if (!rb_gpu_transfer_box(gpu, &result, image->id, p->srcX, p->srcY,
                                 p->width, p->height, NULL)) return command_error(gpu, &result);
        ++gpu->presentReadbacks;
        gpu->presentBytes += p->width * p->height * sizeof(ULONG);
    }
    for (y = 0; y < p->height; ++y) {
        PULONG dst = primary->pixels + (p->dstY + y) * primary->pitchPixels + p->dstX;
        ULONG remaining = p->width;
        while (remaining) {
            PULONG src = span;
            ULONG count = sizeof(span) / sizeof(span[0]);
            if (!solid) {
                ULONG inSegment = offset % segmentPixels;
                src = (PULONG)gpu->staging[offset / segmentPixels].data + inSegment;
                count = segmentPixels - inSegment;
            }
            if (count > remaining) count = remaining;
            VideoPortWriteRegisterBufferUlong(dst, src, count);
            dst += count;
            offset += count;
            remaining -= count;
        }
    }
    ++gpu->presents;
    return NO_ERROR;
}

static VP_STATUS image_request(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    ULONG id, bytes;
    RB2K_IMAGE *image;
    RB2K_GPU_RESULT result;
    BOOLEAN success;
    if (packet->InputBufferLength < sizeof(id)) return ERROR_INVALID_PARAMETER;
    id = *(PULONG)packet->InputBuffer;
    image = find_image(gpu, id);
    if (!image) return ERROR_INVALID_HANDLE;
    bytes = image->width * image->height * sizeof(ULONG);
    switch (packet->IoControlCode) {
    case IOCTL_RB2K_D3D7_TRIANGLES: {
        const RB2K_D3D7_BATCH *b = packet->InputBuffer;
        ULONG i;
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces || gpu->primary.bitsPerPixel != 32)
            return ERROR_NOT_SUPPORTED;
        if (packet->OutputBufferLength || packet->InputBufferLength < FIELD_OFFSET(RB2K_D3D7_BATCH,vertices) ||
            !b->count || b->count > RB2K_D3D7_VERTICES || b->count % 3 ||
            packet->InputBufferLength != FIELD_OFFSET(RB2K_D3D7_BATCH,vertices)+b->count*sizeof(b->vertices[0]))
            return ERROR_INVALID_PARAMETER;
        for (i = 0; i < b->count; ++i) {
            const RB2K_D3D7_VERTEX *v = &b->vertices[i];
            if ((v->x & 0x7fffffff) >= 0x7f800000 || (v->y & 0x7fffffff) >= 0x7f800000 ||
                (v->z & 0x7fffffff) >= 0x7f800000 || !v->rhw || v->rhw >= 0x7f800000)
                return ERROR_INVALID_PARAMETER;
        }
        success = rb_gpu_triangles(gpu, &result, image, b);
        break;
    }
    case IOCTL_RB2K_DD_UNIFORM: {
        RB2K_DD_UNIFORM *answer = packet->OutputBuffer;
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength < sizeof(*answer))
            return ERROR_INVALID_PARAMETER;
        answer->known = !image->defined || image->uniform;
        answer->color = image->defined && image->uniform ? image->color : 0;
        packet->StatusBlock->Information = sizeof(*answer);
        return NO_ERROR;
    }
    case IOCTL_RB2K_D3D7_CLEAR:
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
        /* The control uses the same single fenced fill as DirectDraw. */
    case IOCTL_RB2K_DD_FILL: {
        RB2K_DD_FILL *fill = packet->InputBuffer;
        if (packet->InputBufferLength != sizeof(*fill) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_draw_image(gpu, &result, id, fill->color, (BOOLEAN)!image->hasSurface);
        if (success) {
            ++gpu->hostFills;
            image->hasSurface = image->defined = image->uniform = TRUE;
            image->color = fill->color;
        }
        break;
    }
    case IOCTL_RB2K_DD_READ:
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength < bytes)
            return ERROR_INVALID_PARAMETER;
        /* Never expose uninitialized renderer memory or a reused DMA page.
         * Every supported write defines the entire image, including uploads. */
        if (!image->defined) {
            VideoPortZeroMemory(packet->OutputBuffer, bytes);
            packet->StatusBlock->Information = bytes;
            return NO_ERROR;
        }
        success = rb_gpu_transfer_image(gpu, &result, image, packet->OutputBuffer, TRUE);
        if (success) {
            packet->StatusBlock->Information = bytes;
            ++gpu->readbacks;
        }
        break;
    case IOCTL_RB2K_DD_WRITE:
        if (packet->InputBufferLength != sizeof(id) + bytes || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_transfer_image(gpu, &result, image, (PUCHAR)packet->InputBuffer + sizeof(id), FALSE);
        if (success) {
            image->defined = TRUE;
            image->uniform = FALSE;
            ++gpu->uploads;
        }
        break;
    case IOCTL_RB2K_DD_COPY: {
        RB2K_DD_COPY *copy = packet->InputBuffer;
        RB2K_IMAGE *destination;
        RB2K_RECT region = {0};
        if (packet->InputBufferLength != sizeof(*copy) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        destination = find_image(gpu, copy->destination);
        if (!destination) return ERROR_INVALID_HANDLE;
        if (image->width != destination->width || image->height != destination->height)
            return ERROR_INVALID_PARAMETER;
        if (image == destination) return NO_ERROR;
        /* A fresh surface is logically zero until its first complete write. */
        if (!image->defined) {
            destination->defined = FALSE;
            return NO_ERROR;
        }
        region.right = image->width;
        region.bottom = image->height;
        success = rb_gpu_copy_rects(gpu, &result, destination->id, image->id, &region, 1, 0, 0, NULL);
        if (success) {
            destination->defined = TRUE;
            destination->uniform = image->uniform;
            destination->color = image->color;
            ++gpu->hostCopies;
        }
        break;
    }
    case IOCTL_RB2K_DD_PRESENT:
        if (packet->InputBufferLength != sizeof(RB2K_DD_PRESENT) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        return present(gpu, image, packet->InputBuffer);
    case IOCTL_RB2K_DD_DESTROY:
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_destroy_image(gpu, &result, id);
        if (success) {
            VideoPortZeroMemory(image, sizeof(*image));
            if (!--gpu->imageCount && !gpu->rvdContexts) return rb_gpu_stop(gpu);
        }
        break;
    default:
        return ERROR_INVALID_FUNCTION;
    }
    if (success) return NO_ERROR;
    return command_error(gpu, &result);
}

VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    VP_STATUS status;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_PROFILE) {
        status = ERROR_INVALID_PARAMETER;
        if (!packet->InputBufferLength && packet->OutputBufferLength == sizeof(gpu->d7Profile)) {
            rb_counter_ticks(&gpu->d7Profile.frequency);
            VideoPortMoveMemory(packet->OutputBuffer, &gpu->d7Profile, sizeof(gpu->d7Profile));
            packet->StatusBlock->Information = sizeof(gpu->d7Profile);
            status = NO_ERROR;
        }
    }
    else if (packet->IoControlCode == IOCTL_RB2K_DD_QUERY) status = query(gpu, packet);
    else if (packet->IoControlCode == IOCTL_RB2K_DD_CREATE ||
             packet->IoControlCode == IOCTL_RB2K_DD_CREATE_TEXTURE) status = create(gpu, packet);
    else {
        ULONG op = RB2K_D7_OPS;
        LONGLONG start = 0;
        switch (packet->IoControlCode) {
        case IOCTL_RB2K_D3D7_CLEAR: op = RB2K_D7_CLEAR; break;
        case IOCTL_RB2K_D3D7_TRIANGLES: op = RB2K_D7_DRAW; break;
        case IOCTL_RB2K_DD_READ: op = RB2K_D7_READ; break;
        case IOCTL_RB2K_DD_WRITE: op = RB2K_D7_UPLOAD; break;
        }
        if (gpu->nativeD3D7 && op < RB2K_D7_OPS) {
            gpu->d7Measure = &gpu->d7Profile.op[op];
            start = rb_counter_ticks(NULL);
        }
        status = image_request(gpu, packet);
        if (gpu->d7Measure) {
            gpu->d7Measure->ticks += rb_counter_ticks(NULL)-start;
            ++gpu->d7Measure->calls;
            if (status) ++gpu->d7Measure->failed;
            gpu->d7Measure = NULL;
        }
    }
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status;
}

VP_STATUS rb_gpu_surface_reset(RB2K_GPU *gpu)
{
    VP_STATUS status;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    rb_cursor_forget(gpu);
    if (gpu->rvdContexts && gpu->ready) {
        /* A mode change loses the DirectDraw surfaces but not OpenGL's
         * contexts: free the surfaces one by one and keep the transport. */
        RB2K_GPU_RESULT result;
        ULONG slot;
        for (slot = 0; slot < RB2K_DD_SLOTS; ++slot) {
            if (gpu->images[slot].id) rb_gpu_destroy_image(gpu, &result, gpu->images[slot].id);
            VideoPortZeroMemory(&gpu->images[slot], sizeof(gpu->images[slot]));
        }
        gpu->imageCount = 0;
        /* Only a transport timeout leaves a reset to do. */
        status = gpu->ready ? NO_ERROR : rb_gpu_stop(gpu);
    } else {
        status = rb_gpu_stop(gpu);
    }
    gpu->primary.width = gpu->primary.height = gpu->primary.pitchPixels = gpu->primary.bitsPerPixel = 0;
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status;
}
