/* Does multisampling smooth edges? Direct3D 9, with several samples a pixel: a white
 * triangle with slanted edges is drawn on black, and a pixel an edge runs through then
 * comes out a grey, by how many of its samples the triangle covers. Without multisampling
 * every pixel is white or black.
 *   d3d9msaa [SAMPLES [full]]   SAMPLES: 2, 4 or 8; 0 or nothing: the most the driver offers;
 *                               1: no multisampling, as a control (no grey may show)
 * First the sample counts Direct3D says the driver offers, for a window and for full screen.
 * Then the steps, one line each:
 *   target  a multisampled render target with a multisampled depth buffer, resolved to a
 *           plain surface (StretchRect) and read back
 *   scaled  the same target resolved to a surface half its size
 *   window  a window whose back buffer is multisampled, read back before it is presented
 *   screen  that window on the screen, after the present
 *   full    with "full": the same full screen, in the display's mode, read back; the picture
 *           stays for three seconds, for a look from outside
 * Each step counts the grey pixels and lists the grey levels, and checks that a red triangle
 * drawn behind the white one stays hidden, which needs the depth buffer to work.
 * Exit code 0 if every step passed, 1 if not, 3 if the driver offers no multisampling.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 480, HEIGHT = 320 };

struct vertex { float x, y, z, rhw; DWORD colour; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* The white triangle, and a red one behind it and inside it */
static HRESULT draw_scene(IDirect3DDevice9 *device, float w, float h)
{
    const struct vertex white[3] = {
        { 0.10f * w, 0.08f * h, 0.5f, 1.0f, 0xffffffff },
        { 0.92f * w, 0.25f * h, 0.5f, 1.0f, 0xffffffff },
        { 0.25f * w, 0.93f * h, 0.5f, 1.0f, 0xffffffff },
    };
    const struct vertex red[3] = {
        { 0.35f * w, 0.30f * h, 0.8f, 1.0f, 0xffff0000 },
        { 0.55f * w, 0.35f * h, 0.8f, 1.0f, 0xffff0000 },
        { 0.40f * w, 0.55f * h, 0.8f, 1.0f, 0xffff0000 },
    };
    HRESULT hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                        0xff000000, 1.0f, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_BeginScene(device);
    if (FAILED(hr)) return hr;
    IDirect3DDevice9_SetFVF(device, FVF);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    IDirect3DDevice9_SetRenderState(device, D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1, white, sizeof(white[0]));
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1, red, sizeof(red[0]));
    const HRESULT end = IDirect3DDevice9_EndScene(device);
    return FAILED(hr) ? hr : end;
}

/* What a picture of the scene holds: the grey pixels, their levels, and the pixel in the
 * middle of where the red triangle would show */
static int judge(const char *name, HRESULT hr, const BYTE *bits, int pitch, int w, int h, int samples)
{
    unsigned grey = 0, levels = 0;
    char seen[256] = { 0 }, list[64] = "";
    DWORD middle = 0x01000000;
    if (SUCCEEDED(hr) && bits) {
        for (int y = 0; y < h; ++y) {
            const DWORD *row = (const DWORD *)(bits + (size_t)y * pitch);
            for (int x = 0; x < w; ++x) {
                const unsigned r = (row[x] >> 16) & 0xff, g = (row[x] >> 8) & 0xff, b = row[x] & 0xff;
                if (r == g && g == b && g > 8 && g < 247) {
                    ++grey;
                    seen[g] = 1;
                }
            }
        }
        middle = *(const DWORD *)(bits + (size_t)(h * 40 / 100) * pitch + (size_t)(w * 43 / 100) * 4) & 0xffffff;
        for (int g = 0; g < 256; ++g) {
            if (!seen[g]) continue;
            if (levels < 9) sprintf(list + strlen(list), " %02x", g);
            ++levels;
        }
    }
    /* an edge of this length crosses some hundreds of pixels; a few would be something else */
    const int ok = SUCCEEDED(hr) && middle == 0xffffff && (samples > 1 ? grey >= 100 : grey == 0);
    printf("%-6s hr=0x%08lx %u grey pixels, %u levels:%s%s; behind the white one %06lx %s\n", name,
           (unsigned long)hr, grey, levels, list, levels > 9 ? " ..." : "", (unsigned long)middle,
           ok ? "PASS" : "FAIL");
    return ok;
}

/* A render target's picture: resolved to a plain target of the given size, then read back */
static int judge_target(const char *name, IDirect3DDevice9 *device, IDirect3DSurface9 *target,
                        int w, int h, D3DTEXTUREFILTERTYPE filter, int samples)
{
    IDirect3DSurface9 *plain = NULL, *memory = NULL;
    D3DLOCKED_RECT lock = { 0, NULL };
    HRESULT hr = IDirect3DDevice9_CreateRenderTarget(device, w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                                     FALSE, &plain, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                          &memory, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_StretchRect(device, target, NULL, plain, NULL, filter);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(device, plain, memory);
    if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(memory, &lock, NULL, D3DLOCK_READONLY);
    const int ok = judge(name, hr, lock.pBits, lock.Pitch, w, h, samples);
    if (lock.pBits) IDirect3DSurface9_UnlockRect(memory);
    if (memory) IDirect3DSurface9_Release(memory);
    if (plain) IDirect3DSurface9_Release(plain);
    return ok;
}

/* The window on the screen, as the compositor shows it */
static int judge_screen(HWND window, int samples)
{
    static DWORD pixels[WIDTH * HEIGHT];
    BITMAPINFO info = { { sizeof(BITMAPINFOHEADER), WIDTH, -HEIGHT, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    POINT corner = { 0, 0 };
    ClientToScreen(window, &corner);
    HDC screen = GetDC(NULL);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, WIDTH, HEIGHT);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, WIDTH, HEIGHT, screen, corner.x, corner.y, SRCCOPY);
    SelectObject(memory, old);
    const int rows = GetDIBits(memory, bitmap, 0, HEIGHT, pixels, &info, DIB_RGB_COLORS);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, screen);
    return judge("screen", rows == HEIGHT ? S_OK : E_FAIL, (const BYTE *)pixels, WIDTH * 4, WIDTH, HEIGHT, samples);
}

static void list_offered(IDirect3D9 *d3d)
{
    static const struct { D3DFORMAT format; const char *name; } formats[] = {
        { D3DFMT_X8R8G8B8, "X8R8G8B8" }, { D3DFMT_A8R8G8B8, "A8R8G8B8" }, { D3DFMT_D24S8, "D24S8" },
    };
    static const D3DMULTISAMPLE_TYPE types[] = {
        D3DMULTISAMPLE_NONMASKABLE, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES,
        D3DMULTISAMPLE_8_SAMPLES, D3DMULTISAMPLE_16_SAMPLES,
    };
    for (int windowed = 1; windowed >= 0; --windowed) {
        for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
            printf("offered %-11s %-8s:", windowed ? "in a window" : "full screen", formats[f].name);
            for (unsigned t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
                DWORD quality = 0;
                if (SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                                    formats[f].format, windowed, types[t],
                                                                    &quality))) {
                    if (types[t] == D3DMULTISAMPLE_NONMASKABLE)
                        printf(" quality levels %lu", (unsigned long)quality);
                    else
                        printf(" %d", (int)types[t]);
                }
            }
            printf("\n");
        }
    }
}

static int offered(IDirect3D9 *d3d, D3DFORMAT colour, BOOL windowed, int samples)
{
    return SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, colour,
                                                           windowed, (D3DMULTISAMPLE_TYPE)samples, NULL)) &&
           SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8,
                                                           windowed, (D3DMULTISAMPLE_TYPE)samples, NULL));
}

static HRESULT make_device(IDirect3D9 *d3d, HWND window, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **device)
{
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, pp, device);
    if (FAILED(hr))
        printf("       CreateDevice hr=0x%08lx (back buffer %ux%u format %d, %d samples, %s)\n",
               (unsigned long)hr, pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat,
               (int)pp->MultiSampleType, pp->Windowed ? "window" : "full screen");
    return hr;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int samples = argc > 1 ? atoi(argv[1]) : 0;
    const int full = argc > 2 && !strcmp(argv[2], "full");

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("no Direct3D 9\n"); return 2; }
    list_offered(d3d);
    if (samples == 0) {
        for (samples = 8; samples > 1 && !offered(d3d, D3DFMT_A8R8G8B8, TRUE, samples); samples /= 2) { }
        if (samples <= 1) {
            printf("the driver offers no multisampling\n");
            return 3;
        }
    }
    const D3DMULTISAMPLE_TYPE type = samples > 1 ? (D3DMULTISAMPLE_TYPE)samples : D3DMULTISAMPLE_NONE;
    printf("testing with %d sample%s a pixel\n", samples, samples > 1 ? "s" : "");

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RippleMsaa";
    RegisterClassW(&wc);
    RECT frame = { 0, 0, WIDTH, HEIGHT };
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"multisampling test",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 120, frame.right - frame.left,
                                  frame.bottom - frame.top, NULL, NULL, wc.hInstance, NULL);
    if (!window) { printf("no window\n"); return 2; }
    pump();

    int ok = 1;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;

    /* a multisampled target of its own, on a device with a plain back buffer */
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = WIDTH;
    pp.BackBufferHeight = HEIGHT;
    pp.hDeviceWindow = window;
    if (SUCCEEDED(make_device(d3d, window, &pp, &device))) {
        IDirect3DSurface9 *target = NULL, *depth = NULL;
        HRESULT hr = IDirect3DDevice9_CreateRenderTarget(device, WIDTH, HEIGHT, D3DFMT_A8R8G8B8, type, 0, FALSE,
                                                         &target, NULL);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_CreateDepthStencilSurface(device, WIDTH, HEIGHT, D3DFMT_D24S8, type, 0, TRUE,
                                                            &depth, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderTarget(device, 0, target);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetDepthStencilSurface(device, depth);
        if (SUCCEEDED(hr)) hr = draw_scene(device, WIDTH, HEIGHT);
        if (FAILED(hr)) {
            printf("target hr=0x%08lx FAIL\n", (unsigned long)hr);
            ok = 0;
        } else {
            ok &= judge_target("target", device, target, WIDTH, HEIGHT, D3DTEXF_NONE, samples);
            ok &= judge_target("scaled", device, target, WIDTH / 2, HEIGHT / 2, D3DTEXF_LINEAR, 2);
        }
        if (depth) IDirect3DSurface9_Release(depth);
        if (target) IDirect3DSurface9_Release(target);
        IDirect3DDevice9_Release(device);
        device = NULL;
    } else {
        ok = 0;
    }

    /* a window whose back buffer is multisampled, with the depth buffer Direct3D makes for it */
    const D3DFORMAT colour = samples > 1 && !offered(d3d, D3DFMT_X8R8G8B8, TRUE, samples) ? D3DFMT_A8R8G8B8 :
                                                                                           D3DFMT_X8R8G8B8;
    pp.BackBufferFormat = colour;
    pp.MultiSampleType = type;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    printf("       the window's back buffer is %s\n", colour == D3DFMT_X8R8G8B8 ? "X8R8G8B8" : "A8R8G8B8");
    if (SUCCEEDED(make_device(d3d, window, &pp, &device))) {
        IDirect3DSurface9 *back = NULL;
        HRESULT hr = S_OK;
        for (int i = 0; i < 20 && SUCCEEDED(hr); ++i) {
            hr = draw_scene(device, WIDTH, HEIGHT);
            if (SUCCEEDED(hr) && i == 19) {
                hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
                if (SUCCEEDED(hr)) {
                    ok &= judge_target("window", device, back, WIDTH, HEIGHT, D3DTEXF_NONE, samples);
                    IDirect3DSurface9_Release(back);
                }
            }
            if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
            pump();
            Sleep(30);
        }
        if (FAILED(hr)) {
            printf("window hr=0x%08lx FAIL\n", (unsigned long)hr);
            ok = 0;
        } else {
            Sleep(300);
            pump();
            ok &= judge_screen(window, samples);
        }
        IDirect3DDevice9_Release(device);
        device = NULL;
    } else {
        ok = 0;
    }

    if (full) {
        D3DDISPLAYMODE mode;
        IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
        pp.Windowed = FALSE;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth = mode.Width;
        pp.BackBufferHeight = mode.Height;
        pp.FullScreen_RefreshRateInHz = mode.RefreshRate;
        if (samples > 1 && !offered(d3d, D3DFMT_X8R8G8B8, FALSE, samples)) {
            printf("full   %d samples are not offered full screen FAIL\n", samples);
            ok = 0;
        } else if (SUCCEEDED(make_device(d3d, window, &pp, &device))) {
            HRESULT hr = S_OK;
            for (int i = 0; i < 100 && SUCCEEDED(hr); ++i) {
                hr = draw_scene(device, (float)mode.Width, (float)mode.Height);
                if (SUCCEEDED(hr) && i == 19) {
                    IDirect3DSurface9 *back = NULL;
                    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
                    if (SUCCEEDED(hr)) {
                        ok &= judge_target("full", device, back, mode.Width, mode.Height, D3DTEXF_NONE, samples);
                        IDirect3DSurface9_Release(back);
                    }
                }
                if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
                pump();
                Sleep(30);
            }
            if (FAILED(hr)) {
                printf("full   hr=0x%08lx FAIL\n", (unsigned long)hr);
                ok = 0;
            }
            IDirect3DDevice9_Release(device);
        } else {
            ok = 0;
        }
    }

    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    printf("%s\n", ok ? "all passed" : "FAILED");
    return ok ? 0 : 1;
}
