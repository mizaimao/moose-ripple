/* A separate virtio queue moves the pointer without painting the desktop. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include <linux/virtio_gpu.h>

static VP_STATUS position(RB2K_GPU *gpu, BOOLEAN newShape)
{
    RB2K_CURSOR_REQUEST *p = &gpu->cursorShape.info;
    struct virtio_gpu_update_cursor cmd;
    BOOLEAN show = gpu->cursorValid && p->x >= 0 && p->y >= 0 &&
        (ULONG)p->x < gpu->primary.width && (ULONG)p->y < gpu->primary.height;
    BOOLEAN move = show && gpu->cursorVisible && !newShape;
    VP_STATUS status;
    if (!show && !gpu->cursorVisible) return NO_ERROR;
    VideoPortZeroMemory(&cmd, sizeof(cmd));
    cmd.hdr.type = move ? VIRTIO_GPU_CMD_MOVE_CURSOR : VIRTIO_GPU_CMD_UPDATE_CURSOR;
    if (show) {
        cmd.pos.x = (ULONG)p->x;
        cmd.pos.y = (ULONG)p->y;
        cmd.hot_x = p->hotX;
        cmd.hot_y = p->hotY;
        cmd.resource_id = gpu->cursorResource;
    }
    status = rb_gpu_cursor_command(gpu, &cmd, sizeof(cmd));
    if (status) return status;
    gpu->cursorVisible = show;
    if (!show) ++gpu->cursorStats.hides;
    else if (move) ++gpu->cursorStats.moves;
    else ++gpu->cursorStats.updates;
    return NO_ERROR;
}

VP_STATUS rb_cursor_restore(RB2K_GPU *gpu)
{
    RB2K_GPU_RESULT result;
    VP_STATUS status;
    if (!gpu->ready) {
        /* A lost rendering queue is recovered by its owner. */
        if (gpu->imageCount || gpu->rvdContexts) return ERROR_NOT_READY;
        status = rb_gpu_start(gpu, &result);
        if (status) return status;
    }
    status = rb_gpu_cursor_pixels(gpu);
    return status ? status : position(gpu, TRUE);
}

void rb_cursor_forget(RB2K_GPU *gpu)
{
    gpu->cursorValid = FALSE;
    if (gpu->cursorVisible && gpu->ready && position(gpu, TRUE)) ++gpu->cursorStats.failed;
}

static VP_STATUS handle_cursor(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    const RB2K_CURSOR_REQUEST *r = packet->InputBuffer;
    RB2K_CURSOR_STATS *stats = &gpu->cursorStats;
    RB2K_CURSOR_REQUEST *saved = &gpu->cursorShape.info;
    VP_STATUS status;
    ULONG i, hash;
    if (packet->InputBufferLength < sizeof(*r)) return ERROR_INVALID_PARAMETER;
    if (r->op == RB2K_CURSOR_QUERY) {
        if (packet->InputBufferLength != sizeof(*r) || (packet->OutputBufferLength != sizeof(*stats) &&
            packet->OutputBufferLength != FIELD_OFFSET(RB2K_CURSOR_STATS, lastDecline)))
            return ERROR_INVALID_PARAMETER;
        stats->enabled = gpu->cursorEnabled && gpu->common != NULL;
        stats->visible = gpu->cursorVisible;
        stats->x = saved->x; stats->y = saved->y;
        VideoPortMoveMemory(packet->OutputBuffer, stats, packet->OutputBufferLength);
        packet->StatusBlock->Information = packet->OutputBufferLength;
        return NO_ERROR;
    }
    if (packet->OutputBufferLength || !gpu->cursorEnabled) return ERROR_NOT_SUPPORTED;
    if (r->op == RB2K_CURSOR_SET) {
        if (packet->InputBufferLength != sizeof(gpu->cursorShape) || r->kind >= RB2K_CURSOR_TYPES ||
            !r->width || !r->height || r->width > RB2K_CURSOR_EDGE || r->height > RB2K_CURSOR_EDGE ||
            r->hotX >= r->width || r->hotY >= r->height || gpu->primary.bitsPerPixel != 32)
            return ERROR_INVALID_PARAMETER;
        VideoPortMoveMemory(&gpu->cursorShape, (PVOID)r, sizeof(gpu->cursorShape));
        gpu->cursorValid = TRUE;
        status = rb_cursor_restore(gpu);
        if (status) { rb_cursor_forget(gpu); ++stats->failed; return status; }
        ++stats->accepted[saved->kind];
        stats->width = saved->width; stats->height = saved->height;
        stats->hotX = saved->hotX; stats->hotY = saved->hotY;
        hash = 2166136261UL;
        for (i = 0; i < RB2K_CURSOR_PIXELS; ++i)
            hash = (hash ^ gpu->cursorShape.pixels[i]) * 16777619UL;
        stats->hash = hash;
        return NO_ERROR;
    }
    if (r->op == RB2K_CURSOR_DROP && packet->InputBufferLength == sizeof(RB2K_CURSOR_DECLINE))
        stats->lastDecline = ((RB2K_CURSOR_DECLINE *)r)->why;
    else if (packet->InputBufferLength != sizeof(*r)) return ERROR_INVALID_PARAMETER;
    if (r->op == RB2K_CURSOR_DROP) {
        if (r->kind != RB2K_CURSOR_TRANSPARENT) ++stats->declined;
        if (r->kind == RB2K_CURSOR_INVERT) ++stats->inverted;
        if (gpu->cursorValid && saved->owner != r->owner) return NO_ERROR;
        gpu->cursorValid = FALSE;
        status = position(gpu, TRUE);
    } else if (r->op == RB2K_CURSOR_MOVE) {
        if (!gpu->cursorValid || saved->owner != r->owner) return NO_ERROR;
        saved->x = r->x; saved->y = r->y;
        status = gpu->ready && gpu->cursorResource ? position(gpu, FALSE) : rb_cursor_restore(gpu);
    } else return ERROR_INVALID_FUNCTION;
    if (status) ++stats->failed;
    return status;
}

VP_STATUS rb_cursor_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    VP_STATUS status;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    status = handle_cursor(gpu, packet);
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status;
}
