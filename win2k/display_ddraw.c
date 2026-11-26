/* NT5 DirectDraw offscreen surfaces live in host textures. The runtime's
 * per-surface staging buffer is synchronized at a CPU lock boundary.
 * Windowed presentation updates the visible part of the GDI framebuffer. */
#include "display_private.h"
#include "d3d8ddi.h"

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

static BOOL primary_caps(const DISPLAY *d, DWORD caps)
{
    /* D3DX7 also marks its windowed presentation surface as a 3D device
     * when the actual render target is a separate offscreen buffer. */
    if (d->nativeD3D7) caps &= ~DDSCAPS_3DDEVICE;
    return (caps & DDSCAPS_PRIMARYSURFACE) &&
           !(caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE |
                      DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM));
}

static BOOL flip_caps(DWORD caps)
{
    DWORD allowed = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER |
                    DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VISIBLE |
                    DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    return !(caps & ~allowed) && (caps & (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER)) &&
           (caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER));
}

/* Native Direct3D draws the back buffer of a full-screen flip chain on the
 * host GPU (in 32-bit modes), so such a chain may be a 3D device. */
static DWORD chain_caps(const DISPLAY *d, DWORD caps)
{
    return d->nativeD3D7 && d->gpuSurfaces && d->mode.BitsPerPlane == 32 ? caps & ~DDSCAPS_3DDEVICE : caps;
}

static ULONG page_size(DISPLAY *d)
{
    RB2K_DD_FLIP_INFO f;
    if (!request(d, IOCTL_RB2K_DD_FLIP_INFO, NULL, 0, &f, sizeof(f)) ||
        f.pageCount != 2 || f.width != d->mode.VisScreenWidth || f.height != d->mode.VisScreenHeight ||
        !f.height || f.pitch != d->mode.ScreenStride || f.width > f.pitch / (d->mode.BitsPerPlane / 8) ||
        !f.pageBytes || f.pageBytes > d->mappingBytes / 2 ||
        f.pageBytes % f.height || f.pageBytes / f.height != f.pitch) return 0;
    return f.pageBytes;
}

/* 32 bits of depth buffer: 24 of depth, and 8 of stencil or unused
 * (Direct3D's D24S8 and D24X8), with the stencil in either byte. */
static BOOL depth_stencil(const DDPIXELFORMAT *f)
{
    if (f->dwZBufferBitDepth != 32) return FALSE;
    if (f->dwFlags == DDPF_ZBUFFER) return f->dwZBitMask == 0xffffff00 || f->dwZBitMask == 0x00ffffff;
    return f->dwFlags == (DDPF_ZBUFFER | DDPF_STENCILBUFFER) && f->dwStencilBitDepth == 8 &&
           ((f->dwZBitMask == 0xffffff00 && f->dwStencilBitMask == 0xff) ||
            (f->dwZBitMask == 0x00ffffff && f->dwStencilBitMask == 0xff000000));
}

static BOOL supported(const DISPLAY *d, const DDSURFACEDESC *desc)
{
    DWORD caps = desc->ddsCaps.dwCaps;
    DWORD storage = caps & (DDSCAPS_TEXTURE | DDSCAPS_OFFSCREENPLAIN);
    const DDPIXELFORMAT *fmt = &desc->ddpfPixelFormat;
    if (caps & DDSCAPS_ZBUFFER) {
        if (!d->z16Storage || !d->nativeD3D7 || !d->gpuSurfaces ||
            (caps & ~(DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM)) ||
            !desc->dwWidth || desc->dwWidth > RB2K_DD_TARGET_SIDE ||
            !desc->dwHeight || desc->dwHeight > RB2K_DD_TARGET_SIDE)
            return FALSE;
        if (desc->dwFlags & DDSD_PIXELFORMAT)
            return fmt->dwSize == sizeof(*fmt) &&
                   ((fmt->dwFlags == DDPF_ZBUFFER && fmt->dwZBufferBitDepth == 16 && fmt->dwZBitMask == 0xffff) ||
                    (d->z24s8 && depth_stencil(fmt)));
        return (desc->dwFlags & DDSD_ZBUFFERBITDEPTH) && desc->dwZBufferBitDepth == 16;
    }
    if ((caps & DDSCAPS_TEXTURE) && (caps & DDSCAPS_3DDEVICE)) {
        /* A render target texture: a host image that Direct3D draws into
         * and samples (32-bit, one level). A cube map one (COMPLEX, DDSCAPS2_CUBEMAP in a
         * DDSURFACEDESC2) is six of them, one per face. */
        BOOL cube = desc->dwSize >= sizeof(DDSURFACEDESC2) &&
                    (((const DDSURFACEDESC2 *)desc)->ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP);
        if (!d->nativeD3D7 || !d->gpuSurfaces || d->mode.BitsPerPlane != 32 ||
            (caps & ~(DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM |
                      (cube ? DDSCAPS_COMPLEX : 0))) ||
            (cube && desc->dwWidth != desc->dwHeight) ||
            !desc->dwWidth || desc->dwWidth > RB2K_DD_TARGET_SIDE ||
            !desc->dwHeight || desc->dwHeight > RB2K_DD_TARGET_SIDE) return FALSE;
        if (!(desc->dwFlags & DDSD_PIXELFORMAT)) return TRUE;
        return fmt->dwRGBBitCount == 32 && fmt->dwRBitMask == 0xff0000 && fmt->dwGBitMask == 0xff00 &&
               fmt->dwBBitMask == 0xff && (fmt->dwFlags == DDPF_RGB ||
               (fmt->dwFlags == (DDPF_RGB | DDPF_ALPHAPIXELS) && fmt->dwRGBAlphaBitMask == 0xff000000));
    }
    if ((caps & DDSCAPS_TEXTURE) && !(caps & DDSCAPS_3DDEVICE)) {
        /* A Direct3D texture in runtime-allocated user memory; the native
         * path reads and uploads it at draw time (display_d3d7.c). */
        DDPIXELFORMAT desktop = {0};
        if (!d->nativeD3D7 ||
            (caps & ~(DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX)) ||
            !desc->dwWidth || desc->dwWidth > RB2K_D7_TEXTURE_SIDE ||
            !desc->dwHeight || desc->dwHeight > RB2K_D7_TEXTURE_SIDE) return FALSE;
        return rb_d7_texture_format((desc->dwFlags & DDSD_PIXELFORMAT) ? fmt : &desktop,
                                    d->mode.BitsPerPlane) != TexNone;
    }
    if (primary_caps(d, caps) || flip_caps(chain_caps(d, caps))) {
        if (flip_caps(chain_caps(d, caps)) && (!d->pageBytes ||
            ((desc->dwFlags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCount > 1))) return FALSE;
        if (!d->mapping || !d->surface ||
            ((desc->dwFlags & DDSD_WIDTH) && desc->dwWidth != d->mode.VisScreenWidth) ||
            ((desc->dwFlags & DDSD_HEIGHT) && desc->dwHeight != d->mode.VisScreenHeight))
            return FALSE;
    } else if (!d->gpuSurfaces || storage != DDSCAPS_OFFSCREENPLAIN ||
               (!d->plainSurfaces && !(caps & DDSCAPS_3DDEVICE)) ||
               /* Textures returned above; render-to-texture is not offered. */
               (caps & ~(DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM |
                         DDSCAPS_3DDEVICE)) ||
               !desc->dwWidth || desc->dwWidth > RB2K_DD_TARGET_SIDE ||
               !desc->dwHeight || desc->dwHeight > RB2K_DD_TARGET_SIDE) return FALSE;
    if (!(desc->dwFlags & DDSD_PIXELFORMAT)) return TRUE;
    if (fmt->dwRGBBitCount != d->mode.BitsPerPlane) return FALSE;
    if (d->mode.BitsPerPlane == 8)
        return (fmt->dwFlags & DDPF_PALETTEINDEXED8) &&
               !(fmt->dwFlags & ~(DDPF_PALETTEINDEXED8 | DDPF_RGB));
    /* A 32-bit render target may keep alpha: the host image stores it. */
    if (fmt->dwFlags == (DDPF_RGB | DDPF_ALPHAPIXELS) && d->mode.BitsPerPlane == 32 && (caps & DDSCAPS_3DDEVICE) &&
        fmt->dwRGBAlphaBitMask == 0xff000000 && fmt->dwRBitMask == d->mode.RedMask &&
        fmt->dwGBitMask == d->mode.GreenMask && fmt->dwBBitMask == d->mode.BlueMask) return TRUE;
    return fmt->dwFlags == DDPF_RGB && fmt->dwRBitMask == d->mode.RedMask &&
           fmt->dwGBitMask == d->mode.GreenMask && fmt->dwBBitMask == d->mode.BlueMask;
}

static DWORD APIENTRY can_create(PDD_CANCREATESURFACEDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    DWORD caps = data->lpDDSurfaceDesc->ddsCaps.dwCaps;
    /* Without host-texture surfaces an offscreen surface goes to system
     * memory, where Windows' own blits are faster here. Native Direct3D
     * keeps only its render targets, depth buffers and textures on the
     * host; plain 2D surfaces stay in system memory unless asked for. */
    if (!primary_caps(d, caps) && !flip_caps(chain_caps(d, caps)) &&
        (!d->gpuSurfaces ||
         (!d->plainSurfaces && !(caps & (DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER)))))
        data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
    else
        data->ddRVal = supported(d, data->lpDDSurfaceDesc) ? DD_OK : DDERR_INVALIDCAPS;
    if (data->ddRVal != DD_OK && (caps & DDSCAPS_TEXTURE)) {   /* counted for d7draw.exe */
        const DDSURFACEDESC *s = data->lpDDSurfaceDesc;
        ++d->d7draw.surfaceRefused;
        d->d7draw.refusedCaps = caps; d->d7draw.refusedFlags = s->dwFlags;
        d->d7draw.refusedWidth = s->dwWidth; d->d7draw.refusedHeight = s->dwHeight;
        d->d7draw.refusedFormat = s->ddpfPixelFormat.dwFlags << 8 | s->ddpfPixelFormat.dwRGBBitCount;
    }
    return DDHAL_DRIVER_HANDLED;
}

DD_SLOT *rb_dd_slot(DISPLAY *d, PDD_SURFACE_GLOBAL global)
{
    ULONG i;
    for (i = 0; i < RB2K_DD_SLOTS; ++i) {
        if (d->slots[i].global == global) return &d->slots[i];
    }
    return NULL;
}

static BOOL select_page(DISPLAY *d, ULONG page)
{
    if (!request(d, IOCTL_RB2K_DD_FLIP, &page, sizeof(page), NULL, 0)) return FALSE;
    d->visiblePage = page;
    return TRUE;
}

static BOOL desktop_page(DISPLAY *d)
{
    return !d->visiblePage || select_page(d, 0);
}

static void release_slot(DISPLAY *d, DD_SLOT *slot);

static void release_chain(DISPLAY *d)
{
    ULONG i;
    for (i = 0; i < RB2K_DD_SLOTS; ++i)
        if (d->slots[i].global && d->slots[i].chain) release_slot(d, &d->slots[i]);
    if (d->flipBack && request(d, IOCTL_RB2K_DD_FLIP_RELEASE, NULL, 0, NULL, 0))
        d->visiblePage = 0;
    else desktop_page(d);
    d->flipBack = d->flipFront = NULL;
    d->flipOwner = NULL;
    d->activeView = NULL;
    d->lockedPages = 0;
}

static ULONG slot_pitch(const DD_SLOT *slot)
{
    return slot->width * (slot->depth16 && !slot->stencil ? sizeof(USHORT) : sizeof(ULONG));
}

static DWORD APIENTRY create_surface(PDD_CREATESURFACEDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    PDD_SURFACE_GLOBAL g;
    DDSURFACEDESC *desc = data->lpDDSurfaceDesc;
    RB2K_DD_CREATE create;
    DD_SLOT *slot;
    ULONG createCode = IOCTL_RB2K_DD_CREATE;
    DWORD rc = DDHAL_DRIVER_HANDLED;
    BOOL cubeTarget = (desc->ddsCaps.dwCaps & (DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)) ==
                      (DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE);
    ULONG i, count = 1;
    if (!supported(d, desc) || !data->dwSCnt ||
        (data->dwSCnt != 1 && !(desc->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && !(cubeTarget && data->dwSCnt == 6))) {
        ++d->d7draw.surfaceRefused;
        d->d7draw.refusedCaps = desc->ddsCaps.dwCaps; d->d7draw.refusedFlags = desc->dwFlags;
        d->d7draw.refusedWidth = desc->dwWidth; d->d7draw.refusedHeight = desc->dwHeight;
        d->d7draw.refusedCount = data->dwSCnt;
        data->ddRVal = DDERR_INVALIDCAPS;
        return rc;
    }
    g = data->lplpSList[0]->lpGbl;
    if ((desc->ddsCaps.dwCaps & DDSCAPS_TEXTURE) && !(desc->ddsCaps.dwCaps & DDSCAPS_3DDEVICE)) {
        DDPIXELFORMAT desktop = {0};
        ULONG format = rb_d7_texture_format((desc->dwFlags & DDSD_PIXELFORMAT) ? &desc->ddpfPixelFormat :
                                            &desktop, d->mode.BitsPerPlane);
        ULONG i, depth = 1;
        PDD_SURFACE_MORE more = data->lplpSList[0]->lpSurfMore;
        /* NT creates a mipmap chain one level per call, each with the
         * top level's description; every level gets its own user memory
         * and the native path draws from the top level. */
        if (more && (more->ddsCapsEx.dwCaps2 & DDSCAPS2_VOLUME)) {
            /* A volume: its slices follow each other, a slice pitch apart (DXT ones too,
             * in rows of 4x4 blocks). */
            depth = more->ddsCapsEx.dwCaps4;
            if (!depth || depth > RB2K_D7_VOLUME_SIDE || format == TexNone) {
                /* counted for d7draw.exe: the count field holds the depth */
                ++d->d7draw.surfaceRefused;
                d->d7draw.refusedCaps = desc->ddsCaps.dwCaps; d->d7draw.refusedFlags = desc->dwFlags;
                d->d7draw.refusedWidth = desc->dwWidth; d->d7draw.refusedHeight = desc->dwHeight;
                d->d7draw.refusedFormat = desc->ddpfPixelFormat.dwFlags << 8 | desc->ddpfPixelFormat.dwRGBBitCount;
                d->d7draw.refusedCount = 0x10000 | depth;
                data->ddRVal = DDERR_INVALIDPARAMS;
                return rc;
            }
        }
        if (!(desc->ddsCaps.dwCaps & DDSCAPS_MIPMAP) &&
            (g->wWidth != desc->dwWidth || g->wHeight != desc->dwHeight)) {
            data->ddRVal = DDERR_INVALIDPARAMS;
            return rc;
        }
        for (i = 0; i < data->dwSCnt; ++i) {
            PDD_SURFACE_GLOBAL level = data->lplpSList[i]->lpGbl;
            if (!level->wWidth || level->wWidth > desc->dwWidth ||
                !level->wHeight || level->wHeight > desc->dwHeight) {
                data->ddRVal = DDERR_INVALIDPARAMS;
                return rc;
            }
            /* Compressed levels hold rows of 4x4 blocks. */
            level->lPitch = (rb_d7_row_bytes(format, level->wWidth) + 3) & ~3;
            level->dwUserMemSize = level->lPitch * rb_d7_rows(format, level->wHeight);
            if (depth > 1) {
                ULONG shift = 0, slices;
                while (shift < 16 && (desc->dwWidth >> shift) > level->wWidth) ++shift;
                slices = depth >> shift ? depth >> shift : 1;
                level->dwBlockSizeY = level->dwUserMemSize;   /* the slice pitch */
                level->dwUserMemSize *= slices;
            }
            level->fpVidMem = DDHAL_PLEASEALLOC_USERMEM;
        }
        desc->lPitch = g->lPitch;
        desc->dwFlags |= DDSD_PITCH;
        data->ddRVal = DD_OK;
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (data->lplpSList[0]->ddsCaps.dwCaps & DDSCAPS_BACKBUFFER) {
        /* A Direct3D chain's back buffer is drawn in a host image as well,
         * which each flip copies into the page about to be shown. */
        BOOL three = (data->lplpSList[0]->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) != 0;
        EngAcquireSemaphore(d->ddLock);
        slot = NULL;
        if (!d->ddEnabled || !d->pageBytes || !flip_caps(chain_caps(d, data->lplpSList[0]->ddsCaps.dwCaps)) ||
            g->wWidth != d->mode.VisScreenWidth || g->wHeight != d->mode.VisScreenHeight)
            data->ddRVal = DDERR_INVALIDCAPS;
        else if (d->flipBack) data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
        else if (three && (rb_dd_slot(d, g) || !(slot = rb_dd_slot(d, NULL))))
            data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
        else if (three && (create.width = g->wWidth, create.height = g->wHeight,
                           !request(d, IOCTL_RB2K_DD_CREATE, &create, sizeof(create), &slot->id, sizeof(slot->id))))
            data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
        else if (!request(d, IOCTL_RB2K_DD_FLIP_ACQUIRE, NULL, 0, NULL, 0)) {
            if (three) request(d, IOCTL_RB2K_DD_DESTROY, &slot->id, sizeof(slot->id), NULL, 0);
            data->ddRVal = DDERR_SURFACEBUSY;
        } else {
            if (three) {
                slot->global = g;
                slot->owner = EngGetCurrentProcessId();
                slot->width = g->wWidth;
                slot->height = g->wHeight;
                slot->chain = TRUE;
                slot->alpha = (desc->dwFlags & DDSD_PIXELFORMAT) && (desc->ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS);
            }
            g->lPitch = desc->lPitch = d->mode.ScreenStride;
            desc->dwFlags |= DDSD_PITCH;
            g->dwBlockSizeX = d->pageBytes;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
            d->flipBack = g;
            d->flipOwner = EngGetCurrentProcessId();
            /* The miniport has just revoked any other process's view of the pages. */
            {
                DD_FLIP_VIEW **p = &d->views;
                while (*p) {
                    if ((*p)->owner != d->flipOwner) {
                        DD_FLIP_VIEW *gone = *p;
                        if (gone == d->activeView) d->activeView = NULL;
                        *p = gone->next;
                        EngFreeMem(gone);
                    } else p = &(*p)->next;
                }
            }
            /* A chain rebuilt in the same DirectDraw session uses the view mapped
             * for the earlier one: DirectDraw does not map again. */
            if (!d->activeView) {
                DD_FLIP_VIEW *v = d->views;
                while (v && v->owner != d->flipOwner) v = v->next;
                d->activeView = v;
            }
            data->ddRVal = DD_OK;
            rc = DDHAL_DRIVER_NOTHANDLED;
        }
        EngReleaseSemaphore(d->ddLock);
        return rc;
    }
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
    {
        ULONG *log = d->d7draw.createLog[d->d7draw.creates++ & 7], used = 0, k;
        for (k = 0; k < RB2K_DD_SLOTS; ++k) if (d->slots[k].global) ++used;
        log[0] = desc->ddsCaps.dwCaps;
        log[1] = desc->dwWidth << 16 | (desc->dwHeight & 0xffff);
        log[2] = data->dwSCnt;
        log[3] = used << 8;
        log[4] = 0;
    }
    create.width = desc->dwWidth;
    create.height = desc->dwHeight;
    if (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER)
        createCode = (desc->dwFlags & DDSD_PIXELFORMAT) && depth_stencil(&desc->ddpfPixelFormat) ?
                     IOCTL_RB2K_DD_CREATE_Z24S8 : IOCTL_RB2K_DD_CREATE_Z16;
    /* A cube map render target may come with its six faces in one call: an image for each. */
    if (cubeTarget) count = data->dwSCnt;
    EngAcquireSemaphore(d->ddLock);
    for (i = 0; i < count; ++i) {
    g = data->lplpSList[i]->lpGbl;
    slot = rb_dd_slot(d, NULL);
    if (!d->ddEnabled || rb_dd_slot(d, g))
        data->ddRVal = DDERR_SURFACELOST;
    else if (!slot || !request(d, createCode, &create, sizeof(create),
                               &slot->id, sizeof(slot->id))) {
        data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
        d->d7draw.createLog[(d->d7draw.creates - 1) & 7][3] |= slot ? 2 : 1;
        d->d7draw.createLog[(d->d7draw.creates - 1) & 7][4] = data->ddRVal;
    }
    else {
        slot->global = g;
        slot->owner = EngGetCurrentProcessId();
        slot->width = create.width;
        slot->height = create.height;
        slot->depth16 = createCode != IOCTL_RB2K_DD_CREATE;
        slot->stencil = createCode == IOCTL_RB2K_DD_CREATE_Z24S8;
        slot->alpha = !slot->depth16 && (desc->dwFlags & DDSD_PIXELFORMAT) &&
                      (desc->ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS);
        slot->texture = (desc->ddsCaps.dwCaps & DDSCAPS_TEXTURE) != 0;
        g->lPitch = slot_pitch(slot);
        g->dwUserMemSize = g->lPitch * create.height;
        g->fpVidMem = DDHAL_PLEASEALLOC_USERMEM;
        desc->lPitch = g->lPitch;
        desc->dwFlags |= DDSD_PITCH;
        data->ddRVal = DD_OK;
        /* NT completes the requested allocation after this return. */
        rc = DDHAL_DRIVER_NOTHANDLED;
    }
    if (data->ddRVal != DD_OK) break;
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
    if (data->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_TEXTURE)
        rb_d7_surface_destroyed(d, data->lpDDSurface->lpGbl);
    EngAcquireSemaphore(d->ddLock);
    slot = rb_dd_slot(d, data->lpDDSurface->lpGbl);
    if (slot) release_slot(d, slot);
    if (d->flipBack == data->lpDDSurface->lpGbl || d->flipFront == data->lpDDSurface->lpGbl)
        release_chain(d);
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

/* The front buffer of a full-screen chain that Direct3D draws to. */
BOOL rb_dd_chain_front(const DISPLAY *d, const DD_SURFACE_LOCAL *local)
{
    DWORD caps = local->ddsCaps.dwCaps;
    return d->ddEnabled && (caps & DDSCAPS_PRIMARYSURFACE) && (caps & DDSCAPS_3DDEVICE) &&
           flip_caps(chain_caps(d, caps));
}

/* The Direct3D 8 and 9 runtimes draw to both surfaces of a chain in turn
 * (they swap the surfaces after each flip), so the front buffer gets a host
 * image too, when Direct3D registers it. The caller holds ddLock. */
DD_SLOT *rb_dd_chain_front_slot(DISPLAY *d, const DD_SURFACE_LOCAL *local)
{
    PDD_SURFACE_GLOBAL g = local->lpGbl;
    RB2K_DD_CREATE create;
    DD_SLOT *slot, *back;
    if (!rb_dd_chain_front(d, local) || g->wWidth != d->mode.VisScreenWidth ||
        g->wHeight != d->mode.VisScreenHeight) return NULL;
    if ((slot = rb_dd_slot(d, g))) return slot;
    if (!(slot = rb_dd_slot(d, NULL))) return NULL;
    create.width = g->wWidth;
    create.height = g->wHeight;
    if (!request(d, IOCTL_RB2K_DD_CREATE, &create, sizeof(create), &slot->id, sizeof(slot->id))) return NULL;
    back = d->flipBack ? rb_dd_slot(d, d->flipBack) : NULL;
    slot->global = g;
    slot->owner = EngGetCurrentProcessId();
    slot->width = create.width;
    slot->height = create.height;
    slot->chain = TRUE;
    slot->alpha = back ? back->alpha : FALSE;
    ++d->d7draw.chainFronts;
    return slot;
}

BOOL rb_dd_mapped_primary(const DISPLAY *d, const DD_SURFACE_LOCAL *local)
{
    const DD_SURFACE_GLOBAL *g = local->lpGbl;
    return d->ddEnabled && d->mapping && d->surface && primary_caps(d, local->ddsCaps.dwCaps) &&
           g->wWidth == d->mode.VisScreenWidth && g->wHeight == d->mode.VisScreenHeight &&
           g->lPitch > 0 && (ULONG)g->lPitch == d->mode.ScreenStride &&
           (g->fpVidMem == 0 || g->fpVidMem == (FLATPTR)d->mapping);
}

static BOOL rect_inside(const RECTL *r, ULONG width, ULONG height)
{
    return r->left >= 0 && r->top >= 0 && r->right > r->left && r->bottom > r->top &&
           (ULONG)r->right <= width && (ULONG)r->bottom <= height;
}

static BOOL surface_page(DISPLAY *d, const DD_SURFACE_LOCAL *l, ULONG *page)
{
    const DD_SURFACE_GLOBAL *g;
    ULONG offset;
    if (!l || !l->lpGbl || !d->flipBack || !d->pageBytes || !d->ddEnabled ||
        d->flipOwner != EngGetCurrentProcessId()) return FALSE;
    g = l->lpGbl;
    if (g != d->flipBack && g != d->flipFront &&
        (d->flipFront || !(l->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))) return FALSE;
    if (g->wWidth != d->mode.VisScreenWidth || g->wHeight != d->mode.VisScreenHeight ||
        g->lPitch <= 0 || (ULONG)g->lPitch != d->mode.ScreenStride) return FALSE;
    offset = g->fpVidMem;
    if (offset != 0 && offset != d->pageBytes) return FALSE;
    *page = offset ? 1 : 0;
    return TRUE;
}

static HRESULT present(DISPLAY *d, const DD_SLOT *src, const RECTL *from, const RECTL *to, ULONG page);

static DWORD APIENTRY flip(PDD_FLIPDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    ULONG current, target, *log;
    EngAcquireSemaphore(d->ddLock);
    log = d->d7draw.flipLog[d->d7draw.flips++ & 7];
    log[0] = data->lpSurfCurr ? data->lpSurfCurr->lpGbl->fpVidMem : 0xffffffff;
    log[1] = data->lpSurfTarg ? data->lpSurfTarg->lpGbl->fpVidMem : 0xffffffff;
    log[2] = log[3] = 0;
    if (data->dwFlags & ~(DDFLIP_WAIT | DDFLIP_DONOTWAIT | DDFLIP_NOVSYNC))
        data->ddRVal = DDERR_UNSUPPORTED;
    else if (d->lockedPages) data->ddRVal = DDERR_SURFACEBUSY;
    else if (!surface_page(d, data->lpSurfCurr, &current) ||
             !surface_page(d, data->lpSurfTarg, &target) || current == target ||
             (d->flipBack != data->lpSurfCurr->lpGbl && d->flipBack != data->lpSurfTarg->lpGbl))
        data->ddRVal = DDERR_SURFACELOST;
    else {
        DD_SLOT *drawn = rb_dd_slot(d, data->lpSurfTarg->lpGbl), *shown = rb_dd_slot(d, data->lpSurfCurr->lpGbl);
        HRESULT hr = DD_OK;
        log[2] = shown ? shown->id : 0;
        log[3] = drawn ? drawn->id : 0;
        if (drawn && drawn->chain) {
            /* What Direct3D drew goes into the page about to be shown. */
            RECTL all;
            all.left = all.top = 0;
            all.right = d->mode.VisScreenWidth;
            all.bottom = d->mode.VisScreenHeight;
            if (drawn->lost || drawn->owner != EngGetCurrentProcessId()) hr = DDERR_SURFACELOST;
            else hr = present(d, drawn, &all, &all, target);
            ++d->d7draw.flipPresents;
        }
        d->flipFront = d->flipBack == data->lpSurfTarg->lpGbl ?
                       data->lpSurfCurr->lpGbl : data->lpSurfTarg->lpGbl;
        data->ddRVal = FAILED(hr) ? hr : select_page(d, target) ? DD_OK : DDERR_GENERIC;
    }
    log[4] = data->ddRVal;
    EngReleaseSemaphore(d->ddLock);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY flip_status(PDD_GETFLIPSTATUSDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    ULONG page;
    EngAcquireSemaphore(d->ddLock);
    data->ddRVal = surface_page(d, data->lpDDSurface, &page) ? DD_OK : DDERR_SURFACELOST;
    EngReleaseSemaphore(d->ddLock);
    return DDHAL_DRIVER_HANDLED;
}

static HRESULT fill_page(DISPLAY *d, ULONG page, const RECTL *r, ULONG color)
{
    ULONG x, y, width;
    BYTE *row;
    if (d->lockedPages & (1UL << page)) return DDERR_SURFACEBUSY;
    if (!rect_inside(r, d->mode.VisScreenWidth, d->mode.VisScreenHeight)) return DDERR_INVALIDRECT;
    row = (BYTE *)d->mapping + page * d->pageBytes + r->top * d->mode.ScreenStride +
          r->left * (d->mode.BitsPerPlane / 8);
    width = r->right - r->left;
    for (y = 0; y < (ULONG)(r->bottom - r->top); ++y, row += d->mode.ScreenStride) {
        if (d->mode.BitsPerPlane == 8) {
            RtlFillMemory(row, width, (BYTE)color);
        } else if (d->mode.BitsPerPlane == 16) {
            for (x = 0; x < width; ++x) ((USHORT *)row)[x] = (USHORT)color;
        } else {
            for (x = 0; x < width; ++x) ((ULONG *)row)[x] = color;
        }
    }
    if (d->memoryPrimary) rb_memory_primary_dirty(d, page, r);   /* written past GDI: send it */
    return DD_OK;
}

/* Copy an image into the visible page, or the other one of a flip chain. */
static HRESULT present(DISPLAY *d, const DD_SLOT *src, const RECTL *from, const RECTL *to, ULONG page)
{
    RB2K_DD_PRESENT p;
    if (!rect_inside(from, src->width, src->height) ||
        !rect_inside(to, d->mode.VisScreenWidth, d->mode.VisScreenHeight))
        return DDERR_INVALIDRECT;
    if (from->right - from->left != to->right - to->left ||
        from->bottom - from->top != to->bottom - to->top) return DDERR_UNSUPPORTED;
    p.source = src->id;
    p.srcX = from->left; p.srcY = from->top;
    p.dstX = to->left; p.dstY = to->top + page * d->mode.VisScreenHeight;
    p.width = from->right - from->left; p.height = from->bottom - from->top;
    return request(d, IOCTL_RB2K_DD_PRESENT, &p, sizeof(p), NULL, 0)
         ? DD_OK : DDERR_GENERIC;
}

static DWORD APIENTRY blt(PDD_BLTDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    DD_SLOT *dst, *src = NULL;
    /* A present is complete when this returns, so the present flags of a
     * Direct3D 8 runtime need no queue accounting. */
    DWORD operation = data->dwFlags & ~(DDBLT_WAIT | DDBLT_DONOTWAIT | RB_DDBLT_PRESENTATION |
                                        RB_DDBLT_LAST_PRESENTATION);
    /* NT may omit the low GDI operation word below the ROP3. */
    BOOL sourceCopy = operation == 0 || (operation == DDBLT_ROP &&
                      (data->bltFX.dwROP >> 16) == (SRCCOPY >> 16));
    BOOL primary;
    ULONG page;
    HANDLE owner = EngGetCurrentProcessId();
    DWORD rc = DDHAL_DRIVER_HANDLED;
    if (data->lpDDDestSurface->ddsCaps.dwCaps & DDSCAPS_TEXTURE)
        rb_d7_texture_written(d, data->lpDDDestSurface->lpGbl);
    EngAcquireSemaphore(d->ddLock);
    dst = rb_dd_slot(d, data->lpDDDestSurface->lpGbl);
    if (data->lpDDSrcSurface) src = rb_dd_slot(d, data->lpDDSrcSurface->lpGbl);
    primary = !dst && src && rb_dd_mapped_primary(d, data->lpDDDestSurface);
    if ((dst && dst->chain) || (src && src->chain)) ++d->d7draw.chainBlts;
    if (!dst && !data->lpDDSrcSurface && operation == DDBLT_COLORFILL && !data->IsClipped &&
        surface_page(d, data->lpDDDestSurface, &page))
        data->ddRVal = fill_page(d, page, &data->rDest, data->bltFX.dwFillColor);
    else if (!dst && !src) rc = DDHAL_DRIVER_NOTHANDLED;
    else if ((!dst && !primary) || (data->lpDDSrcSurface && !src)) data->ddRVal = DDERR_UNSUPPORTED;
    else if ((dst && (dst->lost || dst->owner != owner)) || (src && (src->lost || src->owner != owner)))
        data->ddRVal = DDERR_SURFACELOST;
    else if ((dst && dst->locked) || (src && src->locked)) data->ddRVal = DDERR_SURFACEBUSY;
    else if ((dst && dst->depth16) || (src && src->depth16)) data->ddRVal = DDERR_UNSUPPORTED;
    /* NT5 has already intersected a window clipper with the visible region.
     * Each callback describes one visible source/destination rectangle pair. */
    else if (data->IsClipped) data->ddRVal = DDERR_UNSUPPORTED;
    else if (primary)
        data->ddRVal = sourceCopy ? present(d, src, &data->rSrc, &data->rDest, 0) : DDERR_UNSUPPORTED;
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
static BOOL prepare_staging(PVOID user, const VOID *pixels, ULONG bytes, const ULONG *uniformColor)
{
    __try {
        EngProbeForReadAndWrite(user, bytes, 1);
        if (uniformColor) {
            ULONG *out = user;
            ULONG i;
            for (i = 0; i < bytes / sizeof(*out); ++i) out[i] = *uniformColor;
        } else if (pixels) {
            RtlCopyMemory(user, pixels, bytes);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    return TRUE;
}

static BOOL read_staging(PVOID user, PVOID pixels, ULONG bytes)
{
    __try {
        EngProbeForRead(user, bytes, 1);
        RtlCopyMemory(pixels, user, bytes);
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
    RB2K_DD_UNIFORM uniform;
    const ULONG *seed = NULL;
    ULONG bytes, page, offset = 0;
    DWORD rc = DDHAL_DRIVER_HANDLED;
    if (data->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_TEXTURE) rb_d7_texture_written(d, g);
    EngAcquireSemaphore(d->ddLock);
    s = rb_dd_slot(d, g);
    if (s && s->chain) s = NULL;   /* a chain's back buffer locks its page */
    if (!s) {
        if (!surface_page(d, data->lpDDSurface, &page)) {
            if (flip_caps(chain_caps(d, data->lpDDSurface->ddsCaps.dwCaps))) data->ddRVal = DDERR_SURFACELOST;
            else if ((data->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_TEXTURE) && g->fpVidMem && g->lPitch > 0) {
                /* Video-memory textures live in user memory the runtime
                 * allocated for us. Windows would add the process's view of
                 * the frame buffer to that address once a full-screen chain
                 * has mapped one, so the address is given here. */
                ULONG format = rb_d7_texture_format(&g->ddpfSurface, d->mode.BitsPerPlane), front = 0;
                RECTL area = data->rArea;
                if (data->bHasRect && (data->dwFlags & DDLOCK_HASVOLUMETEXTUREBOXRECT)) {
                    /* A box of a volume: front and back ride in the high words. */
                    front = (ULONG)area.left >> 16;
                    area.left &= 0xffff;
                    area.right &= 0xffff;
                }
                if (data->bHasRect && !rect_inside(&area, g->wWidth, g->wHeight))
                    data->ddRVal = DDERR_INVALIDRECT;
                else {
                    offset = front * g->dwBlockSizeY;   /* the slice pitch of a volume */
                    if (data->bHasRect) {
                        if (RB_TEX_COMPRESSED(format))   /* rows of 4x4 blocks */
                            offset += (area.top / 4) * g->lPitch + (area.left / 4) * (format == TexDxt1 ? 8 : 16);
                        else
                            offset += area.top * g->lPitch + area.left *
                                      (format ? rb_d7_texel_bytes(format) : g->ddpfSurface.dwRGBBitCount / 8);
                    }
                    data->lpSurfData = (BYTE *)g->fpVidMem + offset;
                    data->ddRVal = DD_OK;
                }
            }
            else rc = DDHAL_DRIVER_NOTHANDLED;
        } else if (!d->activeView || data->fpProcess != d->activeView->address)
            data->ddRVal = DDERR_CANTLOCKSURFACE;
        else if (d->lockedPages & (1UL << page)) data->ddRVal = DDERR_SURFACEBUSY;
        else if (data->bHasRect && !rect_inside(&data->rArea, g->wWidth, g->wHeight))
            data->ddRVal = DDERR_INVALIDRECT;
        else {
            if (data->bHasRect)
                offset = data->rArea.top * g->lPitch + data->rArea.left * (d->mode.BitsPerPlane / 8);
            /* With the desktop in guest memory, presents may have left the page stale. */
            if (d->memoryPrimary) rb_memory_primary_read(d, page);
            /* NT supplies the user-mode view created by our DdMapMemory. */
            data->lpSurfData = (BYTE *)data->fpProcess + page * d->pageBytes + offset;
            d->lockedPages |= 1UL << page;
            data->ddRVal = DD_OK;
        }
        goto done;
    }
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
        offset = (ULONG)r->top * slot_pitch(s) + r->left * (s->depth16 && !s->stencil ? sizeof(USHORT) : sizeof(ULONG));
    }
    bytes = slot_pitch(s) * s->height;
    if (!(data->dwFlags & DDLOCK_WRITEONLY) ||
        (data->bHasRect && !full_rect(&data->rArea, s))) {
        if (!s->depth16 && !(data->dwFlags & DDLOCK_READONLY) &&
            request(d, IOCTL_RB2K_DD_UNIFORM, &s->id, sizeof(s->id), &uniform, sizeof(uniform)) &&
            uniform.known == 1) {
            seed = &uniform.color;
        } else {
            tmp = EngAllocMem(0, bytes, 'l2bR');
            if (!tmp) { data->ddRVal = DDERR_OUTOFMEMORY; goto done; }
            if (!request(d, IOCTL_RB2K_DD_READ, &s->id, sizeof(s->id), tmp, bytes)) {
                s->lost = TRUE; data->ddRVal = DDERR_SURFACELOST; goto done;
            }
        }
    }
    if (!prepare_staging((PVOID)g->fpVidMem, tmp, bytes, seed)) {
        data->ddRVal = DDERR_GENERIC; goto done;
    }
    s->locked = TRUE;
    s->writable = !(data->dwFlags & DDLOCK_READONLY);
    data->lpSurfData = (BYTE *)g->fpVidMem + offset;
    data->ddRVal = DD_OK;
done:
    {
        ULONG *log = d->d7draw.lockLog[d->d7draw.locks++ & 7];
        log[0] = data->lpDDSurface->ddsCaps.dwCaps;
        log[1] = (ULONG)g->wWidth << 16 | g->wHeight;
        log[2] = g->fpVidMem;
        log[3] = rc == DDHAL_DRIVER_HANDLED && data->ddRVal == DD_OK ? (ULONG)data->lpSurfData : 0;
        log[4] = rc == DDHAL_DRIVER_HANDLED ? data->ddRVal : 0xffffffff;
    }
    if (tmp) EngFreeMem(tmp);
    EngReleaseSemaphore(d->ddLock);
    return rc;
}

static DWORD APIENTRY unlock_surface(PDD_UNLOCKDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    PDD_SURFACE_GLOBAL g = data->lpDDSurface->lpGbl;
    DD_SLOT *s;
    ULONG page;
    DWORD rc = DDHAL_DRIVER_HANDLED;
    if (data->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_TEXTURE) rb_d7_texture_written(d, g);
    EngAcquireSemaphore(d->ddLock);
    s = rb_dd_slot(d, g);
    if (s && s->chain) s = NULL;
    if (!s) {
        if (surface_page(d, data->lpDDSurface, &page) && (d->lockedPages & (1UL << page))) {
            d->lockedPages &= ~(1UL << page);
            /* With the desktop in guest memory, what the program wrote goes to the host. */
            if (d->memoryPrimary) rb_memory_primary_dirty(d, page, NULL);
            data->ddRVal = DD_OK;
        } else rc = DDHAL_DRIVER_NOTHANDLED;
    }
    else if (s->owner != EngGetCurrentProcessId() || s->lost)
        data->ddRVal = DDERR_SURFACELOST;
    else if (!s->locked) data->ddRVal = DDERR_NOTLOCKED;
    else {
        data->ddRVal = DD_OK;
        if (s->writable) {
            ULONG bytes = slot_pitch(s) * s->height;
            ULONG *packet = EngAllocMem(0, sizeof(ULONG) + bytes, 'u2bR');
            if (packet) {
                *packet = s->id;
                if (!read_staging((PVOID)g->fpVidMem, packet + 1, bytes) ||
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

/* A process may receive the same address for its next chain before DirectDraw
 * releases its previous mapping. Count both callbacks against that address. */
static DD_FLIP_VIEW **find_view(DISPLAY *d, HANDLE owner, FLATPTR address)
{
    DD_FLIP_VIEW **p = &d->views;
    while (*p && ((*p)->owner != owner || (*p)->address != address)) p = &(*p)->next;
    return p;
}

void rb_dd_free_views(DISPLAY *d)
{
    while (d->views) {
        DD_FLIP_VIEW *v = d->views;
        d->views = v->next;
        EngFreeMem(v);
    }
}

static HRESULT map_chain(DISPLAY *d, PDD_MAPMEMORYDATA data, HANDLE owner)
{
    VIDEO_SHARE_MEMORY view;
    VIDEO_SHARE_MEMORY_INFORMATION out;
    DD_FLIP_VIEW *record = d->activeView, **link;
    if (!record) {
        record = EngAllocMem(FL_ZERO_MEMORY, sizeof(*record), 'v2bR');
        if (!record) return DDERR_OUTOFMEMORY;
        RtlZeroMemory(&view, sizeof(view));
        view.ProcessHandle = data->hProcess;
        view.ViewSize = d->pageBytes * 2;
        if (!request(d, IOCTL_VIDEO_SHARE_VIDEO_MEMORY, &view, sizeof(view), &out, sizeof(out))) {
            EngFreeMem(record);
            return DDERR_GENERIC;
        }
        link = find_view(d, owner, (FLATPTR)out.VirtualAddress);
        if (*link) {
            EngFreeMem(record);
            record = *link;
        } else {
            record->owner = owner;
            record->address = (FLATPTR)out.VirtualAddress;
            *link = record;
        }
        d->activeView = record;
        record->process = data->hProcess;
    }
    if (record->references == (ULONG)-1 || record->process != data->hProcess) return DDERR_GENERIC;
    ++record->references;
    data->fpProcess = record->address;
    return DD_OK;
}

static DWORD APIENTRY map_memory(PDD_MAPMEMORYDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    HANDLE owner = EngGetCurrentProcessId();
    DD_FLIP_VIEW **link, *record;
    VIDEO_SHARE_MEMORY view;
    DWORD rc = DDHAL_DRIVER_NOTHANDLED;
    EngAcquireSemaphore(d->ddLock);
    data->ddRVal = DD_OK;
    if (data->bMap) {
        /* Windowed surfaces use DirectDraw's own per-surface staging memory. */
        data->fpProcess = 0;
        if (d->flipBack && d->flipOwner == owner) {
            rc = DDHAL_DRIVER_HANDLED;
            data->ddRVal = map_chain(d, data, owner);
        }
    } else {
        link = find_view(d, owner, data->fpProcess);
        record = *link;
        if (record) {
            rc = DDHAL_DRIVER_HANDLED;
            /* The view outlives its chain (a rebuilt chain reuses it), so the last
             * unmap releases it whether or not a chain is active. */
            if (record->references == 1) {
                RtlZeroMemory(&view, sizeof(view));
                view.ProcessHandle = record->process;
                view.RequestedVirtualAddress = (PVOID)record->address;
                if (!request(d, IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY, &view, sizeof(view), NULL, 0))
                    data->ddRVal = DDERR_GENERIC;
                else if (record == d->activeView) d->activeView = NULL;
            }
            if (data->ddRVal == DD_OK && !--record->references) {
                *link = record->next;
                EngFreeMem(record);
            }
        }
    }
    EngReleaseSemaphore(d->ddLock);
    return rc;
}

static DWORD APIENTRY exclusive_mode(PDD_SETEXCLUSIVEMODEDATA data)
{
    DISPLAY *d = (DISPLAY *)data->lpDD->dhpdev;
    EngAcquireSemaphore(d->ddLock);
    data->ddRVal = data->dwEnterExcl || desktop_page(d) ? DD_OK : DDERR_GENERIC;
    EngReleaseSemaphore(d->ddLock);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY driver_info(PDD_GETDRIVERINFODATA data)
{
    /* GUID_NTCallbacks from the Windows 2000 DDK. */
    static const GUID nt = {0x6fe9ecde,0xdf89,0x11d1,{0x9d,0xb0,0,0x60,0x08,0x27,0x71,0xba}};
    DD_NTCALLBACKS callbacks;
    ULONG bytes;
    data->dwActualSize = 0;
    data->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    if (((DISPLAY *)data->dhpdev)->d3dDescribed && rb_d7_info(data))
        return DDHAL_DRIVER_HANDLED;
    if (IsEqualGUID(&data->guidInfo, &nt)) {
        RtlZeroMemory(&callbacks, sizeof(callbacks));
        callbacks.dwSize = sizeof(callbacks);
        callbacks.dwFlags = DDHAL_NTCB32_SETEXCLUSIVEMODE;
        callbacks.SetExclusiveMode = exclusive_mode;
        bytes = min(data->dwExpectedSize, sizeof(callbacks));
        if (bytes) RtlCopyMemory(data->lpvData, &callbacks, bytes);
        data->dwActualSize = sizeof(callbacks);
        data->ddRVal = DD_OK;
    }
    return DDHAL_DRIVER_HANDLED;
}

/* Shared by every PDEV of this driver; d7draw.exe prints them. */
ULONG rb_dd_mode_stats[3][4];

static ULONG *mode_stats(const DISPLAY *d)
{
    return rb_dd_mode_stats[d->mode.BitsPerPlane == 8 ? 0 : d->mode.BitsPerPlane == 16 ? 1 : 2];
}

BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV pdev, DD_HALINFO *info, DWORD *heapCount,
                                   VIDEOMEMORY *heaps, DWORD *fourCount, DWORD *fourCC)
{
    DISPLAY *d = (DISPLAY *)pdev;
    RB2K_DD_CAPS caps;
    VIDEOMEMORYINFO *v = &info->vmiData;
    ULONG pageBytes;
    *heapCount = *fourCount = 0;
    ++mode_stats(d)[0];
    if (!d->ddLock || !d->mapping || !rb_dd_caps(d, &caps)) {
        ++mode_stats(d)[1];
        mode_stats(d)[2] = !d->ddLock ? 1 : !d->mapping ? 2 : 3;
        return FALSE;
    }
    /* DirectDraw's user-mode side drops the hardware layer of a mode that
     * lacks the Direct3D description an earlier mode had, and then fails
     * page flipping there. So every depth describes the Direct3D HAL when
     * native Direct3D is on; below 32 bits it cannot be used: it renders
     * only to 32-bit targets, and textures and host surfaces are refused. */
    d->d3dDescribed = (caps.gpuSurfaces & RB2K_DD_NATIVE_D3D7) != 0;
    /* Likewise the Direct3D 8 interface; its format list offers 3D only in
     * the 32-bit display format. */
    d->d3d8 = d->d3dDescribed && (caps.gpuSurfaces & RB2K_DD_D3D8) != 0;
    if (d->d3d8) {
        /* The DXT texture formats (display_d3d7.c decodes them). */
        static const DWORD codes[] = { RbFmtDxt1, RbFmtDxt2, RbFmtDxt3, RbFmtDxt4, RbFmtDxt5 };
        ULONG k;
        *fourCount = sizeof(codes) / sizeof(codes[0]);
        if (fourCC) for (k = 0; k < *fourCount; ++k) fourCC[k] = codes[k];
    }
    d->vertexShaders = d->d3d8 && (caps.gpuSurfaces & RB2K_DD_VERTEX_SHADERS) != 0;
    d->d3d9Ddi = d->vertexShaders && (caps.gpuSurfaces & RB2K_DD_D3D9_DDI) != 0;
    if (d->mode.BitsPerPlane != 32) caps.gpuSurfaces = 0;
    RtlZeroMemory(info, sizeof(*info));
    info->dwSize = sizeof(*info);
    v->pvPrimary = d->mapping;
    v->dwDisplayWidth = d->mode.VisScreenWidth;
    v->dwDisplayHeight = d->mode.VisScreenHeight;
    v->lDisplayPitch = d->mode.ScreenStride;
    v->ddpfDisplay.dwSize = sizeof(v->ddpfDisplay);
    v->ddpfDisplay.dwFlags = DDPF_RGB;
    v->ddpfDisplay.dwRGBBitCount = d->mode.BitsPerPlane;
    v->ddpfDisplay.dwRBitMask = d->mode.RedMask;
    v->ddpfDisplay.dwGBitMask = d->mode.GreenMask;
    v->ddpfDisplay.dwBBitMask = d->mode.BlueMask;
    /* The indexed primary uses the desktop DAC through DrvSetPalette. */
    if (d->mode.BitsPerPlane == 8)
        v->ddpfDisplay.dwFlags |= DDPF_PALETTEINDEXED8;
    v->dwOffscreenAlign = 4;
    info->ddCaps.dwSize = sizeof(info->ddCaps);
    info->ddCaps.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL;
    /* DirectDraw indexes the raster-op bitset by the eight-bit ROP3 code. */
    info->ddCaps.dwRops[((SRCCOPY >> 16) & 255) / 32] |= 1UL << ((SRCCOPY >> 16) & 31);
    info->ddCaps.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY |
                                  DDSCAPS_LOCALVIDMEM;
    d->gpuSurfaces = (caps.gpuSurfaces & RB2K_DD_HOST_SURFACES) != 0;
    d->plainSurfaces = (caps.gpuSurfaces & RB2K_DD_PLAIN_SURFACES) != 0;
    d->nativeD3D7 = (caps.gpuSurfaces & RB2K_DD_NATIVE_D3D7) != 0;
    d->z16Storage = (caps.gpuSurfaces & RB2K_DD_Z16_STORAGE) != 0;
    d->z16Render = d->z16Storage && (caps.gpuSurfaces & RB2K_DD_Z16_RENDER) != 0;
    d->z24s8 = d->z16Render && (caps.gpuSurfaces & RB2K_DD_Z24S8) != 0;
    if (d->z16Storage) {
        info->ddCaps.ddsCaps.dwCaps |= DDSCAPS_ZBUFFER;
        v->dwZBufferAlign = sizeof(USHORT);
    }
    if (d->gpuSurfaces)
        info->ddCaps.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE;
    else
        caps.slots = 0;
    info->ddCaps.dwVidMemTotal = caps.maxBytes * caps.slots;
    if (d->d3dDescribed) {
        info->ddCaps.ddsCaps.dwCaps |= DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
        v->dwTextureAlign = 4;
        rb_d7_caps(info, d->z16Render);
        info->dwFlags |= DDHALINFO_GETDRIVERINFOSET;
        if (d->d3d8) info->dwFlags |= RB_DDHALINFO_GETDRIVERINFO2;
        info->GetDriverInfo = driver_info;
    }
    pageBytes = page_size(d);
    if (pageBytes) {
        *heapCount = 1;
        if (heaps) {
            RtlZeroMemory(heaps, sizeof(*heaps));
            heaps->dwFlags = VIDMEM_ISLINEAR;
            heaps->fpStart = pageBytes;
            heaps->fpEnd = pageBytes * 2 - 1;
            heaps->ddsCaps.dwCaps = heaps->ddsCapsAlt.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        }
        info->dwFlags |= DDHALINFO_GETDRIVERINFOSET;
        info->GetDriverInfo = driver_info;
        info->ddCaps.ddsCaps.dwCaps |= DDSCAPS_FLIP;
        info->ddCaps.dwCaps2 |= DDCAPS2_FLIPNOVSYNC;
        info->ddCaps.dwVidMemTotal += pageBytes;
    }
    {
        ULONG i;
        EngAcquireSemaphore(d->ddLock);
        ++d->d7.halInfoCalls;
        d->d7.nativeEnabled = d->nativeD3D7;
        for (i = 0; i < caps.slots; ++i)
            if (!d->slots[i].global) info->ddCaps.dwVidMemFree += caps.maxBytes;
        if (!d->flipBack) info->ddCaps.dwVidMemFree += pageBytes;
        EngReleaseSemaphore(d->ddLock);
    }
    return TRUE;
}

BOOL APIENTRY DrvEnableDirectDraw(DHPDEV pdev, DD_CALLBACKS *c, DD_SURFACECALLBACKS *sc,
                                  DD_PALETTECALLBACKS *pc)
{
    DISPLAY *d = (DISPLAY *)pdev;
    RB2K_DD_CAPS caps;
    ULONG pageBytes;
    if (!d->ddLock || !rb_dd_caps(d, &caps)) {
        ++mode_stats(d)[3];
        return FALSE;
    }
    if (d->mode.BitsPerPlane != 32) caps.gpuSurfaces = 0;
    d->gpuSurfaces = (caps.gpuSurfaces & RB2K_DD_HOST_SURFACES) != 0;
    d->plainSurfaces = (caps.gpuSurfaces & RB2K_DD_PLAIN_SURFACES) != 0;
    d->z16Storage = (caps.gpuSurfaces & RB2K_DD_Z16_STORAGE) != 0;
    d->z16Render = d->z16Storage && (caps.gpuSurfaces & RB2K_DD_Z16_RENDER) != 0;
    d->z24s8 = d->z16Render && (caps.gpuSurfaces & RB2K_DD_Z24S8) != 0;
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
    pageBytes = page_size(d);
    if (pageBytes) {
        c->dwFlags |= DDHAL_CB32_MAPMEMORY;
        c->MapMemory = map_memory;
        sc->dwFlags |= DDHAL_SURFCB32_FLIP | DDHAL_SURFCB32_GETFLIPSTATUS;
        sc->Flip = flip;
        sc->GetFlipStatus = flip_status;
    }
    EngAcquireSemaphore(d->ddLock);
    d->pageBytes = pageBytes;
    d->ddEnabled = TRUE;
    ++d->d7.enableCalls;
    EngReleaseSemaphore(d->ddLock);
    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV pdev)
{
    DISPLAY *d = (DISPLAY *)pdev;
    ULONG i;
    if (!d->ddLock) return;
    EngAcquireSemaphore(d->ddLock);
    release_chain(d);
    d->pageBytes = 0;
    d->ddEnabled = FALSE;
    for (i = 0; i < RB2K_DD_SLOTS; ++i)
        if (d->slots[i].global) release_slot(d, &d->slots[i]);
    EngReleaseSemaphore(d->ddLock);
    rb_d7_disable(d);
}
