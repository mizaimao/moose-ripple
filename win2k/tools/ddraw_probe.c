/* Test ordinary DirectDraw colour fills through hardware-only and emulated
 * devices. Both paths use an offscreen surface and the current pixel format. */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT (WINAPI *CREATE_DIRECTDRAW)(GUID *, LPDIRECTDRAW *, IUnknown *);
enum { Unavailable, Passed, Mismatch, NoHardware };

static HRESULT report(const char *path, const char *operation, HRESULT hr)
{
    printf("DirectDraw %s %s hr=%08lX\n", path, operation, (DWORD)hr);
    fflush(stdout);
    return hr;
}

static int fill_test(const char *path, LPDIRECTDRAW dd, DWORD requestedMemory)
{
    LPDIRECTDRAWSURFACE surface = NULL;
    DDSURFACEDESC desc, lock;
    DDPIXELFORMAT format;
    DDBLTFX effect;
    DWORD rgb, expected, sample = 0, byteCount;
    int result = Unavailable;
    HRESULT hr;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.dwWidth = desc.dwHeight = 16;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | requestedMemory;
    if (FAILED(report(path, "CreateSurface", IDirectDraw_CreateSurface(dd, &desc, &surface, NULL))))
        return result;
    desc.dwSize = sizeof(desc);
    if (FAILED(report(path, "GetSurfaceDesc", IDirectDrawSurface_GetSurfaceDesc(surface, &desc))))
        goto done;
    printf("DirectDraw %s surface_caps=%08lX requested_memory=%08lX\n", path,
           desc.ddsCaps.dwCaps, requestedMemory);
    if (!(desc.ddsCaps.dwCaps & requestedMemory)) goto done;
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
    byteCount = format.dwRGBBitCount / 8;
    rgb = format.dwRBitMask | format.dwGBitMask | format.dwBBitMask;
    expected = format.dwRBitMask | (format.dwGBitMask & ~(format.dwGBitMask >> 1));
    ZeroMemory(&effect, sizeof(effect));
    effect.dwSize = sizeof(effect);
    effect.dwFillColor = expected;
    hr = IDirectDrawSurface_Blt(surface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &effect);
    if (FAILED(report(path, "Blt(colorfill)", hr))) goto done;
    ZeroMemory(&lock, sizeof(lock));
    lock.dwSize = sizeof(lock);
    hr = IDirectDrawSurface_Lock(surface, NULL, &lock, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(report(path, "Lock", hr))) goto done;
    if (lock.lpSurface) {
        memcpy(&sample, lock.lpSurface, byteCount);
        printf("DirectDraw %s rgb_expected=%08lX rgb_observed=%08lX match=%u\n", path,
               expected, sample & rgb, (sample & rgb) == expected);
        result = (sample & rgb) == expected ? Passed : Mismatch;
    }
    if (FAILED(report(path, "Unlock", IDirectDrawSurface_Unlock(surface, lock.lpSurface))))
        result = Unavailable;
done:
    IDirectDrawSurface_Release(surface);
    return result;
}

static int check_path(CREATE_DIRECTDRAW create, const char *path, DWORD createFlags, DWORD memory)
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
    result = fill_test(path, dd, memory);
    if (createFlags == DDCREATE_HARDWAREONLY && result != Mismatch) {
        if (FAILED(hr)) result = Unavailable;
        else if (hardware.dwCaps & DDCAPS_NOHARDWARE) result = NoHardware;
        else if (!(hardware.dwCaps & DDCAPS_BLTCOLORFILL)) result = Unavailable;
    }
done:
    IDirectDraw_Release(dd);
    return result;
}

int probe_ddraw(void)
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
    hardware = check_path(create, "hardware-only", DDCREATE_HARDWAREONLY, DDSCAPS_VIDEOMEMORY);
    emulation = check_path(create, "emulation-only", DDCREATE_EMULATIONONLY, DDSCAPS_SYSTEMMEMORY);
    FreeLibrary(module);
    printf("DirectDraw hardware_video_fill=%s emulation_system_fill=%s\n",
           states[hardware], states[emulation]);
    if (fflush(stdout) || ferror(stdout) || emulation != Passed || hardware == Mismatch) return 1;
    if (hardware == NoHardware) return 3;
    return hardware == Passed ? 0 : 5;
}
