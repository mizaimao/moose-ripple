/* Verify a Z16 video-memory surface through DirectDraw. Count miniport
 * transfers as well as matching every value in the final surface lock. */
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include "rb2kprobe.h"

static BOOL success(const char *op, HRESULT hr)
{
    if (FAILED(hr)) printf("ZSTORE %s hr=%08lx\n", op, (DWORD)hr);
    return SUCCEEDED(hr);
}

static WORD pattern(DWORD x, DWORD y)
{
    return (WORD)((x * 7951 + y * 683) ^ 0x7c35);
}

static BOOL lock(IDirectDrawSurface7 *surface, RECT *rect, DWORD flags, DDSURFACEDESC2 *view,
                 DWORD width)
{
    ZeroMemory(view, sizeof(*view));
    view->dwSize = sizeof(*view);
    if (!success("lock", IDirectDrawSurface7_Lock(surface, rect, view, DDLOCK_WAIT | flags, NULL)))
        return FALSE;
    if (!view->lpSurface || view->lPitch < (LONG)(width * sizeof(WORD))) {
        printf("ZSTORE bad lock width=%lu pitch=%ld\n", width, view->lPitch);
        IDirectDrawSurface7_Unlock(surface, rect);
        return FALSE;
    }
    return TRUE;
}

static BOOL transfer_case(IDirectDraw7 *dd, HDC dc, DWORD width, DWORD height)
{
    DDSURFACEDESC2 desc = {0}, view;
    RECT edit = {1, 1, 2, 2};
    IDirectDrawSurface7 *surface = NULL;
    RB2K_DD_STATS first = {0}, last = {0};
    BOOL ok = FALSE;
    DWORD x, y, mismatches = 0;
    LONG pitch;
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    desc.dwWidth = width;
    desc.dwHeight = height;
    desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    desc.ddpfPixelFormat.dwZBufferBitDepth = 16;
    desc.ddpfPixelFormat.dwZBitMask = 0xffff;
    if (ExtEscape(dc, RB2K_DD_STATS_ESCAPE, 0, NULL, sizeof(first), (char *)&first) != 1 ||
        !success("create Z16 video surface", IDirectDraw7_CreateSurface(dd, &desc, &surface, NULL)))
        goto done;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (!success("get surface", IDirectDrawSurface7_GetSurfaceDesc(surface, &desc)) ||
        (desc.ddsCaps.dwCaps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY)) !=
        (DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER) || desc.ddpfPixelFormat.dwZBufferBitDepth != 16 ||
        !lock(surface, NULL, DDLOCK_WRITEONLY, &view, width)) goto done;
    pitch = view.lPitch;
    for (y = 0; y < height; ++y) {
        WORD *row = (WORD *)((BYTE *)view.lpSurface + y * pitch);
        for (x = 0; x < width; ++x) row[x] = pattern(x, y);
    }
    if (!success("unlock whole write", IDirectDrawSurface7_Unlock(surface, NULL)) ||
        !lock(surface, &edit, DDLOCK_WRITEONLY, &view, 1)) goto done;
    *(WORD *)view.lpSurface = 0xacea;
    if (!success("unlock one write", IDirectDrawSurface7_Unlock(surface, &edit)) ||
        !lock(surface, NULL, DDLOCK_READONLY, &view, width)) goto done;
    for (y = 0; y < height; ++y) {
        const WORD *row = (const WORD *)((const BYTE *)view.lpSurface + y * view.lPitch);
        for (x = 0; x < width; ++x) {
            WORD expected = (x == 1 && y == 1) ? 0xacea : pattern(x, y);
            if (row[x] != expected && ++mismatches == 1)
                printf("ZSTORE first mismatch xy=%lu,%lu expected=%04x actual=%04x\n", x,y,expected,row[x]);
        }
    }
    if (!success("unlock whole read", IDirectDrawSurface7_Unlock(surface, NULL)) ||
        ExtEscape(dc, RB2K_DD_STATS_ESCAPE, 0, NULL, sizeof(last), (char *)&last) != 1)
        goto done;
    printf("ZSTORE size=%lux%lu pitch=%ld values=%lu mismatches=%lu reads=%lu writes=%lu\n",
           width, height, pitch, width*height, mismatches, last.readbacks-first.readbacks,
           last.uploads-first.uploads);
    ok = !mismatches && last.uploads-first.uploads == 2 && last.readbacks-first.readbacks == 2;
done:
    if (surface) IDirectDrawSurface7_Release(surface);
    return ok;
}

int main(void)
{
    IDirectDraw7 *dd = NULL;
    HWND window = CreateWindowA("STATIC", "Z16 storage", WS_POPUP, 0, 0, 64, 64,
                                NULL, NULL, GetModuleHandleA(NULL), NULL);
    HDC dc = GetDC(NULL);
    BOOL ok = FALSE;
    if (window && dc &&
        success("DirectDrawCreateEx", DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) &&
        success("cooperative", IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL))) {
        BOOL small = transfer_case(dd, dc, 3, 3);
        BOOL large = transfer_case(dd, dc, 257, 129);
        ok = small && large;
    }
    if (dd) IDirectDraw7_Release(dd);
    if (dc) ReleaseDC(NULL, dc);
    if (window) DestroyWindow(window);
    printf("ZSTORE result=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
