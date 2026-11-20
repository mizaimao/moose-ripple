/* NT5 DirectDraw colour fills. The authoritative pixels live in a host
 * texture. DirectDraw allocates an individual user-mode staging buffer
 * for each surface, which is synchronized only at a CPU lock boundary. */
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

static BOOL supported(const DDSURFACEDESC *desc)
{
    DWORD caps = desc->ddsCaps.dwCaps;
    const DDPIXELFORMAT *fmt = &desc->ddpfPixelFormat;
    if (!(caps & DDSCAPS_OFFSCREENPLAIN) ||
        (caps & ~(DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM)) ||
        !desc->dwWidth || desc->dwWidth > RB2K_DD_MAX_BYTES / 4 ||
        !desc->dwHeight || desc->dwHeight > RB2K_DD_MAX_BYTES / (4 * desc->dwWidth))
        return FALSE;
    return !(desc->dwFlags & DDSD_PIXELFORMAT) ||
        (fmt->dwFlags == DDPF_RGB && fmt->dwRGBBitCount == 32 &&
         fmt->dwRBitMask == 0xff0000 && fmt->dwGBitMask == 0xff00 &&
         fmt->dwBBitMask == 0xff);
}

static DWORD APIENTRY can_create(PDD_CANCREATESURFACEDATA data)
{
    data->ddRVal = supported(data->lpDDSurfaceDesc) ? DD_OK : DDERR_INVALIDCAPS;
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
    if (!supported(desc) || data->dwSCnt != 1) {
        data->ddRVal = DDERR_INVALIDCAPS;
        return rc;
    }
    g = data->lplpSList[0]->lpGbl;
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

static DWORD APIENTRY blt(PDD_BLTDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    RB2K_DD_FILL fill;
    DD_SLOT *s;
    DWORD rc = DDHAL_DRIVER_HANDLED;
    EngAcquireSemaphore(d->ddLock);
    s = find_slot(d, data->lpDDDestSurface->lpGbl);
    if (!s) {
        if (data->lpDDSrcSurface && find_slot(d, data->lpDDSrcSurface->lpGbl))
            data->ddRVal = DDERR_UNSUPPORTED;
        else rc = DDHAL_DRIVER_NOTHANDLED;
    }
    else if (s->lost || s->owner != EngGetCurrentProcessId()) data->ddRVal = DDERR_SURFACELOST;
    else if (s->locked) data->ddRVal = DDERR_SURFACEBUSY;
    else if (data->IsClipped || !full_rect(&data->rDest, s) ||
             !(data->dwFlags & DDBLT_COLORFILL) ||
             (data->dwFlags & ~(DDBLT_COLORFILL | DDBLT_WAIT | DDBLT_DONOTWAIT)))
        data->ddRVal = DDERR_UNSUPPORTED;
    else {
        fill.id = s->id;
        fill.color = data->bltFX.dwFillColor;
        data->ddRVal = request(d, IOCTL_RB2K_DD_FILL, &fill, sizeof(fill), NULL, 0)
                     ? DD_OK : DDERR_SURFACELOST;
        if (FAILED(data->ddRVal)) s->lost = TRUE;
    }
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
        if (r->left < 0 || r->top < 0 || r->right <= r->left || r->bottom <= r->top ||
            r->right > (LONG)s->width || r->bottom > (LONG)s->height) {
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
    info->ddCaps.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY |
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
