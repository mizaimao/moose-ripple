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

static DWORD native_pixel(DWORD rgb, DWORD bpp)
{
    if (bpp == 8)
        return ((rgb >> 16) & 0xe0) | ((rgb >> 11) & 0x1c) | ((rgb >> 6) & 3);
    if (bpp == 16)
        return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
    return rgb;
}

static BOOL pixels(LPDIRECTDRAWSURFACE surface, DWORD width, DWORD height, DWORD bpp,
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
            lock.lPitch >= (LONG)(width * (bpp / 8)) && (lock.ddpfPixelFormat.dwFlags & DDPF_RGB) &&
            lock.ddpfPixelFormat.dwRGBBitCount == bpp;
    if (bpp == 8) valid = valid && (lock.ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8);
    else valid = valid && lock.ddpfPixelFormat.dwRBitMask == native_pixel(0xff0000, bpp) &&
                 lock.ddpfPixelFormat.dwGBitMask == native_pixel(0x00ff00, bpp) &&
                 lock.ddpfPixelFormat.dwBBitMask == native_pixel(0x0000ff, bpp);
    if (valid) for (y = 0; y < height; ++y) {
        BYTE *row = (BYTE *)lock.lpSurface + y * lock.lPitch;
        for (x = 0; x < width; ++x) {
            DWORD expected = native_pixel(pixel(x, y, image), bpp);
            if (bpp == 8) {
                if (write) row[x] = (BYTE)expected;
                else if (row[x] != expected) ++wrong;
            } else if (bpp == 16) {
                if (write) ((WORD *)row)[x] = (WORD)expected;
                else if (((WORD *)row)[x] != expected) ++wrong;
            } else {
                if (write) ((DWORD *)row)[x] = expected;
                else if ((((DWORD *)row)[x] & 0xffffff) != expected) ++wrong;
            }
        }
    }
    printf("Flip %s image=%lu pixels=%lu mismatches=%lu pitch=%ld valid=%d\n",
           write ? "write" : "check", image, width * height, wrong, lock.lPitch, valid);
    return call("unlock", IDirectDrawSurface_Unlock(surface, lock.lpSurface)) && valid && !wrong;
}

/* Change one colour twice without repainting its pixel or its neighbour. */
static BOOL indexed_palette(LPDIRECTDRAW dd, LPDIRECTDRAWSURFACE front, LPDIRECTDRAWPALETTE *palette)
{
    PALETTEENTRY colors[256], saved, changed = { 19, 159, 221, 0 };
    DDSURFACEDESC lock;
    RECT pair = { 17, 15, 19, 16 };
    DWORD i;
    BOOL ok = TRUE;
    for (i = 0; i < 256; ++i) {
        colors[i].peRed = (BYTE)((i >> 5) * 255 / 7);
        colors[i].peGreen = (BYTE)(((i >> 2) & 7) * 255 / 7);
        colors[i].peBlue = (BYTE)((i & 3) * 255 / 3);
        colors[i].peFlags = 0;
    }
    if (!call("create 256 colours", IDirectDraw_CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256,
              colors, palette, NULL)) ||
        !call("attach palette", IDirectDrawSurface_SetPalette(front, *palette))) return FALSE;
    ZeroMemory(&lock, sizeof(lock)); lock.dwSize = sizeof(lock);
    if (!call("lock index pair", IDirectDrawSurface_Lock(front, &pair, &lock,
              DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL))) return FALSE;
    ok = lock.lpSurface && lock.ddpfPixelFormat.dwRGBBitCount == 8;
    if (ok) {
        ((BYTE *)lock.lpSurface)[0] = 73;
        ((BYTE *)lock.lpSurface)[1] = 74;
    }
    if (!call("unlock index pair", IDirectDrawSurface_Unlock(front, lock.lpSurface)) || !ok) return FALSE;
    saved = colors[73];
    for (i = 0; i < 3; ++i) {
        HDC dc;
        COLORREF a, b;
        if (i) {
            colors[73] = i == 1 ? changed : saved;
            if (!call("cycle one colour", IDirectDrawPalette_SetEntries(*palette, 0, 73, 1, &colors[73])))
                return FALSE;
        }
        if (!call("indexed surface DC", IDirectDrawSurface_GetDC(front, &dc))) return FALSE;
        a = GetPixel(dc, pair.left, pair.top);
        b = GetPixel(dc, pair.left + 1, pair.top);
        if (!call("release indexed DC", IDirectDrawSurface_ReleaseDC(front, dc))) return FALSE;
        if (a != RGB(colors[73].peRed, colors[73].peGreen, colors[73].peBlue) ||
            b != RGB(colors[74].peRed, colors[74].peGreen, colors[74].peBlue)) ok = FALSE;
    }
    printf("Indexed palette animation and neighbour match=%d\n", ok);
    return ok;
}

/* An odd horizontal origin exercises byte offsets for both Blt and Lock.
 * Neighbouring words must keep the background colour. */
static BOOL fill_rect16(LPDIRECTDRAWSURFACE surface)
{
    RECT rect = {7, 5, 12, 9};
    DDBLTFX fx;
    DDSURFACEDESC locked;
    LONG x, y;
    BOOL ok;
    ZeroMemory(&fx, sizeof(fx)); fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x1234;
    if (!call("16-bit background fill", IDirectDrawSurface_Blt(surface, NULL, NULL, NULL,
             DDBLT_COLORFILL | DDBLT_WAIT, &fx))) return FALSE;
    fx.dwFillColor = 0xa35b;
    if (!call("16-bit rectangle fill", IDirectDrawSurface_Blt(surface, &rect, NULL, NULL,
             DDBLT_COLORFILL | DDBLT_WAIT, &fx))) return FALSE;
    ZeroMemory(&locked, sizeof(locked)); locked.dwSize = sizeof(locked);
    if (!call("16-bit guard lock", IDirectDrawSurface_Lock(surface, NULL, &locked,
             DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return FALSE;
    ok = locked.lpSurface && locked.lPitch >= (rect.right + 1) * (LONG)sizeof(WORD) &&
         locked.dwHeight > (DWORD)rect.bottom && locked.ddpfPixelFormat.dwRGBBitCount == 16;
    if (ok) for (y = rect.top - 1; y <= rect.bottom; ++y) {
        WORD *row = (WORD *)((BYTE *)locked.lpSurface + y * locked.lPitch);
        for (x = rect.left - 1; x <= rect.right; ++x) {
            WORD expected = (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                          ? 0xa35b : 0x1234;
            if (row[x] != expected) ok = FALSE;
        }
    }
    if (!call("16-bit guard unlock", IDirectDrawSurface_Unlock(surface, locked.lpSurface))) return FALSE;
    ZeroMemory(&locked, sizeof(locked)); locked.dwSize = sizeof(locked);
    if (!call("16-bit rectangle lock", IDirectDrawSurface_Lock(surface, &rect, &locked,
             DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return FALSE;
    ok = ok && locked.lpSurface && *(WORD *)locked.lpSurface == 0xa35b;
    printf("Flip RGB565 rectangle and guards match=%d\n", ok);
    return call("16-bit rectangle unlock", IDirectDrawSurface_Unlock(surface, locked.lpSurface)) && ok;
}

static BOOL scanout(HDC dc, RB2K_DD_FLIP_INFO *info)
{
    return dc && ExtEscape(dc, RB2K_DD_SCANOUT_ESCAPE, 0, NULL,
                           sizeof(*info), (LPSTR)info) > 0;
}

static BOOL sample_window(HWND window, DWORD width, DWORD height, DWORD bpp, DWORD image)
{
    HDC dc = GetDC(window);
    DWORD col, row, wrong = 0;
    if (!dc) return FALSE;
    for (row = 0; row <= 8; ++row) {
        for (col = 0; col <= 8; ++col) {
            int x = col * (width - 1) / 8, y = row * (height - 1) / 8;
            POINT at;
            DWORD rgb = pixel(x, y, image);
            COLORREF sample;
            at.x = x; at.y = y;
            if (!ClientToScreen(window, &at) || WindowFromPoint(at) != window) { ++wrong; continue; }
            sample = GetPixel(dc, x, y);
            if (sample == CLR_INVALID || native_pixel(((DWORD)GetRValue(sample) << 16) |
                    ((DWORD)GetGValue(sample) << 8) | GetBValue(sample), bpp) != native_pixel(rgb, bpp)) ++wrong;
        }
    }
    ReleaseDC(window, dc);
    printf("Flip visible window image=%lu samples=81 mismatches=%lu\n", image, wrong);
    return wrong == 0;
}

static BOOL exercise(LPDIRECTDRAW dd, HWND window, const char *path, BOOL hardware,
                     DWORD width, DWORD height, DWORD bpp)
{
    LPDIRECTDRAWSURFACE front = NULL, back = NULL;
    LPDIRECTDRAWPALETTE palette = NULL;
    DDSURFACEDESC desc;
    DDSCAPS caps;
    HDC dc = NULL;
    RB2K_DD_FLIP_INFO start, now;
    DWORD frame;
    BOOL passed = FALSE;
    printf("Flip %s requested=%lux%lux%lu\n", path, width, height, bpp);
    if (!call("set mode", IDirectDraw_SetDisplayMode(dd, width, height, bpp))) return FALSE;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    desc.dwBackBufferCount = 1;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
    if (hardware) desc.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY;
    if (!call("create double buffer", IDirectDraw_CreateSurface(dd, &desc, &front, NULL))) goto done;
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (!call("get attached back", IDirectDrawSurface_GetAttachedSurface(front, &caps, &back))) goto done;
    if (bpp == 8 && !indexed_palette(dd, front, &palette)) goto done;
    if (hardware) {
        dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
        if (!scanout(dc, &start) || start.pageCount < 2 || start.visiblePage || start.yOffset ||
            start.width != width || start.height != height || start.pitch != width * (bpp / 8) ||
            start.pageBytes != start.pitch * height) {
            puts("Flip initial scanout geometry unavailable or incorrect");
            goto done;
        }
        if (bpp == 16 && !fill_rect16(back)) goto done;
    }
    /* Release an odd number of flips in, with page one still on screen. */
    for (frame = 0; frame < 5; ++frame) {
        DWORD page = (frame + 1) % 2;
        if ((frame < 2 || !hardware) && !pixels(back, width, height, bpp, frame % 2, TRUE)) goto done;
        if (!call("swap", IDirectDrawSurface_Flip(front, NULL, DDFLIP_WAIT))) goto done;
        if (hardware) {
            if (!scanout(dc, &now)) goto done;
            printf("Flip frame=%lu page=%lu y_offset=%lu flips=%lu\n",
                   frame + 1, now.visiblePage, now.yOffset, now.flips - start.flips);
            if (now.visiblePage != page || now.yOffset != page * height ||
                now.flips - start.flips != frame + 1) goto done;
        }
        if (hardware) {
            if (!pixels(front, width, height, bpp, frame % 2, FALSE)) goto done;
        } else if (!sample_window(window, width, height, bpp, frame % 2)) goto done;
    }
    passed = TRUE;
done:
    if (back) IDirectDrawSurface_Release(back);
    if (front) IDirectDrawSurface_Release(front);
    if (palette) IDirectDrawPalette_Release(palette);
    if (dc) {
        if (!scanout(dc, &now) || now.yOffset || now.visiblePage) passed = FALSE;
        else puts("Flip chain released on desktop page zero");
        DeleteDC(dc);
    }
    return passed;
}

BOOL probe_ddraw_flip(LPDIRECTDRAW dd, const char *path, BOOL hardware, DWORD bpp)
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
            passed = exercise(dd, window, path, hardware, 640, 480, bpp);
            if (passed && hardware) passed = exercise(dd, window, path, hardware, 1024, 768, bpp);
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
