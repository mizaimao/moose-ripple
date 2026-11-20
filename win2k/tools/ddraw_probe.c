/* Verify both DirectDraw paths and the transfers at the first CPU lock. */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>
#include "rb2kprobe.h"

typedef HRESULT (WINAPI *CREATE_DIRECTDRAW)(GUID *, LPDIRECTDRAW *, IUnknown *);
enum { Unavailable, Passed, Mismatch, NoHardware };

static HRESULT report(const char *path, const char *operation, HRESULT hr)
{
    printf("DirectDraw %s %s hr=%08lX\n", path, operation, (DWORD)hr);
    fflush(stdout);
    return hr;
}

static BOOL stats(HDC dc, RB2K_DD_COPY_STATS *out, BOOL copy)
{
    return dc && ExtEscape(dc, RB2K_DD_STATS_ESCAPE, 0, NULL,
                           copy ? sizeof(*out) : sizeof(out->transfers),
                           (LPSTR)out) > 0;
}

static int fill_test(const char *path, LPDIRECTDRAW dd, DWORD requestedMemory, BOOL copy)
{
    LPDIRECTDRAWSURFACE surface = NULL, source = NULL;
    HDC dc = NULL;
    RB2K_DD_COPY_STATS start, filled, locked;
    BOOL measured;
    DDSURFACEDESC desc, lock;
    DDPIXELFORMAT format;
    DDBLTFX effect;
    DWORD rgb, expected, byteCount, x, y, sample, mismatches;
    int result = Unavailable;
    HRESULT hr;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.dwWidth = copy ? RB2K_DEFAULT_WIDTH : 17;
    desc.dwHeight = copy ? RB2K_DEFAULT_HEIGHT : 13;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | requestedMemory;
    if (FAILED(report(path, "CreateSurface", IDirectDraw_CreateSurface(dd, &desc, &surface, NULL))))
        return result;
    if (copy && FAILED(report(path, "CreateSurface(source)",
            IDirectDraw_CreateSurface(dd, &desc, &source, NULL)))) goto done;
    desc.dwSize = sizeof(desc);
    if (FAILED(report(path, "GetSurfaceDesc", IDirectDrawSurface_GetSurfaceDesc(surface, &desc))))
        goto done;
    printf("DirectDraw %s surface_caps=%08lX requested_memory=%08lX\n", path,
           desc.ddsCaps.dwCaps, requestedMemory);
    if (!(desc.ddsCaps.dwCaps & requestedMemory) ||
        desc.dwWidth != (copy ? RB2K_DEFAULT_WIDTH : 17) ||
        desc.dwHeight != (copy ? RB2K_DEFAULT_HEIGHT : 13))
        goto done;
    ZeroMemory(&format, sizeof(format));
    format.dwSize = sizeof(format);
    if (FAILED(report(path, "GetPixelFormat", IDirectDrawSurface_GetPixelFormat(surface, &format))))
        goto done;
    printf("DirectDraw %s bits=%lu flags=%08lX r=%08lX g=%08lX b=%08lX\n",
           path, format.dwRGBBitCount, format.dwFlags, format.dwRBitMask, format.dwGBitMask,
           format.dwBBitMask);
    if (!(format.dwFlags & DDPF_RGB) || !format.dwRBitMask || !format.dwGBitMask ||
        !format.dwBBitMask || (format.dwRGBBitCount != 16 && format.dwRGBBitCount != 24 &&
                              format.dwRGBBitCount != 32)) goto done;
    if (copy && (format.dwRGBBitCount != 32 || format.dwRBitMask != 0x00ff0000 ||
                 format.dwGBitMask != 0x0000ff00 || format.dwBBitMask != 0x000000ff))
        goto done;
    byteCount = format.dwRGBBitCount / 8;
    rgb = format.dwRBitMask | format.dwGBitMask | format.dwBBitMask;
    expected = format.dwRBitMask | (format.dwGBitMask & ~(format.dwGBitMask >> 1));
    if (format.dwRBitMask == 0x00ff0000 && format.dwGBitMask == 0x0000ff00 &&
        format.dwBBitMask == 0x000000ff) expected = 0x003a87c5;
    ZeroMemory(&effect, sizeof(effect));
    effect.dwSize = sizeof(effect);
    if (requestedMemory == DDSCAPS_VIDEOMEMORY)
        dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    measured = stats(dc, &start, copy);
    if (copy && requestedMemory == DDSCAPS_VIDEOMEMORY && !measured) {
        puts("DirectDraw host copy counters unavailable");
        goto done;
    }
    effect.dwFillColor = copy ? expected : rgb ^ expected;
    hr = IDirectDrawSurface_Blt(copy ? source : surface, NULL, NULL, NULL,
                                DDBLT_COLORFILL | DDBLT_WAIT, &effect);
    if (FAILED(report(path, "Blt(first fill)", hr))) goto done;
    if (copy) {
        hr = IDirectDrawSurface_Blt(surface, NULL, source, NULL, DDBLT_WAIT, NULL);
        if (FAILED(report(path, "Blt(full copy)", hr))) goto done;
    }
    /* Overwrite the source after copying. The destination must retain the
     * first colour, even though no CPU has mapped either surface yet. */
    effect.dwFillColor = copy ? rgb ^ expected : expected;
    hr = IDirectDrawSurface_Blt(copy ? source : surface, NULL, NULL, NULL,
                                DDBLT_COLORFILL | DDBLT_WAIT, &effect);
    if (FAILED(report(path, "Blt(second fill)", hr))) goto done;
    if (measured && !stats(dc, &filled, copy)) goto done;
    ZeroMemory(&lock, sizeof(lock));
    lock.dwSize = sizeof(lock);
    hr = IDirectDrawSurface_Lock(surface, NULL, &lock, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(report(path, "Lock", hr))) goto done;
    if (lock.lpSurface && (lock.lPitch >= (LONG)(desc.dwWidth * byteCount) ||
                          lock.lPitch <= -(LONG)(desc.dwWidth * byteCount))) {
        mismatches = 0;
        for (y = 0; y < desc.dwHeight; ++y) {
            const BYTE *row = (const BYTE *)lock.lpSurface + (LONG)y * lock.lPitch;
            for (x = 0; x < desc.dwWidth; ++x) {
                sample = 0;
                memcpy(&sample, row + x * byteCount, byteCount);
                if ((sample & rgb) != expected) ++mismatches;
            }
        }
        printf("DirectDraw %s rgb_expected=%08lX pixels=%lu mismatches=%lu pitch=%ld\n", path,
               expected, desc.dwWidth * desc.dwHeight, mismatches, lock.lPitch);
        result = mismatches ? Mismatch : Passed;
    }
    if (FAILED(report(path, "Unlock", IDirectDrawSurface_Unlock(surface, lock.lpSurface))))
        result = Unavailable;
    if (measured && result == Passed) {
        if (!stats(dc, &locked, copy)) { result = Unavailable; goto done; }
        printf("DirectDraw host fills=%lu pre-lock readbacks=%lu lock readbacks=%lu uploads=%lu\n",
               filled.transfers.hostFills - start.transfers.hostFills,
               filled.transfers.readbacks - start.transfers.readbacks,
               locked.transfers.readbacks - filled.transfers.readbacks,
               locked.transfers.uploads - start.transfers.uploads);
        if (copy)
            printf("DirectDraw host copies=%lu\n", filled.hostCopies - start.hostCopies);
        if (filled.transfers.hostFills - start.transfers.hostFills != 2 ||
            filled.transfers.readbacks != start.transfers.readbacks ||
            locked.transfers.readbacks - filled.transfers.readbacks != 1 ||
            locked.transfers.uploads != start.transfers.uploads ||
            (copy && filled.hostCopies - start.hostCopies != 1))
            result = Mismatch;
    }
done:
    if (dc) DeleteDC(dc);
    if (source) IDirectDrawSurface_Release(source);
    IDirectDrawSurface_Release(surface);
    return result;
}

static int check_path(CREATE_DIRECTDRAW create, const char *path, DWORD createFlags, DWORD memory,
                      BOOL copy)
{
    LPDIRECTDRAW dd = NULL;
    DDCAPS hardware, emulation;
    HRESULT hr;
    int result = Unavailable;
    hr = create((GUID *)createFlags, &dd, NULL);
    if (hr == DDERR_NODIRECTDRAWHW && createFlags == DDCREATE_HARDWAREONLY)
        result = NoHardware;
    if (FAILED(report(path, "DirectDrawCreate", hr))) return result;
    hr = IDirectDraw_SetCooperativeLevel(dd, NULL, DDSCL_NORMAL);
    if (FAILED(report(path, "SetCooperativeLevel(normal)", hr))) goto done;
    ZeroMemory(&hardware, sizeof(hardware));
    ZeroMemory(&emulation, sizeof(emulation));
    hardware.dwSize = sizeof(hardware);
    emulation.dwSize = sizeof(emulation);
    hr = IDirectDraw_GetCaps(dd, &hardware, &emulation);
    if (SUCCEEDED(report(path, "GetCaps", hr))) {
        printf("DirectDraw %s HAL caps=%08lX nohardware=%d colorfill=%d "
               "vidtotal=%lu vidfree=%lu\n", path, hardware.dwCaps,
               !!(hardware.dwCaps & DDCAPS_NOHARDWARE), !!(hardware.dwCaps & DDCAPS_BLTCOLORFILL),
               hardware.dwVidMemTotal, hardware.dwVidMemFree);
        printf("DirectDraw %s HEL caps=%08lX colorfill=%d\n", path, emulation.dwCaps,
               !!(emulation.dwCaps & DDCAPS_BLTCOLORFILL));
    }
    result = fill_test(path, dd, memory, copy);
    if (createFlags == DDCREATE_HARDWAREONLY && result != Mismatch) {
        if (FAILED(hr)) result = Unavailable;
        else if (hardware.dwCaps & DDCAPS_NOHARDWARE) result = NoHardware;
        else if (!(hardware.dwCaps & DDCAPS_BLTCOLORFILL)) result = Unavailable;
    }
done:
    IDirectDraw_Release(dd);
    return result;
}

int probe_ddraw(BOOL copy)
{
    static const char *const states[] = {"unavailable", "pixel-pass", "pixel-mismatch", "no-HAL"};
    char filename[MAX_PATH];
    UINT length;
    HMODULE module;
    CREATE_DIRECTDRAW create;
    int hardware, emulation;
    length = GetSystemDirectoryA(filename, sizeof(filename));
    if (!length || length + sizeof("\\ddraw.dll") > sizeof(filename)) {
        puts("DirectDraw system directory unavailable");
        return 4;
    }
    lstrcatA(filename, "\\ddraw.dll");
    module = LoadLibraryA(filename);
    if (!module) {
        printf("DirectDraw runtime unavailable error=%lu\n", GetLastError());
        return 4;
    }
    create = (CREATE_DIRECTDRAW)GetProcAddress(module, "DirectDrawCreate");
    if (!create) {
        puts("DirectDrawCreate unavailable");
        FreeLibrary(module);
        return 4;
    }
    hardware = check_path(create, "hardware-only", DDCREATE_HARDWAREONLY, DDSCAPS_VIDEOMEMORY, copy);
    emulation = check_path(create, "emulation-only", DDCREATE_EMULATIONONLY, DDSCAPS_SYSTEMMEMORY, copy);
    FreeLibrary(module);
    printf("DirectDraw hardware_video_fill=%s emulation_system_fill=%s\n",
           states[hardware], states[emulation]);
    if (fflush(stdout) || ferror(stdout) || emulation != Passed || hardware == Mismatch) return 1;
    if (hardware == NoHardware) return 3;
    return hardware == Passed ? 0 : 5;
}
