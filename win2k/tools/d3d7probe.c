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
#ifdef RB_D3D7_INDEXED_PROBE
    /* Only these three indices select the original RGB triangle. */
    VERTEX pool[5] = {
        {124,124,0.5f,1,0xffffffff}, vertices[1], {123,124,0.5f,1,0xffffffff},
        vertices[2], vertices[0]
    };
    WORD indices[] = {4,1,3};
    drawn = IDirect3DDevice7_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE, pool, 5, indices, 3, 0);
#else
    drawn = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE, vertices, 3, 0);
#endif
    ended = IDirect3DDevice7_EndScene(device);
    return call("triangle list", drawn) && call("end", ended);
}

/* D3DX7 opens a 3D-tagged primary for windowed presentation. Check only
 * pixels in this test's client, including two guards outside the blit. */
static BOOL window_pixels(HWND window, IDirectDrawSurface7 *primary, IDirectDrawSurface7 *target)
{
    static const struct { LONG x, y; int r, g, b, tolerance; } samples[] = {
        {3, 3, 0, 0, 0, 0}, {16, 16, 0, 0, 102, 0},
        {48, 48, 128, 64, 64, 4}, {124, 124, 0, 0, 0, 0}
    };
    RECT src = {0, 0, 96, 96}, dst = {8, 8, 104, 104};
    POINT origin = {0, 0};
    HDC client = GetDC(window);
    unsigned i, matched = 0;
    if (!client) return FALSE;
    if (!ClientToScreen(window, &origin) || !PatBlt(client, 0, 0, Side, Side, BLACKNESS)) goto done;
    GdiFlush();
    OffsetRect(&dst, origin.x, origin.y);
    if (!call("window primary blit", IDirectDrawSurface7_Blt(primary, &dst, target, &src, DDBLT_WAIT, NULL)))
        goto done;
    for (i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
        POINT point = {origin.x + samples[i].x, origin.y + samples[i].y};
        COLORREF color;
        if (WindowFromPoint(point) != window) continue;
        color = GetPixel(client, samples[i].x, samples[i].y);
        if (color != CLR_INVALID && abs(GetRValue(color)-samples[i].r) <= samples[i].tolerance &&
            abs(GetGValue(color)-samples[i].g) <= samples[i].tolerance &&
            abs(GetBValue(color)-samples[i].b) <= samples[i].tolerance) ++matched;
    }
done:
    ReleaseDC(window, client);
    printf("D7 primary samples=%u/%u\n", matched, (unsigned)(sizeof(samples)/sizeof(samples[0])));
    return matched == sizeof(samples)/sizeof(samples[0]);
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

static BOOL get_profile(HDC dc, RB2K_D7_PROFILE *p)
{
    return dc && ExtEscape(dc, RB2K_D3D7_PROFILE_ESCAPE, 0, NULL, sizeof(*p), (LPSTR)p) > 0 &&
           p->frequency > 0;
}

static void print_profile(const RB2K_D7_PROFILE *before, const RB2K_D7_PROFILE *after)
{
    static const char *const names[] = {"clear", "draw", "read", "upload", "flush", "present"};
    unsigned i;
    for (i = 0; i < RB2K_D7_OPS; ++i) {
        const RB2K_D7_TIME *a = &before->op[i], *b = &after->op[i];
        printf("D7 frame profile op=%s calls=%lu submits=%lu fences=%lu bytes=%lu failed=%lu "
               "op_us=%I64d queue_us=%I64d fence_us=%I64d\n",
               names[i], b->calls-a->calls, b->submits-a->submits, b->fences-a->fences,
               b->bytes-a->bytes, b->failed-a->failed,
               (b->ticks-a->ticks)*1000000/after->frequency,
               (b->queueTicks-a->queueTicks)*1000000/after->frequency,
               (b->fenceTicks-a->fenceTicks)*1000000/after->frequency);
    }
}

static BOOL scene(IDirect3DDevice7 *device, IDirectDrawSurface7 *target, HDC dc, BOOL hardware)
{
    D3DVIEWPORT7 vp = {0, 0, Side, Side, 0, 1};
    LARGE_INTEGER clock, start, end;
    RB2K_D7_PROFILE before, after;
    RB2K_D7_FLIGHT first, last;
    BOOL profiling, flights;
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
    profiling = get_profile(dc, &before);
    flights = dc && ExtEscape(dc, RB2K_D3D7_FLIGHT_ESCAPE, 0, NULL, sizeof(first), (LPSTR)&first) > 0;
    if (!QueryPerformanceFrequency(&clock) || !QueryPerformanceCounter(&start)) return FALSE;
    for (n = 0; n < Frames; ++n) if (!draw(device)) return FALSE;
    if (!pixels(target, TRUE, &end) || end.QuadPart <= start.QuadPart) return FALSE;
    if (profiling && get_profile(dc, &after) && before.frequency == after.frequency)
        print_profile(&before, &after);
    if (flights) {
        ULONG queued, completed;
        if (ExtEscape(dc, RB2K_D3D7_FLIGHT_ESCAPE, 0, NULL, sizeof(last), (LPSTR)&last) <= 0 ||
            first.frequency <= 0 || first.frequency != last.frequency) return FALSE;
        queued = last.queued-first.queued;
        completed = last.completed-first.completed;
        printf("D7 frame flight limit=%lu peak=%lu queued=%lu completed=%lu pending=%lu failed=%lu "
               "throttle_waits=%lu throttle_us=%I64d access_waits=%lu access_us=%I64d\n",
               last.limit, last.peak, queued, completed, last.pending, last.failed-first.failed,
               last.throttleWaits-first.throttleWaits, (last.throttleTicks-first.throttleTicks)*1000000/last.frequency,
               last.accessWaits-first.accessWaits, (last.accessTicks-first.accessTicks)*1000000/last.frequency);
        if (queued != (hardware && last.limit == 2 ? Frames : 0) || completed != queued ||
            last.peak > last.limit || last.pending || last.failed != first.failed) return FALSE;
    }
    printf("D7 timed frames=%d us=%I64d\n", Frames, (end.QuadPart-start.QuadPart)*1000000/clock.QuadPart);
    return TRUE;
}

int main(int argc, char **argv)
{
    BOOL hal, clearOnly, targetOnly, windowed, gotStats, good = FALSE, restored;
    ENUM_DEVICE e;
    D3DDEVICEDESC7 caps;
    DEVMODEA before = {0}, after = {0};
    RB2K_D3D7_STATS base = {0}, final = {0};
    RB2K_D3D7_DIAGNOSTICS trace;
    IDirectDraw7 *dd = NULL;
    IDirect3D7 *d3d = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDrawSurface7 *target = NULL, *primary = NULL;
    IDirectDrawClipper *clipper = NULL;
    DDSURFACEDESC2 d = {0};
    WNDCLASSA wc = {0};
    HWND window;
    HDC dc;
    int result = 1;
    if (argc != 2 || (strcmp(argv[1], "hal") && strcmp(argv[1], "clear") &&
                      strcmp(argv[1], "rgb") && strcmp(argv[1], "target") && strcmp(argv[1], "window"))) {
        puts("Usage: d3d7probe rgb|target|clear|hal|window"); return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    clearOnly = !strcmp(argv[1], "clear");
    windowed = !strcmp(argv[1], "window");
    hal = clearOnly || windowed || !strcmp(argv[1], "hal");
    targetOnly = !strcmp(argv[1], "target");
    e.wanted = hal || targetOnly ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice;
    e.found = FALSE;
    before.dmSize = after.dmSize = sizeof(before);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &before)) return 1;
    if (windowed) {
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "RippleD3D7Window";
        if (!RegisterClassA(&wc)) return 1;
    }
    window = CreateWindowExA(windowed ? WS_EX_TOPMOST : 0, windowed ? wc.lpszClassName : "STATIC",
                             "Direct3D 7 pixel test", WS_POPUP, 0, 0, Side, Side,
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
    if (windowed) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        d.dwFlags = DDSD_CAPS;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE;
        if (!call("3D primary", IDirectDraw7_CreateSurface(dd, &d, &primary, NULL)) ||
            !call("create clipper", IDirectDraw7_CreateClipper(dd, 0, &clipper, NULL)) ||
            !call("clip window", IDirectDrawClipper_SetHWnd(clipper, 0, window)) ||
            !call("attach clipper", IDirectDrawSurface7_SetClipper(primary, clipper))) goto done;
    }
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
    if (clearOnly) {
        D3DRECT rect = {0, 0, Side, Side};
        good = call("clear", IDirect3DDevice7_Clear(device, 1, &rect, D3DCLEAR_TARGET, CLEAR_COLOR, 1, 0)) &&
               pixels(target, FALSE, NULL);
    } else {
        good = scene(device, target, dc, hal);
        if (good && windowed) good = window_pixels(window, primary, target);
    }
done:
    if (device) IDirect3DDevice7_Release(device);
    if (target) IDirectDrawSurface7_Release(target);
    if (primary) IDirectDrawSurface7_Release(primary);
    if (clipper) IDirectDrawClipper_Release(clipper);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) IDirectDraw7_Release(dd);
    if (gotStats && get_stats(dc, &final)) {
        ULONG clears = final.clears-base.clears, triangles = final.triangles-base.triangles;
        printf("D7 host clears=%lu triangles=%lu live_contexts=%lu/%lu live_targets=%lu/%lu\n",
               clears, triangles, base.contexts, final.contexts, base.targets, final.targets);
        good &= final.contexts == base.contexts && final.targets == base.targets;
        if (clearOnly) good &= clears == 1 && !triangles;
        else if (hal) good &= clears >= Frames+2 && triangles >= Frames+1;
        else good &= !clears && !triangles;
        if (ExtEscape(dc, RB2K_D3D7_STATS_ESCAPE, 0, NULL, sizeof(trace), (LPSTR)&trace) > 0) {
            ULONG i;
            printf("D7 caps info=%lu enabled=%lu native=%lu dp2_table=%lu surface_table=%lu\n",
                   trace.halInfoCalls, trace.enableCalls, trace.nativeEnabled, trace.callbackQueries, trace.surfaceQueries);
            printf("D7 query IDs");
            for (i = 0; i < trace.queryCount && i < RB2K_D3D7_QUERY_LIMIT; ++i) printf(" %08lx", trace.queryIds[i]);
            putchar('\n');
            printf("D7 callbacks surface=%lu context=%lu clear_tokens=%lu dp2=%lu last_op=%lu hr=%08lx offset=%lu\n",
                   trace.surfaceCalls, trace.contextCalls, trace.clearCalls, trace.commandCalls,
                   trace.lastCommand, trace.lastError, trace.errorOffset);
        }
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
