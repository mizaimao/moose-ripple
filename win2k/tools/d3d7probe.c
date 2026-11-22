/* Exercise the system Direct3D 7 runtime and inspect only our render target.
 * The dark-blue clear and RGB Gouraud centre are the 9x HAL reference colours. */
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rb2kd3d7.h"
#include "rb2kprobe.h"

enum { Side = 128, Frames = 120 };
#define CLEAR_COLOR 0xff000066UL
typedef struct { float x, y, z, rhw; DWORD diffuse; } VERTEX;
static VERTEX vertices[] = {
    {16, 16, 0.5f, 1, 0xffff0000}, {112, 16, 0.5f, 1, 0xff00ff00},
    {16, 112, 0.5f, 1, 0xff0000ff}
};
typedef struct { const GUID *wanted; BOOL found; } ENUM_DEVICE;

static HRESULT CALLBACK device_info(LPSTR description, LPSTR name, D3DDEVICEDESC7 *d, LPVOID user)
{
    ENUM_DEVICE *e = user;
    (void)description;
    printf("D7 device %s guid=%08lx caps=%08lx\n", name, d->deviceGUID.Data1, d->dwDevCaps);
    if (IsEqualGUID(&d->deviceGUID, e->wanted)) e->found = TRUE;
    return D3DENUMRET_OK;
}

static BOOL call(const char *what, HRESULT hr)
{
    if (FAILED(hr)) printf("D7 %s hr=%08lx\n", what, (DWORD)hr);
    return SUCCEEDED(hr);
}

static BOOL get_stats(HDC dc, RB2K_D3D7_STATS *s)
{
    return dc && ExtEscape(dc, RB2K_D3D7_STATS_ESCAPE, 0, NULL, sizeof(*s), (LPSTR)s) > 0 &&
           s->version == RB2K_D3D7_STATS_VERSION;
}

static BOOL pixels(IDirectDrawSurface7 *target, BOOL triangle, LARGE_INTEGER *completed)
{
    DDSURFACEDESC2 d = {0};
    unsigned x, y, inside = 0, outside = 0, wrong = 0;
    BOOL layout;
    d.dwSize = sizeof(d);
    if (!call("lock pixels", IDirectDrawSurface7_Lock(target, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL)))
        return FALSE;
    if (completed && !QueryPerformanceCounter(completed)) completed->QuadPart = 0;
    layout = d.dwWidth == Side && d.dwHeight == Side && d.lpSurface && d.lPitch >= Side * 4 &&
        d.ddpfPixelFormat.dwRGBBitCount == 32 && d.ddpfPixelFormat.dwRBitMask == 0xff0000 &&
        d.ddpfPixelFormat.dwGBitMask == 0xff00 && d.ddpfPixelFormat.dwBBitMask == 0xff;
    if (layout) for (y = 0; y < Side; ++y) {
        const DWORD *row = (const DWORD *)((const BYTE *)d.lpSurface + y*d.lPitch);
        for (x = 0; x < Side; ++x) {
            int u = (int)x - 16, v = (int)y - 16, red = 0, green = 0, blue = 102, tolerance = 0;
            DWORD value = row[x] & 0xffffff;
            if (triangle && u > 1 && v > 1 && u + v < 94) {
                green = (u * 255 + 48) / 96;
                blue = (v * 255 + 48) / 96;
                red = ((96 - u - v) * 255 + 48) / 96;
                tolerance = 4;
                ++inside;
            } else if (triangle && u >= -1 && v >= -1 && u + v <= 98) {
                continue; /* Runtime rasterizers differ at triangle boundaries. */
            } else {
                ++outside;
            }
            if (abs((int)(value >> 16) - red) > tolerance ||
                abs((int)((value >> 8) & 255) - green) > tolerance ||
                abs((int)(value & 255) - blue) > tolerance) {
                if (!wrong) printf("D7 first mismatch at %u,%u expected=%02x%02x%02x got=%06lx\n",
                                   x, y, red, green, blue, value);
                ++wrong;
            }
        }
    }
    printf("D7 %s interior=%u background=%u mismatches=%u layout=%d\n",
           triangle ? "triangle" : "clear", inside, outside, wrong, layout);
    return call("unlock pixels", IDirectDrawSurface7_Unlock(target, NULL)) && layout &&
           outside && (!triangle || inside) && !wrong;
}

static BOOL draw(IDirect3DDevice7 *device)
{
    HRESULT drawn, ended;
    if (!call("clear", IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1, 0)) ||
        !call("begin", IDirect3DDevice7_BeginScene(device))) return FALSE;
    drawn = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE, vertices, 3, 0);
    ended = IDirect3DDevice7_EndScene(device);
    return call("triangle list", drawn) && call("end", ended);
}

/* Check the DirectDraw render target independently of HAL enumeration. */
static BOOL target_fill(IDirectDrawSurface7 *target, HDC dc)
{
    DDBLTFX effect = {0};
    RB2K_DD_STATS before, after;
    DDSCAPS2 caps = {0};
    effect.dwSize = sizeof(effect);
    effect.dwFillColor = CLEAR_COLOR & 0xffffff;
    if (!dc || !call("surface caps", IDirectDrawSurface7_GetCaps(target, &caps))) return FALSE;
    printf("D7 target surface caps=%08lx\n", caps.dwCaps);
    if (!(caps.dwCaps & DDSCAPS_VIDEOMEMORY) || !(caps.dwCaps & DDSCAPS_3DDEVICE) ||
        ExtEscape(dc, RB2K_DD_STATS_ESCAPE, 0, NULL, sizeof(before), (LPSTR)&before) <= 0 ||
        !call("target Blt", IDirectDrawSurface7_Blt(target, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &effect)) ||
        !pixels(target, FALSE, NULL) ||
        ExtEscape(dc, RB2K_DD_STATS_ESCAPE, 0, NULL, sizeof(after), (LPSTR)&after) <= 0) return FALSE;
    printf("D7 target host_fills=%lu readbacks=%lu uploads=%lu\n",
           after.hostFills-before.hostFills, after.readbacks-before.readbacks, after.uploads-before.uploads);
    return after.hostFills-before.hostFills == 1 && after.readbacks-before.readbacks == 1 &&
           before.uploads == after.uploads;
}

static BOOL scene(IDirect3DDevice7 *device, IDirectDrawSurface7 *target)
{
    D3DVIEWPORT7 vp = {0, 0, Side, Side, 0, 1};
    LARGE_INTEGER clock, start, end;
    int n;
    if (!call("viewport", IDirect3DDevice7_SetViewport(device, &vp)) ||
        !call("lighting", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE)) ||
        !call("depth", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE)) ||
        !call("cull", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE)) ||
        !call("shade", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD)) ||
        !call("diffuse colour", IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE)) ||
        !call("select colour", IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1)) ||
        !call("clear", IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1, 0)) ||
        !pixels(target, FALSE, NULL) || !draw(device) || !pixels(target, TRUE, NULL))
        return FALSE;
    if (!QueryPerformanceFrequency(&clock) || !QueryPerformanceCounter(&start)) return FALSE;
    for (n = 0; n < Frames; ++n) if (!draw(device)) return FALSE;
    if (!pixels(target, TRUE, &end) || end.QuadPart <= start.QuadPart) return FALSE;
    printf("D7 timed frames=%d us=%I64d\n", Frames, (end.QuadPart-start.QuadPart)*1000000/clock.QuadPart);
    return TRUE;
}

int main(int argc, char **argv)
{
    BOOL hal, targetOnly, gotStats, good = FALSE, restored;
    ENUM_DEVICE e;
    D3DDEVICEDESC7 caps;
    DEVMODEA before = {0}, after = {0};
    RB2K_D3D7_STATS base = {0}, final = {0};
    IDirectDraw7 *dd = NULL;
    IDirect3D7 *d3d = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDrawSurface7 *target = NULL;
    DDSURFACEDESC2 d = {0};
    HWND window;
    HDC dc;
    int result = 1;
    if (argc != 2 || (strcmp(argv[1], "hal") && strcmp(argv[1], "rgb") && strcmp(argv[1], "target"))) {
        puts("Usage: d3d7probe rgb|hal|target"); return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    hal = !strcmp(argv[1], "hal");
    targetOnly = !strcmp(argv[1], "target");
    e.wanted = hal || targetOnly ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice;
    e.found = FALSE;
    before.dmSize = after.dmSize = sizeof(before);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &before)) return 1;
    window = CreateWindowExA(0, "STATIC", "Direct3D 7 pixel test", WS_POPUP, 0, 0, Side, Side,
                             NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!window) return 1;
    dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    gotStats = get_stats(dc, &base);
    printf("D7 begin path=%s target=%dx%d desktop=%lux%lux%lu host_counters=%d\n",
           argv[1], Side, Side, before.dmPelsWidth, before.dmPelsHeight, before.dmBitsPerPel, gotStats);
    if (!call("DirectDrawCreateEx", DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) ||
        !call("cooperative level", IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL)) ||
        !call("IDirect3D7", IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d)) ||
        !call("enumerate", IDirect3D7_EnumDevices(d3d, device_info, &e))) goto done;
    printf("D7 requested=%s listed=%d\n", argv[1], e.found);
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = Side;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE |
                       (hal || targetOnly ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xff0000; d.ddpfPixelFormat.dwGBitMask = 0xff00; d.ddpfPixelFormat.dwBBitMask = 0xff;
    if (!call("render target", IDirectDraw7_CreateSurface(dd, &d, &target, NULL))) {
        if ((hal || targetOnly) && !e.found) result = 2;
        goto done;
    }
    if (targetOnly) {
        good = target_fill(target, dc);
        goto done;
    }
    if (!call("CreateDevice", IDirect3D7_CreateDevice(d3d, e.wanted, target, &device))) {
        if (hal && !e.found) result = 2;
        goto done;
    }
    if (!call("device caps", IDirect3DDevice7_GetCaps(device, &caps))) goto done;
    printf("D7 selected device guid=%08lx caps=%08lx\n", caps.deviceGUID.Data1, caps.dwDevCaps);
    if (!e.found ||
        !IsEqualGUID(&caps.deviceGUID, e.wanted) || !!(caps.dwDevCaps & D3DDEVCAPS_HWRASTERIZATION) != hal)
        goto done;
    good = scene(device, target);
done:
    if (device) IDirect3DDevice7_Release(device);
    if (target) IDirectDrawSurface7_Release(target);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) IDirectDraw7_Release(dd);
    if (gotStats && get_stats(dc, &final)) {
        ULONG clears = final.clears-base.clears, triangles = final.triangles-base.triangles;
        printf("D7 host clears=%lu triangles=%lu live_contexts=%lu/%lu live_targets=%lu/%lu\n",
               clears, triangles, base.contexts, final.contexts, base.targets, final.targets);
        good &= final.contexts == base.contexts && final.targets == base.targets;
        good &= hal ? (clears >= Frames+2 && triangles >= Frames+1) : (!clears && !triangles);
    } else if (hal || gotStats) {
        good = FALSE;
    }
    if (dc) DeleteDC(dc);
    DestroyWindow(window);
    restored = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &after) &&
        before.dmPelsWidth == after.dmPelsWidth && before.dmPelsHeight == after.dmPelsHeight &&
        before.dmBitsPerPel == after.dmBitsPerPel && before.dmDisplayFrequency == after.dmDisplayFrequency;
    if (!restored) result = 1;
    if (good && restored) result = 0;
    printf("D7 end path=%s desktop_restored=%d result=%s\n", argv[1], restored,
           result == 0 ? "PASS" : result == 2 ? "UNAVAILABLE" : "FAIL");
    return result;
}
