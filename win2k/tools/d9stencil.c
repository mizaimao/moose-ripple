/* Check stencil, a 24-bit depth buffer and a back buffer with alpha on the
 * Direct3D 9 hardware device: a 128x128 A8R8G8B8 lockable back buffer with
 * a D24S8 depth-stencil buffer, transformed quads, a few pixels read back.
 * Prints D9ST lines and D9ST end ... result=PASS or FAIL. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

typedef struct { float x, y, z, rhw; DWORD color; } V;
typedef struct { int x, y; DWORD rgb; } SAMPLE;
enum { Side = 128, Clear = 0x40000066 };
static IDirect3DDevice9 *device;
static int passes, failures;
static BOOL noStencil;      /* a D16 buffer: no stencil to clear or test */

static void quad(float x0, float y0, float x1, float y1, float z, DWORD color)
{
    V v[4] = { { x0, y0, z, 1, color }, { x1, y0, z, 1, color }, { x0, y1, z, 1, color }, { x1, y1, z, 1, color } };
    IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
}

static void check(const char *name, const SAMPLE *s, int count)
{
    IDirect3DSurface9 *back = NULL;
    D3DLOCKED_RECT r;
    int i, bad = 0;
    IDirect3DDevice9_EndScene(device);
    if (FAILED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) ||
        FAILED(IDirect3DSurface9_LockRect(back, &r, NULL, D3DLOCK_READONLY))) {
        printf("D9ST case=%s lock failed\n", name);
        ++failures;
        if (back) IDirect3DSurface9_Release(back);
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
            printf("D9ST case=%s sample=%d at=%d,%d got=%06lx want=%06lx\n", name, i, s[i].x, s[i].y,
                   (unsigned long)got, (unsigned long)s[i].rgb);
            ++bad;
        }
    }
    IDirect3DSurface9_UnlockRect(back);
    IDirect3DSurface9_Release(back);
    printf("D9ST case=%s samples=%d mismatches=%d\n", name, count, bad);
    if (bad) ++failures; else ++passes;
}

static void begin(DWORD clearStencil)
{
    IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | (noStencil ? 0 : D3DCLEAR_STENCIL), Clear,
                           1.0f, clearStencil);
    IDirect3DDevice9_BeginScene(device);
}

static void stencil(BOOL on, DWORD func, DWORD ref, DWORD pass)
{
    IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, on);
    IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFUNC, func);
    IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILREF, ref);
    IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILPASS, pass);
}

int main(int argc, char **argv)
{
    IDirect3D9 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d9.dll");
    IDirect3D9 *d3d;
    D3DPRESENT_PARAMETERS pp;
    D3DCAPS9 caps;
    HWND window;
    HRESULT hr;
    /* "x8": an X8R8G8B8 back buffer; "d16": a 16-bit depth buffer (no stencil). */
    BOOL x8 = FALSE, d16 = FALSE;
    int a;
    for (a = 1; a < argc; ++a) { if (!lstrcmpiA(argv[a], "x8")) x8 = TRUE; if (!lstrcmpiA(argv[a], "d16")) d16 = TRUE; }
    setvbuf(stdout, NULL, _IONBF, 0);
    create = lib ? (IDirect3D9 *(WINAPI *)(UINT))GetProcAddress(lib, "Direct3DCreate9") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { printf("D9ST no Direct3D 9\nD9ST end result=FAIL\n"); return 1; }
    IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    printf("D9ST caps stencil=%08lx alpha_backbuffer=%08lx d24s8=%08lx\n", (unsigned long)caps.StencilCaps,
           (unsigned long)IDirect3D9_CheckDeviceType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                     D3DFMT_A8R8G8B8, TRUE),
           (unsigned long)IDirect3D9_CheckDepthStencilMatch(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                            D3DFMT_A8R8G8B8, D3DFMT_D24S8));
    window = CreateWindowA("STATIC", "d9stencil", WS_POPUP | WS_VISIBLE, 0, 0, Side, Side, NULL, NULL, NULL, NULL);
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = x8 ? D3DFMT_X8R8G8B8 : D3DFMT_A8R8G8B8;
    pp.BackBufferWidth = pp.BackBufferHeight = Side;
    pp.hDeviceWindow = window;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = d16 ? D3DFMT_D16 : D3DFMT_D24S8;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &device);
    printf("D9ST device hr=%08lx back=%s depth=%s\n", (unsigned long)hr, x8 ? "x8r8g8b8" : "a8r8g8b8", d16 ? "d16" : "d24s8");
    if (FAILED(hr)) { printf("D9ST end result=UNAVAILABLE\n"); return 2; }
    noStencil = d16;
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_ALWAYS);
    if (!d16) {   /* mark the middle with 1, then draw blue only where the stencil is 1 */
        static const SAMPLE s[] = { { 64, 64, 0x0000ff }, { 8, 8, 0x000066 }, { 120, 64, 0x000066 } };
        begin(0);
        stencil(TRUE, D3DCMP_ALWAYS, 1, D3DSTENCILOP_REPLACE);
        quad(32, 32, 96, 96, 0.5f, 0xffff0000);
        stencil(TRUE, D3DCMP_EQUAL, 1, D3DSTENCILOP_KEEP);
        quad(0, 0, Side, Side, 0.5f, 0xff0000ff);
        stencil(FALSE, D3DCMP_ALWAYS, 0, D3DSTENCILOP_KEEP);
        check("stencil_mask", s, 3);
    }
    if (!d16) {   /* a stencil clear to 1 lets the whole blue quad through */
        static const SAMPLE s[] = { { 64, 64, 0x0000ff }, { 8, 8, 0x0000ff } };
        begin(1);
        stencil(TRUE, D3DCMP_EQUAL, 1, D3DSTENCILOP_KEEP);
        quad(0, 0, Side, Side, 0.5f, 0xff0000ff);
        stencil(FALSE, D3DCMP_ALWAYS, 0, D3DSTENCILOP_KEEP);
        check("stencil_clear", s, 2);
    }
    if (!d16) {   /* two increments in the middle, then green where the stencil is 2 */
        static const SAMPLE s[] = { { 64, 64, 0x00ff00 }, { 8, 8, 0x000066 } };
        begin(0);
        stencil(TRUE, D3DCMP_ALWAYS, 0, D3DSTENCILOP_INCRSAT);
        quad(32, 32, 96, 96, 0.5f, 0xffff0000);
        quad(32, 32, 96, 96, 0.5f, 0xffff0000);
        stencil(TRUE, D3DCMP_EQUAL, 2, D3DSTENCILOP_KEEP);
        quad(0, 0, Side, Side, 0.5f, 0xff00ff00);
        stencil(FALSE, D3DCMP_ALWAYS, 0, D3DSTENCILOP_KEEP);
        check("stencil_incr", s, 2);
    }
    {   /* white scaled by the back buffer's alpha (0x40 from the clear; one without alpha) */
        SAMPLE s[2] = { { 64, 64, 0x404040 }, { 8, 8, 0x000066 } };
        if (x8) s[0].rgb = 0xffffff;
        begin(0);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_DESTALPHA);
        IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
        quad(32, 32, 96, 96, 0.5f, 0xffffffff);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
        check("dest_alpha", s, 2);
    }
    {   /* the depth test on the 24-bit buffer: the far quad stays behind */
        static const SAMPLE s[] = { { 64, 64, 0xff0000 }, { 8, 8, 0x000066 } };
        begin(0);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESS);
        quad(32, 32, 96, 96, 0.25f, 0xffff0000);
        quad(32, 32, 96, 96, 0.75f, 0xff00ff00);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_ALWAYS);
        check("depth24", s, 2);
    }
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    printf("D9ST end pass=%d fail=%d result=%s\n", passes, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
