/* Windows 2000 XDDM framebuffer display driver for the Ripple miniport. */
#include "display_private.h"
#include "rb2kd3d7.h"
#include "rb2krvd.h"

/* Ask the paired miniport for its current mode table on each enumeration. */
static VIDEO_MODE_INFORMATION *modes(HANDLE miniport, ULONG *count)
{
    VIDEO_NUM_MODES header;
    VIDEO_MODE_INFORMATION *table;
    ULONG bytes, returned;
    *count = 0;
    if (EngDeviceIoControl(miniport, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES, NULL, 0,
            &header, sizeof(header), &returned) || returned != sizeof(header) ||
            !header.NumModes || header.NumModes > 64 ||
            header.ModeInformationLength != sizeof(*table))
        return NULL;
    bytes = header.NumModes * sizeof(*table);
    table = EngAllocMem(0, bytes, 'm2bR');
    if (table && (EngDeviceIoControl(miniport, IOCTL_VIDEO_QUERY_AVAIL_MODES, NULL, 0,
            table, bytes, &returned) || returned != bytes)) {
        EngFreeMem(table);
        return NULL;
    }
    if (table) *count = header.NumModes;
    return table;
}

ULONG DrvGetModes(HANDLE miniport, ULONG bytes, DEVMODEW *out)
{
    ULONG count, i, written = 0;
    VIDEO_MODE_INFORMATION *table = modes(miniport, &count);
    static const WCHAR name[] = L"rb2kdisp";
    if (!table) return 0;
    if (!out) {
        EngFreeMem(table);
        return count * sizeof(DEVMODEW);
    }
    for (i = 0; i < count && bytes >= sizeof(*out); ++i, ++out) {
        RtlZeroMemory(out, sizeof(*out));
        RtlCopyMemory(out->dmDeviceName, name, sizeof(name));
        out->dmSize = sizeof(*out);
        out->dmSpecVersion = DM_SPECVERSION;
        out->dmDriverVersion = 1;
        out->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                        DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
        out->dmBitsPerPel = table[i].BitsPerPlane * table[i].NumberOfPlanes;
        out->dmPelsWidth = table[i].VisScreenWidth;
        out->dmPelsHeight = table[i].VisScreenHeight;
        out->dmDisplayFrequency = table[i].Frequency;
        bytes -= sizeof(*out);
        written += sizeof(*out);
    }
    EngFreeMem(table);
    return written;
}

DHPDEV DrvEnablePDEV(DEVMODEW *dm, LPWSTR address, ULONG patternCount,
                     HSURF *patterns, ULONG capsSize, ULONG *caps,
                     ULONG infoSize, DEVINFO *info, HDEV device,
                     LPWSTR deviceName, HANDLE miniport)
{
    VIDEO_MODE_INFORMATION *table;
    VIDEO_MODE_INFORMATION *selected = NULL;
    DISPLAY *display = NULL;
    GDIINFO *gdi = (GDIINFO *)caps;
    ULONG count, i;
    if (capsSize < sizeof(*gdi) || infoSize < sizeof(*info)) return NULL;
    table = modes(miniport, &count);
    if (!table) return NULL;
    for (i = 0; i < count; ++i) {
        VIDEO_MODE_INFORMATION *m = &table[i];
        if ((m->BitsPerPlane != 8 && m->BitsPerPlane != 16 && m->BitsPerPlane != 32) || m->NumberOfPlanes != 1 ||
            (dm->dmBitsPerPel && dm->dmBitsPerPel != m->BitsPerPlane) ||
            (dm->dmPelsWidth && dm->dmPelsWidth != m->VisScreenWidth) ||
            (dm->dmPelsHeight && dm->dmPelsHeight != m->VisScreenHeight) ||
            (dm->dmDisplayFrequency > 1 && dm->dmDisplayFrequency != m->Frequency))
            continue;
        if (!selected) selected = m;
        if (dm->dmPelsWidth || dm->dmPelsHeight) break;
        if (m->VisScreenWidth == RB2K_DEFAULT_WIDTH &&
            m->VisScreenHeight == RB2K_DEFAULT_HEIGHT) {
            selected = m;
            break;
        }
    }
    if (selected) {
        display = EngAllocMem(FL_ZERO_MEMORY, sizeof(*display), 'd2bR');
        if (display) {
            display->mode = *selected;
            switch (selected->BitsPerPlane) {
            case 8: display->bitmapFormat = BMF_8BPP; break;
            case 16: display->bitmapFormat = BMF_16BPP; break;
            default: display->bitmapFormat = BMF_32BPP; break;
            }
            display->miniport = miniport;
        }
    }
    EngFreeMem(table);
    if (!display) return NULL;
    display->palette = rb_create_palette(display);
    if (!display->palette) {
        EngFreeMem(display);
        return NULL;
    }
    RtlZeroMemory(gdi, sizeof(*gdi));
    gdi->ulVersion = GDI_DRIVER_VERSION;
    gdi->ulTechnology = DT_RASDISPLAY;
    /* GDI realizes raster fonts on this surface. Without TC_RA_ABLE the font
     * mapper drops them, and the console's default Terminal face is one. */
    gdi->flTextCaps = TC_RA_ABLE;
    gdi->ulHorzRes = display->mode.VisScreenWidth;
    gdi->ulVertRes = display->mode.VisScreenHeight;
    gdi->ulHorzSize = display->mode.XMillimeter;
    gdi->ulVertSize = display->mode.YMillimeter;
    gdi->cPlanes = 1;
    gdi->cBitsPixel = display->mode.BitsPerPlane;
    gdi->ulNumColors = (ULONG)-1;
    gdi->ulLogPixelsX = gdi->ulLogPixelsY = dm->dmLogPixels ? dm->dmLogPixels : 96;
    gdi->ulDACRed = display->mode.NumberRedBits;
    gdi->ulDACGreen = display->mode.NumberGreenBits;
    gdi->ulDACBlue = display->mode.NumberBlueBits;
    gdi->ulAspectX = gdi->ulAspectY = 36;
    gdi->ulAspectXY = 51;
    gdi->xStyleStep = gdi->yStyleStep = gdi->denStyleStep = 1;
    gdi->ulVRefresh = display->mode.Frequency;
    gdi->ulPanningHorzRes = gdi->ulHorzRes;
    gdi->ulPanningVertRes = gdi->ulVertRes;
    gdi->ulHTPatternSize = HT_PATSIZE_4x4_M;
    gdi->ulHTOutputFormat = display->mode.BitsPerPlane == 16 ? HT_FORMAT_16BPP : HT_FORMAT_32BPP;
    gdi->flHTFlags = HT_FLAG_ADDITIVE_PRIMS;
    RtlZeroMemory(info, sizeof(*info));
    info->hpalDefault = display->palette;
    info->iDitherFormat = display->bitmapFormat;
    if (display->bitmapFormat == BMF_32BPP) {
        RB2K_CURSOR_STATS cursor;
        if (rb_cursor_stats(display, &cursor) && cursor.enabled)
            display->cursorLock = EngCreateSemaphore();
        if (display->cursorLock) info->flGraphicsCaps2 |= GCAPS2_ALPHACURSOR;
    }
    display->ddLock = EngCreateSemaphore();
    if (display->bitmapFormat == BMF_8BPP) {
        if (!display->ddLock) {
            EngDeletePalette(display->palette);
            EngFreeMem(display);
            return NULL;
        }
        gdi->ulNumColors = 20;
        gdi->ulNumPalReg = 256;
        gdi->ulHTOutputFormat = HT_FORMAT_8BPP;
        info->flGraphicsCaps |= GCAPS_PALMANAGED;
    }
    return (DHPDEV)display;
}

VOID DrvCompletePDEV(DHPDEV pdev, HDEV device)
{
    ((DISPLAY *)pdev)->device = device;
}

VOID DrvDisablePDEV(DHPDEV pdev)
{
    DISPLAY *display = (DISPLAY *)pdev;
    if (display->cursorLock) {
        rb_display_cursor_release(display);
        EngDeleteSemaphore(display->cursorLock);
    }
    if (display->ddLock) {
        DrvDisableDirectDraw(pdev);
        rb_dd_free_views(display);
        EngDeleteSemaphore(display->ddLock);
    }
    EngDeletePalette(display->palette);
    EngFreeMem(display);
}

BOOL DrvAssertMode(DHPDEV pdev, BOOL enable)
{
    DISPLAY *display = (DISPLAY *)pdev;
    VIDEO_MODE mode;
    ULONG returned;
    BOOL indexed = display->bitmapFormat == BMF_8BPP, ok;
    if (indexed) EngAcquireSemaphore(display->ddLock);
    mode.RequestedMode = display->mode.ModeIndex;
    if (enable)
        ok = !EngDeviceIoControl(display->miniport, IOCTL_VIDEO_SET_CURRENT_MODE,
            &mode, sizeof(mode), NULL, 0, &returned);
    else
        ok = !EngDeviceIoControl(display->miniport, IOCTL_VIDEO_RESET_DEVICE,
            NULL, 0, NULL, 0, &returned);
    if (indexed) {
        if (enable && ok) ok = rb_restore_palette(display);
        if (enable || ok) display->paletteActive = enable && ok;
        EngReleaseSemaphore(display->ddLock);
    }
    return ok;
}

VOID DrvDisableSurface(DHPDEV pdev)
{
    DISPLAY *display = (DISPLAY *)pdev;
    VIDEO_MEMORY request;
    ULONG returned;
    rb_display_cursor_release(display);
    if (display->ddEnabled) DrvDisableDirectDraw(pdev);
    if (display->bitmapFormat == BMF_8BPP) {
        EngAcquireSemaphore(display->ddLock);
        display->paletteActive = FALSE;
        EngReleaseSemaphore(display->ddLock);
    }
    if (display->surface) {
        EngDeleteSurface(display->surface);
        display->surface = NULL;
    }
    if (display->cpuSurface) { EngUnlockSurface(display->cpuSurface); display->cpuSurface = NULL; }
    if (display->bitmap) { EngDeleteSurface(display->bitmap); display->bitmap = NULL; }
    display->glActive = FALSE;
    if (display->mapping) {
        request.RequestedVirtualAddress = display->mapping;
        EngDeviceIoControl(display->miniport, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY,
            &request, sizeof(request), NULL, 0, &returned);
        display->mapping = NULL;
        display->mappingBytes = 0;
    }
}

HSURF DrvEnableSurface(DHPDEV pdev)
{
    DISPLAY *display = (DISPLAY *)pdev;
    VIDEO_MEMORY request;
    VIDEO_MEMORY_INFORMATION mem;
    SIZEL size;
    ULONG returned;
    if (!DrvAssertMode(pdev, TRUE)) return NULL;
    request.RequestedVirtualAddress = NULL;
    RtlZeroMemory(&mem, sizeof(mem));
    if (EngDeviceIoControl(display->miniport, IOCTL_VIDEO_MAP_VIDEO_MEMORY,
            &request, sizeof(request), &mem, sizeof(mem), &returned))
        return NULL;
    display->mapping = mem.VideoRamBase;
    size.cx = display->mode.VisScreenWidth;
    size.cy = display->mode.VisScreenHeight;
    if (returned != sizeof(mem) || !mem.FrameBufferBase || !mem.VideoRamBase ||
        !size.cy || display->mode.ScreenStride <= 0 ||
        mem.FrameBufferLength / (ULONG)size.cy < (ULONG)display->mode.ScreenStride)
        goto failed;
    if (mem.VideoRamBase == mem.FrameBufferBase) display->mappingBytes = mem.FrameBufferLength;
    display->bitmap = (HSURF)EngCreateBitmap(size, display->mode.ScreenStride,
        display->bitmapFormat, BMF_TOPDOWN, mem.FrameBufferBase);
    if (!display->bitmap) goto failed;
    if (display->bitmapFormat == BMF_32BPP && rb_gl_device_surface(display)) {
        display->cpuSurface = EngLockSurface(display->bitmap);
        display->surface = EngCreateDeviceSurface((DHSURF)display, size, display->bitmapFormat);
        if (!display->cpuSurface || !display->surface ||
            !EngAssociateSurface(display->surface, display->device, HOOK_BITBLT | HOOK_COPYBITS |
                HOOK_STROKEPATH | HOOK_FILLPATH | HOOK_STROKEANDFILLPATH | HOOK_TEXTOUT)) goto failed;
    } else {
        display->surface = display->bitmap;
        display->bitmap = NULL;
        if (!EngAssociateSurface(display->surface, display->device, 0)) goto failed;
    }
    return display->surface;
failed:
    DrvDisableSurface(pdev);
    return NULL;
}

/* Windows' opengl32.dll asks for the name of the installable client driver
 * (ICD) with OPENGL_GETINFO and looks it up under
 * HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers. The two
 * numbers are the ICD interface version (2) and the driver's own version. */
static ULONG opengl_info(ULONG outSize, PVOID out)
{
    static const WCHAR name[] = L"RIPPLEGL";
    ULONG *info = out;
    if (!out || outSize < 2 * sizeof(ULONG) + sizeof(name)) return 0;
    info[0] = 2;
    info[1] = 1;
    RtlCopyMemory(info + 2, name, sizeof(name));
    return 1;
}

ULONG DrvEscape(SURFOBJ *surface, ULONG code, ULONG inSize, PVOID in,
                ULONG outSize, PVOID out)
{
    DISPLAY *display;
    RB2K_GPU_RESULT result;
    ULONG returned;
    if (code == QUERYESCSUPPORT)
        return in && inSize == sizeof(ULONG) && (*(ULONG *)in == RB2K_GPU_ESCAPE ||
                   *(ULONG *)in == RB2K_CURSOR_ESCAPE || *(ULONG *)in == RB2K_CURSOR_SOURCE_ESCAPE ||
                   *(ULONG *)in == RB2K_DD_STATS_ESCAPE || *(ULONG *)in == RB2K_DD_SCANOUT_ESCAPE ||
                   *(ULONG *)in == OPENGL_GETINFO || *(ULONG *)in == RB2K_RVD_ESCAPE ||
                   *(ULONG *)in == RB2K_RVD_STATS_ESCAPE || *(ULONG *)in == RB2K_GL_PRESENT_ESCAPE ||
                   *(ULONG *)in == RB2K_GL_STATS_ESCAPE || *(ULONG *)in == RB2K_GL_TIMING_ESCAPE ||
                   *(ULONG *)in == RB2K_GL_PROFILE_ESCAPE || *(ULONG *)in == RB2K_GL_RECT_PRESENT_ESCAPE ||
                   *(ULONG *)in == RB2K_GL_INLINE_PRESENT_ESCAPE || *(ULONG *)in == RB2K_D3D7_STATS_ESCAPE ||
                   *(ULONG *)in == RB2K_D3D7_PROFILE_ESCAPE || *(ULONG *)in == RB2K_D3D7_FLIGHT_ESCAPE ||
                   *(ULONG *)in == RB2K_D3D7_DRAW_ESCAPE);
    /* opengl32.dll passes a small input buffer of its own with this one. */
    if (code == OPENGL_GETINFO) return opengl_info(outSize, out);
    /* Mesa's winsys on Windows 2000 (display_rvd.c). */
    if (code == RB2K_RVD_ESCAPE)
        return surface && surface->dhpdev ?
               rb_rvd_escape((DISPLAY *)surface->dhpdev, inSize, in, outSize, out) : 0;
    if (code == RB2K_RVD_STATS_ESCAPE)
        return surface && surface->dhpdev ? rb_rvd_stats((DISPLAY *)surface->dhpdev, outSize, out) : 0;
    if (code == RB2K_CURSOR_SOURCE_ESCAPE) {
        if (!surface || !(display = (DISPLAY *)surface->dhpdev) || !display->cursorLock ||
            !out || outSize != sizeof(display->cursorSource) ||
            (inSize && (inSize != sizeof(ULONG) || !in || *(ULONG *)in != 1))) return 0;
        EngAcquireSemaphore(display->cursorLock);
        if (inSize) {
            RtlZeroMemory(&display->cursorSource, sizeof(display->cursorSource));
            display->sampleCursor = TRUE;
        }
        RtlCopyMemory(out, &display->cursorSource, outSize);
        EngReleaseSemaphore(display->cursorLock);
        return 1;
    }
    if (inSize || !out || !surface || !surface->dhpdev) return 0;
    display = (DISPLAY *)surface->dhpdev;
    if (code == RB2K_CURSOR_ESCAPE) {
        RB2K_CURSOR_STATS cursor;
        if ((outSize != sizeof(cursor) && outSize != FIELD_OFFSET(RB2K_CURSOR_STATS, lastDecline)) ||
            !rb_cursor_stats(display, &cursor)) return 0;
        RtlCopyMemory(out, &cursor, outSize);
        return 1;
    }
    if (code == RB2K_GL_STATS_ESCAPE && outSize == sizeof(RB2K_GL_STATS))
        return rb_gl_stats(display, RB2K_RVD_OP_GL_STATS, outSize, out);
    if (code == RB2K_GL_TIMING_ESCAPE && outSize == sizeof(RB2K_GL_TIMING))
        return rb_gl_stats(display, RB2K_RVD_OP_GL_TIMING, outSize, out);
    if (code == RB2K_GL_PROFILE_ESCAPE && (outSize == sizeof(RB2K_GL_PROFILE) ||
        outSize == FIELD_OFFSET(RB2K_GL_PROFILE, inlineBatches)))
        return rb_gl_stats(display, RB2K_RVD_OP_GL_PROFILE, outSize, out);
    if (code == RB2K_D3D7_FLIGHT_ESCAPE)
        return outSize == sizeof(RB2K_D7_FLIGHT) &&
               !EngDeviceIoControl(display->miniport, IOCTL_RB2K_D3D7_FLIGHT,
                                   NULL, 0, out, outSize, &returned) && returned == outSize;
    if (code == RB2K_D3D7_PROFILE_ESCAPE)
        return outSize == sizeof(RB2K_D7_PROFILE) &&
               !EngDeviceIoControl(display->miniport, IOCTL_RB2K_D3D7_PROFILE,
                                   NULL, 0, out, outSize, &returned) && returned == outSize;
    if (code == RB2K_D3D7_STATS_ESCAPE) return rb_d7_stats(display, outSize, out);
    if (code == RB2K_D3D7_DRAW_ESCAPE) return rb_d7_drawstats(display, outSize, out);
    if (code == RB2K_DD_SCANOUT_ESCAPE)
        return outSize == sizeof(RB2K_DD_FLIP_INFO) &&
               !EngDeviceIoControl(display->miniport, IOCTL_RB2K_DD_FLIP_INFO,
                                   NULL, 0, out, outSize, &returned) && returned == outSize;
    if (code == RB2K_DD_STATS_ESCAPE) {
        RB2K_DD_CAPS caps;
        RB2K_DD_STATS *stats = out;
        if ((outSize != sizeof(*stats) && outSize != sizeof(RB2K_DD_COPY_STATS) &&
             outSize != sizeof(RB2K_DD_PRESENT_STATS)) ||
            !rb_dd_caps(display, &caps)) return 0;
        stats->hostFills = caps.hostFills;
        stats->readbacks = caps.readbacks;
        stats->uploads = caps.uploads;
        if (outSize >= sizeof(RB2K_DD_COPY_STATS))
            ((RB2K_DD_COPY_STATS *)out)->hostCopies = caps.hostCopies;
        if (outSize == sizeof(RB2K_DD_PRESENT_STATS)) {
            RB2K_DD_PRESENT_STATS *present = out;
            present->presents = caps.presents;
            present->presentReadbacks = caps.presentReadbacks;
            present->presentBytes = caps.presentBytes;
        }
        return 1;
    }
    if (code != RB2K_GPU_ESCAPE || outSize != sizeof(result)) return 0;
    if (EngDeviceIoControl(display->miniport, IOCTL_RB2K_GPU_PROBE, NULL, 0,
            &result, sizeof(result), &returned) || returned != sizeof(result))
        return 0;
    RtlCopyMemory(out, &result, sizeof(result));
    return 1;
}

static DRVFN callbacks[] = {
    { INDEX_DrvGetModes, (PFN)DrvGetModes },
    { INDEX_DrvEnablePDEV, (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV, (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV, (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface, (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode, (PFN)DrvAssertMode },
    { INDEX_DrvSetPalette, (PFN)DrvSetPalette },
    { INDEX_DrvSetPointerShape, (PFN)DrvSetPointerShape },
    { INDEX_DrvMovePointer, (PFN)DrvMovePointer },
    { INDEX_DrvEscape, (PFN)DrvEscape },
    { INDEX_DrvDrawEscape, (PFN)DrvDrawEscape },
    { INDEX_DrvBitBlt, (PFN)DrvBitBlt },
    { INDEX_DrvCopyBits, (PFN)DrvCopyBits },
    { INDEX_DrvTextOut, (PFN)DrvTextOut },
    { INDEX_DrvStrokePath, (PFN)DrvStrokePath },
    { INDEX_DrvFillPath, (PFN)DrvFillPath },
    { INDEX_DrvStrokeAndFillPath, (PFN)DrvStrokeAndFillPath },
    { INDEX_DrvGetDirectDrawInfo, (PFN)DrvGetDirectDrawInfo },
    { INDEX_DrvEnableDirectDraw, (PFN)DrvEnableDirectDraw },
    { INDEX_DrvDisableDirectDraw, (PFN)DrvDisableDirectDraw }
};

BOOL DrvEnableDriver(ULONG engine, ULONG size, DRVENABLEDATA *data)
{
    if (engine < DDI_DRIVER_VERSION_NT5 || size < sizeof(*data)) return FALSE;
    rb_rvd_init();
    rb_d7_init();
    data->iDriverVersion = DDI_DRIVER_VERSION_NT5;
    data->c = sizeof(callbacks) / sizeof(callbacks[0]);
    data->pdrvfn = callbacks;
    return TRUE;
}
