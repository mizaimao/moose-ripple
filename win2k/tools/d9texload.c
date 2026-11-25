/* Time how long a Direct3D 9 program takes to bring compressed textures
 * into use: create managed DXT1 and DXT5 textures of 512x512 with every
 * mipmap level, fill them with varied blocks, then draw a quad with each
 * once and read the back buffer back (which waits for the draws).
 *
 *   d9texload [count]
 *
 * Prints D9LOAD format=... textures=... ms=... for each format. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { float x, y, z, rhw; float u, v; } V;

static IDirect3DDevice9 *device;

static void fill(IDirect3DTexture9 *t, D3DFORMAT format, DWORD seed)
{
    DWORD level, levels = IDirect3DTexture9_GetLevelCount(t);
    for (level = 0; level < levels; ++level) {
        D3DSURFACE_DESC desc;
        D3DLOCKED_RECT r;
        DWORD y, x, blockBytes = format == D3DFMT_DXT1 ? 8 : 16;
        IDirect3DTexture9_GetLevelDesc(t, level, &desc);
        if (FAILED(IDirect3DTexture9_LockRect(t, level, &r, NULL, 0))) continue;
        for (y = 0; y < (desc.Height + 3) / 4; ++y) {
            DWORD *row = (DWORD *)((BYTE *)r.pBits + y * r.Pitch);
            for (x = 0; x < (desc.Width + 3) / 4 * blockBytes / 4; ++x) {
                seed = seed * 1664525 + 1013904223;
                row[x] = seed;
            }
        }
        IDirect3DTexture9_UnlockRect(t, level);
    }
}

static void run(D3DFORMAT format, const char *name, int count)
{
    IDirect3DTexture9 **t = calloc(count, sizeof(*t));
    IDirect3DSurface9 *back = NULL;
    D3DLOCKED_RECT r;
    LARGE_INTEGER f, a, b;
    V v[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 64, 0, 0.5f, 1, 1, 0 }, { 0, 64, 0.5f, 1, 0, 1 }, { 64, 64, 0.5f, 1, 1, 1 } };
    int i, made = 0;
    for (i = 0; i < count; ++i)
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(device, 512, 512, 0, 0, format, D3DPOOL_MANAGED, &t[i], NULL))) {
            fill(t[i], format, 12345 + i);
            ++made;
        }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    IDirect3DDevice9_BeginScene(device);
    for (i = 0; i < count; ++i) {
        if (!t[i]) continue;
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t[i]);
        IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
    }
    IDirect3DDevice9_EndScene(device);
    IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (back && SUCCEEDED(IDirect3DSurface9_LockRect(back, &r, NULL, D3DLOCK_READONLY))) IDirect3DSurface9_UnlockRect(back);
    QueryPerformanceCounter(&b);
    printf("D9LOAD format=%s textures=%d ms=%lu\n", name, made, (unsigned long)((b.QuadPart - a.QuadPart) * 1000 / f.QuadPart));
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    if (back) IDirect3DSurface9_Release(back);
    for (i = 0; i < count; ++i) if (t[i]) IDirect3DTexture9_Release(t[i]);
    free(t);
}

int main(int argc, char **argv)
{
    IDirect3D9 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d9.dll");
    IDirect3D9 *d3d;
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    HWND window;
    int count = argc > 1 ? atoi(argv[1]) : 32;
    setvbuf(stdout, NULL, _IONBF, 0);
    create = lib ? (IDirect3D9 *(WINAPI *)(UINT))GetProcAddress(lib, "Direct3DCreate9") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { printf("D9LOAD no Direct3D 9\n"); return 1; }
    IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    window = CreateWindowA("STATIC", "d9texload", WS_POPUP | WS_VISIBLE, 0, 0, 128, 128, NULL, NULL, NULL, NULL);
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = pp.BackBufferHeight = 128;
    pp.hDeviceWindow = window;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                       &pp, &device))) { printf("D9LOAD no device\n"); return 1; }
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    run(D3DFMT_DXT1, "dxt1", count);
    run(D3DFMT_DXT5, "dxt5", count);
    run(D3DFMT_DXT1, "dxt1", count);
    run(D3DFMT_DXT5, "dxt5", count);
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    return 0;
}
