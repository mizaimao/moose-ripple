/*
 * d3dtest: check a display driver's Direct3D 7 HAL from a program, the way a game uses it.
 *
 *   d3dtest [width height bpp [seconds]]       default: 640 480 32, 3 seconds
 *
 * Lists the Direct3D devices and the HAL device's capabilities, then (full-screen, exclusive)
 * makes a flipping chain with a z-buffer, creates the HAL device and draws a frame with known
 * colours: a clear, a Gouraud-shaded triangle, a pair of z-tested quads and a textured quad. It
 * reads pixels of the back buffer back and compares them with what must be there, then spins
 * the scene for the given time and prints the frame rate. Output on standard output and
 * QEMU's debug port. Exit status 0 when every check passed.
 *
 * Built by tools/win9x/build-tools.sh with the DirectX 7 SDK's headers and libraries (not in
 * this repository).
 */
#include <windows.h>
#include <math.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int failures;

static int say(const char *fmt, ...)
{
    char buf[512];
    char *p;
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p; p++) {
        char c = *p;
        __asm {
            mov al, c
            out 0E9h, al
        }
    }
    fputs(buf, stdout);
    return n;
}
#define printf say

static void check(int ok, const char *what)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "WRONG");
    if (!ok)
        failures++;
}

static HRESULT CALLBACK enum_device(LPSTR desc, LPSTR name, LPD3DDEVICEDESC7 d, LPVOID ctx)
{
    printf("device: %s (%s)%s\n", name, desc,
           (d->dwDevCaps & D3DDEVCAPS_HWRASTERIZATION) ? ", hardware rasterization" : "");
    if (IsEqualGUID(&d->deviceGUID, &IID_IDirect3DHALDevice)) {
        *(D3DDEVICEDESC7 *)ctx = *d;
    }
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK enum_z(LPDDPIXELFORMAT pf, LPVOID ctx)
{
    DDPIXELFORMAT *best = (DDPIXELFORMAT *)ctx;
    printf("z format: %lu bits, flags %08lx\n", pf->dwZBufferBitDepth, pf->dwFlags);
    if (pf->dwFlags & DDPF_ZBUFFER && pf->dwZBufferBitDepth >= best->dwZBufferBitDepth &&
        !(pf->dwFlags & DDPF_STENCILBUFFER))
        *best = *pf;
    return D3DENUMRET_OK;
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProc(w, m, wp, lp);
}

/* A transformed, lit vertex: x, y in pixels, z 0..1, rhw, colour, one texture coordinate. */
typedef struct { float x, y, z, rhw; DWORD c; float u, v; } TLV;
#define TLV_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static TLV tlv(float x, float y, float z, DWORD c, float u, float v)
{
    TLV t;
    t.x = x; t.y = y; t.z = z; t.rhw = 1.0f; t.c = c; t.u = u; t.v = v;
    return t;
}

static void scene(LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 tex, int w, int h, float spin)
{
    TLV v[8];
    float cx = w * 0.25f, cy = h * 0.25f, r = h * 0.2f;
    int i;

    IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000066, 1.0f, 0);
    IDirect3DDevice7_BeginScene(dev);
    IDirect3DDevice7_SetTexture(dev, 0, NULL);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, TRUE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

    /* top left: a Gouraud-shaded triangle around (cx, cy), turning */
    for (i = 0; i < 3; i++) {
        float a = spin + i * 2.0944f;
        static const DWORD col[3] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF };
        v[i] = tlv(cx + r * (float)sin(a), cy - r * (float)cos(a), 0.5f, col[i], 0, 0);
    }
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLELIST, TLV_FVF, v, 3, 0);

    /* top right: green quad nearer (z 0.2) drawn first, a larger red quad behind (z 0.8) */
    v[0] = tlv(w * 0.6f, h * 0.15f, 0.2f, 0xFF00FF00, 0, 0);
    v[1] = tlv(w * 0.9f, h * 0.15f, 0.2f, 0xFF00FF00, 0, 0);
    v[2] = tlv(w * 0.6f, h * 0.35f, 0.2f, 0xFF00FF00, 0, 0);
    v[3] = tlv(w * 0.9f, h * 0.35f, 0.2f, 0xFF00FF00, 0, 0);
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);
    v[0] = tlv(w * 0.55f, h * 0.1f, 0.8f, 0xFFFF0000, 0, 0);
    v[1] = tlv(w * 0.95f, h * 0.1f, 0.8f, 0xFFFF0000, 0, 0);
    v[2] = tlv(w * 0.55f, h * 0.4f, 0.8f, 0xFFFF0000, 0, 0);
    v[3] = tlv(w * 0.95f, h * 0.4f, 0.8f, 0xFFFF0000, 0, 0);
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);

    /* bottom left: a 2x2 texture over a quad, point-sampled: four flat squares */
    if (tex) {
        IDirect3DDevice7_SetTexture(dev, 0, tex);
        IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
        IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
        IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        v[0] = tlv(w * 0.05f, h * 0.55f, 0.5f, 0xFFFFFFFF, 0, 0);
        v[1] = tlv(w * 0.45f, h * 0.55f, 0.5f, 0xFFFFFFFF, 1, 0);
        v[2] = tlv(w * 0.05f, h * 0.95f, 0.5f, 0xFFFFFFFF, 0, 1);
        v[3] = tlv(w * 0.45f, h * 0.95f, 0.5f, 0xFFFFFFFF, 1, 1);
        IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);
        IDirect3DDevice7_SetTexture(dev, 0, NULL);
        IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    }

    /* bottom right: half-transparent white over the clear colour */
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    v[0] = tlv(w * 0.55f, h * 0.55f, 0.5f, 0x80FFFFFF, 0, 0);
    v[1] = tlv(w * 0.95f, h * 0.55f, 0.5f, 0x80FFFFFF, 0, 0);
    v[2] = tlv(w * 0.55f, h * 0.95f, 0.5f, 0x80FFFFFF, 0, 0);
    v[3] = tlv(w * 0.95f, h * 0.95f, 0.5f, 0x80FFFFFF, 0, 0);
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice7_EndScene(dev);
}

/* A pixel of a 32-bit or 16-bit surface as 8-bit red, green, blue. */
static void pixel(LPDIRECTDRAWSURFACE7 s, int bpp, int x, int y, int *r, int *g, int *b)
{
    DDSURFACEDESC2 d;
    BYTE *row;

    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    *r = *g = *b = -1;
    if (IDirectDrawSurface7_Lock(s, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL) != DD_OK)
        return;
    row = (BYTE *)d.lpSurface + y * d.lPitch;
    if (bpp == 32) {
        DWORD v = ((DWORD *)row)[x];
        *r = (v >> 16) & 255; *g = (v >> 8) & 255; *b = v & 255;
    } else {
        WORD v = ((WORD *)row)[x];
        *r = ((v >> 11) & 31) * 255 / 31; *g = ((v >> 5) & 63) * 255 / 63; *b = (v & 31) * 255 / 31;
    }
    IDirectDrawSurface7_Unlock(s, NULL);
}

static void expect(LPDIRECTDRAWSURFACE7 s, int bpp, const char *what, int x, int y, int r, int g, int b)
{
    int pr, pg, pb, ok;

    pixel(s, bpp, x, y, &pr, &pg, &pb);
    ok = abs(pr - r) <= 16 && abs(pg - g) <= 16 && abs(pb - b) <= 16;
    printf("  %-28s at %3d,%3d: %3d %3d %3d  %s\n", what, x, y, pr, pg, pb, ok ? "ok" : "WRONG");
    if (!ok) {
        printf("    expected %d %d %d\n", r, g, b);
        failures++;
    }
}

static LPDIRECTDRAWSURFACE7 make_texture(LPDIRECTDRAW7 dd, int bpp)
{
    static const DWORD texels[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00 };
    LPDIRECTDRAWSURFACE7 t = NULL;
    DDSURFACEDESC2 d;
    HRESULT r;
    int i;

    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    d.dwWidth = d.dwHeight = 2;
    d.ddpfPixelFormat.dwSize = sizeof d.ddpfPixelFormat;
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    d.ddpfPixelFormat.dwGBitMask = 0xFF00;
    d.ddpfPixelFormat.dwBBitMask = 0xFF;
    d.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
    r = IDirectDraw7_CreateSurface(dd, &d, &t, NULL);
    printf("2x2 texture in video memory: %08lx\n", r);
    if (r != DD_OK)
        return NULL;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (IDirectDrawSurface7_Lock(t, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) == DD_OK) {
        for (i = 0; i < 4; i++)
            ((DWORD *)((BYTE *)d.lpSurface + (i / 2) * d.lPitch))[i % 2] = texels[i];
        IDirectDrawSurface7_Unlock(t, NULL);
    }
    (void)bpp;
    return t;
}

int main(int argc, char **argv)
{
    int w = argc > 3 ? atoi(argv[1]) : 640, h = argc > 3 ? atoi(argv[2]) : 480;
    int bpp = argc > 3 ? atoi(argv[3]) : 32, seconds = argc > 4 ? atoi(argv[4]) : 3;
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECT3D7 d3d = NULL;
    LPDIRECT3DDEVICE7 dev = NULL;
    LPDIRECTDRAWSURFACE7 front = NULL, back = NULL, z = NULL, tex = NULL;
    D3DDEVICEDESC7 hal;
    DDPIXELFORMAT zpf;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    WNDCLASS wc;
    HWND win;
    HRESULT r;
    DWORD start, frames = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL) != DD_OK) {
        printf("DirectDrawCreateEx failed (DirectX 7 installed?)\n");
        return 2;
    }
    if (IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d) != DD_OK) {
        printf("no Direct3D 7\n");
        return 2;
    }
    memset(&hal, 0, sizeof hal);
    IDirect3D7_EnumDevices(d3d, enum_device, &hal);
    check(hal.dwDevCaps != 0, "a Direct3D HAL device exists");
    if (!hal.dwDevCaps)
        goto out;
    printf("HAL: dev caps %08lx, tri raster %08lx, texture %08lx, filter %08lx, blend %08lx/%08lx\n",
           hal.dwDevCaps, hal.dpcTriCaps.dwRasterCaps, hal.dpcTriCaps.dwTextureCaps,
           hal.dpcTriCaps.dwTextureFilterCaps, hal.dpcTriCaps.dwSrcBlendCaps,
           hal.dpcTriCaps.dwDestBlendCaps);
    printf("HAL: render depths %08lx, z depths %08lx, textures %lux%lu max, %u stages\n",
           hal.dwDeviceRenderBitDepth, hal.dwDeviceZBufferBitDepth, hal.dwMaxTextureWidth,
           hal.dwMaxTextureHeight, hal.wMaxTextureBlendStages);
    memset(&zpf, 0, sizeof zpf);
    IDirect3D7_EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, enum_z, &zpf);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "d3dtest";
    RegisterClass(&wc);
    win = CreateWindowEx(WS_EX_TOPMOST, "d3dtest", "d3dtest", WS_POPUP | WS_VISIBLE, 0, 0,
                         GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                         NULL, NULL, wc.hInstance, NULL);
    IDirectDraw7_SetCooperativeLevel(dd, win, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    r = IDirectDraw7_SetDisplayMode(dd, w, h, bpp, 0, 0);
    printf("SetDisplayMode %dx%d, %d bits: %08lx\n", w, h, bpp, r);
    if (r != DD_OK) { failures++; goto out; }

    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    sd.dwBackBufferCount = 1;
    r = IDirectDraw7_CreateSurface(dd, &sd, &front, NULL);
    printf("flipping chain for 3D: %08lx\n", r);
    if (r != DD_OK) { failures++; goto out; }
    memset(&caps, 0, sizeof caps);
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7_GetAttachedSurface(front, &caps, &back);

    if (zpf.dwSize) {
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        sd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        sd.dwWidth = w;
        sd.dwHeight = h;
        sd.ddpfPixelFormat = zpf;
        r = IDirectDraw7_CreateSurface(dd, &sd, &z, NULL);
        printf("z-buffer (%lu bits): %08lx\n", zpf.dwZBufferBitDepth, r);
        if (r == DD_OK)
            IDirectDrawSurface7_AddAttachedSurface(back, z);
    }
    r = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev);
    check(r == D3D_OK, "the HAL device is created");
    if (r != D3D_OK) { printf("CreateDevice: %08lx\n", r); goto out; }
    {
        D3DVIEWPORT7 vp = { 0, 0, 0, 0, 0.0f, 1.0f };
        vp.dwWidth = w; vp.dwHeight = h;
        IDirect3DDevice7_SetViewport(dev, &vp);
    }
    tex = make_texture(dd, bpp);

    scene(dev, tex, w, h, 0.0f);
    printf("pixel checks (%dx%d):\n", w, h);
    expect(back, bpp, "clear colour", w / 2, h / 2, 0, 0, 102);
    expect(back, bpp, "triangle, centre", w / 4, h / 4, 85, 85, 85);
    expect(back, bpp, "z test: near quad", w * 3 / 4, h / 4, 0, 255, 0);
    expect(back, bpp, "z test: far quad", w * 57 / 100, h / 4, 255, 0, 0);
    if (tex) {
        expect(back, bpp, "texture: red texel", w * 15 / 100, h * 65 / 100, 255, 0, 0);
        expect(back, bpp, "texture: green texel", w * 35 / 100, h * 65 / 100, 0, 255, 0);
        expect(back, bpp, "texture: blue texel", w * 15 / 100, h * 85 / 100, 0, 0, 255);
        expect(back, bpp, "texture: yellow texel", w * 35 / 100, h * 85 / 100, 255, 255, 0);
    }
    expect(back, bpp, "blend: white on blue", w * 3 / 4, h * 3 / 4, 128, 128, 179);
    IDirectDrawSurface7_Flip(front, NULL, DDFLIP_WAIT);

    start = GetTickCount();
    while (GetTickCount() - start < (DWORD)seconds * 1000) {
        scene(dev, tex, w, h, (GetTickCount() - start) * 0.002f);
        IDirectDrawSurface7_Flip(front, NULL, DDFLIP_WAIT | DDFLIP_NOVSYNC);
        frames++;
    }
    printf("%lu frames in %d s: %.1f a second\n", frames, seconds, frames / (double)seconds);

out:
    if (tex) IDirectDrawSurface7_Release(tex);
    if (dev) IDirect3DDevice7_Release(dev);
    if (z) IDirectDrawSurface7_Release(z);
    if (front) IDirectDrawSurface7_Release(front);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) {
        IDirectDraw7_RestoreDisplayMode(dd);
        IDirectDraw7_Release(dd);
    }
    printf(failures ? "d3dtest FAILED: %d checks wrong\n" : "d3dtest passed\n", failures);
    return failures ? 1 : 0;
}
