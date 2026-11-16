/*
 * ddinfo: what DirectDraw itself recorded about the display driver's HAL.
 *
 *   ddinfo > out.txt
 *
 * Creates a DirectDraw object and prints fields of DirectDraw's own driver record
 * (DDRAWI_DIRECTDRAW_GBL, from the DDK's ddrawi.h: flags such as "no hardware", the
 * capabilities and video memory it took from the HAL, the value DriverInit returned). For
 * finding out why DirectDraw ignores a HAL. Built by tools/win9x/build-tools.sh with the
 * Windows Me DDK's DirectDraw headers (not in this repository).
 */
#include <windows.h>
#include <stdio.h>
#include "ddraw.h"
#include "ddrawi.h"

static const struct { DWORD bit; const char *name; } flags[] = {
    { DDRAWI_DISPLAYDRV, "DISPLAYDRV" }, { DDRAWI_FULLSCREEN, "FULLSCREEN" },
    { DDRAWI_MODECHANGED, "MODECHANGED" }, { DDRAWI_NOHARDWARE, "NOHARDWARE" },
    { DDRAWI_NOEMULATION, "NOEMULATION" }, { DDRAWI_EMULATIONINITIALIZED, "EMULATIONINITIALIZED" },
    { DDRAWI_MODEXILLEGAL, "MODEXILLEGAL" }, { DDRAWI_STANDARDVGA, "STANDARDVGA" },
    { DDRAWI_MODEX, "MODEX" }, { DDRAWI_GDIDRV, "GDIDRV" }, { DDRAWI_ATTACHEDTODESKTOP, "ATTACHEDTODESKTOP" },
    { DDRAWI_NEEDSWIN16FORVRAMLOCK, "NEEDSWIN16FORVRAMLOCK" }, { DDRAWI_VIRTUALDESKTOP, "VIRTUALDESKTOP" },
};

int main(void)
{
    LPDIRECTDRAW dd;
    LPDDRAWI_DIRECTDRAW_GBL g;
    DWORD i;

    if (DirectDrawCreate(NULL, &dd, NULL) != DD_OK) {
        printf("DirectDrawCreate failed\n");
        return 1;
    }
    g = ((LPDDRAWI_DIRECTDRAW_INT)dd)->lpLcl->lpGbl;
    printf("flags %08lx:", g->dwFlags);
    for (i = 0; i < sizeof flags / sizeof flags[0]; i++)
        if (g->dwFlags & flags[i].bit)
            printf(" %s", flags[i].name);
    printf("\n");
    printf("driver caps %08lx caps2 %08lx surface caps %08lx, size %lu\n", g->ddCaps.dwCaps,
           g->ddCaps.dwCaps2, g->ddCaps.ddsCaps.dwCaps, g->ddCaps.dwSize);
    printf("primary %08lx (orig %08lx), %lux%lu pitch %ld, %lu bits; heaps %lu at %p\n",
           g->vmiData.fpPrimary, g->fpPrimaryOrig, g->vmiData.dwDisplayWidth,
           g->vmiData.dwDisplayHeight, g->vmiData.lDisplayPitch,
           g->vmiData.ddpfDisplay.dwRGBBitCount, g->vmiData.dwNumHeaps, g->vmiData.pvmList);
    printf("mode %lu of %lu, monitor %lu Hz\n", g->dwModeIndex, g->dwNumModes, g->dwMonitorFrequency);
    printf("HAL callbacks %p, driver handle %p, reserved3 (DriverInit) %08lx, hInstance %08lx, "
           "PDEVICE %08lx\n", g->lpDDCBtmp, g->lpDriverHandle, (DWORD)g->dwReserved3,
           g->hInstance, g->dwPDevice);
    printf("D3D global data %08lx, callbacks %08lx, callbacks2 %08lx\n",
           (DWORD)g->lpD3DGlobalDriverData, (DWORD)g->lpD3DHALCallbacks, (DWORD)g->lpD3DHALCallbacks2);
    if (g->lpDDCBtmp) {
        LPDDHAL_CALLBACKS cb = g->lpDDCBtmp;
        printf("HAL tables: dd flags %08lx size %lu, surface flags %08lx size %lu, Flip %p\n",
               cb->HALDD.dwFlags, cb->HALDD.dwSize, cb->HALDDSurface.dwFlags,
               cb->HALDDSurface.dwSize, cb->HALDDSurface.Flip);
    }
    IDirectDraw_Release(dd);
    return 0;
}
