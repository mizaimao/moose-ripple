/* The display DLL side of the RIPPLEVD interface (rb2krvd.h) for Mesa's
 * virgl winsys: it turns Mesa's ExtEscape calls into miniport requests,
 * which run in the calling process (gpu_rvd.c keeps the contexts, buffers
 * and their pages). Command batches go to the miniport's staging first.
 * One lock, shared by every display PDEV, orders the staging's use. */
#include "display_private.h"
#include "rb2krvd.h"

static HSEMAPHORE lock;
static ULONG paintTag;                  /* protected by lock for one GDI callback */
static RB2K_RVD_STAGING_INFO staging;
static RB2K_RVD_STATS stats;

void rb_rvd_init(void)
{
    if (!lock) lock = EngCreateSemaphore();
}

static BOOL request(DISPLAY *d, RB2K_RVD_REQUEST *r, PVOID out, ULONG outSize)
{
    ULONG returned = 0;
    return !EngDeviceIoControl(d->miniport, IOCTL_RB2K_RVD, r, sizeof(*r), out, outSize, &returned) &&
           returned == outSize;
}

static void begin(RB2K_RVD_REQUEST *r, ULONG op)
{
    RtlZeroMemory(r, sizeof(*r));
    r->op = op;
}

static BOOL ready(DISPLAY *d)
{
    RB2K_RVD_REQUEST r;
    if (staging.count == RB2K_RVD_STAGING_SEGMENTS) return TRUE;
    begin(&r, RB2K_RVD_OP_STAGING);
    return request(d, &r, &staging, sizeof(staging)) &&
           staging.count == RB2K_RVD_STAGING_SEGMENTS &&
           staging.segmentBytes == RB2K_RVD_SEGMENT_BYTES;
}

static BOOL submit(DISPLAY *d, const struct rvd_submit *s, ULONG payload)
{
    RB2K_RVD_REQUEST r;
    const UCHAR *commands = (const UCHAR *)(s + 1);
    ULONG done, part;
    if (payload < sizeof(*s) || s->bytes != payload - sizeof(*s) || s->bytes > RB2K_RVD_MAX_SUBMIT)
        return FALSE;
    for (done = 0; done < s->bytes; done += part) {
        part = s->bytes - done;
        if (part > RB2K_RVD_SEGMENT_BYTES) part = RB2K_RVD_SEGMENT_BYTES;
        RtlCopyMemory(staging.data[done / RB2K_RVD_SEGMENT_BYTES], commands + done, part);
    }
    begin(&r, RB2K_RVD_OP_SUBMIT);
    r.context = s->context;
    r.bytes = s->bytes;
    return request(d, &r, NULL, 0);
}

static BOOL dispatch(DISPLAY *d, ULONG code, const UCHAR *in, ULONG inSize, PVOID out, ULONG outSize)
{
    RB2K_RVD_REQUEST r;
    switch (code) {
    case RVD_GET_INFO: {
        struct rvd_info *info = out;
        if (inSize || outSize != sizeof(*info)) return FALSE;
        begin(&r, RB2K_RVD_OP_INFO);
        if (!request(d, &r, info, sizeof(*info))) return FALSE;
        info->display_w = d->mode.VisScreenWidth;
        info->display_h = d->mode.VisScreenHeight;
        return TRUE;
    }
    case RVD_GET_CAPS:
        if (inSize != 2 * sizeof(ULONG) || !outSize) return FALSE;
        begin(&r, RB2K_RVD_OP_CAPS);
        r.a = ((const ULONG *)in)[0];
        r.b = ((const ULONG *)in)[1];
        r.bytes = outSize;
        return request(d, &r, out, outSize);
    case RVD_CONTEXT_CREATE:
        if (outSize != sizeof(ULONG)) return FALSE;
        begin(&r, RB2K_RVD_OP_CONTEXT_CREATE);
        r.bytes = inSize > RB2K_RVD_MAX_NAME ? RB2K_RVD_MAX_NAME : inSize;
        if (r.bytes) RtlCopyMemory(r.u.name, in, r.bytes);
        return request(d, &r, out, outSize);
    case RVD_CONTEXT_DESTROY:
        if (inSize != sizeof(ULONG)) return FALSE;
        begin(&r, RB2K_RVD_OP_CONTEXT_DESTROY);
        r.context = *(const ULONG *)in;
        return request(d, &r, NULL, 0);
    case RVD_BUFFER_CREATE:
        if (inSize != sizeof(struct rvd_buffer_create) || outSize != sizeof(struct rvd_buffer))
            return FALSE;
        begin(&r, RB2K_RVD_OP_BUFFER_CREATE);
        r.u.create = *(const struct rvd_buffer_create *)in;
        return request(d, &r, out, outSize);
    case RVD_BUFFER_DESTROY:
        if (inSize != sizeof(ULONG)) return FALSE;
        begin(&r, RB2K_RVD_OP_BUFFER_DESTROY);
        r.a = *(const ULONG *)in;
        return request(d, &r, NULL, 0);
    case RVD_SUBMIT:
        return submit(d, (const struct rvd_submit *)in, inSize);
    case RVD_TRANSFER:
        if (inSize != sizeof(struct rvd_transfer)) return FALSE;
        begin(&r, RB2K_RVD_OP_TRANSFER);
        r.u.transfer = *(const struct rvd_transfer *)in;
        return request(d, &r, NULL, 0);
    case RVD_PRESENT: {
        /* A full-screen frame in the program's own scanout image, of the mode's size. */
        const struct rvd_present *p = (const struct rvd_present *)in;
        if (inSize != sizeof(*p) || outSize || p->width != d->mode.VisScreenWidth ||
            p->height != d->mode.VisScreenHeight) return FALSE;
        begin(&r, RB2K_RVD_OP_SCANOUT);
        r.context = p->context;
        r.a = p->buffer;
        r.b = p->width;
        r.bytes = p->height;
        return request(d, &r, NULL, 0);
    }
    case RVD_DESKTOP:
        if (!inSize) {
            /* Whether full-screen frames may be shown from the GPU: only with the
             * 32-bit GPU desktop (RippleGpuDesktop), which the display returns to. */
            ULONG on = 0;
            if (outSize != sizeof(ULONG)) return FALSE;
            begin(&r, RB2K_RVD_OP_GL_OPTION);
            if (!request(d, &r, &on, sizeof(on))) return FALSE;
            *(PULONG)out = on && d->mode.BitsPerPlane * d->mode.NumberOfPlanes == 32;
            return TRUE;
        }
        if (outSize) return FALSE;
        begin(&r, RB2K_RVD_OP_DESKTOP);
        return request(d, &r, NULL, 0);
    default:
        return FALSE;
    }
}

ULONG rb_rvd_escape(DISPLAY *d, ULONG inSize, PVOID in, ULONG outSize, PVOID out)
{
    LONGLONG start, end;
    ULONG code;
    BOOL done;
    if (!lock || !in || inSize < sizeof(ULONG) || (outSize && !out)) return 0;
    code = *(const ULONG *)in;
    EngQueryPerformanceCounter(&start);
    EngAcquireSemaphore(lock);
    done = ready(d) && dispatch(d, code, (const UCHAR *)in + sizeof(ULONG),
                                inSize - sizeof(ULONG), out, outSize);
    EngQueryPerformanceCounter(&end);
    if (code < RB2K_RVD_STAT_CODES) {
        /* Readbacks are counted apart from uploads, in slot 14. */
        ULONG slot = code == RVD_TRANSFER && inSize == sizeof(ULONG) + sizeof(struct rvd_transfer) &&
                     ((const struct rvd_transfer *)((const UCHAR *)in + sizeof(ULONG)))->direction ==
                     RVD_FROM_HOST ? 14 : code;
        ++stats.calls[slot];
        stats.ticks[slot] += end - start;
        if (!done) ++stats.failures[slot];
    }
    EngReleaseSemaphore(lock);
    return done ? 1 : 0;
}

ULONG rb_rvd_stats(DISPLAY *d, ULONG outSize, PVOID out)
{
    RB2K_RVD_REQUEST r;
    ULONG held[3];
    if (!lock || outSize != sizeof(stats) || !out) return 0;
    EngAcquireSemaphore(lock);
    EngQueryPerformanceFrequency(&stats.frequency);
    begin(&r, RB2K_RVD_OP_HELD);
    if (!request(d, &r, held, sizeof(held))) {
        EngReleaseSemaphore(lock);
        return 0;
    }
    stats.contexts = held[0];
    stats.buffers = held[1];
    stats.pages = held[2];
    RtlCopyMemory(out, &stats, sizeof(stats));
    EngReleaseSemaphore(lock);
    return 1;
}

static BOOL add_visible(RB2K_GL_FRAME *out, const RECTL *window, const RECTL *clip, SIZEL screen)
{
    RB2K_RECT *p;
    RECTL r = *window;
    if (r.left < clip->left) r.left = clip->left;
    if (r.top < clip->top) r.top = clip->top;
    if (r.right > clip->right) r.right = clip->right;
    if (r.bottom > clip->bottom) r.bottom = clip->bottom;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > screen.cx) r.right = screen.cx;
    if (r.bottom > screen.cy) r.bottom = screen.cy;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;
    if (out->count == RB2K_GL_MAX_RECTS) return FALSE;
    p = &out->rects[out->count++];
    p->left = r.left; p->top = r.top; p->right = r.right; p->bottom = r.bottom;
    return TRUE;
}

ULONG DrvDrawEscape(SURFOBJ *surface, ULONG code, CLIPOBJ *clip, RECTL *window,
                    ULONG inSize, PVOID in)
{
    DISPLAY *d;
    struct rvd_present *frame = in;
    RB2K_GL_FRAME out;
    ULONG bytes, returned;
    BOOL ok;
    if (code != RB2K_GL_PRESENT_ESCAPE && code != RB2K_GL_RECT_PRESENT_ESCAPE &&
        code != RB2K_GL_INLINE_PRESENT_ESCAPE) return (ULONG)-1;
    if (!lock || !surface || !surface->dhpdev || !clip || !window ||
        !frame || inSize != sizeof(*frame)) return 0;
    d = (DISPLAY *)surface->dhpdev;
    if (!d->cpuSurface || surface->hsurf != d->surface || d->bitmapFormat != BMF_32BPP || d->flipOwner ||
        (__int64)window->right - window->left != frame->width ||
        (__int64)window->bottom - window->top != frame->height) return 0;
    out.context = frame->context; out.source = frame->buffer;
    out.width = frame->width; out.height = frame->height;
    out.originX = window->left; out.originY = window->top; out.count = 0;
    out.mode = RB2K_FRAME_BATCH;
    if (code == RB2K_GL_RECT_PRESENT_ESCAPE) out.mode = RB2K_FRAME_PER_RECT;
    if (code == RB2K_GL_INLINE_PRESENT_ESCAPE) out.mode = RB2K_FRAME_INLINE_UPLOADS;
    if (clip->iDComplexity == DC_COMPLEX) {
        struct { ULONG count; RECTL rects[8]; } batch;
        BOOL more;
        ULONG i;
        CLIPOBJ_cEnumStart(clip, FALSE, CT_RECTANGLES, CD_ANY, 0);
        do {
            more = CLIPOBJ_bEnum(clip, sizeof(batch), (ULONG *)&batch);
            if (batch.count > 8) return 0;
            for (i = 0; i < batch.count; ++i)
                if (!add_visible(&out, window, &batch.rects[i], surface->sizlBitmap)) return 0;
        } while (more);
    } else if (clip->iDComplexity == DC_TRIVIAL || clip->iDComplexity == DC_RECT) {
        add_visible(&out, window, clip->iDComplexity == DC_RECT ? &clip->rclBounds : window, surface->sizlBitmap);
    } else return 0;
    if (!out.count) return 1;
    bytes = FIELD_OFFSET(RB2K_GL_FRAME, rects) + out.count * sizeof(out.rects[0]);
    EngAcquireSemaphore(lock);
    ok = EngDeviceIoControl(d->miniport, IOCTL_RB2K_GL_FRAME, &out, bytes, NULL, 0, &returned) == 0;
    if (ok) d->glActive = TRUE;
    EngReleaseSemaphore(lock);
    return ok ? 1 : 0;
}

BOOL rb_gl_device_surface(DISPLAY *d)
{
    RB2K_RVD_REQUEST r;
    ULONG enabled = 0;
    begin(&r, RB2K_RVD_OP_GL_OPTION);
    return lock && request(d, &r, &enabled, sizeof(enabled)) && enabled != 0;
}

void rb_gl_cpu_lock(ULONG writer, ULONG rop)
{
    EngAcquireSemaphore(lock);
    paintTag = (rop << 16) | writer;
}
void rb_gl_cpu_unlock(void) { EngReleaseSemaphore(lock); }

/* Only the real GDI drawing callbacks enter here. GDI also synchronizes
 * before DrawEscape; taking a read-back there defeats GPU presentation. */
static BOOL cpu_request(DISPLAY *d, RB2K_RVD_REQUEST *r)
{
    ULONG active;
    if (!request(d, r, &active, sizeof(active))) return FALSE;
    d->glActive = active != 0;
    return TRUE;
}

BOOL rb_gl_cpu_access(DISPLAY *d, const RECTL *rect, ULONG access)
{
    RB2K_RVD_REQUEST r;
    LONG w, h;
    if (!d || !d->glActive) return TRUE;
    w = (LONG)d->mode.VisScreenWidth; h = (LONG)d->mode.VisScreenHeight;
    begin(&r, RB2K_RVD_OP_GDI_ACCESS);
    r.a = access;
    r.b = paintTag;
    if (rect && (rect->right <= 0 || rect->bottom <= 0 || rect->left >= w || rect->top >= h)) return TRUE;
    r.u.rect.left = rect && rect->left > 0 ? rect->left : 0;
    r.u.rect.top = rect && rect->top > 0 ? rect->top : 0;
    r.u.rect.right = rect && rect->right < w ? rect->right : w;
    r.u.rect.bottom = rect && rect->bottom < h ? rect->bottom : h;
    if (r.u.rect.left >= r.u.rect.right || r.u.rect.top >= r.u.rect.bottom) return TRUE;
    return cpu_request(d, &r);
}

/* The desktop in guest memory: send what a drawing changed in a page (0, or a flip
 * chain's second page; rect NULL: all of it) to the host, which shows it. The request
 * uses no staging, so it needs no lock. */
BOOL rb_memory_primary_dirty(DISPLAY *d, ULONG page, const RECTL *rect)
{
    RB2K_RVD_REQUEST r;
    LONG w, h;
    if (!d || !d->memoryPrimary) return TRUE;
    w = (LONG)d->mode.VisScreenWidth; h = (LONG)d->mode.VisScreenHeight;
    begin(&r, RB2K_RVD_OP_PRIMARY_DIRTY);
    r.a = page;
    r.u.rect.left = rect && rect->left > 0 ? rect->left : 0;
    r.u.rect.top = rect && rect->top > 0 ? rect->top : 0;
    r.u.rect.right = rect && rect->right < w ? rect->right : w;
    r.u.rect.bottom = rect && rect->bottom < h ? rect->bottom : h;
    if ((LONG)r.u.rect.left >= (LONG)r.u.rect.right || (LONG)r.u.rect.top >= (LONG)r.u.rect.bottom) return TRUE;
    return request(d, &r, NULL, 0);
}

BOOL rb_gl_cpu_flush(DISPLAY *d)
{
    RB2K_RVD_REQUEST r;
    if (!d || !d->glActive) return TRUE;
    begin(&r, RB2K_RVD_OP_GDI_FLUSH);
    return cpu_request(d, &r);
}

ULONG rb_gl_stats(DISPLAY *d, ULONG operation, ULONG bytes, PVOID out)
{
    RB2K_RVD_REQUEST r;
    BOOL ok;
    if (!lock) return 0;
    begin(&r, operation);
    EngAcquireSemaphore(lock);
    ok = request(d, &r, out, bytes);
    EngReleaseSemaphore(lock);
    return ok ? 1 : 0;
}
