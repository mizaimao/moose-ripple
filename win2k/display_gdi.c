/* GDI renders into the VGA bitmap. A device-managed primary makes pixel
 * reads visible to the driver, including GetPixel and screen-to-DIB copies.
 * The unhooked bitmap prevents a call to Eng from re-entering its own hook.
 * With the desktop in guest memory (RipplePrimaryInMemory), each call then
 * sends the area it drew to the host, which shows it. */
#include "display_private.h"

static DISPLAY *primary(SURFOBJ *surface)
{
    return surface && surface->iType == STYPE_DEVICE ? (DISPLAY *)surface->dhsurf : NULL;
}

static SURFOBJ *bitmap(SURFOBJ *surface)
{
    DISPLAY *d = primary(surface);
    return d ? d->cpuSurface : surface;
}

static const RECTL *bounds(CLIPOBJ *clip)
{
    return clip && clip->iDComplexity != DC_TRIVIAL ? &clip->rclBounds : NULL;
}

static BOOL read_source(SURFOBJ *surface, POINTL *origin, RECTL *dest)
{
    DISPLAY *d = primary(surface);
    RECTL rect;
    __int64 right, bottom;
    if (!d || !d->glActive) return TRUE;
    if (!origin || !dest) return rb_gl_cpu_access(d, NULL, FALSE);
    right = (__int64)origin->x + dest->right - dest->left;
    bottom = (__int64)origin->y + dest->bottom - dest->top;
    if (right > 0x7fffffff || bottom > 0x7fffffff || right < origin->x || bottom < origin->y)
        return rb_gl_cpu_access(d, NULL, FALSE);
    rect.left = origin->x; rect.top = origin->y;
    rect.right = (LONG)right; rect.bottom = (LONG)bottom;
    return rb_gl_cpu_access(d, &rect, FALSE);
}

/* The area a call drew, for the desktop in guest memory: limited to the clip. */
static RECTL *clipped(RECTL *r, CLIPOBJ *clip)
{
    if (clip && clip->iDComplexity != DC_TRIVIAL) {
        if (r->left < clip->rclBounds.left) r->left = clip->rclBounds.left;
        if (r->top < clip->rclBounds.top) r->top = clip->rclBounds.top;
        if (r->right > clip->rclBounds.right) r->right = clip->rclBounds.right;
        if (r->bottom > clip->rclBounds.bottom) r->bottom = clip->rclBounds.bottom;
    }
    return r;
}

static void add(RECTL *r, const RECTL *more)
{
    if (more->left >= more->right || more->top >= more->bottom) return;
    if (r->left >= r->right || r->top >= r->bottom) { *r = *more; return; }
    if (more->left < r->left) r->left = more->left;
    if (more->top < r->top) r->top = more->top;
    if (more->right > r->right) r->right = more->right;
    if (more->bottom > r->bottom) r->bottom = more->bottom;
}

/* A path's bounds in whole pixels (they are 28.4 fixed point), a pixel wider all round. */
static RECTL *path_area(PATHOBJ *path, RECTL *r)
{
    RECTFX b;
    PATHOBJ_vGetBounds(path, &b);
    r->left = (b.xLeft >> 4) - 1;
    r->top = (b.yTop >> 4) - 1;
    r->right = ((b.xRight + 15) >> 4) + 2;
    r->bottom = ((b.yBottom + 15) >> 4) + 2;
    return r;
}

/* dirty: the area drawn, NULL for the whole desktop. A failed send leaves the host
 * a step behind until the area is drawn again; the drawing itself has succeeded. */
static BOOL finish(DISPLAY *d, BOOL painted, const RECTL *dirty)
{
    BOOL flushed = rb_gl_cpu_flush(d);
    if (painted && d && d->memoryPrimary) rb_memory_primary_dirty(d, 0, dirty);
    rb_gl_cpu_unlock();
    return painted && flushed;
}

/* A plain copy or fill supplies the destination pixels. Only an exact
 * rectangle can skip fetching the old contents. A complex clip may leave
 * holes whose pixels must survive the upload. */
static BOOL prepare_blt(DISPLAY *d, const RECTL *rect, CLIPOBJ *clip, BOOL replaces)
{
    RECTL r;
    if (!replaces || !rect || (clip && clip->iDComplexity == DC_COMPLEX))
        return rb_gl_cpu_access(d, rect, RB2K_CPU_DRAW);
    r = *rect;
    if (clip && clip->iDComplexity == DC_RECT) {
        if (r.left < clip->rclBounds.left) r.left = clip->rclBounds.left;
        if (r.top < clip->rclBounds.top) r.top = clip->rclBounds.top;
        if (r.right > clip->rclBounds.right) r.right = clip->rclBounds.right;
        if (r.bottom > clip->rclBounds.bottom) r.bottom = clip->rclBounds.bottom;
    }
    return rb_gl_cpu_access(d, &r, RB2K_CPU_REPLACE);
}

BOOL DrvCopyBits(SURFOBJ *dst, SURFOBJ *src, CLIPOBJ *clip, XLATEOBJ *xlate,
                 RECTL *rect, POINTL *origin)
{
    DISPLAY *d = primary(dst);
    BOOL ok;
    RECTL area;
    rb_gl_cpu_lock(primary(src) ? RB2K_PAINT_COPY_DESKTOP : RB2K_PAINT_COPY_BITMAP, 0);
    ok = read_source(src, origin, rect) && prepare_blt(d, rect, clip, TRUE) &&
        EngCopyBits(bitmap(dst), bitmap(src), clip, xlate, rect, origin);
    area = *rect;
    return finish(d, ok, clipped(&area, clip));
}

BOOL DrvBitBlt(SURFOBJ *dst, SURFOBJ *src, SURFOBJ *mask, CLIPOBJ *clip, XLATEOBJ *xlate,
               RECTL *rect, POINTL *origin, POINTL *maskOrigin, BRUSHOBJ *brush, POINTL *brushOrigin, ROP4 rop)
{
    DISPLAY *d = primary(dst);
    BOOL ok;
    RECTL area;
    rb_gl_cpu_lock(mask ? RB2K_PAINT_MASKBLT : RB2K_PAINT_BLT, rop);
    ok = read_source(src, origin, rect) && read_source(mask, maskOrigin, rect) &&
        prepare_blt(d, rect, clip, ((rop ^ (rop >> 1)) & 0x5555) == 0) &&
        EngBitBlt(bitmap(dst), bitmap(src), bitmap(mask),
        clip, xlate, rect, origin, maskOrigin, brush, brushOrigin, rop);
    area = *rect;
    return finish(d, ok, clipped(&area, clip));
}

BOOL DrvTextOut(SURFOBJ *dst, STROBJ *text, FONTOBJ *font, CLIPOBJ *clip,
                RECTL *extra, RECTL *opaque, BRUSHOBJ *fore, BRUSHOBJ *back, POINTL *origin, MIX mix)
{
    DISPLAY *d = primary(dst);
    BOOL ok;
    RECTL area;
    rb_gl_cpu_lock(RB2K_PAINT_TEXT, 0);
    ok = rb_gl_cpu_access(d, &text->rclBkGround, TRUE) &&
        (!opaque || rb_gl_cpu_access(d, opaque, TRUE)) &&
        EngTextOut(bitmap(dst), text, font, clip, extra, opaque, fore, back, origin, mix);
    area = text->rclBkGround;
    if (opaque) add(&area, opaque);
    /* extra: underlines and strike-outs, ended by an empty rectangle */
    for (; extra && (extra->left || extra->top || extra->right || extra->bottom); ++extra) add(&area, extra);
    return finish(d, ok, clipped(&area, clip));
}

BOOL DrvStrokePath(SURFOBJ *dst, PATHOBJ *path, CLIPOBJ *clip, XFORMOBJ *transform,
                   BRUSHOBJ *brush, POINTL *origin, LINEATTRS *line, MIX mix)
{
    DISPLAY *d = primary(dst);
    BOOL ok;
    RECTL area;
    rb_gl_cpu_lock(RB2K_PAINT_STROKE, 0);
    ok = rb_gl_cpu_access(d, bounds(clip), TRUE) &&
        EngStrokePath(bitmap(dst), path, clip, transform, brush, origin, line, mix);
    /* A wide (geometric) line reaches past its path by a width given as a float. */
    if (line && (line->fl & LA_GEOMETRIC))
        return finish(d, ok, clip && clip->iDComplexity != DC_TRIVIAL ? &clip->rclBounds : NULL);
    return finish(d, ok, clipped(path_area(path, &area), clip));
}

BOOL DrvFillPath(SURFOBJ *dst, PATHOBJ *path, CLIPOBJ *clip, BRUSHOBJ *brush,
                 POINTL *origin, MIX mix, FLONG options)
{
    DISPLAY *d = primary(dst);
    BOOL ok;
    RECTL area;
    rb_gl_cpu_lock(RB2K_PAINT_FILL, 0);
    ok = rb_gl_cpu_access(d, bounds(clip), TRUE) &&
        EngFillPath(bitmap(dst), path, clip, brush, origin, mix, options);
    return finish(d, ok, clipped(path_area(path, &area), clip));
}

BOOL DrvStrokeAndFillPath(SURFOBJ *dst, PATHOBJ *path, CLIPOBJ *clip, XFORMOBJ *transform,
                          BRUSHOBJ *stroke, LINEATTRS *line, BRUSHOBJ *fill, POINTL *origin,
                          MIX mix, FLONG options)
{
    DISPLAY *d = primary(dst);
    BOOL ok;
    RECTL area;
    rb_gl_cpu_lock(RB2K_PAINT_STROKE_FILL, 0);
    ok = rb_gl_cpu_access(d, bounds(clip), TRUE) &&
        EngStrokeAndFillPath(bitmap(dst), path, clip, transform, stroke, line, fill, origin, mix, options);
    if (line && (line->fl & LA_GEOMETRIC))
        return finish(d, ok, clip && clip->iDComplexity != DC_TRIVIAL ? &clip->rclBounds : NULL);
    return finish(d, ok, clipped(path_area(path, &area), clip));
}
