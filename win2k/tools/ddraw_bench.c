/* DirectDraw timing on Windows 2000: the same frames through the driver
 * (hardware-only DirectDraw, video-memory surfaces) and through Windows'
 * software path (emulation-only DirectDraw, system-memory surfaces), and as a
 * program gets them that lets DirectDraw choose (default).
 * Results go to standard output when it is redirected, otherwise to COM1.
 * Each case runs for about three seconds. */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>

typedef HRESULT (WINAPI *CREATE_DIRECTDRAW)(GUID *, LPDIRECTDRAW *, IUnknown *);
enum { RunMs = 3000, MaxFrames = 5000, WinW = 640, WinH = 480, Sprite = 64, SpriteCount = 100 };

static HANDLE serial;
static CREATE_DIRECTDRAW create;

static void line(const char *text)
{
    DWORD n;
    WriteFile(serial, text, lstrlenA(text), &n, NULL);
    WriteFile(serial, "\r\n", 2, &n, NULL);
}

typedef struct {
    const char *name, *path;
    DWORD frames, ms, failures;
    HRESULT first;
} RESULT;

static void report(const RESULT *r)
{
    char msg[200];
    DWORD tenths = r->ms ? (r->frames * 10000UL) / r->ms : 0;
    wsprintfA(msg, "RBBENCH %s %s frames=%lu ms=%lu fps=%lu.%lu failures=%lu first_hr=%08lX",
              r->name, r->path, r->frames, r->ms, tenths / 10, tenths % 10, r->failures, (DWORD)r->first);
    line(msg);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

static void count(RESULT *r, HRESULT hr)
{
    if (FAILED(hr)) {
        if (!r->failures) r->first = hr;
        ++r->failures;
    }
}

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_SETCURSOR) { SetCursor(NULL); return TRUE; }
    return DefWindowProcA(w, m, wp, lp);
}

static LPDIRECTDRAWSURFACE offscreen(LPDIRECTDRAW dd, DWORD memory, DWORD w, DWORD h, HRESULT *hr)
{
    DDSURFACEDESC d;
    LPDIRECTDRAWSURFACE s = NULL;
    ZeroMemory(&d, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = w; d.dwHeight = h;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | memory;
    *hr = IDirectDraw_CreateSurface(dd, &d, &s, NULL);
    return SUCCEEDED(*hr) ? s : NULL;
}

/* Write every pixel, as a software renderer would. */
static HRESULT draw(LPDIRECTDRAWSURFACE s, DWORD w, DWORD h, DWORD frame)
{
    DDSURFACEDESC d;
    DWORD x, y;
    HRESULT hr;
    ZeroMemory(&d, sizeof(d));
    d.dwSize = sizeof(d);
    hr = IDirectDrawSurface_Lock(s, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    if (FAILED(hr)) return hr;
    for (y = 0; y < h; ++y) {
        DWORD *row = (DWORD *)((BYTE *)d.lpSurface + y * d.lPitch);
        for (x = 0; x < w; ++x) row[x] = ((x + frame) & 255) << 16 | ((y + frame) & 255) << 8 | (x ^ y);
    }
    return IDirectDrawSurface_Unlock(s, d.lpSurface);
}

static void windowed(const char *path, DWORD flags, DWORD memory, BOOL cpu)
{
    static const char cls[] = "RBBenchWindow";
    RESULT r;
    WNDCLASSA wc;
    HWND window = NULL;
    LPDIRECTDRAW dd = NULL;
    LPDIRECTDRAWSURFACE primary = NULL, source = NULL;
    LPDIRECTDRAWCLIPPER clipper = NULL;
    DDSURFACEDESC d;
    DDBLTFX fx;
    RECT to;
    POINT origin = {0, 0};
    DWORD start;
    HRESULT hr;
    ZeroMemory(&r, sizeof(r));
    r.name = cpu ? "window-cpu-present" : "window-fill-present";
    r.path = path;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = cls;
    RegisterClassA(&wc);
    window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls, "bench", WS_POPUP | WS_VISIBLE,
                             100, 100, WinW, WinH, NULL, NULL, wc.hInstance, NULL);
    hr = window ? create((GUID *)flags, &dd, NULL) : E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_NORMAL);
    if (SUCCEEDED(hr)) {
        ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d); d.dwFlags = DDSD_CAPS;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        hr = IDirectDraw_CreateSurface(dd, &d, &primary, NULL);
    }
    if (SUCCEEDED(hr)) hr = IDirectDraw_CreateClipper(dd, 0, &clipper, NULL);
    if (SUCCEEDED(hr)) hr = IDirectDrawClipper_SetHWnd(clipper, 0, window);
    if (SUCCEEDED(hr)) hr = IDirectDrawSurface_SetClipper(primary, clipper);
    if (SUCCEEDED(hr)) source = offscreen(dd, memory, WinW, WinH, &hr);
    if (FAILED(hr)) { count(&r, hr); goto done; }
    pump();
    ClientToScreen(window, &origin);
    SetRect(&to, origin.x, origin.y, origin.x + WinW, origin.y + WinH);
    start = GetTickCount();
    do {
        if (cpu) {
            count(&r, draw(source, WinW, WinH, r.frames));
        } else {
            ZeroMemory(&fx, sizeof(fx)); fx.dwSize = sizeof(fx);
            fx.dwFillColor = (r.frames * 0x010307UL) & 0xffffff;
            count(&r, IDirectDrawSurface_Blt(source, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx));
        }
        count(&r, IDirectDrawSurface_Blt(primary, &to, source, NULL, DDBLT_WAIT, NULL));
        ++r.frames;
        pump();
        r.ms = GetTickCount() - start;
    } while (r.ms < RunMs && r.frames < MaxFrames);
done:
    if (source) IDirectDrawSurface_Release(source);
    if (primary) IDirectDrawSurface_Release(primary);
    if (clipper) IDirectDrawClipper_Release(clipper);
    if (dd) IDirectDraw_Release(dd);
    if (window) DestroyWindow(window);
    UnregisterClassA(cls, wc.hInstance);
    report(&r);
}

enum { FillFlip, Sprites, KeyedSprites, Cases };
static const char *const names[Cases] = {"flip-fill", "flip-sprites", "flip-keyed-sprites"};

static void fullscreen_case(LPDIRECTDRAW dd, const char *path, DWORD chain, DWORD memory, int which)
{
    RESULT r;
    DDSURFACEDESC d;
    DDSCAPS caps;
    DDBLTFX fx;
    DDCOLORKEY key;
    LPDIRECTDRAWSURFACE front = NULL, back = NULL, sprite = NULL;
    DWORD start, i;
    HRESULT hr;
    ZeroMemory(&r, sizeof(r));
    r.name = names[which];
    r.path = path;
    ZeroMemory(&d, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.dwBackBufferCount = 1;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
    d.ddsCaps.dwCaps |= chain;
    hr = IDirectDraw_CreateSurface(dd, &d, &front, NULL);
    if (SUCCEEDED(hr)) {
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        hr = IDirectDrawSurface_GetAttachedSurface(front, &caps, &back);
    }
    if (SUCCEEDED(hr) && which != FillFlip) {
        sprite = offscreen(dd, memory, Sprite, Sprite, &hr);
        if (sprite) hr = draw(sprite, Sprite, Sprite, 0);
        if (SUCCEEDED(hr) && which == KeyedSprites) {
            key.dwColorSpaceLowValue = key.dwColorSpaceHighValue = 0;
            hr = IDirectDrawSurface_SetColorKey(sprite, DDCKEY_SRCBLT, &key);
        }
    }
    if (FAILED(hr)) { count(&r, hr); goto done; }
    start = GetTickCount();
    do {
        ZeroMemory(&fx, sizeof(fx)); fx.dwSize = sizeof(fx);
        fx.dwFillColor = (r.frames * 0x010307UL) & 0xffffff;
        count(&r, IDirectDrawSurface_Blt(back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx));
        if (sprite) {
            for (i = 0; i < SpriteCount; ++i) {
                DWORD x = (i * 37 + r.frames * 3) % (WinW - Sprite);
                DWORD y = (i * 53 + r.frames * 2) % (WinH - Sprite);
                count(&r, IDirectDrawSurface_BltFast(back, x, y, sprite, NULL, DDBLTFAST_WAIT |
                      (which == KeyedSprites ? DDBLTFAST_SRCCOLORKEY : DDBLTFAST_NOCOLORKEY)));
            }
        }
        count(&r, IDirectDrawSurface_Flip(front, NULL, DDFLIP_WAIT));
        ++r.frames;
        pump();
        r.ms = GetTickCount() - start;
    } while (r.ms < RunMs && r.frames < MaxFrames);
done:
    if (sprite) IDirectDrawSurface_Release(sprite);
    if (back) IDirectDrawSurface_Release(back);
    if (front) IDirectDrawSurface_Release(front);
    report(&r);
}

static void fullscreen(const char *path, DWORD flags, DWORD chain, DWORD memory)
{
    static const char cls[] = "RBBenchFull";
    WNDCLASSA wc;
    HWND window;
    LPDIRECTDRAW dd = NULL;
    HRESULT hr;
    int which;
    char msg[120];
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = cls;
    RegisterClassA(&wc);
    window = CreateWindowExA(WS_EX_TOPMOST, cls, "bench", WS_POPUP | WS_VISIBLE, 0, 0,
                             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                             NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(window);
    hr = window ? create((GUID *)flags, &dd, NULL) : E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (SUCCEEDED(hr)) hr = IDirectDraw_SetDisplayMode(dd, WinW, WinH, 32);
    if (FAILED(hr)) {
        wsprintfA(msg, "RBBENCH fullscreen %s setup hr=%08lX", path, (DWORD)hr);
        line(msg);
    } else {
        for (which = 0; which < Cases; ++which) fullscreen_case(dd, path, chain, memory, which);
        IDirectDraw_RestoreDisplayMode(dd);
        IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_NORMAL);
    }
    if (dd) IDirectDraw_Release(dd);
    if (window) DestroyWindow(window);
    UnregisterClassA(cls, wc.hInstance);
    pump();
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    HMODULE ddraw;
    DEVMODEA mode;
    char msg[120];
    (void)inst; (void)prev; (void)args; (void)show;
    /* A program started from Explorer may hold a standard handle that leads nowhere. */
    serial = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!serial || serial == INVALID_HANDLE_VALUE || GetFileType(serial) == FILE_TYPE_UNKNOWN)
        serial = CreateFileA("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (serial == INVALID_HANDLE_VALUE) return 1;
    ZeroMemory(&mode, sizeof(mode)); mode.dmSize = sizeof(mode);
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode);
    wsprintfA(msg, "RBBENCH begin desktop=%lux%lux%lu", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmBitsPerPel);
    line(msg);
    ddraw = LoadLibraryA("ddraw.dll");
    create = ddraw ? (CREATE_DIRECTDRAW)GetProcAddress(ddraw, "DirectDrawCreate") : NULL;
    if (create) {
        windowed("driver", (DWORD)DDCREATE_HARDWAREONLY, DDSCAPS_VIDEOMEMORY, FALSE);
        windowed("software", (DWORD)DDCREATE_EMULATIONONLY, DDSCAPS_SYSTEMMEMORY, FALSE);
        windowed("default", 0, 0, FALSE);
        windowed("driver", (DWORD)DDCREATE_HARDWAREONLY, DDSCAPS_VIDEOMEMORY, TRUE);
        windowed("software", (DWORD)DDCREATE_EMULATIONONLY, DDSCAPS_SYSTEMMEMORY, TRUE);
        windowed("default", 0, 0, TRUE);
        fullscreen("driver", (DWORD)DDCREATE_HARDWAREONLY, DDSCAPS_VIDEOMEMORY, DDSCAPS_VIDEOMEMORY);
        fullscreen("software", (DWORD)DDCREATE_EMULATIONONLY, 0, DDSCAPS_SYSTEMMEMORY);
        /* What a program gets that lets DirectDraw choose the memory. */
        fullscreen("default", 0, 0, 0);
    }
    ZeroMemory(&mode, sizeof(mode)); mode.dmSize = sizeof(mode);
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode);
    wsprintfA(msg, "RBBENCH end desktop=%lux%lux%lu", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmBitsPerPel);
    line(msg);
    CloseHandle(serial);
    return 0;
}
