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
 * QEMU's debug port. Exit status 0 when every check passed; a texture that can't be made counts
 * as a failure when the HAL lists its format.
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

/* A surface that checks need was not made. That is wrong when the HAL offers it (it lists the
   format, and for a texture in system memory sets D3DDEVCAPS_TEXTURESYSTEMMEMORY); otherwise it
   is only reported, as cards of the time lack some formats. */
static void not_made(const char *what, int offered)
{
    printf("  %s: not made%s\n", what, offered ? "  WRONG" : ", not offered by the HAL");
    if (offered)
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
    /* the deepest, and of those one with a stencil buffer (the stencil check needs it) */
    if ((pf->dwFlags & DDPF_ZBUFFER) && (pf->dwZBufferBitDepth > best->dwZBufferBitDepth ||
        (pf->dwZBufferBitDepth == best->dwZBufferBitDepth && (pf->dwFlags & DDPF_STENCILBUFFER))))
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
    /* (indexed, as games draw most of their geometry: two triangles from four vertices, the
       vertices not starting at the first) */
    {
        static WORD idx[6] = { 2, 3, 4, 4, 3, 5 };
        v[2] = tlv(w * 0.55f, h * 0.1f, 0.8f, 0xFFFF0000, 0, 0);
        v[3] = tlv(w * 0.95f, h * 0.1f, 0.8f, 0xFFFF0000, 0, 0);
        v[4] = tlv(w * 0.55f, h * 0.4f, 0.8f, 0xFFFF0000, 0, 0);
        v[5] = tlv(w * 0.95f, h * 0.4f, 0.8f, 0xFFFF0000, 0, 0);
        IDirect3DDevice7_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, TLV_FVF, v, 6, idx, 6, 0);
    }

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

/* ---- Texture updates: a 4x4 texture filled with one texel value, drawn over the middle of the
   screen; then changed in some way and drawn again. */
typedef struct { const char *name; int bits; DWORD r, g, b, a; DWORD red, green, blue; } TEXFMT;
static const TEXFMT texfmts[] = {
    { "32-bit ARGB", 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF },
    { "16-bit 565", 16, 0xF800, 0x07E0, 0x001F, 0, 0xF800, 0x07E0, 0x001F },
    { "16-bit 1555", 16, 0x7C00, 0x03E0, 0x001F, 0x8000, 0xFC00, 0x83E0, 0x801F },
    { "16-bit 4444", 16, 0x0F00, 0x00F0, 0x000F, 0xF000, 0xFF00, 0xF0F0, 0xF00F },
};

/* Which of texfmts the HAL device lists (IDirect3DDevice7_EnumTextureFormats). */
static int tex_listed[sizeof texfmts / sizeof texfmts[0]];

static HRESULT CALLBACK enum_texture(LPDDPIXELFORMAT pf, LPVOID ctx)
{
    int i;

    printf("texture format: %lu bits, masks %08lx %08lx %08lx %08lx, flags %08lx\n",
           pf->dwRGBBitCount, pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask,
           pf->dwRGBAlphaBitMask, pf->dwFlags);
    for (i = 0; i < (int)(sizeof texfmts / sizeof texfmts[0]); i++) {
        const TEXFMT *f = &texfmts[i];
        if ((pf->dwFlags & DDPF_RGB) && pf->dwRGBBitCount == (DWORD)f->bits &&
            pf->dwRBitMask == f->r && pf->dwGBitMask == f->g && pf->dwBBitMask == f->b &&
            ((pf->dwFlags & DDPF_ALPHAPIXELS) ? pf->dwRGBAlphaBitMask : 0) == f->a)
            tex_listed[i] = 1;
    }
    (*(int *)ctx)++;
    return D3DENUMRET_OK;
}

static LPDIRECTDRAWSURFACE7 tex_make(LPDIRECTDRAW7 dd, const TEXFMT *f, DWORD memcaps, DWORD extra)
{
    LPDIRECTDRAWSURFACE7 t = NULL;
    DDSURFACEDESC2 d;

    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = extra | memcaps;
    d.dwWidth = d.dwHeight = 4;
    d.ddpfPixelFormat.dwSize = sizeof d.ddpfPixelFormat;
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | (f->a ? DDPF_ALPHAPIXELS : 0);
    d.ddpfPixelFormat.dwRGBBitCount = f->bits;
    d.ddpfPixelFormat.dwRBitMask = f->r;
    d.ddpfPixelFormat.dwGBitMask = f->g;
    d.ddpfPixelFormat.dwBBitMask = f->b;
    d.ddpfPixelFormat.dwRGBAlphaBitMask = f->a;
    if (IDirectDraw7_CreateSurface(dd, &d, &t, NULL) != DD_OK)
        return NULL;
    return t;
}

static void tex_fill(LPDIRECTDRAWSURFACE7 t, int bits, DWORD value)
{
    DDSURFACEDESC2 d;
    int x, y;

    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (IDirectDrawSurface7_Lock(t, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) != DD_OK)
        return;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
            BYTE *row = (BYTE *)d.lpSurface + y * d.lPitch;
            if (bits == 32)
                ((DWORD *)row)[x] = value;
            else
                ((WORD *)row)[x] = (WORD)value;
        }
    IDirectDrawSurface7_Unlock(t, NULL);
}

/* One scene: clear, the texture over the middle; then the colour in the middle. */
static void tex_draw(LPDIRECTDRAW7 dd, LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 back,
                     LPDIRECTDRAWSURFACE7 tex, int w, int h, int bpp, const char *what,
                     int r, int g, int b)
{
    TLV v[4];

    IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    IDirect3DDevice7_BeginScene(dev);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice7_SetTexture(dev, 0, tex);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    v[0] = tlv(w * 0.25f, h * 0.25f, 0.5f, 0xFFFFFFFF, 0, 0);
    v[1] = tlv(w * 0.75f, h * 0.25f, 0.5f, 0xFFFFFFFF, 1, 0);
    v[2] = tlv(w * 0.25f, h * 0.75f, 0.5f, 0xFFFFFFFF, 0, 1);
    v[3] = tlv(w * 0.75f, h * 0.75f, 0.5f, 0xFFFFFFFF, 1, 1);
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);
    IDirect3DDevice7_SetTexture(dev, 0, NULL);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, TRUE);
    IDirect3DDevice7_EndScene(dev);
    expect(back, bpp, what, w / 2, h / 2, r, g, b);
    (void)dd;
}

static void texture_updates(LPDIRECTDRAW7 dd, LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 back,
                            int w, int h, int bpp, DWORD devcaps)
{
    char what[64];
    int i;

    printf("texture updates:\n");
    for (i = 0; i < (int)(sizeof texfmts / sizeof texfmts[0]); i++) {
        const TEXFMT *f = &texfmts[i];
        LPDIRECTDRAWSURFACE7 t = tex_make(dd, f, DDSCAPS_VIDEOMEMORY, DDSCAPS_TEXTURE);
        LPDIRECTDRAWSURFACE7 s;

        if (!t) {
            sprintf(what, "%s texture in video memory", f->name);
            not_made(what, tex_listed[i]);
            continue;
        }
        tex_fill(t, f->bits, f->red);
        sprintf(what, "%s, filled red", f->name);
        tex_draw(dd, dev, back, t, w, h, bpp, what, 255, 0, 0);
        tex_fill(t, f->bits, f->green);
        sprintf(what, "%s, locked again: green", f->name);
        tex_draw(dd, dev, back, t, w, h, bpp, what, 0, 255, 0);
        /* a blit from a surface in system memory, as DirectX 7's texture manager does */
        s = tex_make(dd, f, DDSCAPS_SYSTEMMEMORY, DDSCAPS_OFFSCREENPLAIN);
        if (s) {
            tex_fill(s, f->bits, f->blue);
            IDirectDrawSurface7_Blt(t, NULL, s, NULL, DDBLT_WAIT, NULL);
            sprintf(what, "%s, blitted: blue", f->name);
            tex_draw(dd, dev, back, t, w, h, bpp, what, 0, 0, 255);
            IDirectDrawSurface7_Release(s);
        } else {
            sprintf(what, "%s surface in system memory, to blit from", f->name);
            not_made(what, tex_listed[i]);
        }
        IDirectDrawSurface7_Release(t);
    }
    /* a texture in system memory, written between scenes */
    {
        LPDIRECTDRAWSURFACE7 t = tex_make(dd, &texfmts[0], DDSCAPS_SYSTEMMEMORY, DDSCAPS_TEXTURE);
        if (t) {
            tex_fill(t, 32, texfmts[0].red);
            tex_draw(dd, dev, back, t, w, h, bpp, "system memory texture: red", 255, 0, 0);
            tex_fill(t, 32, texfmts[0].blue);
            tex_draw(dd, dev, back, t, w, h, bpp, "system memory, rewritten: blue", 0, 0, 255);
            IDirectDrawSurface7_Release(t);
        } else {
            not_made("texture in system memory",
                     tex_listed[0] && (devcaps & D3DDEVCAPS_TEXTURESYSTEMMEMORY));
        }
    }
}

/* The program draws into the back buffer itself between scenes (games draw text with GDI or a
   locked surface): that must survive the next scene's 3D drawing. */
static void cpu_writes(LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 back, int w, int h, int bpp)
{
    DDSURFACEDESC2 d;
    TLV v[3];
    int x, y;

    printf("drawing into the back buffer between scenes:\n");
    IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000066, 1.0f, 0);
    IDirect3DDevice7_BeginScene(dev);
    IDirect3DDevice7_EndScene(dev);
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (IDirectDrawSurface7_Lock(back, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) == DD_OK) {
        for (y = 16; y < 48; y++)
            for (x = 16; x < 48; x++) {
                BYTE *row = (BYTE *)d.lpSurface + y * d.lPitch;
                if (bpp == 32)
                    ((DWORD *)row)[x] = 0x00FFFFFF;
                else
                    ((WORD *)row)[x] = 0xFFFF;
            }
        IDirectDrawSurface7_Unlock(back, NULL);
    }
    IDirect3DDevice7_BeginScene(dev);
    IDirect3DDevice7_SetTexture(dev, 0, NULL);
    v[0] = tlv(w * 0.5f, h * 0.4f, 0.5f, 0xFFFF0000, 0, 0);
    v[1] = tlv(w * 0.6f, h * 0.6f, 0.5f, 0xFFFF0000, 0, 0);
    v[2] = tlv(w * 0.4f, h * 0.6f, 0.5f, 0xFFFF0000, 0, 0);
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLELIST, TLV_FVF, v, 3, 0);
    IDirect3DDevice7_EndScene(dev);
    expect(back, bpp, "written by the program", 32, 32, 255, 255, 255);
    expect(back, bpp, "3D drawn after it", w / 2, h / 2 + h / 20, 255, 0, 0);
    expect(back, bpp, "clear colour around them", w * 3 / 4, h * 3 / 4, 0, 0, 102);
}

/* ---- Mipmaps: a 64x64 texture whose 7 levels each have a colour of their own; a quad of 8x8
   pixels must show level 3, one of 64x64 level 0. */
static const DWORD mip_colours[7] = {
    0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF
};

static LPDIRECTDRAWSURFACE7 mip_make(LPDIRECTDRAW7 dd, DWORD caps, DWORD caps2)
{
    LPDIRECTDRAWSURFACE7 t = NULL;
    DDSURFACEDESC2 d;

    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | caps;
    d.ddsCaps.dwCaps2 = caps2;
    d.dwWidth = d.dwHeight = 64;
    d.dwMipMapCount = 7;
    d.ddpfPixelFormat.dwSize = sizeof d.ddpfPixelFormat;
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    d.ddpfPixelFormat.dwGBitMask = 0xFF00;
    d.ddpfPixelFormat.dwBBitMask = 0xFF;
    d.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
    if (IDirectDraw7_CreateSurface(dd, &d, &t, NULL) != DD_OK)
        return NULL;
    return t;
}

/* Fill one level (0: the top) with a colour; 0 if that level isn't there. */
static int mip_fill(LPDIRECTDRAWSURFACE7 top, int level, DWORD colour)
{
    LPDIRECTDRAWSURFACE7 s = top, next;
    DDSURFACEDESC2 d;
    DDSCAPS2 caps;
    DWORD x, y;
    int i, ok = 0;

    IDirectDrawSurface7_AddRef(s);
    for (i = 0; i < level; i++) {
        memset(&caps, 0, sizeof caps);
        caps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
        next = NULL;
        IDirectDrawSurface7_GetAttachedSurface(s, &caps, &next);
        IDirectDrawSurface7_Release(s);
        if (!next)
            return 0;
        s = next;
    }
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (IDirectDrawSurface7_Lock(s, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) == DD_OK) {
        for (y = 0; y < d.dwHeight; y++)
            for (x = 0; x < d.dwWidth; x++)
                ((DWORD *)((BYTE *)d.lpSurface + y * d.lPitch))[x] = colour;
        IDirectDrawSurface7_Unlock(s, NULL);
        ok = 1;
    }
    IDirectDrawSurface7_Release(s);
    return ok;
}

/* A square of size pixels with the texture, its top left corner at 96,96; then the colour in
   its middle. */
static void mip_draw(LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 back, LPDIRECTDRAWSURFACE7 tex,
                     int bpp, float size, DWORD mipfilter, float bias, const char *what,
                     DWORD expect_colour)
{
    TLV v[4];

    IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    IDirect3DDevice7_BeginScene(dev);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice7_SetTexture(dev, 0, tex);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, mipfilter);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MIPMAPLODBIAS, *(DWORD *)&bias);
    v[0] = tlv(96.0f, 96.0f, 0.5f, 0xFFFFFFFF, 0, 0);
    v[1] = tlv(96.0f + size, 96.0f, 0.5f, 0xFFFFFFFF, 1, 0);
    v[2] = tlv(96.0f, 96.0f + size, 0.5f, 0xFFFFFFFF, 0, 1);
    v[3] = tlv(96.0f + size, 96.0f + size, 0.5f, 0xFFFFFFFF, 1, 1);
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);
    bias = 0.0f;
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MIPMAPLODBIAS, *(DWORD *)&bias);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTFP_NONE);
    IDirect3DDevice7_SetTexture(dev, 0, NULL);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, TRUE);
    IDirect3DDevice7_EndScene(dev);
    expect(back, bpp, what, 96 + (int)size / 2, 96 + (int)size / 2, (expect_colour >> 16) & 255,
           (expect_colour >> 8) & 255, expect_colour & 255);
}

static void mipmaps(LPDIRECTDRAW7 dd, LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 back, int bpp,
                    const D3DDEVICEDESC7 *hal)
{
    static const struct { const char *name; DWORD caps, caps2; } kinds[] = {
        { "video memory", DDSCAPS_VIDEOMEMORY, 0 },
        { "managed", 0, DDSCAPS2_TEXTUREMANAGE },      /* uploaded with texture blits */
    };
    int offered = (hal->dpcTriCaps.dwTextureFilterCaps & D3DPTFILTERCAPS_MIPFPOINT) != 0;
    char what[80];
    int k, l;

    printf("mipmaps (%s):\n", offered ? "offered" : "not offered");
    for (k = 0; k < 2; k++) {
        LPDIRECTDRAWSURFACE7 t = mip_make(dd, kinds[k].caps, kinds[k].caps2);
        if (!t) {
            sprintf(what, "mipmapped texture, %s", kinds[k].name);
            not_made(what, offered);
            continue;
        }
        for (l = 0; l < 7; l++) {
            if (!mip_fill(t, l, mip_colours[l])) {
                printf("  %s: level %d missing\n", kinds[k].name, l);
                failures++;
                break;
            }
        }
        if (l == 7) {
            sprintf(what, "%s, 64x64: level 0", kinds[k].name);
            mip_draw(dev, back, t, bpp, 64.0f, D3DTFP_POINT, 0.0f, what, mip_colours[0]);
            sprintf(what, "%s, 8x8: level 3", kinds[k].name);
            mip_draw(dev, back, t, bpp, 8.0f, D3DTFP_POINT, 0.0f, what, mip_colours[3]);
            sprintf(what, "%s, 8x8, no mip filter: 0", kinds[k].name);
            mip_draw(dev, back, t, bpp, 8.0f, D3DTFP_NONE, 0.0f, what, mip_colours[0]);
            if (hal->dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS) {
                sprintf(what, "%s, 8x8, bias +1: level 4", kinds[k].name);
                mip_draw(dev, back, t, bpp, 8.0f, D3DTFP_POINT, 1.0f, what, mip_colours[4]);
            }
            /* one level written again */
            mip_fill(t, 3, mip_colours[6]);
            sprintf(what, "%s, level 3 rewritten: white", kinds[k].name);
            mip_draw(dev, back, t, bpp, 8.0f, D3DTFP_POINT, 0.0f, what, mip_colours[6]);
        }
        IDirectDrawSurface7_Release(t);
    }
}

/* ---- Colour key: a 4x4 texture, black on the left half (the key) and red on the right, drawn
   over the middle of a blue screen; with the key on, blue must show through on the left. */
static void colour_key(LPDIRECTDRAW7 dd, LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 back, int w,
                       int h, int bpp, const D3DDEVICEDESC7 *hal)
{
    static const TEXFMT xrgb = { "32-bit XRGB", 32, 0xFF0000, 0xFF00, 0xFF, 0, 0xFFFF0000, 0, 0 };
    static const struct { const char *name; const TEXFMT *f; } kinds[] = {
        { "32-bit XRGB", &xrgb }, { "16-bit 565", &texfmts[1] },
    };
    int offered = (hal->dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_TRANSPARENCY) != 0;
    char what[80];
    int k, on;

    printf("colour key (%s):\n", offered ? "offered" : "not offered");
    for (k = 0; k < 2; k++) {
        const TEXFMT *f = kinds[k].f;
        LPDIRECTDRAWSURFACE7 t = tex_make(dd, f, DDSCAPS_VIDEOMEMORY, DDSCAPS_TEXTURE);
        DDCOLORKEY key = { 0, 0 };
        DDSURFACEDESC2 d;
        DWORD x, y;

        if (!t) {
            sprintf(what, "%s texture for the colour key", kinds[k].name);
            not_made(what, offered);
            continue;
        }
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        if (IDirectDrawSurface7_Lock(t, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) == DD_OK) {
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    BYTE *row = (BYTE *)d.lpSurface + y * d.lPitch;
                    DWORD v = x < 2 ? 0 : (f->bits == 32 ? 0xFF0000 : 0xF800);
                    if (f->bits == 32)
                        ((DWORD *)row)[x] = v;
                    else
                        ((WORD *)row)[x] = (WORD)v;
                }
            IDirectDrawSurface7_Unlock(t, NULL);
        }
        if (IDirectDrawSurface7_SetColorKey(t, DDCKEY_SRCBLT, &key) != DD_OK) {
            printf("  %s: SetColorKey failed\n", kinds[k].name);
            failures++;
        }
        for (on = 1; on >= 0; on--) {
            TLV v[4];
            IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x000000FF,
                                   1.0f, 0);
            IDirect3DDevice7_BeginScene(dev);
            IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
            IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_COLORKEYENABLE, on);
            IDirect3DDevice7_SetTexture(dev, 0, t);
            IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            v[0] = tlv(w * 0.25f, h * 0.25f, 0.5f, 0xFFFFFFFF, 0, 0);
            v[1] = tlv(w * 0.75f, h * 0.25f, 0.5f, 0xFFFFFFFF, 1, 0);
            v[2] = tlv(w * 0.25f, h * 0.75f, 0.5f, 0xFFFFFFFF, 0, 1);
            v[3] = tlv(w * 0.75f, h * 0.75f, 0.5f, 0xFFFFFFFF, 1, 1);
            IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);
            IDirect3DDevice7_SetTexture(dev, 0, NULL);
            IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, TRUE);
            IDirect3DDevice7_EndScene(dev);
            sprintf(what, "%s, key %s: left", kinds[k].name, on ? "on" : "off");
            if (on)
                expect(back, bpp, what, w * 3 / 8, h / 2, 0, 0, 255);
            else
                expect(back, bpp, what, w * 3 / 8, h / 2, 0, 0, 0);
            sprintf(what, "%s, key %s: right", kinds[k].name, on ? "on" : "off");
            expect(back, bpp, what, w * 5 / 8, h / 2, 255, 0, 0);
        }
        IDirectDrawSurface7_Release(t);
    }
}

/* A quad over a part of the screen, one colour, no texture. */
static void quad(LPDIRECT3DDEVICE7 dev, float x0, float y0, float x1, float y1, DWORD colour)
{
    TLV v[4];

    v[0] = tlv(x0, y0, 0.5f, colour, 0, 0);
    v[1] = tlv(x1, y0, 0.5f, colour, 0, 0);
    v[2] = tlv(x0, y1, 0.5f, colour, 0, 0);
    v[3] = tlv(x1, y1, 0.5f, colour, 0, 0);
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, TLV_FVF, v, 4, 0);
}

/* ---- Stencil: a quad on the left half marks the stencil buffer (1); a green quad over the
   whole screen, drawn only where the stencil is 1, must cover the left half and not the right.
   Then incrementing: a second mark over the middle makes 2 there. */
static void stencil(LPDIRECT3DDEVICE7 dev, LPDIRECTDRAWSURFACE7 back, int w, int h, int bpp,
                    const DDPIXELFORMAT *zpf)
{
    if (!(zpf->dwFlags & DDPF_STENCILBUFFER)) {
        printf("stencil: no z-buffer format with a stencil buffer\n");
        return;
    }
    printf("stencil (%lu bits):\n", zpf->dwStencilBitDepth);
    IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                           0x000000FF, 1.0f, 0);
    IDirect3DDevice7_BeginScene(dev);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILENABLE, TRUE);
    /* mark: always passes, writes the reference value 1, the colour is red */
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILFUNC, D3DCMP_ALWAYS);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILREF, 1);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILPASS, D3DSTENCILOP_REPLACE);
    quad(dev, 0, 0, w * 0.5f, (float)h, 0xFFFF0000);
    /* the middle third: one more */
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILPASS, D3DSTENCILOP_INCRSAT);
    quad(dev, w / 3.0f, 0, w * 2 / 3.0f, (float)h, 0xFFFF0000);
    /* green where the stencil is 1, white where it is 2 */
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILPASS, D3DSTENCILOP_KEEP);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILFUNC, D3DCMP_EQUAL);
    quad(dev, 0, 0, (float)w, (float)h, 0xFF00FF00);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILREF, 2);
    quad(dev, 0, 0, (float)w, (float)h, 0xFFFFFFFF);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILENABLE, FALSE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILFUNC, D3DCMP_ALWAYS);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_STENCILREF, 0);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, TRUE);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    IDirect3DDevice7_EndScene(dev);
    expect(back, bpp, "stencil 1: green", w / 6, h / 2, 0, 255, 0);
    expect(back, bpp, "stencil 2: white", w * 5 / 12, h / 2, 255, 255, 255);
    expect(back, bpp, "stencil 0+1 (right of 2): green", w * 7 / 12, h / 2, 0, 255, 0);
    expect(back, bpp, "stencil 0: clear colour", w * 5 / 6, h / 2, 0, 0, 255);
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
    int formats = 0;

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
    IDirect3DDevice7_EnumTextureFormats(dev, enum_texture, &formats);
    check(formats > 0, "the HAL lists texture formats");
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
    } else {
        not_made("2x2 texture in video memory", tex_listed[0]);    /* 32-bit ARGB */
    }
    expect(back, bpp, "blend: white on blue", w * 3 / 4, h * 3 / 4, 128, 128, 179);
    texture_updates(dd, dev, back, w, h, bpp, hal.dwDevCaps);
    mipmaps(dd, dev, back, bpp, &hal);
    colour_key(dd, dev, back, w, h, bpp, &hal);
    stencil(dev, back, w, h, bpp, &zpf);
    cpu_writes(dev, back, w, h, bpp);
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
