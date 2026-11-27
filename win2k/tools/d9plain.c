/* Check offscreen plain surfaces of the default pool (video memory) on Direct3D 9: made, filled
 * with ColorFill, written through a lock, updated from a system-memory surface, and copied
 * into the back buffer with StretchRect. A 128x128 lockable back buffer in a window; "ref"
 * runs the same on the reference rasterizer (it needs Microsoft's d3dref9.dll).
 * Prints D9PL lines and D9PL end ... result=PASS or FAIL. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

typedef struct { int x, y; DWORD rgb; } SAMPLE;
enum { Side = 128, Plain = 64, Clear = 0xff000066 };
static IDirect3DDevice9 *device;
static IDirect3DSurface9 *back;
static int passes, failures;

/* Samples of a surface read through a lock; the case passes when all are within 2 of their colours. */
static void check(const char *name, IDirect3DSurface9 *surface, const SAMPLE *s, int count)
{
    D3DLOCKED_RECT r;
    int i, bad = 0;
    HRESULT hr = IDirect3DSurface9_LockRect(surface, &r, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        printf("D9PL case=%s lock hr=%08lx\n", name, (unsigned long)hr);
        ++failures;
        return;
    }
    for (i = 0; i < count; ++i) {
        DWORD got = ((const DWORD *)((const BYTE *)r.pBits + s[i].y * r.Pitch))[s[i].x] & 0xffffff;
        int k, off = 0;
        for (k = 0; k < 24; k += 8) {
            int a = (got >> k) & 255, b = (s[i].rgb >> k) & 255;
            if (a - b > 2 || b - a > 2) off = 1;
        }
        if (off) {
            printf("D9PL case=%s sample=%d at=%d,%d got=%06lx want=%06lx\n", name, i, s[i].x, s[i].y,
                   (unsigned long)got, (unsigned long)s[i].rgb);
            ++bad;
        }
    }
    IDirect3DSurface9_UnlockRect(surface);
    printf("D9PL case=%s samples=%d mismatches=%d\n", name, count, bad);
    if (bad) ++failures; else ++passes;
}

/* A call that must succeed for its case; FALSE (and a failure counted) when it did not. */
static BOOL done(const char *name, const char *what, HRESULT hr)
{
    if (SUCCEEDED(hr)) return TRUE;
    printf("D9PL case=%s %s hr=%08lx\n", name, what, (unsigned long)hr);
    ++failures;
    return FALSE;
}

/* Two colours into a surface through a lock: a above (or left of, with sideways) b. */
static HRESULT halves(IDirect3DSurface9 *surface, DWORD a, DWORD b, BOOL sideways)
{
    D3DLOCKED_RECT r;
    int x, y;
    HRESULT hr = IDirect3DSurface9_LockRect(surface, &r, NULL, 0);
    if (FAILED(hr)) return hr;
    for (y = 0; y < Plain; ++y)
        for (x = 0; x < Plain; ++x)
            ((DWORD *)((BYTE *)r.pBits + y * r.Pitch))[x] = (sideways ? x : y) < Plain / 2 ? a : b;
    return IDirect3DSurface9_UnlockRect(surface);
}

int main(int argc, char **argv)
{
    IDirect3D9 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d9.dll");
    IDirect3D9 *d3d;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DSurface9 *plain = NULL, *memory = NULL, *alpha = NULL;
    HWND window;
    HRESULT hr;
    D3DDEVTYPE type = argc > 1 && !lstrcmpiA(argv[1], "ref") ? D3DDEVTYPE_REF : D3DDEVTYPE_HAL;
    RECT middle = { 32, 32, 96, 96 }, left = { 0, 0, Plain / 2, Plain };
    setvbuf(stdout, NULL, _IONBF, 0);
    create = lib ? (IDirect3D9 *(WINAPI *)(UINT))GetProcAddress(lib, "Direct3DCreate9") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { printf("D9PL no Direct3D 9\nD9PL end result=FAIL\n"); return 1; }
    window = CreateWindowA("STATIC", "d9plain", WS_POPUP | WS_VISIBLE, 0, 0, Side, Side, NULL, NULL, NULL, NULL);
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = pp.BackBufferHeight = Side;
    pp.hDeviceWindow = window;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, type, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    printf("D9PL device=%s hr=%08lx\n", type == D3DDEVTYPE_REF ? "reference" : "hardware", (unsigned long)hr);
    if (FAILED(hr)) { printf("D9PL end result=UNAVAILABLE\n"); return 2; }
    if (FAILED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) {
        printf("D9PL no back buffer\nD9PL end result=FAIL\n");
        return 1;
    }
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, Plain, Plain, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &plain, NULL);
    if (!done("create", "default pool X8R8G8B8", hr)) {
        printf("D9PL end pass=%d fail=%d result=FAIL\n", passes, failures);
        return 1;
    }
    ++passes;
    printf("D9PL case=create made\n");
    /* an A8R8G8B8 one can be made too */
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, Plain, Plain, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &alpha, NULL);
    if (done("create_alpha", "default pool A8R8G8B8", hr)) { ++passes; printf("D9PL case=create_alpha made\n"); }
    {   /* filled red, copied into the middle of the back buffer */
        static const SAMPLE s[] = { { 64, 64, 0xff0000 }, { 8, 8, 0x000066 }, { 100, 64, 0x000066 } };
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, Clear, 1.0f, 0);
        if (done("fill_stretch", "ColorFill", IDirect3DDevice9_ColorFill(device, plain, NULL, 0xffff0000)) &&
            done("fill_stretch", "StretchRect", IDirect3DDevice9_StretchRect(device, plain, NULL, back, &middle, D3DTEXF_NONE)))
            check("fill_stretch", back, s, 3);
    }
    {   /* a rectangle of it filled blue: read through a lock */
        static const SAMPLE s[] = { { 16, 32, 0x0000ff }, { 48, 32, 0xff0000 } };
        if (done("fill_rect", "ColorFill", IDirect3DDevice9_ColorFill(device, plain, &left, 0xff0000ff)))
            check("fill_rect", plain, s, 2);
    }
    {   /* written through a lock, green left and blue right, copied into the middle */
        static const SAMPLE s[] = { { 48, 64, 0x00ff00 }, { 80, 64, 0x0000ff }, { 8, 8, 0x000066 } };
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, Clear, 1.0f, 0);
        if (done("lock_stretch", "lock", halves(plain, 0xff00ff00, 0xff0000ff, TRUE)) &&
            done("lock_stretch", "StretchRect", IDirect3DDevice9_StretchRect(device, plain, NULL, back, &middle, D3DTEXF_NONE)))
            check("lock_stretch", back, s, 3);
    }
    {   /* updated from a system-memory surface, yellow above magenta */
        static const SAMPLE s[] = { { 64, 48, 0xffff00 }, { 64, 80, 0xff00ff }, { 8, 8, 0x000066 } };
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, Clear, 1.0f, 0);
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, Plain, Plain, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &memory, NULL);
        if (done("update_surface", "system memory surface", hr) &&
            done("update_surface", "lock", halves(memory, 0xffffff00, 0xffff00ff, FALSE)) &&
            done("update_surface", "UpdateSurface", IDirect3DDevice9_UpdateSurface(device, memory, NULL, plain, NULL)) &&
            done("update_surface", "StretchRect", IDirect3DDevice9_StretchRect(device, plain, NULL, back, &middle, D3DTEXF_NONE)))
            check("update_surface", back, s, 3);
    }
    if (memory) IDirect3DSurface9_Release(memory);
    if (alpha) IDirect3DSurface9_Release(alpha);
    IDirect3DSurface9_Release(plain);
    IDirect3DSurface9_Release(back);
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    printf("D9PL end pass=%d fail=%d result=%s\n", passes, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
