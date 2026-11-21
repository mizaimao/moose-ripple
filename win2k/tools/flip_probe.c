/* Write two distinct images once, then check which one each flip displays. */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include "rb2kprobe.h"

static BOOL call(const char *operation, HRESULT hr)
{
    printf("Flip %s hr=%08lX\n", operation, (DWORD)hr);
    fflush(stdout);
    return SUCCEEDED(hr);
}

static LRESULT CALLBACK fullscreen_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) { ValidateRect(window, NULL); return 0; }
    if (message == WM_SETCURSOR) { SetCursor(NULL); return TRUE; }
    return DefWindowProcA(window, message, wp, lp);
}

static DWORD pixel(DWORD x, DWORD y, DWORD image)
{
    DWORD rgb = ((x & 255) << 16) | ((y & 255) << 8) | ((x ^ y) & 255);
    return image ? (rgb ^ 0x00ffffffUL) : rgb;
}

static BOOL pixels(LPDIRECTDRAWSURFACE surface, DWORD width, DWORD height,
                   DWORD image, BOOL write)
{
    DDSURFACEDESC lock;
    DWORD x, y, wrong = 0;
    BOOL valid;
    ZeroMemory(&lock, sizeof(lock));
    lock.dwSize = sizeof(lock);
    if (!call(write ? "lock back" : "lock front", IDirectDrawSurface_Lock(surface, NULL,
        &lock, DDLOCK_WAIT | (write ? DDLOCK_WRITEONLY : DDLOCK_READONLY), NULL))) return FALSE;
    valid = lock.lpSurface && lock.dwWidth == width && lock.dwHeight == height &&
            lock.lPitch >= (LONG)(width * 4) && lock.ddpfPixelFormat.dwRGBBitCount == 32 &&
            lock.ddpfPixelFormat.dwRBitMask == 0xff0000 && lock.ddpfPixelFormat.dwGBitMask == 0xff00 &&
            lock.ddpfPixelFormat.dwBBitMask == 0xff;
    if (valid) for (y = 0; y < height; ++y) {
        DWORD *row = (DWORD *)((BYTE *)lock.lpSurface + y * lock.lPitch);
        for (x = 0; x < width; ++x) {
            DWORD expected = pixel(x, y, image);
            if (write) row[x] = expected;
            else if ((row[x] & 0xffffff) != expected) ++wrong;
        }
    }
    printf("Flip %s image=%lu pixels=%lu mismatches=%lu pitch=%ld valid=%d\n",
           write ? "write" : "check", image, width * height, wrong, lock.lPitch, valid);
    return call("unlock", IDirectDrawSurface_Unlock(surface, lock.lpSurface)) && valid && !wrong;
}

static BOOL scanout(HDC dc, RB2K_DD_FLIP_INFO *info)
{
    return dc && ExtEscape(dc, RB2K_DD_SCANOUT_ESCAPE, 0, NULL,
                           sizeof(*info), (LPSTR)info) > 0;
}

static BOOL sample_window(HWND window, DWORD width, DWORD height, DWORD image)
{
    HDC dc = GetDC(window);
    DWORD col, row, wrong = 0;
    if (!dc) return FALSE;
    for (row = 0; row <= 8; ++row) {
        for (col = 0; col <= 8; ++col) {
            int x = col * (width - 1) / 8, y = row * (height - 1) / 8;
            POINT at;
            DWORD rgb = pixel(x, y, image);
            at.x = x; at.y = y;
            if (!ClientToScreen(window, &at) || WindowFromPoint(at) != window ||
                GetPixel(dc, x, y) != RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255)) ++wrong;
        }
    }
    ReleaseDC(window, dc);
    printf("Flip visible window image=%lu samples=81 mismatches=%lu\n", image, wrong);
    return wrong == 0;
}

static BOOL exercise(LPDIRECTDRAW dd, HWND window, const char *path, BOOL hardware, DWORD width, DWORD height)
{
    LPDIRECTDRAWSURFACE front = NULL, back = NULL;
    DDSURFACEDESC desc;
    DDSCAPS caps;
    HDC dc = NULL;
    RB2K_DD_FLIP_INFO start, now;
    DWORD frame;
    BOOL passed = FALSE;
    printf("Flip %s requested=%lux%lux32\n", path, width, height);
    if (!call("set mode", IDirectDraw_SetDisplayMode(dd, width, height, 32))) return FALSE;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    desc.dwBackBufferCount = 1;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
    if (hardware) desc.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY;
    if (!call("create double buffer", IDirectDraw_CreateSurface(dd, &desc, &front, NULL))) goto done;
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (!call("get attached back", IDirectDrawSurface_GetAttachedSurface(front, &caps, &back))) goto done;
    if (hardware) {
        dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
        if (!scanout(dc, &start) || start.pageCount < 2 || start.visiblePage || start.yOffset ||
            start.width != width || start.height != height || start.pitch != width*4 ||
            start.pageBytes != start.pitch * height) {
            puts("Flip initial scanout geometry unavailable or incorrect");
            goto done;
        }
    }
    /* Release an odd number of flips in, with page one still on screen. */
    for (frame = 0; frame < 5; ++frame) {
        DWORD page = (frame + 1) % 2;
        if ((frame < 2 || !hardware) && !pixels(back, width, height, frame % 2, TRUE)) goto done;
        if (!call("swap", IDirectDrawSurface_Flip(front, NULL, DDFLIP_WAIT))) goto done;
        if (hardware) {
            if (!scanout(dc, &now)) goto done;
            printf("Flip frame=%lu page=%lu y_offset=%lu flips=%lu\n",
                   frame + 1, now.visiblePage, now.yOffset, now.flips - start.flips);
            if (now.visiblePage != page || now.yOffset != page * height ||
                now.flips - start.flips != frame + 1) goto done;
        }
        if (hardware) {
            if (!pixels(front, width, height, frame % 2, FALSE)) goto done;
        } else if (!sample_window(window, width, height, frame % 2)) goto done;
    }
    passed = TRUE;
done:
    if (back) IDirectDrawSurface_Release(back);
    if (front) IDirectDrawSurface_Release(front);
    if (dc) {
        if (!scanout(dc, &now) || now.yOffset || now.visiblePage) passed = FALSE;
        else puts("Flip chain released on desktop page zero");
        DeleteDC(dc);
    }
    return passed;
}

BOOL probe_ddraw_flip(LPDIRECTDRAW dd, const char *path, BOOL hardware)
{
    static const char name[] = "W2KFullscreenFlip";
    WNDCLASSA wc;
    HWND window;
    DEVMODEA original, restored;
    BOOL passed = FALSE, exact;
    ZeroMemory(&original, sizeof(original));
    original.dmSize = sizeof(original);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &original)) return FALSE;
    ZeroMemory(&wc, sizeof(wc));
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpfnWndProc = fullscreen_proc;
    wc.lpszClassName = name;
    if (!RegisterClassA(&wc)) return FALSE;
    window = CreateWindowExA(WS_EX_TOPMOST, name, "Fullscreen flip test", WS_POPUP,
              0, 0, original.dmPelsWidth, original.dmPelsHeight, NULL, NULL, wc.hInstance, NULL);
    if (window) {
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        UpdateWindow(window);
        ShowCursor(FALSE);
        if (call("exclusive", IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN))) {
            passed = exercise(dd, window, path, hardware, 640, 480);
            if (passed && hardware) passed = exercise(dd, window, path, hardware, 1024, 768);
            if (!call("restore display", IDirectDraw_RestoreDisplayMode(dd))) passed = FALSE;
            if (!call("leave exclusive", IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_NORMAL))) passed = FALSE;
        }
        ShowCursor(TRUE);
        DestroyWindow(window);
    }
    UnregisterClassA(name, wc.hInstance);
    ZeroMemory(&restored, sizeof(restored));
    restored.dmSize = sizeof(restored);
    exact = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &restored) &&
            restored.dmPelsWidth == original.dmPelsWidth && restored.dmPelsHeight == original.dmPelsHeight &&
            restored.dmBitsPerPel == original.dmBitsPerPel && restored.dmDisplayFrequency == original.dmDisplayFrequency &&
            restored.dmDisplayFlags == original.dmDisplayFlags;
    printf("Flip %s desktop=%lux%lux%lu Hz=%lu exact_restore=%d\n", path, restored.dmPelsWidth,
           restored.dmPelsHeight, restored.dmBitsPerPel, restored.dmDisplayFrequency, exact);
    return passed && exact;
}
