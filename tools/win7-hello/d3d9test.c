/*
 * A first Direct3D 9 check on the display adapter's driver (docs/windows/kmd-port-notes.md): a
 * hardware device in a window, the back buffer cleared to green, a red triangle drawn from
 * vertices already in screen coordinates, the picture read back and compared, then presented.
 * The window stays up for some seconds so the picture can be looked at.
 * Writes d3d9test.txt next to the program.
 *   d3d9test.exe [SECONDS]      how long the window stays (default 4)
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

struct vertex { float x, y, z, rhw; DWORD colour; };

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    static const struct vertex corners[3] = {   /* clockwise: the front side */
        { 128.0f, 32.0f, 0.5f, 1.0f, 0xffff0000 },
        { 224.0f, 224.0f, 0.5f, 1.0f, 0xffff0000 },
        { 32.0f, 224.0f, 0.5f, 1.0f, 0xffff0000 },
    };
    FILE *f = fopen("d3d9test.txt", "wb");
    int seconds = (cmd && *cmd) ? atoi(cmd) : 4;
    WNDCLASSA wc;
    HWND window;
    RECT frame = { 0, 0, 256, 256 };
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *target = NULL, *copy = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DLOCKED_RECT lock;
    HRESULT hr;
    (void)prev; (void)show;

    if (!f)
        return 1;
    setvbuf(f, NULL, _IONBF, 0);   /* every line reaches the file, also if the next call crashes */

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = "moose-ripple-d3d9test";
    RegisterClassA(&wc);
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowA(wc.lpszClassName, "moose-ripple Direct3D 9 test", WS_OVERLAPPEDWINDOW, 120, 120,
                           frame.right - frame.left, frame.bottom - frame.top, NULL, NULL, inst, NULL);
    ShowWindow(window, SW_SHOWNORMAL);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(f, "Direct3DCreate9 failed\r\n");
        return 1;
    }
    ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    pp.hDeviceWindow = window;
    hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    fprintf(f, "hardware device, vertices processed by the driver: 0x%08lx\r\n", (unsigned long)hr);
    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        fprintf(f, "hardware device, vertices processed by Direct3D: 0x%08lx\r\n", (unsigned long)hr);
    }
    if (FAILED(hr))
        return 1;

    hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0);
    fprintf(f, "clear: 0x%08lx\r\n", (unsigned long)hr);
    hr = IDirect3DDevice9_BeginScene(dev);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, corners, sizeof(corners[0]));
    fprintf(f, "triangle: 0x%08lx\r\n", (unsigned long)hr);
    IDirect3DDevice9_EndScene(dev);

    hr = IDirect3DDevice9_GetRenderTarget(dev, 0, &target);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 256, 256, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTargetData(dev, target, copy);
    if (SUCCEEDED(hr))
        hr = IDirect3DSurface9_LockRect(copy, &lock, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        const unsigned char *in = (const unsigned char *)lock.pBits + 160 * lock.Pitch + 128 * 4;
        const unsigned char *out = (const unsigned char *)lock.pBits + 4 * lock.Pitch + 4 * 4;
        fprintf(f, "inside the triangle (blue green red): %u %u %u (expected 0 0 255)\r\n", in[0], in[1], in[2]);
        fprintf(f, "outside, the clear colour: %u %u %u (expected 0 255 0)\r\n", out[0], out[1], out[2]);
        fprintf(f, in[0] == 0 && in[1] == 0 && in[2] == 255 && out[0] == 0 && out[1] == 255 && out[2] == 0 ?
                "picture read back: right\r\n" : "picture read back: WRONG\r\n");
        IDirect3DSurface9_UnlockRect(copy);
    } else {
        fprintf(f, "reading the picture back failed: 0x%08lx\r\n", (unsigned long)hr);
    }

    hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    fprintf(f, "present: 0x%08lx\r\n", (unsigned long)hr);
    Sleep(seconds * 1000);

    if (copy) IDirect3DSurface9_Release(copy);
    if (target) IDirect3DSurface9_Release(target);
    fprintf(f, "device released: %lu left\r\n", (unsigned long)IDirect3DDevice9_Release(dev));
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    fclose(f);
    return 0;
}
