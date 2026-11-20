/* NT5 DirectDraw offscreen surfaces live in host textures. The runtime's
 * per-surface staging buffer is synchronized at a CPU lock boundary.
 * Windowed presentation updates the visible part of the GDI framebuffer. */
#include "display_private.h"

static BOOL request(DISPLAY *d, ULONG code, PVOID in, ULONG inSize,
                    PVOID out, ULONG size)
{
    ULONG written = 0;
    return !EngDeviceIoControl(d->miniport, code, in, inSize, out, size, &written)
        && written == size;
}

BOOL rb_dd_caps(DISPLAY *d, RB2K_DD_CAPS *caps)
{
    return request(d, IOCTL_RB2K_DD_QUERY, NULL, 0, caps, sizeof(*caps)) &&
           caps->maxBytes == RB2K_DD_MAX_BYTES && caps->slots == RB2K_DD_SLOTS;
}

static BOOL primary_caps(DWORD caps)
{
    return (caps & DDSCAPS_PRIMARYSURFACE) &&
           !(caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE |
                      DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM));
}

static BOOL supported(const DISPLAY *d, const DDSURFACEDESC *desc)
{
    DWORD caps = desc->ddsCaps.dwCaps;
    const DDPIXELFORMAT *fmt = &desc->ddpfPixelFormat;
    if (primary_caps(caps)) {
        if (!d->mapping || !d->surface ||
            ((desc->dwFlags & DDSD_WIDTH) && desc->dwWidth != d->mode.VisScreenWidth) ||
            ((desc->dwFlags & DDSD_HEIGHT) && desc->dwHeight != d->mode.VisScreenHeight))
            return FALSE;
    } else if (!(caps & DDSCAPS_OFFSCREENPLAIN) ||
               (caps & ~(DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM)) ||
               !desc->dwWidth || desc->dwWidth > RB2K_DD_MAX_WIDTH ||
               !desc->dwHeight || desc->dwHeight > RB2K_DD_MAX_HEIGHT) return FALSE;
    return !(desc->dwFlags & DDSD_PIXELFORMAT) ||
        (fmt->dwFlags == DDPF_RGB && fmt->dwRGBBitCount == 32 &&
         fmt->dwRBitMask == 0xff0000 && fmt->dwGBitMask == 0xff00 &&
         fmt->dwBBitMask == 0xff);
}

static DWORD APIENTRY can_create(PDD_CANCREATESURFACEDATA data)
{
    data->ddRVal = supported((DISPLAY *)data->lpDD->dhpdev, data->lpDDSurfaceDesc)
                 ? DD_OK : DDERR_INVALIDCAPS;
    return DDHAL_DRIVER_HANDLED;
}

static DD_SLOT *find_slot(DISPLAY *d, PDD_SURFACE_GLOBAL global)
{
    ULONG i;
    for (i = 0; i < RB2K_DD_SLOTS; ++i) {
        if (d->slots[i].global == global) return &d->slots[i];
    }
    return NULL;
}

static DWORD APIENTRY create_surface(PDD_CREATESURFACEDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    PDD_SURFACE_GLOBAL g;
    DDSURFACEDESC *desc = data->lpDDSurfaceDesc;
    RB2K_DD_CREATE create;
    DD_SLOT *slot;
    DWORD rc = DDHAL_DRIVER_HANDLED;
    if (!supported(d, desc) || data->dwSCnt != 1) {
        data->ddRVal = DDERR_INVALIDCAPS;
        return rc;
    }
    g = data->lplpSList[0]->lpGbl;
    if (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
        /* NT normally creates the primary without entering this callback.
         * Its video-memory offset is zero in each of our display modes. */
        g->fpVidMem = 0;
        g->lPitch = desc->lPitch = d->mode.ScreenStride;
        desc->dwFlags |= DDSD_PITCH;
        data->ddRVal = DD_OK;
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (g->wWidth != desc->dwWidth || g->wHeight != desc->dwHeight) {
        data->ddRVal = DDERR_INVALIDPARAMS;
        return rc;
    }
    create.width = desc->dwWidth;
    create.height = desc->dwHeight;
    EngAcquireSemaphore(d->ddLock);
    slot = find_slot(d, NULL);
    if (!d->ddEnabled || find_slot(d, g))
        data->ddRVal = DDERR_SURFACELOST;
    else if (!slot || !request(d, IOCTL_RB2K_DD_CREATE, &create, sizeof(create),
                               &slot->id, sizeof(slot->id)))
        data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
    else {
        slot->global = g;
        slot->owner = EngGetCurrentProcessId();
        slot->width = create.width;
        slot->height = create.height;
        g->lPitch = create.width * 4;
        g->dwUserMemSize = g->lPitch * create.height;
        g->fpVidMem = DDHAL_PLEASEALLOC_USERMEM;
        desc->lPitch = g->lPitch;
        desc->dwFlags |= DDSD_PITCH;
        data->ddRVal = DD_OK;
        /* NT completes the requested allocation after this return. */
        rc = DDHAL_DRIVER_NOTHANDLED;
    }
    EngReleaseSemaphore(d->ddLock);
    return rc;
}

static void release_slot(DISPLAY *d, DD_SLOT *slot)
{
    request(d, IOCTL_RB2K_DD_DESTROY, &slot->id, sizeof(slot->id), NULL, 0);
    RtlZeroMemory(slot, sizeof(*slot));
}

static DWORD APIENTRY destroy_surface(PDD_DESTROYSURFACEDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    DD_SLOT *slot;
    EngAcquireSemaphore(d->ddLock);
    slot = find_slot(d, data->lpDDSurface->lpGbl);
    if (slot) release_slot(d, slot);
    EngReleaseSemaphore(d->ddLock);
    data->ddRVal = DD_OK;
    /* Let the runtime free its staging allocation. */
    return DDHAL_DRIVER_NOTHANDLED;
}

static BOOL full_rect(const RECTL *r, const DD_SLOT *s)
{
    return !r->left && !r->top && r->right == (LONG)s->width &&
           r->bottom == (LONG)s->height;
}

static BOOL mapped_primary(const DISPLAY *d, const DD_SURFACE_LOCAL *local)
{
    const DD_SURFACE_GLOBAL *g = local->lpGbl;
    return d->ddEnabled && d->mapping && d->surface && primary_caps(local->ddsCaps.dwCaps) &&
           g->wWidth == d->mode.VisScreenWidth && g->wHeight == d->mode.VisScreenHeight &&
           g->lPitch > 0 && (ULONG)g->lPitch == d->mode.ScreenStride &&
           (g->fpVidMem == 0 || g->fpVidMem == (FLATPTR)d->mapping);
}

static BOOL rect_inside(const RECTL *r, ULONG width, ULONG height)
{
    return r->left >= 0 && r->top >= 0 && r->right > r->left && r->bottom > r->top &&
           (ULONG)r->right <= width && (ULONG)r->bottom <= height;
}

static HRESULT present(DISPLAY *d, const DD_SLOT *src, const RECTL *from, const RECTL *to)
{
    RB2K_DD_PRESENT p;
    if (!rect_inside(from, src->width, src->height) ||
        !rect_inside(to, d->mode.VisScreenWidth, d->mode.VisScreenHeight))
        return DDERR_INVALIDRECT;
    if (from->right - from->left != to->right - to->left ||
        from->bottom - from->top != to->bottom - to->top) return DDERR_UNSUPPORTED;
    p.source = src->id;
    p.srcX = from->left; p.srcY = from->top;
    p.dstX = to->left; p.dstY = to->top;
    p.width = from->right - from->left; p.height = from->bottom - from->top;
    return request(d, IOCTL_RB2K_DD_PRESENT, &p, sizeof(p), NULL, 0)
         ? DD_OK : DDERR_GENERIC;
}

static DWORD APIENTRY blt(PDD_BLTDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    DD_SLOT *dst, *src = NULL;
    DWORD operation = data->dwFlags & ~(DDBLT_WAIT | DDBLT_DONOTWAIT);
    /* NT may omit the low GDI operation word below the ROP3. */
    BOOL sourceCopy = operation == 0 || (operation == DDBLT_ROP &&
                      (data->bltFX.dwROP >> 16) == (SRCCOPY >> 16));
    BOOL primary;
    HANDLE owner = EngGetCurrentProcessId();
    DWORD rc = DDHAL_DRIVER_HANDLED;
    EngAcquireSemaphore(d->ddLock);
    dst = find_slot(d, data->lpDDDestSurface->lpGbl);
    if (data->lpDDSrcSurface) src = find_slot(d, data->lpDDSrcSurface->lpGbl);
    primary = !dst && src && mapped_primary(d, data->lpDDDestSurface);
    if (!dst && !src) rc = DDHAL_DRIVER_NOTHANDLED;
    else if ((!dst && !primary) || (data->lpDDSrcSurface && !src)) data->ddRVal = DDERR_UNSUPPORTED;
    else if ((dst && (dst->lost || dst->owner != owner)) || (src && (src->lost || src->owner != owner)))
        data->ddRVal = DDERR_SURFACELOST;
    else if ((dst && dst->locked) || (src && src->locked)) data->ddRVal = DDERR_SURFACEBUSY;
    /* NT5 has already intersected a window clipper with the visible region.
     * Each callback describes one visible source/destination rectangle pair. */
    else if (data->IsClipped) data->ddRVal = DDERR_UNSUPPORTED;
    else if (primary)
        data->ddRVal = sourceCopy ? present(d, src, &data->rSrc, &data->rDest) : DDERR_UNSUPPORTED;
    else if (!full_rect(&data->rDest, dst))
        data->ddRVal = DDERR_UNSUPPORTED;
    else if (!src && operation == DDBLT_COLORFILL) {
        RB2K_DD_FILL fill;
        fill.id = dst->id;
        fill.color = data->bltFX.dwFillColor;
        data->ddRVal = request(d, IOCTL_RB2K_DD_FILL, &fill, sizeof(fill), NULL, 0)
                     ? DD_OK : DDERR_SURFACELOST;
        if (FAILED(data->ddRVal)) dst->lost = TRUE;
    } else if (src && sourceCopy &&
               src->width == dst->width && src->height == dst->height && full_rect(&data->rSrc, src)) {
        RB2K_DD_COPY copy;
        copy.source = src->id;
        copy.destination = dst->id;
        data->ddRVal = request(d, IOCTL_RB2K_DD_COPY, &copy, sizeof(copy), NULL, 0)
                     ? DD_OK : DDERR_SURFACELOST;
    } else data->ddRVal = DDERR_UNSUPPORTED;
    EngReleaseSemaphore(d->ddLock);
    return rc;
}

/* Probe and access the runtime-owned staging buffer within the same SEH
 * frame. No user pointer is passed to the miniport or kept after unlock. */
static BOOL copy_staging(PVOID user, PVOID buffer, ULONG bytes, BOOL toUser)
{
    __try {
        if (toUser) {
            EngProbeForReadAndWrite(user, bytes, 1);
            if (buffer) RtlCopyMemory(user, buffer, bytes);
        } else {
            EngProbeForRead(user, bytes, 1);
            RtlCopyMemory(buffer, user, bytes);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    return TRUE;
}

static DWORD APIENTRY lock_surface(PDD_LOCKDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    PDD_SURFACE_GLOBAL g = data->lpDDSurface->lpGbl;
    DD_SLOT *s;
    PVOID tmp = NULL;
    ULONG bytes, offset = 0;
    DWORD rc = DDHAL_DRIVER_HANDLED;
    EngAcquireSemaphore(d->ddLock);
    s = find_slot(d, g);
    if (!s) { rc = DDHAL_DRIVER_NOTHANDLED; goto done; }
    if (s->lost || s->owner != EngGetCurrentProcessId()) {
        data->ddRVal = DDERR_SURFACELOST; goto done;
    }
    if (s->locked) { data->ddRVal = DDERR_SURFACEBUSY; goto done; }
    if ((data->dwFlags & (DDLOCK_READONLY | DDLOCK_WRITEONLY)) ==
        (DDLOCK_READONLY | DDLOCK_WRITEONLY)) {
        data->ddRVal = DDERR_INVALIDPARAMS; goto done;
    }
    if (data->bHasRect) {
        const RECTL *r = &data->rArea;
        if (!rect_inside(r, s->width, s->height)) {
            data->ddRVal = DDERR_INVALIDRECT; goto done;
        }
        offset = ((ULONG)r->top * s->width + r->left) * 4;
    }
    bytes = s->width * s->height * 4;
    if (!(data->dwFlags & DDLOCK_WRITEONLY) ||
        (data->bHasRect && !full_rect(&data->rArea, s))) {
        tmp = EngAllocMem(0, bytes, 'l2bR');
        if (!tmp) { data->ddRVal = DDERR_OUTOFMEMORY; goto done; }
        if (!request(d, IOCTL_RB2K_DD_READ, &s->id, sizeof(s->id), tmp, bytes)) {
            s->lost = TRUE; data->ddRVal = DDERR_SURFACELOST; goto done;
        }
    }
    if (!copy_staging((PVOID)g->fpVidMem, tmp, bytes, TRUE)) {
        data->ddRVal = DDERR_GENERIC; goto done;
    }
    s->locked = TRUE;
    s->writable = !(data->dwFlags & DDLOCK_READONLY);
    data->lpSurfData = (BYTE *)g->fpVidMem + offset;
    data->ddRVal = DD_OK;
done:
    if (tmp) EngFreeMem(tmp);
    EngReleaseSemaphore(d->ddLock);
    return rc;
}

static DWORD APIENTRY unlock_surface(PDD_UNLOCKDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    PDD_SURFACE_GLOBAL g = data->lpDDSurface->lpGbl;
    DD_SLOT *s;
    DWORD rc = DDHAL_DRIVER_HANDLED;
    EngAcquireSemaphore(d->ddLock);
    s = find_slot(d, g);
    if (!s) rc = DDHAL_DRIVER_NOTHANDLED;
    else if (s->owner != EngGetCurrentProcessId() || s->lost)
        data->ddRVal = DDERR_SURFACELOST;
    else if (!s->locked) data->ddRVal = DDERR_NOTLOCKED;
    else {
        data->ddRVal = DD_OK;
        if (s->writable) {
            ULONG bytes = s->width * s->height * 4;
            ULONG *packet = EngAllocMem(0, sizeof(ULONG) + bytes, 'u2bR');
            if (packet) {
                *packet = s->id;
                if (!copy_staging((PVOID)g->fpVidMem, packet + 1, bytes, FALSE) ||
                    !request(d, IOCTL_RB2K_DD_WRITE, packet, sizeof(ULONG) + bytes, NULL, 0))
                    data->ddRVal = DDERR_SURFACELOST;
                EngFreeMem(packet);
            } else data->ddRVal = DDERR_OUTOFMEMORY;
            if (FAILED(data->ddRVal)) s->lost = TRUE;
        }
        s->locked = s->writable = FALSE;
    }
    EngReleaseSemaphore(d->ddLock);
    return rc;
}

BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV pdev, DD_HALINFO *info, DWORD *heapCount,
                                   VIDEOMEMORY *heaps, DWORD *fourCount, DWORD *fourCC)
{
    DISPLAY *d = (DISPLAY *)pdev;
    RB2K_DD_CAPS caps;
    VIDEOMEMORYINFO *v = &info->vmiData;
    (void)heaps; (void)fourCC;
    *heapCount = *fourCount = 0;
    if (!d->ddLock || !d->mapping || !rb_dd_caps(d, &caps)) return FALSE;
    RtlZeroMemory(info, sizeof(*info));
    info->dwSize = sizeof(*info);
    v->pvPrimary = d->mapping;
    v->dwDisplayWidth = d->mode.VisScreenWidth;
    v->dwDisplayHeight = d->mode.VisScreenHeight;
    v->lDisplayPitch = d->mode.ScreenStride;
    v->ddpfDisplay.dwSize = sizeof(v->ddpfDisplay);
    v->ddpfDisplay.dwFlags = DDPF_RGB;
    v->ddpfDisplay.dwRGBBitCount = 32;
    v->ddpfDisplay.dwRBitMask = d->mode.RedMask;
    v->ddpfDisplay.dwGBitMask = d->mode.GreenMask;
    v->ddpfDisplay.dwBBitMask = d->mode.BlueMask;
    v->dwOffscreenAlign = 4;
    info->ddCaps.dwSize = sizeof(info->ddCaps);
    info->ddCaps.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL;
    /* DirectDraw indexes the raster-op bitset by the eight-bit ROP3 code. */
    info->ddCaps.dwRops[((SRCCOPY >> 16) & 255) / 32] |= 1UL << ((SRCCOPY >> 16) & 31);
    info->ddCaps.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY |
                                  DDSCAPS_LOCALVIDMEM;
    info->ddCaps.dwVidMemTotal = caps.maxBytes * caps.slots;
    {
        ULONG i;
        EngAcquireSemaphore(d->ddLock);
        for (i = 0; i < caps.slots; ++i)
            if (!d->slots[i].global) info->ddCaps.dwVidMemFree += caps.maxBytes;
        EngReleaseSemaphore(d->ddLock);
    }
    return TRUE;
}

BOOL APIENTRY DrvEnableDirectDraw(DHPDEV pdev, DD_CALLBACKS *c, DD_SURFACECALLBACKS *sc,
                                  DD_PALETTECALLBACKS *pc)
{
    DISPLAY *d = (DISPLAY *)pdev;
    RB2K_DD_CAPS caps;
    if (!d->ddLock || !rb_dd_caps(d, &caps)) return FALSE;
    RtlZeroMemory(c, sizeof(*c)); RtlZeroMemory(sc, sizeof(*sc)); RtlZeroMemory(pc, sizeof(*pc));
    c->dwSize = sizeof(*c); sc->dwSize = sizeof(*sc); pc->dwSize = sizeof(*pc);
    c->dwFlags = DDHAL_CB32_CANCREATESURFACE | DDHAL_CB32_CREATESURFACE;
    c->CanCreateSurface = can_create;
    c->CreateSurface = create_surface;
    sc->dwFlags = DDHAL_SURFCB32_DESTROYSURFACE | DDHAL_SURFCB32_BLT |
                  DDHAL_SURFCB32_LOCK | DDHAL_SURFCB32_UNLOCK;
    sc->DestroySurface = destroy_surface;
    sc->Blt = blt;
    sc->Lock = lock_surface;
    sc->Unlock = unlock_surface;
    EngAcquireSemaphore(d->ddLock);
    d->ddEnabled = TRUE;
    EngReleaseSemaphore(d->ddLock);
    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV pdev)
{
    DISPLAY *d = (DISPLAY *)pdev;
    ULONG i;
    if (!d->ddLock) return;
    EngAcquireSemaphore(d->ddLock);
    d->ddEnabled = FALSE;
    for (i = 0; i < RB2K_DD_SLOTS; ++i)
        if (d->slots[i].global) release_slot(d, &d->slots[i]);
    EngReleaseSemaphore(d->ddLock);
}
