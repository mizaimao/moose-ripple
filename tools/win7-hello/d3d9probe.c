/*
 * What Direct3D 9 says about the display adapter (docs/windows/kmd-port-notes.md): whether a
 * Direct3D 9Ex object can be made at all, which is the usual test for a WDDM driver and the one
 * the Desktop Window Manager seems to apply, and what the adapter reports. Writes
 * d3d9probe.txt next to the program.   d3d9probe.exe
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    FILE *f = fopen("d3d9probe.txt", "wb");
    IDirect3D9Ex *ex = NULL;
    IDirect3D9 *d3d;
    HRESULT hr;
    (void)inst; (void)prev; (void)cmd; (void)show;

    if (!f)
        return 1;
    hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &ex);
    fprintf(f, "Direct3DCreate9Ex: 0x%08lx%s\r\n", (unsigned long)hr,
            hr == D3DERR_NOTAVAILABLE ? " (not available: no WDDM driver with a Direct3D 9 part)" : "");
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    fprintf(f, "Direct3DCreate9: %s\r\n", d3d ? "object made" : "failed");
    if (d3d) {
        D3DADAPTER_IDENTIFIER9 id;
        D3DCAPS9 caps;
        hr = IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id);
        fprintf(f, "adapter: 0x%08lx %s / %s\r\n", (unsigned long)hr,
                SUCCEEDED(hr) ? id.Description : "-", SUCCEEDED(hr) ? id.Driver : "-");
        hr = IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps);
        fprintf(f, "hardware device capabilities: 0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr))
            fprintf(f, ", pixel shader version 0x%08lx, DevCaps 0x%08lx", (unsigned long)caps.PixelShaderVersion,
                    (unsigned long)caps.DevCaps);
        fprintf(f, "\r\n");
        IDirect3D9_Release(d3d);
    }
    if (ex)
        IDirect3D9Ex_Release(ex);
    fclose(f);
    return 0;
}
