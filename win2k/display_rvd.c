/* The display DLL side of the RIPPLEVD interface (rb2krvd.h) for Mesa's
 * virgl winsys: it turns Mesa's ExtEscape calls into miniport requests,
 * which run in the calling process (gpu_rvd.c keeps the contexts, buffers
 * and their pages). Command batches go to the miniport's staging first.
 * One lock, shared by every display PDEV, orders the staging's use. */
#include "display_private.h"
#include "rb2krvd.h"

static HSEMAPHORE lock;
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
    }
    EngReleaseSemaphore(lock);
    return done ? 1 : 0;
}

ULONG rb_rvd_stats(ULONG outSize, PVOID out)
{
    if (!lock || outSize != sizeof(stats) || !out) return 0;
    EngAcquireSemaphore(lock);
    EngQueryPerformanceFrequency(&stats.frequency);
    RtlCopyMemory(out, &stats, sizeof(stats));
    EngReleaseSemaphore(lock);
    return 1;
}
