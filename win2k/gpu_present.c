/* Compose a visible OpenGL rectangle on the host. VGA stays available to
 * GDI. Only a GDI access to pixels owned by the host needs a read-back. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include "gpu_memory.h"

static BOOLEAN area(const RB2K_RECT *r)
{
    return r->left < r->right && r->top < r->bottom;
}

static BOOLEAN overlaps(const RB2K_RECT *a, const RB2K_RECT *b)
{
    return area(a) && area(b) && a->left < b->right && b->left < a->right &&
        a->top < b->bottom && b->top < a->bottom;
}

static VP_STATUS read_gpu_pixels(RB2K_GPU *gpu)
{
    RB2K_RECT *r = &gpu->glDesktop.gpuPixels;
    RB2K_GPU_RESULT result;
    if (area(r) && !rb_gpu_primary_transfer(gpu, &result, r, TRUE)) return result.status;
    VideoPortZeroMemory(r, sizeof(*r));
    return NO_ERROR;
}

VP_STATUS rb_gl_access(RB2K_GPU *gpu, const RB2K_RECT *rect, ULONG access, ULONG tag)
{
    RB2K_GL_DESKTOP *d = &gpu->glDesktop;
    RB2K_GPU_RESULT result;
    if (!d->image.id) return ERROR_NOT_READY;
    if (access > RB2K_CPU_REPLACE || (tag & 0xffff) >= RB2K_PAINT_COUNT ||
        !area(rect) || rect->right > d->image.width || rect->bottom > d->image.height)
        return ERROR_INVALID_PARAMETER;
    if (access != RB2K_CPU_REPLACE && overlaps(rect, &d->gpuPixels)) {
        RB2K_RECT part = *rect;
        if (part.left < d->gpuPixels.left) part.left = d->gpuPixels.left;
        if (part.top < d->gpuPixels.top) part.top = d->gpuPixels.top;
        if (part.right > d->gpuPixels.right) part.right = d->gpuPixels.right;
        if (part.bottom > d->gpuPixels.bottom) part.bottom = d->gpuPixels.bottom;
        if (!rb_gpu_primary_transfer(gpu, &result, &part, TRUE)) return result.status;
    }
    if (access != RB2K_CPU_READ) {
        if (d->writeCount >= sizeof(d->gdiWrites) / sizeof(d->gdiWrites[0])) return ERROR_BUSY;
        d->writeTags[d->writeCount] = tag;
        d->gdiWrites[d->writeCount++] = *rect;
    }
    return NO_ERROR;
}

/* Attribute successful uploads to the callback that dirtied the pixels.
 * A small write inside an excluded piece of the last game frame is counted
 * separately from desktop paint outside the window. */
static void record_upload(RB2K_GPU *gpu, ULONG tag, const RB2K_RECT *r)
{
    RB2K_GL_DESKTOP *d = &gpu->glDesktop;
    ULONG width = r->right - r->left, height = r->bottom - r->top, i;
    RB2K_GL_WRITER *w;
    for (i = 0; i < RB2K_GL_WRITERS; ++i) {
        w = &gpu->glProfile.writers[i];
        if (!w->uploads || w->tag == tag) break;
    }
    if (i == RB2K_GL_WRITERS) { ++gpu->glProfile.otherUploads; return; }
    w->tag = tag;
    ++w->uploads;
    w->pixels += width * height;
    if (width > 64 || height > 64) return;
    ++w->small;
    w->width = width;
    w->height = height;
    if (!d->clipCount || r->left < d->gpuPixels.left || r->top < d->gpuPixels.top ||
        r->right > d->gpuPixels.right || r->bottom > d->gpuPixels.bottom) return;
    for (i = 0; i < d->clipCount; ++i) if (overlaps(r, &d->clip[i])) return;
    ++w->excluded;
}

static VP_STATUS upload_gdi(RB2K_GPU *gpu)
{
    RB2K_GL_DESKTOP *d = &gpu->glDesktop;
    RB2K_GPU_RESULT result;
    ULONG i, count;
    while (d->writeCount) {
        count = 0;
        while (d->inlineUploads && count < d->writeCount) {
            const RB2K_RECT *r = &d->gdiWrites[count];
            if (r->right - r->left > 64 || r->bottom - r->top > 64) break;
            ++count;
        }
        if (count) {
            VP_STATUS status = rb_gpu_inline_uploads(gpu, d->gdiWrites, count);
            if (status) return status;
        } else {
            if (!rb_gpu_primary_transfer(gpu, &result, d->gdiWrites, FALSE)) return result.status;
            count = 1;
        }
        for (i = 0; i < count; ++i) record_upload(gpu, d->writeTags[i], &d->gdiWrites[i]);
        d->writeCount -= count;
        for (i = 0; i < d->writeCount; ++i) {
            d->gdiWrites[i] = d->gdiWrites[count + i];
            d->writeTags[i] = d->writeTags[count + i];
        }
    }
    return NO_ERROR;
}

VP_STATUS rb_gl_flush(RB2K_GPU *gpu)
{
    RB2K_GL_DESKTOP *d = &gpu->glDesktop;
    RB2K_GPU_RESULT result;
    VP_STATUS status;
    if (!d->image.id) return ERROR_NOT_READY;
    if (!d->writeCount) return NO_ERROR;
    status = upload_gdi(gpu);
    if (status) return status;
    return rb_gpu_show_desktop(gpu, &result, FALSE) ? NO_ERROR : result.status;
}

VP_STATUS rb_gl_present(RB2K_GPU *gpu, const RB2K_GL_FRAME *f)
{
    static const char name[] = "rb2k-desktop";
    RB2K_GL_DESKTOP *d = &gpu->glDesktop;
    RB2K_PRIMARY *vga = &gpu->primary;
    RB2K_GPU_RESULT result;
    RB2K_RECT next;
    ULONG i, batch;
    VP_STATUS status;
    if (!gpu->glEnabled || !gpu->ready || !vga->pixels || vga->bitsPerPixel != 32 ||
        !vga->width || vga->width > RB2K_DD_MAX_WIDTH || !vga->height ||
        vga->height > RB2K_DD_MAX_HEIGHT || vga->pitchPixels < vga->width ||
        vga->pitchPixels > vga->bytes / sizeof(ULONG) / vga->height)
        return ERROR_NOT_SUPPORTED;
    next = f->rects[0];
    for (i = 0; i < f->count; ++i) {
        const RB2K_RECT *r = &f->rects[i];
        if (!area(r) || r->right > vga->width || r->bottom > vga->height ||
            (__int64)r->left < f->originX || (__int64)r->top < f->originY ||
            (__int64)r->right - f->originX > f->width || (__int64)r->bottom - f->originY > f->height)
            return ERROR_INVALID_PARAMETER;
        if (r->left < next.left) next.left = r->left;
        if (r->top < next.top) next.top = r->top;
        if (r->right > next.right) next.right = r->right;
        if (r->bottom > next.bottom) next.bottom = r->bottom;
    }
    if (!d->image.id) {
        if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
        d->image.id = ++gpu->nextId;
        d->image.width = vga->width;
        d->image.height = vga->height;
        if (!rb_gpu_create_image(gpu, &result, &d->image, name, sizeof(name) - 1, TRUE)) {
            VideoPortZeroMemory(d, sizeof(*d));
            return result.status;
        }
        d->gdiWrites[0].right = vga->width;
        d->gdiWrites[0].bottom = vga->height;
        d->writeTags[0] = RB2K_PAINT_INITIAL;
        d->writeCount = 1;
    }
    if (d->image.width != vga->width || d->image.height != vga->height) return ERROR_NOT_READY;
    if (area(&d->gpuPixels) && (next.left != d->gpuPixels.left || next.top != d->gpuPixels.top ||
        next.right != d->gpuPixels.right || next.bottom != d->gpuPixels.bottom)) {
        status = read_gpu_pixels(gpu);
        if (status) return status;
    }
    d->inlineUploads = f->mode == RB2K_FRAME_INLINE_UPLOADS;
    status = upload_gdi(gpu);
    if (status) return status;
    batch = f->mode == RB2K_FRAME_PER_RECT ? 1 : f->count;
    for (i = 0; i < f->count; i += batch) {
        LONGLONG start = rb_counter_ticks(NULL);
        BOOLEAN ok = rb_gpu_copy_rects(gpu, &result, d->image.id, f->source,
                                       &f->rects[i], batch, f->originX, f->originY, &gpu->glProfile);
        if (!rb_gl_record_time(gpu, RB2K_GL_COPY, start, ok)) return result.status;
    }
    d->gpuPixels = next;
    VideoPortMoveMemory(d->clip, (PVOID)f->rects, f->count * sizeof(d->clip[0]));
    d->clipCount = f->count;
    if (!rb_gpu_show_desktop(gpu, &result, (BOOLEAN)!d->visible)) return result.status;
    ++gpu->glStats.frames;
    return NO_ERROR;
}

/* The transport reset drops native scanout and returns virtio-vga to VGA. */
VP_STATUS rb_gl_finish(RB2K_GPU *gpu)
{
    RB2K_GL_DESKTOP *d = &gpu->glDesktop;
    VP_STATUS status = NO_ERROR;
    if (d->image.id && gpu->ready) status = read_gpu_pixels(gpu);
    if (!status) VideoPortZeroMemory(d, sizeof(*d));
    return status;
}
