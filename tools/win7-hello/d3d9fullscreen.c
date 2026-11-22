/* SPDX-License-Identifier: MIT
 * Show three two-colour fullscreen frames for 3.5 seconds each.
 * Run with (flip|discard) (immediate|sync) and sample the host display at
 * x=320 and 960, y=512. COM1 reports when each Present returns so a host
 * sampler can distinguish a draw the application accepted from a visible frame.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static const char *mode, *interval;

static void report(unsigned frame, HRESULT hr)
{
    FILE *file = fopen("COM1", "w");
    if (file) {
        fprintf(file, "W7VIEW %s-%s frame %u present %08lx\n", mode, interval, frame, (unsigned long)hr);
        fclose(file);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    mode=argv[1]; interval=argv[2];
    if ((strcmp(mode,"flip") && strcmp(mode,"discard")) || (strcmp(interval,"immediate") && strcmp(interval,"sync"))) return 2;
    const D3DCOLOR pairs[][2] = {
        {0xff1674b6, 0xfff9b324}, {0xffe34937, 0xff159463}, {0xff6230aa, 0xffb1e769}
    };
    IDirect3D9 *api = Direct3DCreate9(D3D_SDK_VERSION);
    if (!api) return 1;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "RippleFullscreenColours";
    if (!RegisterClassA(&wc)) return 2;
    HWND win = CreateWindowA(wc.lpszClassName, "Fullscreen colour check", WS_POPUP | WS_VISIBLE,
                             0, 0, 1280, 1024, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(win);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 1024;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = !strcmp(mode,"flip") ? D3DSWAPEFFECT_FLIP : D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = win;
    pp.PresentationInterval = !strcmp(interval,"immediate") ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *device = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(api, 0, D3DDEVTYPE_HAL, win,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr)) { report(0, hr); return 3; }
    D3DRECT right = {640, 0, 1280, 1024};
    for (unsigned i=0; i<3; i++) {
        hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, pairs[i][0], 1, 0);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_Clear(device, 1, &right, D3DCLEAR_TARGET, pairs[i][1], 1, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        report(i+1, hr);
        if (FAILED(hr)) break;
        DWORD start=GetTickCount();
        while (GetTickCount()-start<3500) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
            Sleep(50);
        }
    }
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(api);
    DestroyWindow(win);
    return FAILED(hr);
}
