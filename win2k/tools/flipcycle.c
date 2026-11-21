/* FLIPCYCLE: rebuild a fullscreen flip chain four times in one DirectDraw
 * session (as games do after a mode change), and each time copy colour-keyed
 * system-memory sprites into the back buffer, flip, and lock the back buffer.
 * Default DirectDraw: the memory is DirectDraw's choice. Output goes to
 * standard output when it is redirected, otherwise to COM1. */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>

typedef HRESULT (WINAPI *CREATE_DIRECTDRAW)(GUID *, LPDIRECTDRAW *, IUnknown *);
static HANDLE serial;

static void line(const char *t)
{
    DWORD n;
    WriteFile(serial, t, lstrlenA(t), &n, NULL);
    WriteFile(serial, "\r\n", 2, &n, NULL);
}

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(w, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    static const char cls[] = "FlipCycle";
    CREATE_DIRECTDRAW create;
    LPDIRECTDRAW dd = NULL;
    WNDCLASSA wc;
    HWND window;
    HRESULT hr;
    char msg[160];
    int round, failed = 0;
    (void)prev; (void)args; (void)show;
    serial = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!serial || serial == INVALID_HANDLE_VALUE || GetFileType(serial) == FILE_TYPE_UNKNOWN)
        serial = CreateFileA("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (serial == INVALID_HANDLE_VALUE) return 1;
    create = (CREATE_DIRECTDRAW)GetProcAddress(LoadLibraryA("ddraw.dll"), "DirectDrawCreate");
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = cls;
    RegisterClassA(&wc);
    window = CreateWindowExA(WS_EX_TOPMOST, cls, "flip", WS_POPUP | WS_VISIBLE, 0, 0,
                             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                             NULL, NULL, inst, NULL);
    SetForegroundWindow(window);
    hr = create ? create(NULL, &dd, NULL) : E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (SUCCEEDED(hr)) hr = IDirectDraw_SetDisplayMode(dd, 640, 480, 32);
    wsprintfA(msg, "FLIPCYCLE setup hr=%08lX", (DWORD)hr);
    line(msg);
    for (round = 0; SUCCEEDED(hr) && round < 4; ++round) {
        LPDIRECTDRAWSURFACE front = NULL, back = NULL, sprite = NULL;
        DDSURFACEDESC d;
        DDSCAPS caps;
        DDCOLORKEY key;
        HRESULT create_hr, first = S_OK, lock_hr = E_FAIL, flip_hr = E_FAIL;
        int i, bad = 0;
        ZeroMemory(&d, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        d.dwBackBufferCount = 1;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
        create_hr = IDirectDraw_CreateSurface(dd, &d, &front, NULL);
        if (SUCCEEDED(create_hr)) {
            caps.dwCaps = DDSCAPS_BACKBUFFER;
            create_hr = IDirectDrawSurface_GetAttachedSurface(front, &caps, &back);
        }
        if (SUCCEEDED(create_hr)) {
            ZeroMemory(&d, sizeof(d));
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            d.dwWidth = d.dwHeight = 64;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
            create_hr = IDirectDraw_CreateSurface(dd, &d, &sprite, NULL);
        }
        if (SUCCEEDED(create_hr)) {
            key.dwColorSpaceLowValue = key.dwColorSpaceHighValue = 0;
            create_hr = IDirectDrawSurface_SetColorKey(sprite, DDCKEY_SRCBLT, &key);
        }
        if (SUCCEEDED(create_hr)) {
            for (i = 0; i < 100; ++i) {
                HRESULT b = IDirectDrawSurface_BltFast(back, (i * 37) % 576, (i * 53) % 416, sprite, NULL,
                                                      DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY);
                if (FAILED(b)) { if (!bad) first = b; ++bad; }
            }
            flip_hr = IDirectDrawSurface_Flip(front, NULL, DDFLIP_WAIT);
            ZeroMemory(&d, sizeof(d));
            d.dwSize = sizeof(d);
            lock_hr = IDirectDrawSurface_Lock(back, NULL, &d, DDLOCK_WAIT, NULL);
            if (SUCCEEDED(lock_hr)) IDirectDrawSurface_Unlock(back, d.lpSurface);
        }
        wsprintfA(msg, "FLIPCYCLE round=%d create=%08lX blt_failures=%d first=%08lX flip=%08lX lock=%08lX",
                  round, (DWORD)create_hr, bad, (DWORD)first, (DWORD)flip_hr, (DWORD)lock_hr);
        line(msg);
        if (FAILED(create_hr) || bad || FAILED(flip_hr) || FAILED(lock_hr)) ++failed;
        if (sprite) IDirectDrawSurface_Release(sprite);
        if (back) IDirectDrawSurface_Release(back);
        if (front) IDirectDrawSurface_Release(front);
    }
    if (dd) {
        IDirectDraw_RestoreDisplayMode(dd);
        IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_NORMAL);
        IDirectDraw_Release(dd);
    }
    DestroyWindow(window);
    wsprintfA(msg, "FLIPCYCLE end failed_rounds=%d", failed);
    line(msg);
    CloseHandle(serial);
    return failed;
}
