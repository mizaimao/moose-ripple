/* The miniport serializes this bounded resource table with the GPU probe and
 * mode changes. Applications reach it through the display DLL's DirectDraw
 * callbacks. Every byte returned by READ belongs to the requested surface. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"

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
    if (packet->InputBufferLength != sizeof(*size) || packet->OutputBufferLength < sizeof(ULONG))
        return ERROR_INVALID_PARAMETER;
    if (!size->width || size->width > RB2K_DD_MAX_WIDTH ||
        !size->height || size->height > RB2K_DD_MAX_HEIGHT)
        return ERROR_INVALID_PARAMETER;
    if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
    for (slot = 0; slot < RB2K_DD_SLOTS; ++slot) if (!gpu->images[slot].id) break;
    if (slot == RB2K_DD_SLOTS) return ERROR_NOT_ENOUGH_MEMORY;
    if (!gpu->imageCount) {
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
    if (!rb_gpu_create_image(gpu, &result, image, name, sizeof(name) - 1)) {
        rb_gpu_stop(gpu);
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
    case IOCTL_RB2K_DD_FILL: {
        RB2K_DD_FILL *fill = packet->InputBuffer;
        if (packet->InputBufferLength != sizeof(*fill) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_draw_image(gpu, &result, id, fill->color, (BOOLEAN)!image->hasSurface);
        if (success) {
            ++gpu->hostFills;
            image->hasSurface = image->defined = TRUE;
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
            ++gpu->uploads;
        }
        break;
    case IOCTL_RB2K_DD_COPY: {
        RB2K_DD_COPY *copy = packet->InputBuffer;
        RB2K_IMAGE *destination;
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
        success = rb_gpu_copy_image(gpu, &result, image, destination);
        if (success) {
            destination->defined = TRUE;
            ++gpu->hostCopies;
        }
        break;
    }
    case IOCTL_RB2K_DD_DESTROY:
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_destroy_image(gpu, &result, id);
        if (success) {
            VideoPortZeroMemory(image, sizeof(*image));
            if (!--gpu->imageCount) return rb_gpu_stop(gpu);
        }
        break;
    default:
        return ERROR_INVALID_FUNCTION;
    }
    if (success) return NO_ERROR;
    /* Staging allocation precedes the first command of a transfer. Existing
     * textures can still be filled, copied or destroyed under memory pressure. */
    if (result.stage == RB2K_STAGE_BACKING && result.status == ERROR_NOT_ENOUGH_MEMORY)
        return result.status;
    /* A timed-out command may complete late. Invalidate every host object
     * on this device before reusing descriptors, pages or resource IDs. */
    rb_gpu_stop(gpu);
    return result.status;
}

VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    VP_STATUS status;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    if (packet->IoControlCode == IOCTL_RB2K_DD_QUERY) status = query(gpu, packet);
    else if (packet->IoControlCode == IOCTL_RB2K_DD_CREATE) status = create(gpu, packet);
    else status = image_request(gpu, packet);
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status;
}

VP_STATUS rb_gpu_surface_reset(RB2K_GPU *gpu)
{
    VP_STATUS status;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    status = rb_gpu_stop(gpu);
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status;
}
