/* Windows 2000 XDDM framebuffer display driver for the Ripple miniport. */
#include "display_private.h"
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
        if (m->BitsPerPlane != 32 || m->NumberOfPlanes != 1 ||
            (dm->dmBitsPerPel && dm->dmBitsPerPel != 32) ||
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
            display->miniport = miniport;
        }
    }
    EngFreeMem(table);
    if (!display) return NULL;
    display->palette = EngCreatePalette(PAL_BITFIELDS, 0, NULL,
        display->mode.RedMask, display->mode.GreenMask, display->mode.BlueMask);
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
    gdi->cBitsPixel = 32;
    gdi->ulNumColors = (ULONG)-1;
    gdi->ulLogPixelsX = gdi->ulLogPixelsY = dm->dmLogPixels ? dm->dmLogPixels : 96;
    gdi->ulDACRed = gdi->ulDACGreen = gdi->ulDACBlue = 8;
    gdi->ulAspectX = gdi->ulAspectY = 36;
    gdi->ulAspectXY = 51;
    gdi->xStyleStep = gdi->yStyleStep = gdi->denStyleStep = 1;
    gdi->ulVRefresh = display->mode.Frequency;
    gdi->ulPanningHorzRes = gdi->ulHorzRes;
    gdi->ulPanningVertRes = gdi->ulVertRes;
    gdi->ulHTPatternSize = HT_PATSIZE_4x4_M;
    gdi->ulHTOutputFormat = HT_FORMAT_32BPP;
    gdi->flHTFlags = HT_FLAG_ADDITIVE_PRIMS;
    RtlZeroMemory(info, sizeof(*info));
    info->hpalDefault = display->palette;
    info->iDitherFormat = BMF_32BPP;
    display->ddLock = EngCreateSemaphore();
    return (DHPDEV)display;
}

VOID DrvCompletePDEV(DHPDEV pdev, HDEV device)
{
    ((DISPLAY *)pdev)->device = device;
}

VOID DrvDisablePDEV(DHPDEV pdev)
{
    DISPLAY *display = (DISPLAY *)pdev;
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
    mode.RequestedMode = display->mode.ModeIndex;
    if (enable)
        return !EngDeviceIoControl(display->miniport, IOCTL_VIDEO_SET_CURRENT_MODE,
            &mode, sizeof(mode), NULL, 0, &returned);
    return !EngDeviceIoControl(display->miniport, IOCTL_VIDEO_RESET_DEVICE,
        NULL, 0, NULL, 0, &returned);
}

VOID DrvDisableSurface(DHPDEV pdev)
{
    DISPLAY *display = (DISPLAY *)pdev;
    VIDEO_MEMORY request;
    ULONG returned;
    if (display->ddEnabled) DrvDisableDirectDraw(pdev);
    if (display->surface) {
        EngDeleteSurface(display->surface);
        display->surface = NULL;
    }
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
    /* Let GDI draw directly into the top-down mapped framebuffer. */
    display->surface = (HSURF)EngCreateBitmap(size, display->mode.ScreenStride,
        BMF_32BPP, BMF_TOPDOWN, mem.FrameBufferBase);
    if (!display->surface || !EngAssociateSurface(display->surface, display->device, 0))
        goto failed;
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
                   *(ULONG *)in == RB2K_DD_STATS_ESCAPE || *(ULONG *)in == RB2K_DD_SCANOUT_ESCAPE ||
                   *(ULONG *)in == OPENGL_GETINFO || *(ULONG *)in == RB2K_RVD_ESCAPE ||
                   *(ULONG *)in == RB2K_RVD_STATS_ESCAPE);
    /* opengl32.dll passes a small input buffer of its own with this one. */
    if (code == OPENGL_GETINFO) return opengl_info(outSize, out);
    /* Mesa's winsys on Windows 2000 (display_rvd.c). */
    if (code == RB2K_RVD_ESCAPE)
        return surface && surface->dhpdev ?
               rb_rvd_escape((DISPLAY *)surface->dhpdev, inSize, in, outSize, out) : 0;
    if (code == RB2K_RVD_STATS_ESCAPE) return rb_rvd_stats(outSize, out);
    if (inSize || !out || !surface || !surface->dhpdev) return 0;
    display = (DISPLAY *)surface->dhpdev;
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
    { INDEX_DrvEscape, (PFN)DrvEscape },
    { INDEX_DrvGetDirectDrawInfo, (PFN)DrvGetDirectDrawInfo },
    { INDEX_DrvEnableDirectDraw, (PFN)DrvEnableDirectDraw },
    { INDEX_DrvDisableDirectDraw, (PFN)DrvDisableDirectDraw }
};

BOOL DrvEnableDriver(ULONG engine, ULONG size, DRVENABLEDATA *data)
{
    if (engine < DDI_DRIVER_VERSION_NT5 || size < sizeof(*data)) return FALSE;
    rb_rvd_init();
    data->iDriverVersion = DDI_DRIVER_VERSION_NT5;
    data->c = sizeof(callbacks) / sizeof(callbacks[0]);
    data->pdrvfn = callbacks;
    return TRUE;
}
