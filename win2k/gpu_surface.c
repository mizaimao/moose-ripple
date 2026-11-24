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
    /* The features switched on for this adapter. They use 32-bit pixels, so
     * the display driver offers them only in its 32-bit modes: during a mode
     * change it may ask before the miniport has set the new mode. */
    caps->gpuSurfaces = 0;
    if (gpu->ddGpuSurfaces) {
        caps->gpuSurfaces = RB2K_DD_HOST_SURFACES;
        if (gpu->plainSurfaces) caps->gpuSurfaces |= RB2K_DD_PLAIN_SURFACES;
        if (gpu->nativeD3D7)
            caps->gpuSurfaces |= RB2K_DD_NATIVE_D3D7 | RB2K_DD_Z16_STORAGE | RB2K_DD_Z16_RENDER;
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
    BOOLEAN depth16 = packet->IoControlCode == IOCTL_RB2K_DD_CREATE_Z16;
    if (packet->InputBufferLength != sizeof(*size) || packet->OutputBufferLength < sizeof(ULONG))
        return ERROR_INVALID_PARAMETER;
    if (gpu->primary.bitsPerPixel != 32 || (depth16 && (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces)))
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
    image->depth16 = depth16;
    if (!rb_gpu_create_image(gpu, &result, image, name, sizeof(name) - 1, FALSE)) {
        VideoPortZeroMemory(image, sizeof(*image));
        if (!gpu->ready || (!gpu->imageCount && !gpu->rvdContexts)) rb_gpu_stop(gpu);
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
    ULONG span[64], offset, y;
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
    } else if (gpu->presentCache != image->id) {
        /* Windows presents each visible rectangle separately. Read the whole
         * image back once; the following rectangles copy from staging. */
        LONGLONG read = rb_counter_ticks(NULL);
        if (!rb_gpu_transfer_box(gpu, &result, image->id, 0, 0,
                                 image->width, image->height, NULL)) return command_error(gpu, &result);
        if (gpu->d7Measure) gpu->d7Measure->fenceTicks += rb_counter_ticks(NULL) - read;
        gpu->presentCache = image->id;
        ++gpu->presentReadbacks;
        gpu->presentBytes += image->width * image->height * sizeof(ULONG);
    }
    for (y = 0; y < p->height; ++y) {
        PULONG dst = primary->pixels + (p->dstY + y) * primary->pitchPixels + p->dstX;
        ULONG remaining = p->width;
        offset = (p->srcY + y) * image->width + p->srcX;
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

/* One native draw to the image: validate it, find its depth buffer and
 * textures, and queue its commands. length is the record's byte count. */
static BOOLEAN draw_one(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                        const RB2K_D3D7_DRAW *d, ULONG length, VP_STATUS *status)
{
    RB2K_IMAGE *depth = NULL;
    RB2K_TEXTURE *texture = NULL, *texture1 = NULL;
    ULONG i, header = FIELD_OFFSET(RB2K_D3D7_DRAW, vertices);
    *status = ERROR_INVALID_PARAMETER;
    if (length < header || !d->count || d->count > RB2K_D3D7_VERTICES || d->count % 3 ||
        length != header + d->count*sizeof(d->vertices[0]) || d->target != image->id ||
        !rb_gpu_d7_valid(d) || image->depth16 || (d->depthState && !d->depth))
        return FALSE;
    *status = ERROR_INVALID_HANDLE;
    if (d->depth) {
        depth = find_image(gpu, d->depth);
        if (!depth || !depth->depth16) return FALSE;
    }
    for (i = 0; i < 2; ++i) {
        ULONG j;
        RB2K_TEXTURE *t;
        if (!rb_gpu_d7_samples(d, i)) continue;
        t = rb_gpu_texture(gpu, i ? d->texture1 : d->texture);
        if (!t) return FALSE;
        /* The display DLL attaches a texture when it uploads it here. */
        for (j = 0; j < RB2K_DD_SLOTS && t->contexts[j] != image->id; ++j) {}
        if (j == RB2K_DD_SLOTS) return FALSE;
        if (i) texture1 = t; else texture = t;
    }
    *status = ERROR_INVALID_PARAMETER;
    for (i = 0; i < d->count; ++i) {
        const RB2K_D3D7_VERTEX *v = d->vertices+i;
        if ((v->x & 0x7fffffff) >= 0x7f800000 || (v->y & 0x7fffffff) >= 0x7f800000 ||
            (v->z & 0x7fffffff) >= 0x7f800000 || !(v->rhw & 0x7fffffff) ||
            (v->rhw & 0x7fffffff) >= 0x7f800000)
            return FALSE;
    }
    *status = NO_ERROR;
    if ((depth && !rb_gpu_depth_target(gpu, result, image, depth)) ||
        !rb_gpu_d7_draw(gpu, result, image, d, texture, texture1)) {
        *status = command_error(gpu, result);
        return FALSE;
    }
    gpu->d7Frame.pending.triangles += d->count/3;
    return TRUE;
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
    if (image->depth16 && packet->IoControlCode != IOCTL_RB2K_DD_READ &&
        packet->IoControlCode != IOCTL_RB2K_DD_WRITE &&
        packet->IoControlCode != IOCTL_RB2K_DD_DESTROY)
        return ERROR_NOT_SUPPORTED;
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_FLUSH) {
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        if (image->frameFailed) return ERROR_INVALID_DATA;
        if (gpu->d7Frame.target == image && !rb_gpu_frame_flush(gpu, &result))
            return command_error(gpu, &result);
        return NO_ERROR;
    }
    /* A DirectDraw lock, blit, present or release can observe this image
     * before EndScene. Preserve the ordering of both sides of a blit. A
     * present reads the image back in its own context after the frame in
     * queue order, so it only queues the frame and does not wait for it. */
    /* Anything but a present may change an image the cached readback holds. */
    if (packet->IoControlCode != IOCTL_RB2K_DD_PRESENT) gpu->presentCache = 0;
    if (packet->IoControlCode == IOCTL_RB2K_DD_PRESENT) {
        if (!rb_gpu_order_image(gpu, &result, image)) return command_error(gpu, &result);
    } else if (packet->IoControlCode != IOCTL_RB2K_D3D7_CLEAR && packet->IoControlCode != IOCTL_RB2K_D3D7_DRAW &&
               packet->IoControlCode != IOCTL_RB2K_D3D7_DRAWS &&
               packet->IoControlCode != IOCTL_RB2K_D3D7_ZCLEAR) {
        if (!rb_gpu_sync_image(gpu, &result, image)) return command_error(gpu, &result);
    }
    if ((image->frameFailed || (image->depthOwner && image->depthOwner->frameFailed)) &&
        packet->IoControlCode != IOCTL_RB2K_DD_DESTROY) return ERROR_INVALID_DATA;
    bytes = image->width * image->height * (image->depth16 ? sizeof(USHORT) : sizeof(ULONG));
    switch (packet->IoControlCode) {
    case IOCTL_RB2K_D3D7_DRAW:
    case IOCTL_RB2K_D3D7_DRAWS: {
        VP_STATUS status;
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces || gpu->primary.bitsPerPixel != 32)
            return ERROR_NOT_SUPPORTED;
        if (packet->OutputBufferLength) return ERROR_INVALID_PARAMETER;
        if (packet->IoControlCode == IOCTL_RB2K_D3D7_DRAW) {
            if (!draw_one(gpu, &result, image, packet->InputBuffer, packet->InputBufferLength, &status))
                return status;
        } else {
            /* After the target id: byte count, record, until the end. */
            const UCHAR *p = (const UCHAR *)packet->InputBuffer + sizeof(ULONG);
            ULONG left = packet->InputBufferLength - sizeof(ULONG);
            if (packet->InputBufferLength > RB2K_D7_DRAWS_BYTES + sizeof(ULONG) || !left)
                return ERROR_INVALID_PARAMETER;
            while (left) {
                ULONG length;
                if (left < sizeof(ULONG)) return ERROR_INVALID_PARAMETER;
                length = *(const ULONG *)p;
                if (length > left - sizeof(ULONG) || length & 3) return ERROR_INVALID_PARAMETER;
                if (!draw_one(gpu, &result, image, (const RB2K_D3D7_DRAW *)(p + sizeof(ULONG)), length, &status))
                    return status;
                p += sizeof(ULONG) + length;
                left -= sizeof(ULONG) + length;
            }
        }
        success = TRUE;
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
    case IOCTL_RB2K_D3D7_ZCLEAR: {
        const RB2K_D3D7_ZCLEAR *z = packet->InputBuffer;
        RB2K_IMAGE *depth;
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
        if (packet->OutputBufferLength || packet->InputBufferLength != sizeof(*z) ||
            !z->flags || (z->flags & ~(RB2K_CLEAR_COLOR|RB2K_CLEAR_DEPTH)) ||
            ((z->flags & RB2K_CLEAR_DEPTH) && z->depthBits > 0x3f800000 && z->depthBits != 0x80000000))
            return ERROR_INVALID_PARAMETER;
        depth = find_image(gpu, z->depth);
        if (!depth || !depth->depth16) return ERROR_INVALID_HANDLE;
        if (!rb_gpu_depth_target(gpu, &result, image, depth)) return command_error(gpu, &result);
        success = rb_gpu_clear_buffers(gpu, &result, image, z->flags, z->color, z->depthBits);
        if (success) ++gpu->d7Frame.pending.clears;
        break;
    }
    case IOCTL_RB2K_D3D7_CLEAR:
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
        /* Both paths use the same virgl clear encoding. */
    case IOCTL_RB2K_DD_FILL: {
        RB2K_DD_FILL *fill = packet->InputBuffer;
        if (packet->InputBufferLength != sizeof(*fill) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_draw_image(gpu, &result, id, fill->color, (BOOLEAN)!image->hasSurface,
                                    packet->IoControlCode == IOCTL_RB2K_D3D7_CLEAR ? image : NULL,
                                    image->depthBuffer ? RB2K_D7_DEPTH_VIEW : 0);
        if (success) {
            if (packet->IoControlCode == IOCTL_RB2K_D3D7_CLEAR) ++gpu->d7Frame.pending.clears;
            else ++gpu->hostFills;
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
        if (destination->depth16) return ERROR_NOT_SUPPORTED;
        if (gpu->d7Frame.target == destination && !rb_gpu_frame_flush(gpu, &result))
            return command_error(gpu, &result);
        result.status = rb_gpu_wait_image(gpu, destination);
        if (result.status) {
            result.stage = RB2K_STAGE_TRANSFER;
            return command_error(gpu, &result);
        }
        if (destination->frameFailed) return ERROR_INVALID_DATA;
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
        if (image->depthOwner && !rb_gpu_depth_target(gpu, &result, image->depthOwner, NULL))
            return command_error(gpu, &result);
        if (image->depthBuffer && !rb_gpu_depth_target(gpu, &result, image, NULL))
            return command_error(gpu, &result);
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

/* Create a Direct3D texture or replace rows of one, attached to the target
 * that will sample it. Texture ids share the image counter. */
static VP_STATUS texture_request(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    RB2K_GPU_RESULT result;
    RB2K_TEXTURE *t;
    const RB2K_D3D7_TEXTURE *in = packet->InputBuffer;
    RB2K_IMAGE *image;
    ULONG bytes;
    if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_TEXTURE_FREE) {
        if (packet->InputBufferLength != sizeof(ULONG) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        t = rb_gpu_texture(gpu, *(PULONG)packet->InputBuffer);
        /* A transport reset has already released it. */
        if (!t) return NO_ERROR;
        return rb_gpu_texture_free(gpu, &result, t) ? NO_ERROR : command_error(gpu, &result);
    }
    if (packet->InputBufferLength < sizeof(*in) || packet->OutputBufferLength < sizeof(ULONG))
        return ERROR_INVALID_PARAMETER;
    if (!in->width || in->width > RB2K_D7_TEXTURE_SIDE || !in->height || in->height > RB2K_D7_TEXTURE_SIDE ||
        !in->levels || in->levels > RB2K_D7_LEVELS || in->level >= in->levels ||
        ((in->width | in->height) >> (in->levels - 1)) == 0)
        return ERROR_INVALID_PARAMETER;
    {
        /* Rows of one level, each level half the one above, at least one. */
        ULONG width = in->width >> in->level ? in->width >> in->level : 1;
        ULONG height = in->height >> in->level ? in->height >> in->level : 1;
        if (!in->rows || in->y >= height || in->rows > height - in->y) return ERROR_INVALID_PARAMETER;
        bytes = width * in->rows * sizeof(ULONG);
    }
    if (bytes > RB2K_D7_UPLOAD_BYTES || packet->InputBufferLength != sizeof(*in) + bytes)
        return ERROR_INVALID_PARAMETER;
    image = find_image(gpu, in->target);
    if (!image || image->depth16) return ERROR_INVALID_HANDLE;
    if (image->frameFailed) return ERROR_INVALID_DATA;
    if (in->id) {
        t = rb_gpu_texture(gpu, in->id);
        if (!t) return ERROR_INVALID_HANDLE;
        if (t->width != in->width || t->height != in->height || t->levels != in->levels)
            return ERROR_INVALID_PARAMETER;
    } else {
        /* Levels below the top add up to under a third of it. */
        ULONG i, size = in->width * in->height * sizeof(ULONG);
        if (in->levels > 1) size += size / 3 + 4 * sizeof(ULONG);
        if (in->y || in->level) return ERROR_INVALID_PARAMETER;
        if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
        for (i = 0; i < RB2K_D7_TEXTURES && gpu->textures[i].id; ++i) {}
        if (i == RB2K_D7_TEXTURES || size > RB2K_D7_TEXEL_BUDGET - gpu->textureBytes)
            return ERROR_NOT_ENOUGH_MEMORY;
        t = &gpu->textures[i];
        t->id = ++gpu->nextId;
        t->width = in->width;
        t->height = in->height;
        t->levels = in->levels;
        t->bytes = size;
        if (!rb_gpu_texture_create(gpu, &result, t)) {
            VideoPortZeroMemory(t, sizeof(*t));
            return command_error(gpu, &result);
        }
        ++gpu->textureCount;
        gpu->textureBytes += size;
    }
    if (!rb_gpu_texture_attach(gpu, &result, t, image) ||
        !rb_gpu_texture_upload(gpu, &result, t, image, in->level, in->y, in->rows, (PVOID)(in + 1)))
        return command_error(gpu, &result);
    ++gpu->uploads;
    *(PULONG)packet->OutputBuffer = t->id;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    VP_STATUS status;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_FLIGHT || packet->IoControlCode == IOCTL_RB2K_D3D7_COMPLETED) {
        status = rb_gpu_poll_scenes(gpu);
        if (status) {
            VideoPortInterlockedExchange(&gpu->busy, 0);
            return status;
        }
    }
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_FLIGHT) {
        status = ERROR_INVALID_PARAMETER;
        if (!packet->InputBufferLength && packet->OutputBufferLength == sizeof(gpu->d7Flight)) {
            gpu->d7Flight.limit = gpu->d7WaitCurrent ? 1 : RB2K_D7_FLIGHT_SLOTS;
            rb_counter_ticks(&gpu->d7Flight.frequency);
            *(RB2K_D7_FLIGHT *)packet->OutputBuffer = gpu->d7Flight;
            packet->StatusBlock->Information = sizeof(gpu->d7Flight);
            status = NO_ERROR;
        }
    } else if (packet->IoControlCode == IOCTL_RB2K_D3D7_COMPLETED) {
        status = ERROR_INVALID_PARAMETER;
        if (!packet->InputBufferLength && packet->OutputBufferLength == sizeof(gpu->d7Completed)) {
            *(RB2K_D3D7_COMPLETED *)packet->OutputBuffer = gpu->d7Completed;
            packet->StatusBlock->Information = sizeof(gpu->d7Completed);
            status = NO_ERROR;
        }
    } else if (packet->IoControlCode == IOCTL_RB2K_D3D7_PROFILE) {
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
             packet->IoControlCode == IOCTL_RB2K_DD_CREATE_Z16) status = create(gpu, packet);
    else {
        ULONG op = RB2K_D7_OPS;
        LONGLONG start = 0;
        switch (packet->IoControlCode) {
        case IOCTL_RB2K_D3D7_FLUSH: op = RB2K_D7_FLUSH; break;
        case IOCTL_RB2K_D3D7_CLEAR:
        case IOCTL_RB2K_D3D7_ZCLEAR: op = RB2K_D7_CLEAR; break;
        case IOCTL_RB2K_D3D7_DRAW:
        case IOCTL_RB2K_D3D7_DRAWS: op = RB2K_D7_DRAW; break;
        case IOCTL_RB2K_DD_READ: op = RB2K_D7_READ; break;
        case IOCTL_RB2K_DD_PRESENT: op = RB2K_D7_PRESENT; break;
        case IOCTL_RB2K_DD_WRITE:
        case IOCTL_RB2K_D3D7_TEXTURE: op = RB2K_D7_UPLOAD; break;
        }
        if (gpu->nativeD3D7 && op < RB2K_D7_OPS) {
            gpu->d7Measure = &gpu->d7Profile.op[op];
            start = rb_counter_ticks(NULL);
        }
        if (packet->IoControlCode == IOCTL_RB2K_D3D7_TEXTURE ||
            packet->IoControlCode == IOCTL_RB2K_D3D7_TEXTURE_FREE) status = texture_request(gpu, packet);
        else status = image_request(gpu, packet);
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
    VP_STATUS status, frameStatus = NO_ERROR;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    /* Mode changes lose these DirectDraw surfaces even if the transport
     * survives for an OpenGL client. Finish submitted scenes first. */
    if (gpu->d7Frame.target) {
        RB2K_GPU_RESULT r;
        if (!rb_gpu_frame_flush(gpu, &r)) frameStatus = r.status;
    }
    if (!frameStatus) frameStatus = rb_gpu_wait_image(gpu, NULL);
    rb_cursor_forget(gpu);
    if (gpu->rvdContexts && gpu->ready) {
        /* A mode change loses the DirectDraw surfaces but not OpenGL's
         * contexts: free the surfaces one by one and keep the transport. */
        RB2K_GPU_RESULT result;
        ULONG slot;
        for (slot = 0; slot < RB2K_DD_SLOTS && gpu->ready; ++slot) {
            RB2K_IMAGE *image = &gpu->images[slot];
            if (image->depthBuffer && !rb_gpu_depth_target(gpu, &result, image, NULL)) {
                frameStatus = result.status;
                gpu->ready = FALSE;
            }
        }
        for (slot = 0; slot < RB2K_DD_SLOTS; ++slot) {
            if (gpu->ready && gpu->images[slot].id) rb_gpu_destroy_image(gpu, &result, gpu->images[slot].id);
            VideoPortZeroMemory(&gpu->images[slot], sizeof(gpu->images[slot]));
        }
        gpu->imageCount = 0;
        for (slot = 0; slot < RB2K_D7_TEXTURES; ++slot)
            if (gpu->ready && gpu->textures[slot].id) rb_gpu_texture_free(gpu, &result, &gpu->textures[slot]);
        rb_gpu_texture_forget(gpu);
        /* Only a transport timeout leaves a reset to do. */
        status = gpu->ready ? NO_ERROR : rb_gpu_stop(gpu);
    } else {
        status = rb_gpu_stop(gpu);
    }
    gpu->primary.width = gpu->primary.height = gpu->primary.pitchPixels = gpu->primary.bitsPerPixel = 0;
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status ? status : frameStatus;
}
