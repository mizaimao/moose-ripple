/*
 * What Direct3D 9 says about the display adapter (docs/windows/kmd-port-notes.md): whether a
 * Direct3D 9Ex object can be made at all, which is the usual test for a WDDM driver, and whether
 * the adapter has a hardware device. Writes d3d9probe.txt next to the program.
 *   d3d9probe.exe [HEX]
 * HEX: where in d3d9.dll its record of recent errors is (the offset of g_hrCapture from the
 * module's start, from the public symbols of the exact build: 16 records of 128 bytes, the
 * text at +4, the source file's name at +0x64). Direct3D 9 notes there why it switched a
 * driver off, and says so nowhere else.
 * With HEX given it also prints what the Direct3D 9 object keeps about adapter 0 (Windows 7 SP1
 * x64 layout, read from the disassembly): the formats it kept from the driver, without which it
 * has no hardware device.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

/* The object behind IDirect3D9 in Windows 7 SP1's 64-bit d3d9.dll: adapter count at 0x44, the
 * "software only" switch at 0x4f20, and per adapter (0x308 bytes each, from 0x190) the driver's
 * capabilities with the kept format list: count at +0x160, entries of 0x78 bytes at +0x168, the
 * format at +0x58 and what it may be used for at +0x60 of each.
 */
static void peek(FILE *f, const void *obj, const char *name)
{
    const unsigned char *e = (const unsigned char *)obj;
    const unsigned char *caps = e + 0x190;
    unsigned int n = *(const unsigned int *)(caps + 0x160), i;
    const unsigned char *list = *(const unsigned char *const *)(caps + 0x168);

    fprintf(f, "%s: %u adapters, software only %u, adapter 0: flags 0x%x, word at 0x1a4 0x%x, %u formats at %p\r\n",
            name, *(const unsigned int *)(e + 0x44), *(const unsigned int *)(e + 0x4f20),
            *(const unsigned int *)(caps + 0x15c), *(const unsigned int *)(caps + 0x1a4), n, (const void *)list);
    for (i = 0; i < 0x1b0; i += 4)
        fprintf(f, "%s%08x%s", i % 32 ? "" : "  ", *(const unsigned int *)(caps + i), i % 32 == 28 ? "\r\n" : " ");
    fprintf(f, "\r\n");
    if (IsBadReadPtr(list, n * 0x78))
        return;
    for (i = 0; i < n; i++)
        fprintf(f, "  format 0x%08x: 0x%08x\r\n", *(const unsigned int *)(list + i * 0x78 + 0x58),
                *(const unsigned int *)(list + i * 0x78 + 0x60));
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    FILE *f = fopen("d3d9probe.txt", "wb");
    IDirect3D9Ex *ex = NULL;
    IDirect3D9 *d3d;
    HRESULT hr;
    (void)inst; (void)prev; (void)show;

    if (!f)
        return 1;
    setvbuf(f, NULL, _IONBF, 0);   /* every line reaches the file, also if the next call crashes */
    hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &ex);
    fprintf(f, "Direct3DCreate9Ex: 0x%08lx%s\r\n", (unsigned long)hr,
            hr == D3DERR_NOTAVAILABLE ? " (not available: no WDDM driver with a Direct3D 9 part)" : "");
    fflush(f);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    fprintf(f, "Direct3DCreate9: %s\r\n", d3d ? "object made" : "failed");
    fflush(f);
    if (d3d) {
        D3DADAPTER_IDENTIFIER9 id;
        D3DCAPS9 caps;
        hr = IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id);
        fprintf(f, "adapter: 0x%08lx %s / %s\r\n", (unsigned long)hr,
                SUCCEEDED(hr) ? id.Description : "-", SUCCEEDED(hr) ? id.Driver : "-");
        fflush(f);
        hr = IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps);
        fprintf(f, "hardware device capabilities: 0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr))
            fprintf(f, ", pixel shader version 0x%08lx, DevCaps 0x%08lx", (unsigned long)caps.PixelShaderVersion,
                    (unsigned long)caps.DevCaps);
        fprintf(f, "\r\n");
        if (cmd && *cmd) {
            peek(f, d3d, "Direct3D 9 object");
            if (ex)
                peek(f, ex, "Direct3D 9Ex object");
            const char *rec = (const char *)GetModuleHandleA("d3d9.dll") + strtoul(cmd, NULL, 16);
            int i;
            for (i = 15; i >= 0; i--) {
                const char *r = rec + 128 * i;
                if (r[4])
                    fprintf(f, "d3d9 noted: %.88s (%.28s)\r\n", r + 4, r + 0x64);
            }
        }
        fprintf(f, "Direct3D 9 object released: %lu left\r\n", (unsigned long)IDirect3D9_Release(d3d));
    }
    if (ex)
        fprintf(f, "Direct3D 9Ex object released: %lu left\r\n", (unsigned long)IDirect3D9Ex_Release(ex));
    fclose(f);
    return 0;
}
