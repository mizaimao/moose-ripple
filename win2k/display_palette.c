/* The adapter has one colour table for the indexed desktop and both flip pages. */
#include "display_private.h"

typedef struct {
    USHORT count, first;
    ULONG colors[256];
} COLOR_REQUEST;

HPALETTE rb_create_palette(DISPLAY *d)
{
    if (d->mode.BitsPerPlane != 8)
        return EngCreatePalette(PAL_BITFIELDS, 0, NULL,
                                d->mode.RedMask, d->mode.GreenMask, d->mode.BlueMask);
    if (HT_Get8BPPFormatPalette((PALETTEENTRY *)d->paletteColors, 10000, 10000, 10000) != 256)
        return NULL;
    return EngCreatePalette(PAL_INDEXED, 256, d->paletteColors, 0, 0, 0);
}

static BOOL write_colors(DISPLAY *d, COLOR_REQUEST *request)
{
    ULONG returned, bytes = FIELD_OFFSET(VIDEO_CLUT, LookupTable) + request->count * sizeof(ULONG);
    return !EngDeviceIoControl(d->miniport, IOCTL_VIDEO_SET_COLOR_REGISTERS, request, bytes,
                               NULL, 0, &returned);
}

/* DrvAssertMode holds the semaphore while switching the mode and restoring the DAC. */
BOOL rb_restore_palette(DISPLAY *d)
{
    COLOR_REQUEST request;
    request.first = 0;
    request.count = 256;
    RtlCopyMemory(request.colors, d->paletteColors, sizeof(request.colors));
    return write_colors(d, &request);
}

BOOL APIENTRY DrvSetPalette(DHPDEV pdev, PALOBJ *palette, FLONG flags, ULONG first, ULONG count)
{
    DISPLAY *d = (DISPLAY *)pdev;
    COLOR_REQUEST request;
    BOOL ok;
    (void)flags;
    if (d->mode.BitsPerPlane != 8 || !palette || first > 256 || count > 256 - first)
        return FALSE;
    if (!count) return TRUE;
    if (PALOBJ_cGetColors(palette, first, count, request.colors) != count) return FALSE;
    request.first = (USHORT)first;
    request.count = (USHORT)count;
    EngAcquireSemaphore(d->ddLock);
    ok = !d->paletteActive || write_colors(d, &request);
    if (ok) RtlCopyMemory(d->paletteColors + first, request.colors, count * sizeof(ULONG));
    EngReleaseSemaphore(d->ddLock);
    return ok;
}
