/* Exclusive D3D9 scanout regression. Build with llvm-mingw and -ld3d9 -luser32.
 * Sample the VM display at one-quarter and three-quarters of its width while
 * each line is printed. A successful Present alone cannot verify scanout. MIT. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static void pump_messages(DWORD duration)
{
    DWORD start = GetTickCount();
    MSG message;
    do {
        while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        Sleep(10);
    } while (GetTickCount() - start < duration);
}

static HRESULT show_patterns(IDirect3D9 *d3d, HWND window, const D3DDISPLAYMODE *mode,
                             D3DSWAPEFFECT effect, UINT interval)
{
    static const DWORD patterns[][2] = {{0xff1866b2, 0xffe9c344}, {0xffd33255, 0xff20ac80}};
    IDirect3DDevice9 *device = NULL;
    D3DRECT half = {(LONG)mode->Width / 2, 0, (LONG)mode->Width, (LONG)mode->Height};
    D3DPRESENT_PARAMETERS p = {0};
    p.BackBufferWidth = mode->Width;
    p.BackBufferHeight = mode->Height;
    p.BackBufferFormat = mode->Format;
    p.BackBufferCount = 1;
    p.SwapEffect = effect;
    p.hDeviceWindow = window;
    p.PresentationInterval = interval;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                          D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &device);
    if (FAILED(hr))
        return hr;
    for (unsigned step = 0; step < 2 && SUCCEEDED(hr); ++step) {
        for (unsigned frame = 0; frame < 38 && SUCCEEDED(hr); ++frame) {
            hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, patterns[step][0], 1, 0);
            if (SUCCEEDED(hr))
                hr = IDirect3DDevice9_Clear(device, 1, &half, D3DCLEAR_TARGET, patterns[step][1], 1, 0);
            if (SUCCEEDED(hr))
                hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
            if (frame == 5 && SUCCEEDED(hr))
                printf("%s %s left=%06lx right=%06lx\n",
                       effect == D3DSWAPEFFECT_FLIP ? "flip" : "discard",
                       interval == D3DPRESENT_INTERVAL_ONE ? "vsync" : "immediate",
                       patterns[step][0] & 0xffffff, patterns[step][1] & 0xffffff);
            pump_messages(80);
        }
    }
    IDirect3DDevice9_Release(device);
    return hr;
}

int main(void)
{
    static const D3DSWAPEFFECT effects[] = {D3DSWAPEFFECT_DISCARD, D3DSWAPEFFECT_FLIP};
    static const UINT intervals[] = {D3DPRESENT_INTERVAL_IMMEDIATE, D3DPRESENT_INTERVAL_ONE};
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND window = CreateWindowA("STATIC", "Vista fullscreen present", WS_POPUP | WS_VISIBLE,
                                0, 0, 640, 480, NULL, NULL, GetModuleHandleA(NULL), NULL);
    D3DDISPLAYMODE mode;
    HRESULT hr = E_FAIL;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (d3d && window) {
        SetForegroundWindow(window);
        hr = IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
        for (unsigned i = 0; i < 2 && SUCCEEDED(hr); ++i)
            for (unsigned j = 0; j < 2 && SUCCEEDED(hr); ++j)
                hr = show_patterns(d3d, window, &mode, effects[j], intervals[i]);
    }
    if (window) DestroyWindow(window);
    if (d3d) IDirect3D9_Release(d3d);
    printf("Present result %08lx\n", (unsigned long)hr);
    return FAILED(hr);
}
