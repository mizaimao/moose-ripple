/* Convert GDI pointer bitmaps to the NT5 miniport's independent BGRA plane. */
#include "display_private.h"

static BOOL send(DISPLAY *d, RB2K_CURSOR_REQUEST *r, ULONG bytes, RB2K_CURSOR_STATS *stats)
{
    ULONG returned = 0, size = stats ? sizeof(*stats) : 0;
    r->owner = (ULONG)(ULONG_PTR)d;
    return !EngDeviceIoControl(d->miniport, IOCTL_RB2K_CURSOR, r, bytes, stats, size, &returned) &&
           returned == size;
}

BOOL rb_cursor_stats(DISPLAY *d, RB2K_CURSOR_STATS *stats)
{
    RB2K_CURSOR_REQUEST r = {RB2K_CURSOR_QUERY};
    return send(d, &r, sizeof(r), stats);
}

static BOOL drop(DISPLAY *d, ULONG reason)
{
    RB2K_CURSOR_REQUEST r = {RB2K_CURSOR_DROP};
    r.kind = reason;
    d->cursorOwned = FALSE;
    return send(d, &r, sizeof(r), NULL);
}

void rb_display_cursor_release(DISPLAY *d)
{
    if (!d->cursorLock) return;
    EngAcquireSemaphore(d->cursorLock);
    if (d->cursorOwned) drop(d, RB2K_CURSOR_TRANSPARENT);
    EngReleaseSemaphore(d->cursorLock);
}

static ULONG bit(const UCHAR *row, LONG x) { return (row[x >> 3] >> (7 - (x & 7))) & 1; }
static const UCHAR *row_at(SURFOBJ *s, LONG y) { return (const UCHAR *)s->pvScan0 + y * s->lDelta; }

/* Windows may add its own shadow before calling the DDI. For a requested
 * diagnostic sample, checksum the original SURFOBJ separately from the
 * converted and padded resource whose checksum the miniport reports. */
static void sample_source(RB2K_CURSOR_SOURCE *s, SURFOBJ *color, LONG hotX, LONG hotY)
{
    LONG x, y;
    s->width = color->sizlBitmap.cx; s->height = color->sizlBitmap.cy;
    s->hotX = hotX; s->hotY = hotY;
    s->hash = 2166136261UL;
    for (y = 0; y < RB2K_CURSOR_EDGE; ++y) {
        const ULONG *row = y < color->sizlBitmap.cy ? (const ULONG *)row_at(color,y) : NULL;
        for (x = 0; x < RB2K_CURSOR_EDGE; ++x) {
            ULONG p = row && x < color->sizlBitmap.cx ? row[x] : 0;
            ULONG a = p >> 24;
            if (x == hotX && y == hotY) s->hotPixel = p;
            if (a == 255) ++s->opaque;
            else if (a) {
                ++s->partial;
                if (p & 0xffffff) ++s->partialColor;
            }
            s->hash = (s->hash ^ (a ? p : 0)) * 16777619UL;
        }
    }
}

static BOOL color_at(SURFOBJ *color, XLATEOBJ *xlate, LONG x, LONG y, ULONG *rgb)
{
    const UCHAR *row = row_at(color, y);
    ULONG value;
    switch (color->iBitmapFormat) {
    case BMF_1BPP: value = bit(row, x); break;
    case BMF_4BPP: value = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15; break;
    case BMF_8BPP: value = row[x]; break;
    case BMF_16BPP: value = ((const USHORT *)row)[x]; break;
    case BMF_24BPP: row += 3*x; value = row[0] | (row[1] << 8) | (row[2] << 16); break;
    case BMF_32BPP: value = ((const ULONG *)row)[x]; break;
    default: return FALSE;
    }
    if (xlate && !(xlate->flXlate & XO_TRIVIAL)) value = XLATEOBJ_iXlate(xlate, value);
    else if (color->iBitmapFormat < BMF_24BPP) return FALSE;
    *rgb = value & 0xffffff;
    return TRUE;
}

/* GDI and the display cursor use premultiplied BGRA. An XOR against
 * the desktop has no representation on an independent alpha plane. */
static ULONG convert(RB2K_CURSOR_SHAPE *shape, SURFOBJ *mask, SURFOBJ *color, XLATEOBJ *xlate,
                     LONG hotX, LONG hotY, FLONG flags)
{
    BOOL alpha = (flags & SPS_ALPHA) != 0;
    LONG w, h, x, y;
    if (flags & ~(SPS_CHANGE | SPS_ANIMATESTART | SPS_ANIMATEUPDATE | SPS_ALPHA))
        return RB2K_CURSOR_FLAGS;
    if (!alpha && !mask) return RB2K_CURSOR_TRANSPARENT;
    if (color && (!color->pvScan0 || color->iType != STYPE_BITMAP)) return RB2K_CURSOR_BITMAP;
    if (alpha) {
        if (!color || color->iBitmapFormat != BMF_32BPP) return RB2K_CURSOR_BITMAP;
        w = color->sizlBitmap.cx; h = color->sizlBitmap.cy;
    } else {
        if (!mask->pvScan0 || mask->iType != STYPE_BITMAP || mask->iBitmapFormat != BMF_1BPP)
            return RB2K_CURSOR_BITMAP;
        w = mask->sizlBitmap.cx;
        h = color ? color->sizlBitmap.cy : mask->sizlBitmap.cy / 2;
    }
    if (w <= 0 || h <= 0 || w > RB2K_CURSOR_EDGE || h > RB2K_CURSOR_EDGE ||
        hotX < 0 || hotX >= w || hotY < 0 || hotY >= h) return RB2K_CURSOR_SIZE;
    if (!alpha && ((color && color->sizlBitmap.cx != w) ||
        (mask->sizlBitmap.cy != 2*h && !(color && mask->sizlBitmap.cy == h)))) return RB2K_CURSOR_SIZE;
    shape->info.width = w; shape->info.height = h;
    shape->info.hotX = hotX; shape->info.hotY = hotY;
    shape->info.kind = alpha ? RB2K_CURSOR_ALPHA : color ? RB2K_CURSOR_COLOR : RB2K_CURSOR_MONO;
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        ULONG pixel;
        if (alpha) {
            pixel = ((const ULONG *)row_at(color, y))[x];
            if (!(pixel >> 24)) pixel = 0;
        } else {
            ULONG rgb;
            if (!color) rgb = bit(row_at(mask, y+h), x) ? 0xffffff : 0;
            else if (!color_at(color, xlate, x, y, &rgb)) return RB2K_CURSOR_XLATE;
            if (bit(row_at(mask, y), x)) {
                if (rgb) return RB2K_CURSOR_INVERT;
                pixel = 0;
            } else pixel = 0xff000000UL | rgb;
        }
        shape->pixels[y*RB2K_CURSOR_EDGE+x] = pixel;
    }
    return (ULONG)-1;
}

ULONG DrvSetPointerShape(SURFOBJ *surface, SURFOBJ *mask, SURFOBJ *color, XLATEOBJ *xlate,
                         LONG hotX, LONG hotY, LONG x, LONG y, RECTL *bounds, FLONG flags)
{
    DISPLAY *d = (DISPLAY *)surface->dhpdev;
    RB2K_CURSOR_SHAPE *shape;
    ULONG reason = RB2K_CURSOR_UNSUPPORTED, result = SPS_DECLINE;
    (void)bounds;
    if (!d || !d->cursorLock) return SPS_DECLINE;
    EngAcquireSemaphore(d->cursorLock);
    shape = EngAllocMem(FL_ZERO_MEMORY, sizeof(*shape), 'c2bR');
    if (shape) {
        reason = convert(shape, mask, color, xlate, hotX, hotY, flags);
        if (reason == (ULONG)-1) {
            shape->info.op = RB2K_CURSOR_SET;
            shape->info.x = x; shape->info.y = y;
            if (send(d, &shape->info, sizeof(*shape), NULL)) {
                d->cursorOwned = TRUE;
                if (d->sampleCursor && shape->info.kind == RB2K_CURSOR_ALPHA) {
                    sample_source(&d->cursorSource, color, hotX, hotY);
                    d->sampleCursor = FALSE;
                }
                result = SPS_ACCEPT_NOEXCLUDE;
            } else reason = RB2K_CURSOR_DEVICE;
        }
        EngFreeMem(shape);
    }
    if (result == SPS_DECLINE) {
        if (reason == RB2K_CURSOR_TRANSPARENT) {
            if (drop(d, reason)) result = SPS_ACCEPT_NOEXCLUDE;
        } else {
            RB2K_CURSOR_DECLINE r = {0};
            r.info.op = RB2K_CURSOR_DROP; r.info.kind = reason;
            r.why.reason = reason; r.why.flags = flags;
            if (mask) {
                r.why.maskFormat = mask->iBitmapFormat;
                r.why.maskWidth = mask->sizlBitmap.cx; r.why.maskHeight = mask->sizlBitmap.cy;
            }
            if (color) {
                r.why.colorFormat = color->iBitmapFormat;
                r.why.colorWidth = color->sizlBitmap.cx; r.why.colorHeight = color->sizlBitmap.cy;
            }
            d->cursorOwned = FALSE;
            send(d, &r.info, sizeof(r), NULL);
        }
    }
    EngReleaseSemaphore(d->cursorLock);
    return result;
}

VOID DrvMovePointer(SURFOBJ *surface, LONG x, LONG y, RECTL *bounds)
{
    DISPLAY *d = (DISPLAY *)surface->dhpdev;
    RB2K_CURSOR_REQUEST r = {RB2K_CURSOR_MOVE};
    (void)bounds;
    if (!d || !d->cursorLock) return;
    EngAcquireSemaphore(d->cursorLock);
    if (d->cursorOwned) {
        r.x = x; r.y = y;
        send(d, &r, sizeof(r), NULL);
    }
    EngReleaseSemaphore(d->cursorLock);
}
