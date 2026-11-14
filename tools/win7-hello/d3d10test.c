/*
 * A minimal Direct3D 10 check for Windows (docs/windows/mesa-win7.md): a software device on a
 * given driver DLL (D3D10_DRIVER_TYPE_SOFTWARE, as Mesa's targets/d3d10umd/README.md describes),
 * a render target cleared to green, a red triangle drawn with shaders (tri.hlsl, embedded as
 * DXBC), copied to a staging texture and read back. Writes d3d10test.txt next to the program
 * and shows a message box.
 *   d3d10test.exe [DRIVER.dll]        default: libgallium_d3d10.dll
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>

#include "tri_vs.h"   /* tri.hlsl compiled to DXBC by hlsl2dxbc.c */
#include "tri_ps.h"

static char msg[2048];

static void report(void)
{
    FILE *f = fopen("d3d10test.txt", "wb");
    if (f) { fputs(msg, f); fclose(f); }
    MessageBoxA(0, msg, "moose-ripple D3D10 test", MB_OK);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    const char *dll = (cmd && *cmd) ? cmd : "libgallium_d3d10.dll";
    D3D10_TEXTURE2D_DESC td;
    D3D10_MAPPED_TEXTURE2D map;
    ID3D10Device *dev = 0;
    ID3D10Texture2D *rt = 0, *staging = 0;
    ID3D10RenderTargetView *rtv = 0;
    ID3D10VertexShader *vs = 0;
    ID3D10PixelShader *ps = 0;
    HRESULT shaders;
    const float colour[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    HMODULE sw;
    HRESULT hr;
    (void)inst; (void)prev; (void)show;

    sw = LoadLibraryA(dll);
    if (!sw) {
        snprintf(msg, sizeof(msg), "LoadLibrary(%s) failed: error %lu\r\n", dll, GetLastError());
        report();
        return 1;
    }
    hr = D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_SOFTWARE, sw, 0, D3D10_SDK_VERSION, &dev);
    if (FAILED(hr)) {
        snprintf(msg, sizeof(msg), "D3D10CreateDevice on %s failed: 0x%08lx\r\n", dll, (unsigned long)hr);
        report();
        return 1;
    }
    ZeroMemory(&td, sizeof(td));
    td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT; td.BindFlags = D3D10_BIND_RENDER_TARGET;
    hr = ID3D10Device_CreateTexture2D(dev, &td, NULL, &rt);
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateRenderTargetView(dev, (ID3D10Resource *)rt, NULL, &rtv);
    td.Usage = D3D10_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateTexture2D(dev, &td, NULL, &staging);
    if (FAILED(hr)) {
        snprintf(msg, sizeof(msg), "device made on %s, but creating textures failed: 0x%08lx\r\n",
                 dll, (unsigned long)hr);
        report();
        return 1;
    }
    ID3D10Device_ClearRenderTargetView(dev, rtv, colour);
    hr = ID3D10Device_CreateVertexShader(dev, tri_vs, sizeof(tri_vs), &vs);
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreatePixelShader(dev, tri_ps, sizeof(tri_ps), &ps);
    if (SUCCEEDED(hr)) {
        D3D10_VIEWPORT vp = { 0, 0, 64, 64, 0.0f, 1.0f };
        ID3D10Device_OMSetRenderTargets(dev, 1, &rtv, NULL);
        ID3D10Device_RSSetViewports(dev, 1, &vp);
        ID3D10Device_IASetPrimitiveTopology(dev, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D10Device_VSSetShader(dev, vs);
        ID3D10Device_PSSetShader(dev, ps);
        ID3D10Device_Draw(dev, 3, 0);
    }
    shaders = hr;
    ID3D10Device_CopyResource(dev, (ID3D10Resource *)staging, (ID3D10Resource *)rt);
    hr = ID3D10Texture2D_Map(staging, 0, D3D10_MAP_READ, 0, &map);
    if (SUCCEEDED(hr)) {
        const unsigned char *in = (const unsigned char *)map.pData + 40 * map.RowPitch + 32 * 4;
        const unsigned char *out = (const unsigned char *)map.pData + 2 * map.RowPitch + 2 * 4;
        snprintf(msg, sizeof(msg), "Direct3D 10 device on %s: created\r\n"
                 "shaders: %s (0x%08lx)\r\n"
                 "inside the triangle: %u %u %u %u (expected 255 0 0 255)\r\n"
                 "outside, the clear colour: %u %u %u %u (expected 0 255 0 255)\r\n",
                 dll, SUCCEEDED(shaders) ? "created" : "failed", (unsigned long)shaders,
                 in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
        ID3D10Texture2D_Unmap(staging, 0);
    } else {
        snprintf(msg, sizeof(msg), "Map of the staging texture failed: 0x%08lx\r\n", (unsigned long)hr);
    }
    report();
    if (vs) ID3D10VertexShader_Release(vs);
    if (ps) ID3D10PixelShader_Release(ps);
    ID3D10Texture2D_Release(staging);
    ID3D10RenderTargetView_Release(rtv);
    ID3D10Texture2D_Release(rt);
    ID3D10Device_Release(dev);
    return 0;
}
