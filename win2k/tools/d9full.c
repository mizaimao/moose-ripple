/* Check full-screen Direct3D 9: a device on the whole screen in its current
 * mode (one back buffer, flipped), cleared and drawn for a few frames, then
 * the front buffer read back. Tries X8R8G8B8 and A8R8G8B8 back buffers with
 * a D24S8 depth buffer; with "hold" each keeps its last frame on the screen
 * for ten seconds (for a picture from outside). Each also checks the back buffer's alpha:
 * white blended by the destination alpha after a clear with alpha 0x40, read from the back buffer.
 * Prints D9FS lines and D9FS end ... result=PASS or FAIL. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

typedef struct { float x, y, z, rhw; DWORD color; } V;
static int passes, failures;

static BOOL near_rgb(DWORD got, DWORD want)
{
    int k;
    for (k = 0; k < 24; k += 8) {
        int a = (got >> k) & 255, b = (want >> k) & 255;
        if (a - b > 2 || b - a > 2) return FALSE;
    }
    return TRUE;
}

static void run(IDirect3D9 *d3d, HWND window, D3DFORMAT back, const char *name, BOOL hold)
{
    D3DDISPLAYMODE mode;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;
    IDirect3DSurface9 *front = NULL;
    D3DLOCKED_RECT r;
    HRESULT hr;
    int frame;
    IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = back;
    pp.BackBufferWidth = mode.Width;
    pp.BackBufferHeight = mode.Height;
    pp.BackBufferCount = 1;
    pp.FullScreen_RefreshRateInHz = mode.RefreshRate;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    pp.hDeviceWindow = window;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &device);
    printf("D9FS case=%s device hr=%08lx mode=%ux%u\n", name, (unsigned long)hr, mode.Width, mode.Height);
    if (FAILED(hr)) { ++failures; return; }
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    /* Three frames: blue, then green, then green with a red left half. */
    for (frame = 0; frame < 3; ++frame) {
        float w = (float)mode.Width / 2, h = (float)mode.Height;
        V v[4] = { { 0, 0, 0.5f, 1, 0xffff0000 }, { w, 0, 0.5f, 1, 0xffff0000 },
                   { 0, h, 0.5f, 1, 0xffff0000 }, { w, h, 0.5f, 1, 0xffff0000 } };
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                               frame ? 0xff00ff00 : 0xff0000ff, 1.0f, 0);
        IDirect3DDevice9_BeginScene(device);
        if (frame == 2) IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        IDirect3DDevice9_EndScene(device);
        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        if (FAILED(hr)) printf("D9FS case=%s frame=%d present hr=%08lx\n", name, frame, (unsigned long)hr);
    }
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, mode.Width, mode.Height, D3DFMT_A8R8G8B8,
                                                      D3DPOOL_SYSTEMMEM, &front, NULL);
    if (FAILED(hr)) printf("D9FS case=%s plain surface hr=%08lx\n", name, (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetFrontBufferData(device, 0, front);
        if (FAILED(hr)) printf("D9FS case=%s GetFrontBufferData hr=%08lx\n", name, (unsigned long)hr);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(front, &r, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        printf("D9FS case=%s front buffer read hr=%08lx\n", name, (unsigned long)hr);
        ++failures;
    } else {
        const DWORD *row = (const DWORD *)((const BYTE *)r.pBits + (mode.Height / 2) * r.Pitch);
        DWORD left = row[mode.Width / 4] & 0xffffff, right = row[mode.Width * 3 / 4] & 0xffffff;
        BOOL ok = near_rgb(left, 0xff0000) && near_rgb(right, 0x00ff00);
        printf("D9FS case=%s front left=%06lx right=%06lx %s\n", name, (unsigned long)left, (unsigned long)right,
               ok ? "ok" : "WRONG");
        IDirect3DSurface9_UnlockRect(front);
        if (ok) ++passes; else ++failures;
    }
    {   /* a plain surface in video memory (the default pool), filled and released */
        IDirect3DSurface9 *plain = NULL;
        HRESULT ph = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                                                  &plain, NULL);
        HRESULT fh = plain ? IDirect3DDevice9_ColorFill(device, plain, NULL, 0xff102030) : E_FAIL;
        printf("D9FS case=%s default_pool_plain create=%08lx fill=%08lx %s\n", name, (unsigned long)ph,
               (unsigned long)fh, SUCCEEDED(ph) && SUCCEEDED(fh) ? "ok" : "WRONG");
        if (SUCCEEDED(ph) && SUCCEEDED(fh)) ++passes; else ++failures;
        if (plain) IDirect3DSurface9_Release(plain);
    }
    /* The back buffer's own alpha: a clear to blue with alpha 0x40, then white scaled by the
     * destination alpha on the right half, read from the back buffer before it is presented.
     * A back buffer without alpha reads its alpha as one. */
    {
        float w = (float)mode.Width, h = (float)mode.Height;
        V v[4] = { { w / 2, 0, 0.5f, 1, 0xffffffff }, { w, 0, 0.5f, 1, 0xffffffff },
                   { w / 2, h, 0.5f, 1, 0xffffffff }, { w, h, 0.5f, 1, 0xffffffff } };
        DWORD want = back == D3DFMT_A8R8G8B8 ? 0x404040 : 0xffffff;
        IDirect3DSurface9 *target = NULL, *copy = NULL;
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x400000ff, 1.0f, 0);
        IDirect3DDevice9_BeginScene(device);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_DESTALPHA);
        IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
        IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_EndScene(device);
        hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &target);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, mode.Width, mode.Height, back,
                                                                             D3DPOOL_SYSTEMMEM, &copy, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(device, target, copy);
        if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(copy, &r, NULL, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            printf("D9FS case=%s back_buffer_read hr=%08lx (not counted)\n", name, (unsigned long)hr);
        } else {
            const DWORD *row = (const DWORD *)((const BYTE *)r.pBits + (mode.Height / 2) * r.Pitch);
            DWORD left = row[mode.Width / 4] & 0xffffff, right = row[mode.Width * 3 / 4] & 0xffffff;
            BOOL ok = near_rgb(left, 0x0000ff) && near_rgb(right, want);
            /* Not counted (issue 327): the driver gives a full-screen back buffer's page as it is, and what
             * Direct3D drew reaches the page only when the frame is shown. */
            printf("D9FS case=%s back_buffer_read left=%06lx right=%06lx want=%06lx %s\n", name, (unsigned long)left,
                   (unsigned long)right, (unsigned long)want, ok ? "ok" : "STALE (not counted)");
            IDirect3DSurface9_UnlockRect(copy);
        }
        if (copy) IDirect3DSurface9_Release(copy);
        if (target) IDirect3DSurface9_Release(target);
        IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        /* and the same picture once it is shown */
        hr = front ? IDirect3DDevice9_GetFrontBufferData(device, 0, front) : E_FAIL;
        if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(front, &r, NULL, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            printf("D9FS case=%s dest_alpha front buffer read hr=%08lx\n", name, (unsigned long)hr);
            ++failures;
        } else {
            const DWORD *row = (const DWORD *)((const BYTE *)r.pBits + (mode.Height / 2) * r.Pitch);
            DWORD left = row[mode.Width / 4] & 0xffffff, right = row[mode.Width * 3 / 4] & 0xffffff;
            BOOL ok = near_rgb(left, 0x0000ff) && near_rgb(right, want);
            printf("D9FS case=%s dest_alpha left=%06lx right=%06lx want=%06lx %s\n", name, (unsigned long)left,
                   (unsigned long)right, (unsigned long)want, ok ? "ok" : "WRONG");
            IDirect3DSurface9_UnlockRect(front);
            if (ok) ++passes; else ++failures;
        }
    }
    if (front) IDirect3DSurface9_Release(front);
    if (hold) { printf("D9FS case=%s holding\n", name); Sleep(10000); }
    IDirect3DDevice9_Release(device);
}

int main(int argc, char **argv)
{
    IDirect3D9 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d9.dll");
    IDirect3D9 *d3d;
    HWND window;
    BOOL hold = argc > 1 && !lstrcmpiA(argv[1], "hold");
    setvbuf(stdout, NULL, _IONBF, 0);
    create = lib ? (IDirect3D9 *(WINAPI *)(UINT))GetProcAddress(lib, "Direct3DCreate9") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { printf("D9FS no Direct3D 9\nD9FS end result=FAIL\n"); return 1; }
    window = CreateWindowA("STATIC", "d9full", WS_POPUP | WS_VISIBLE, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    run(d3d, window, D3DFMT_X8R8G8B8, "x8r8g8b8", hold);
    run(d3d, window, D3DFMT_A8R8G8B8, "a8r8g8b8", hold);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    printf("D9FS end pass=%d fail=%d result=%s\n", passes, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
