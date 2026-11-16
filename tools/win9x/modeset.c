/*
 * modeset: change the screen mode from the command line, as Display Properties does.
 *
 *   modeset                      prints the current mode
 *   modeset WIDTH HEIGHT BPP     sets that mode now and keeps it (in the registry)
 *
 * For tests that need a 16-bit or 8-bit desktop without driving the dialog with keys.
 * Built by tools/win9x/build-tools.sh.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *answer(LONG r)
{
    switch (r) {
    case DISP_CHANGE_SUCCESSFUL: return "done";
    case DISP_CHANGE_RESTART:    return "Windows wants a restart for it";
    case DISP_CHANGE_BADMODE:    return "the driver doesn't offer that mode";
    case DISP_CHANGE_FAILED:     return "the driver failed to set it";
    case DISP_CHANGE_NOTUPDATED: return "set, but not written to the registry";
    case DISP_CHANGE_BADFLAGS:   return "bad flags";
    default:                     return "unknown answer";
    }
}

int main(int argc, char **argv)
{
    DEVMODE dm;
    HDC dc;
    LONG r;

    dc = GetDC(NULL);
    printf("now: %dx%d, %d bits a pixel\n", GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES),
           GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES));
    ReleaseDC(NULL, dc);
    if (argc < 4)
        return 0;

    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    dm.dmPelsWidth = atoi(argv[1]);
    dm.dmPelsHeight = atoi(argv[2]);
    dm.dmBitsPerPel = atoi(argv[3]);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    r = ChangeDisplaySettings(&dm, CDS_UPDATEREGISTRY);
    printf("set %lux%lu, %lu bits: %s (%ld)\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel,
           answer(r), r);

    dc = GetDC(NULL);
    printf("now: %dx%d, %d bits a pixel\n", GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES),
           GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES));
    ReleaseDC(NULL, dc);
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
}
