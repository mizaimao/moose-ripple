/* Does a Direct3D 9Ex flip-model window show what the program draws? Windows' video renderer
 * presents that way under the compositor: the program draws into back buffers it shares, and
 * the compositor takes them from there itself. Three steps, each checked by reading the
 * screen where the window is:
 *   clear   the back buffer is cleared to a colour
 *   upload  a system-memory surface is filled and copied to the back buffer (UpdateSurface),
 *           as the renderer does with frames its own software processor scaled
 *   blit    a video-memory surface is filled the same way and stretched to the back buffer
 *   blitd   the same, filled under a lock that discards the old contents
 *   video   a frame goes through Windows' software video processor into a second surface,
 *           which is copied to the back buffer: what the video renderer does for every
 *           frame when the driver offers no video processor of its own
 * "d3d9flipex copy" runs the same steps with the ordinary copy-model swap chain as a control.
 * Prints one line per step; exit code 0 if every step showed its colour.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <stdio.h>
#include <string.h>

#ifndef D3DSWAPEFFECT_FLIPEX
#define D3DSWAPEFFECT_FLIPEX ((D3DSWAPEFFECT)5)
#endif

enum { WIDTH = 480, HEIGHT = 320 };

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static COLORREF screen_pixel(HWND window)
{
    POINT p = { WIDTH / 2, HEIGHT / 2 };
    ClientToScreen(window, &p);
    HDC screen = GetDC(NULL);
    COLORREF c = GetPixel(screen, p.x, p.y);
    ReleaseDC(NULL, screen);
    return c;
}

/* Present `frames` times what draw() makes, then compare the screen with rgb */
static int step(const char *name, IDirect3DDevice9Ex *device, HWND window, DWORD rgb,
                HRESULT (*draw)(IDirect3DDevice9Ex *, IDirect3DSurface9 *, DWORD))
{
    HRESULT hr = S_OK;
    for (int i = 0; i < 20 && SUCCEEDED(hr); ++i) {
        IDirect3DSurface9 *back = NULL;
        hr = IDirect3DDevice9Ex_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
        if (SUCCEEDED(hr)) {
            hr = draw(device, back, rgb);
            IDirect3DSurface9_Release(back);
        }
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_PresentEx(device, NULL, NULL, NULL, NULL, 0);
        pump();
        Sleep(30);
    }
    Sleep(300);
    pump();
    COLORREF got = screen_pixel(window);
    DWORD shown = (GetRValue(got) << 16) | (GetGValue(got) << 8) | GetBValue(got);
    int ok = SUCCEEDED(hr) && shown == rgb;
    printf("%-6s hr=0x%08lx drew %06lx screen %06lx %s\n", name, (unsigned long)hr,
           (unsigned long)rgb, (unsigned long)shown, ok ? "PASS" : "FAIL");
    return ok;
}

static HRESULT draw_clear(IDirect3DDevice9Ex *device, IDirect3DSurface9 *back, DWORD rgb)
{
    (void)back;
    return IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000000 | rgb, 1.0f, 0);
}

static DWORD fill_flags;  /* LockRect flags for the write */
static DWORD held = 0x01000000;  /* what a read of the filled video-memory surface returned */

static HRESULT fill(IDirect3DSurface9 *surface, DWORD rgb)
{
    D3DLOCKED_RECT lock;
    HRESULT hr = IDirect3DSurface9_LockRect(surface, &lock, NULL, fill_flags);
    if (FAILED(hr)) return hr;
    for (int y = 0; y < HEIGHT; ++y) {
        DWORD *row = (DWORD *)((BYTE *)lock.pBits + (size_t)y * lock.Pitch);
        for (int x = 0; x < WIDTH; ++x) row[x] = 0xff000000 | rgb;
    }
    return IDirect3DSurface9_UnlockRect(surface);
}

static HRESULT draw_upload(IDirect3DDevice9Ex *device, IDirect3DSurface9 *back, DWORD rgb)
{
    static IDirect3DSurface9 *memory;
    HRESULT hr = S_OK;
    if (!memory)
        hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, WIDTH, HEIGHT, D3DFMT_X8R8G8B8,
                                                            D3DPOOL_SYSTEMMEM, &memory, NULL);
    if (SUCCEEDED(hr)) hr = fill(memory, rgb);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9Ex_UpdateSurface(device, memory, NULL, back, NULL);
    return hr;
}

static HRESULT draw_blit(IDirect3DDevice9Ex *device, IDirect3DSurface9 *back, DWORD rgb)
{
    static IDirect3DSurface9 *video;
    HRESULT hr = S_OK;
    if (!video)
        hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, WIDTH, HEIGHT, D3DFMT_X8R8G8B8,
                                                            D3DPOOL_DEFAULT, &video, NULL);
    if (SUCCEEDED(hr)) hr = fill(video, rgb);
    D3DLOCKED_RECT lock;
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(video, &lock, NULL, D3DLOCK_READONLY))) {
        held = ((const DWORD *)((const BYTE *)lock.pBits + (size_t)(HEIGHT / 2) * lock.Pitch))[WIDTH / 2] & 0xffffff;
        IDirect3DSurface9_UnlockRect(video);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9Ex_StretchRect(device, video, NULL, back, NULL, D3DTEXF_LINEAR);
    return hr;
}

/* The video renderer's path without a decoder: both surfaces are DirectX Video Acceleration
 * "software render targets" (system memory), the processor is the software device. */
static DWORD processed = 0x01000000;  /* what the processor's output surface held */

static HRESULT draw_video(IDirect3DDevice9Ex *device, IDirect3DSurface9 *back, DWORD rgb)
{
    static IDirectXVideoProcessorService *service;
    static IDirectXVideoProcessor *processor;
    static IDirect3DSurface9 *frame, *output;
    static DXVA2_VideoDesc desc;
    HRESULT hr = S_OK;
    if (!service) {
        hr = DXVA2CreateVideoService((IDirect3DDevice9 *)device, &IID_IDirectXVideoProcessorService,
                                     (void **)&service);
        if (FAILED(hr)) { printf("       DXVA2CreateVideoService hr=0x%08lx\n", (unsigned long)hr); return hr; }
        desc.SampleWidth = WIDTH;
        desc.SampleHeight = HEIGHT;
        desc.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
        desc.Format = D3DFMT_X8R8G8B8;
        desc.InputSampleFreq.Numerator = desc.OutputFrameFreq.Numerator = 30;
        desc.InputSampleFreq.Denominator = desc.OutputFrameFreq.Denominator = 1;
        UINT count = 0;
        GUID *guids = NULL;
        hr = IDirectXVideoProcessorService_GetVideoProcessorDeviceGuids(service, &desc, &count, &guids);
        printf("       video processors offered: hr=0x%08lx count=%u\n", (unsigned long)hr, count);
        if (guids) CoTaskMemFree(guids);
        hr = IDirectXVideoProcessorService_CreateSurface(service, WIDTH, HEIGHT, 0, D3DFMT_X8R8G8B8,
                 D3DPOOL_SYSTEMMEM, 0, DXVA2_VideoSoftwareRenderTarget, &frame, NULL);
        if (SUCCEEDED(hr))
            hr = IDirectXVideoProcessorService_CreateSurface(service, WIDTH, HEIGHT, 0, D3DFMT_X8R8G8B8,
                     D3DPOOL_SYSTEMMEM, 0, DXVA2_VideoSoftwareRenderTarget, &output, NULL);
        if (FAILED(hr)) { printf("       CreateSurface hr=0x%08lx\n", (unsigned long)hr); return hr; }
        hr = IDirectXVideoProcessorService_CreateVideoProcessor(service, &DXVA2_VideoProcSoftwareDevice,
                 &desc, D3DFMT_X8R8G8B8, 0, &processor);
        if (FAILED(hr)) { printf("       CreateVideoProcessor hr=0x%08lx\n", (unsigned long)hr); return hr; }
    }
    fill_flags = D3DLOCK_DONOTWAIT;
    hr = fill(frame, rgb);
    fill_flags = 0;
    if (FAILED(hr)) return hr;

    static const RECT all = { 0, 0, WIDTH, HEIGHT };
    DXVA2_VideoProcessBltParams blt;
    memset(&blt, 0, sizeof(blt));
    blt.TargetRect = all;
    blt.BackgroundColor.Cr = blt.BackgroundColor.Cb = 0x8000;
    blt.BackgroundColor.Y = 0x1000;
    blt.BackgroundColor.Alpha = 0xffff;
    blt.DestFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
    DXVA2_ValueRange range;
    if (SUCCEEDED(IDirectXVideoProcessor_GetProcAmpRange(processor, DXVA2_ProcAmp_Brightness, &range)))
        blt.ProcAmpValues.Brightness = range.DefaultValue;
    if (SUCCEEDED(IDirectXVideoProcessor_GetProcAmpRange(processor, DXVA2_ProcAmp_Contrast, &range)))
        blt.ProcAmpValues.Contrast = range.DefaultValue;
    if (SUCCEEDED(IDirectXVideoProcessor_GetProcAmpRange(processor, DXVA2_ProcAmp_Hue, &range)))
        blt.ProcAmpValues.Hue = range.DefaultValue;
    if (SUCCEEDED(IDirectXVideoProcessor_GetProcAmpRange(processor, DXVA2_ProcAmp_Saturation, &range)))
        blt.ProcAmpValues.Saturation = range.DefaultValue;
    blt.Alpha.Value = 1;
    DXVA2_VideoSample sample;
    memset(&sample, 0, sizeof(sample));
    sample.End = 333333;
    sample.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
    sample.SrcSurface = frame;
    sample.SrcRect = sample.DstRect = all;
    sample.PlanarAlpha.Value = 1;
    hr = IDirectXVideoProcessor_VideoProcessBlt(processor, output, &blt, &sample, 1, NULL);
    if (FAILED(hr)) return hr;
    D3DLOCKED_RECT lock;
    if (SUCCEEDED(IDirect3DSurface9_LockRect(output, &lock, NULL, D3DLOCK_READONLY))) {
        processed = ((const DWORD *)((const BYTE *)lock.pBits + (size_t)(HEIGHT / 2) * lock.Pitch))[WIDTH / 2] & 0xffffff;
        IDirect3DSurface9_UnlockRect(output);
    }
    return IDirect3DDevice9Ex_UpdateSurface(device, output, NULL, back, NULL);
}

int main(int argc, char **argv)
{
    const int flip = !(argc > 1 && !strcmp(argv[1], "copy"));
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RippleFlipEx";
    RegisterClassW(&wc);
    RECT frame = { 0, 0, WIDTH, HEIGHT };
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"flip model test",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 150,
                                  frame.right - frame.left, frame.bottom - frame.top, NULL, NULL,
                                  wc.hInstance, NULL);
    if (!window) { printf("no window\n"); return 2; }
    pump();

    IDirect3D9Ex *d3d = NULL;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);
    if (FAILED(hr)) { printf("Direct3DCreate9Ex hr=0x%08lx\n", (unsigned long)hr); return 2; }
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.BackBufferWidth = WIDTH;
    pp.BackBufferHeight = HEIGHT;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = flip ? 3 : 1;
    pp.SwapEffect = flip ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_COPY;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9Ex *device = NULL;
    hr = IDirect3D9Ex_CreateDeviceEx(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                     &pp, NULL, &device);
    printf("%s model, CreateDeviceEx hr=0x%08lx\n", flip ? "flip" : "copy", (unsigned long)hr);
    if (FAILED(hr)) return 2;

    int ok = 1;
    ok &= step("clear", device, window, 0xd02020, draw_clear);
    ok &= step("upload", device, window, 0x20c040, draw_upload);
    ok &= step("blit", device, window, 0x3050e0, draw_blit);
    printf("       the filled surface read back as %06lx\n", (unsigned long)held);
    fill_flags = D3DLOCK_DISCARD;
    ok &= step("blitd", device, window, 0xe0a030, draw_blit);
    printf("       the filled surface read back as %06lx\n", (unsigned long)held);
    fill_flags = 0;
    ok &= step("video", device, window, 0x40d0d0, draw_video);
    printf("       the processor's output read back as %06lx\n", (unsigned long)processed);
    fflush(stdout);
    IDirect3DDevice9Ex_Release(device);
    IDirect3D9Ex_Release(d3d);
    DestroyWindow(window);
    return ok ? 0 : 1;
}
