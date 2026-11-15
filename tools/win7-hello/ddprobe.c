/*
 * What DirectDraw and the Direct3D of DirectX 7 say about the display adapter's driver
 * (docs/windows/mesa-win7.md): the hardware capabilities DirectDraw reports, which is what
 * DirectX's diagnostics call "Direct3D acceleration", and the Direct3D 7 devices it lists.
 * On Windows 7 both come from the Direct3D 9 user-mode driver, asked with older questions.
 * Writes ddprobe.txt next to the program.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>

static FILE *f;

static HRESULT CALLBACK device(LPSTR description, LPSTR name, LPD3DDEVICEDESC7 desc, LPVOID context)
{
    (void)context;
    fprintf(f, "  Direct3D 7 device: %s (%s), device caps 0x%08lx, %lu texture stages\r\n", name, description,
            (unsigned long)desc->dwDevCaps, (unsigned long)desc->wMaxTextureBlendStages);
    return D3DENUMRET_OK;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    IDirectDraw7 *dd = NULL;
    IDirect3D7 *d3d = NULL;
    DDCAPS hal, hel;
    HRESULT hr;
    (void)inst; (void)prev; (void)cmd; (void)show;

    f = fopen("ddprobe.txt", "wb");
    if (!f)
        return 1;
    setvbuf(f, NULL, _IONBF, 0);   /* every line reaches the file, also if the next call crashes */
    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    fprintf(f, "DirectDrawCreateEx: 0x%08lx\r\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    ZeroMemory(&hal, sizeof(hal)); hal.dwSize = sizeof(hal);
    ZeroMemory(&hel, sizeof(hel)); hel.dwSize = sizeof(hel);
    hr = IDirectDraw7_GetCaps(dd, &hal, &hel);
    fprintf(f, "capabilities: 0x%08lx\r\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        fprintf(f, "  hardware: caps 0x%08lx, caps2 0x%08lx, surface caps 0x%08lx, video memory %lu MB\r\n",
                (unsigned long)hal.dwCaps, (unsigned long)hal.dwCaps2, (unsigned long)hal.ddsCaps.dwCaps,
                (unsigned long)(hal.dwVidMemTotal >> 20));
        fprintf(f, "  emulation: caps 0x%08lx, caps2 0x%08lx\r\n", (unsigned long)hel.dwCaps,
                (unsigned long)hel.dwCaps2);
        fprintf(f, "  3D in hardware (DDCAPS_3D): %s\r\n", (hal.dwCaps & DDCAPS_3D) ? "yes" : "NO");
    }
    hr = IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d);
    fprintf(f, "Direct3D 7 interface: 0x%08lx\r\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = IDirect3D7_EnumDevices(d3d, device, NULL);
        fprintf(f, "device list: 0x%08lx\r\n", (unsigned long)hr);
        IDirect3D7_Release(d3d);
    }
    IDirectDraw7_Release(dd);
    fclose(f);
    return 0;
}
