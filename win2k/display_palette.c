/* The adapter has one colour table for the indexed desktop and both flip pages. */
#include "display_private.h"

typedef struct {
    USHORT count, first;
    ULONG colors[256];
} COLOR_REQUEST;

HPALETTE rb_create_palette(DISPLAY *d)
{
    /* GDI reserves the first and last ten entries of a managed palette. */
    static const ULONG systemColors[20] = {
        RGB(0,0,0), RGB(128,0,0), RGB(0,128,0), RGB(128,128,0), RGB(0,0,128),
        RGB(128,0,128), RGB(0,128,128), RGB(192,192,192), RGB(192,220,192), RGB(166,202,240),
        RGB(255,251,240), RGB(160,160,164), RGB(128,128,128), RGB(255,0,0), RGB(0,255,0),
        RGB(255,255,0), RGB(0,0,255), RGB(255,0,255), RGB(0,255,255), RGB(255,255,255)
    };
    ULONG i;
    if (d->mode.BitsPerPlane != 8)
        return EngCreatePalette(PAL_BITFIELDS, 0, NULL,
                                d->mode.RedMask, d->mode.GreenMask, d->mode.BlueMask);

    /* Start with a full 3:3:2 RGB cube so every index has a defined colour. */
    for (i = 0; i < 256; ++i)
        d->paletteColors[i] = RGB((i & 7) * 255 / 7, ((i >> 3) & 7) * 255 / 7,
                                  (i >> 6) * 255 / 3);
    RtlCopyMemory(d->paletteColors, systemColors, 10 * sizeof(ULONG));
    RtlCopyMemory(d->paletteColors + 246, systemColors + 10, 10 * sizeof(ULONG));
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
