/* Check the Direct3D 7 features a game's first frames need, on the HAL or
 * Microsoft's RGB device: textures in each offered format, texture and
 * diffuse modulation, alpha test, two blend modes, both cull modes, flat
 * shading, a texture rewritten between scenes, and textures in video memory and managed by the runtime.
 * Each case draws on a 128x128 target and inspects a few pixels well inside
 * the shapes, so rasterization rules at edges cannot change the result. */
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rb2kd3d7.h"
#include "drvcheck.h"

enum { Side = 128 };
#define CLEAR_COLOR 0xff000066UL
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)
typedef struct { float x, y, z, rhw; DWORD diffuse, specular; float u, v; } VERTEX;
/* The same vertex in object space, for the T&L path (xyz mode). */
typedef struct { float x, y, z; DWORD diffuse, specular; float u, v; } OBJECT_VERTEX;
#define OBJECT_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)
typedef struct { int x, y; DWORD rgb; int tolerance; } SAMPLE;
enum { Argb8888, Rgb565, Argb1555, Argb4444, Formats };
static const char *const formatNames[] = { "argb8888", "rgb565", "argb1555", "argb4444" };

static IDirectDraw7 *dd;
static IDirect3DDevice7 *device;
static IDirectDrawSurface7 *target;
static HDC displayDc;
static int passed, failed;
/* Pixels of a case that may differ from the picture of the DirectX 7 software device: 2 in 100 of
 * 128x128. That device breaks ties on texel borders the other way (182 pixels, one row and one
 * column, in every textured case), which the Direct3D 9 reference rasterizer does not. */
enum { RefLimit = 328 };
static BOOL xyz, perspective;

/* Draw a triangle list, in object space in xyz mode: the projection set in
 * main maps object x and y to the same pixels as the transformed vertex. */
static HRESULT triangles(VERTEX *v, DWORD n)
{
    OBJECT_VERTEX o[6];
    DWORD i;
    if (!xyz) return IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST, FVF, v, n, 0);
    for (i = 0; i < n && i < 6; ++i) {
        if (perspective) {
            /* Two units in front of a right-handed camera: w = 2. */
            o[i].x = (v[i].x / 64 - 1) * 2; o[i].y = (1 - v[i].y / 64) * 2; o[i].z = -2;
        } else { o[i].x = v[i].x; o[i].y = v[i].y; o[i].z = v[i].z; }
        o[i].diffuse = v[i].diffuse; o[i].specular = v[i].specular; o[i].u = v[i].u; o[i].v = v[i].v;
    }
    return IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST, OBJECT_FVF, o, n, 0);
}

static BOOL call(const char *what, HRESULT hr)
{
    if (FAILED(hr)) printf("TPIX %s hr=%08lx\n", what, (DWORD)hr);
    return SUCCEEDED(hr);
}

static void set(D3DRENDERSTATETYPE s, DWORD v) { IDirect3DDevice7_SetRenderState(device, s, v); }
static void stage(DWORD n, D3DTEXTURESTAGESTATETYPE s, DWORD v)
{
    IDirect3DDevice7_SetTextureStageState(device, n, s, v);
}

static void defaults(void)
{
    set(D3DRENDERSTATE_LIGHTING, FALSE);
    set(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    set(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    set(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    set(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
    set(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
    set(D3DRENDERSTATE_SPECULARENABLE, FALSE);
    set(D3DRENDERSTATE_FOGENABLE, FALSE);
    set(D3DRENDERSTATE_DITHERENABLE, FALSE);
    stage(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    stage(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    stage(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    stage(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    stage(0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    stage(0, D3DTSS_MINFILTER, D3DTFN_POINT);
    stage(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
    stage(0, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP);
    stage(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    stage(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice7_SetTexture(device, 0, NULL);
}

static VERTEX vertex(float x, float y, DWORD diffuse, float u, float v)
{
    VERTEX r;
    r.x = x; r.y = y; r.z = 0.5f; r.rhw = 1;
    r.diffuse = diffuse; r.specular = 0; r.u = u; r.v = v;
    return r;
}

/* Two clockwise triangles covering 16..112 with texture coordinates 0..1. */
static BOOL quad(DWORD diffuse)
{
    VERTEX v[6];
    v[0] = vertex(16, 16, diffuse, 0, 0); v[1] = vertex(112, 16, diffuse, 1, 0);
    v[2] = vertex(16, 112, diffuse, 0, 1); v[3] = vertex(112, 16, diffuse, 1, 0);
    v[4] = vertex(112, 112, diffuse, 1, 1); v[5] = vertex(16, 112, diffuse, 0, 1);
    return call("quad", triangles(v, 6));
}

static BOOL begin(void)
{
    return call("clear", IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1, 0)) &&
           call("begin", IDirect3DDevice7_BeginScene(device));
}

static BOOL end(void) { return call("end", IDirect3DDevice7_EndScene(device)); }

static void report(const char *name, const SAMPLE *s, int count)
{
    DDSURFACEDESC2 d = {0};
    RB2K_D7_DRAWSTATS stats = {0};
    int i, good = 0, bad = -1, worst = 0;
    long differ = -1;
    unsigned long skipped;
    DWORD got = 0, first = 0;
    d.dwSize = sizeof(d);
    if (call("lock target", IDirectDrawSurface7_Lock(target, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) {
        for (i = 0; i < count; ++i) {
            DWORD v = ((const DWORD *)((const BYTE *)d.lpSurface + s[i].y * d.lPitch))[s[i].x] & 0xffffff;
            int dr = abs((int)(v >> 16) - (int)(s[i].rgb >> 16));
            int dg = abs((int)((v >> 8) & 255) - (int)((s[i].rgb >> 8) & 255));
            int db = abs((int)(v & 255) - (int)(s[i].rgb & 255));
            if (dr <= s[i].tolerance && dg <= s[i].tolerance && db <= s[i].tolerance) ++good;
            else if (bad < 0) { bad = i; got = v; first = s[i].rgb; }
        }
        /* The automatic checks (drvcheck.h): the picture against the one Microsoft's software
         * device drew in the "rgb" run, and below what the driver skipped. */
        if (d.dwWidth == d.dwHeight && d.ddpfPixelFormat.dwRGBBitCount == 32)
            differ = ref_case(name, d.lpSurface, d.lPitch, (int)d.dwWidth, &worst);
        IDirectDrawSurface7_Unlock(target, NULL);
    }
    if (displayDc) ExtEscape(displayDc, RB2K_D3D7_DRAW_ESCAPE, 0, NULL, sizeof(stats), (LPSTR)&stats);
    printf("TPIX case=%s samples=%d/%d", name, good, count);
    if (bad >= 0) printf(" first_bad=%d,%d expected=%06lx got=%06lx", s[bad].x, s[bad].y, first, got);
    printf(" drawn=%lu skipped_calls=%lu uploads=%lu", stats.drawn, stats.skippedCalls, stats.uploads);
    if (refMode == RefCompare && differ >= 0) {
        printf(" ref_differ=%ld ref_worst=%d", differ, worst);
        if (differ > RefLimit) ++refFailed;
    }
    printf("\n");
    skipped = skips_case("TPIX", name);
    if (good == count && !skipped && !(refMode == RefCompare && differ > RefLimit)) ++passed; else ++failed;
}

static DWORD pack(int format, DWORD argb)
{
    DWORD a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    switch (format) {
    case Rgb565: return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    case Argb1555: return (a >= 128) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
    case Argb4444: return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
    }
    return argb;
}

/* An 8x8 texture with four 4x4 quadrants. */
enum { SystemMemory, Managed, VideoMemory };
static IDirectDrawSurface7 *texture(int format, int memory, const DWORD quadrants[4])
{
    DDSURFACEDESC2 d = {0};
    IDirectDrawSurface7 *t = NULL;
    int x, y;
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = 8;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | (memory == SystemMemory ? DDSCAPS_SYSTEMMEMORY :
                                          memory == VideoMemory ? DDSCAPS_VIDEOMEMORY : 0);
    if (memory == Managed) d.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | (format == Rgb565 ? 0 : DDPF_ALPHAPIXELS);
    d.ddpfPixelFormat.dwRGBBitCount = format == Argb8888 ? 32 : 16;
    switch (format) {
    case Argb8888:
        d.ddpfPixelFormat.dwRBitMask = 0xff0000; d.ddpfPixelFormat.dwGBitMask = 0xff00;
        d.ddpfPixelFormat.dwBBitMask = 0xff; d.ddpfPixelFormat.dwRGBAlphaBitMask = 0xff000000; break;
    case Rgb565:
        d.ddpfPixelFormat.dwRBitMask = 0xf800; d.ddpfPixelFormat.dwGBitMask = 0x07e0;
        d.ddpfPixelFormat.dwBBitMask = 0x1f; break;
    case Argb1555:
        d.ddpfPixelFormat.dwRBitMask = 0x7c00; d.ddpfPixelFormat.dwGBitMask = 0x03e0;
        d.ddpfPixelFormat.dwBBitMask = 0x1f; d.ddpfPixelFormat.dwRGBAlphaBitMask = 0x8000; break;
    case Argb4444:
        d.ddpfPixelFormat.dwRBitMask = 0x0f00; d.ddpfPixelFormat.dwGBitMask = 0x00f0;
        d.ddpfPixelFormat.dwBBitMask = 0x0f; d.ddpfPixelFormat.dwRGBAlphaBitMask = 0xf000; break;
    }
    if (!call(formatNames[format], IDirectDraw7_CreateSurface(dd, &d, &t, NULL))) return NULL;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (!call("lock texture", IDirectDrawSurface7_Lock(t, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL))) {
        IDirectDrawSurface7_Release(t);
        return NULL;
    }
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x) {
            DWORD p = pack(format, quadrants[(y >= 4) * 2 + (x >= 4)]);
            BYTE *row = (BYTE *)d.lpSurface + y * d.lPitch;
            if (format == Argb8888) ((DWORD *)row)[x] = p; else ((WORD *)row)[x] = (WORD)p;
        }
    IDirectDrawSurface7_Unlock(t, NULL);
    return t;
}

static void quadrant_samples(SAMPLE *s, const DWORD rgb[4], int tolerance)
{
    static const int at[4][2] = { {40, 40}, {88, 40}, {40, 88}, {88, 88} };
    int i;
    for (i = 0; i < 4; ++i) {
        s[i].x = at[i][0]; s[i].y = at[i][1]; s[i].rgb = rgb[i] & 0xffffff; s[i].tolerance = tolerance;
    }
    s[4].x = 4; s[4].y = 4; s[4].rgb = CLEAR_COLOR & 0xffffff; s[4].tolerance = 0;
}

static void textured(const char *name, IDirectDrawSurface7 *t, DWORD op, DWORD diffuse,
                     const DWORD expect[4], int tolerance)
{
    SAMPLE s[5];
    defaults();
    stage(0, D3DTSS_COLOROP, op);
    stage(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    stage(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    stage(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    stage(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (!t || !call("set texture", IDirect3DDevice7_SetTexture(device, 0, t)) || !begin() ||
        !quad(diffuse) || !end()) { ++failed; printf("TPIX case=%s setup failed\n", name); return; }
    quadrant_samples(s, expect, tolerance);
    report(name, s, 5);
}

static const DWORD primaries[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };

static void formats(void)
{
    int f;
    for (f = 0; f < Formats; ++f) {
        IDirectDrawSurface7 *t = texture(f, SystemMemory, primaries);
        char name[32];
        sprintf(name, "select-%s", formatNames[f]);
        /* Microsoft's RGB device widens 5-bit red to 248 where the HAL replicates bits. */
        textured(name, t, D3DTOP_SELECTARG1, 0xffffffff, primaries, f == Rgb565 || f == Argb1555 ? 8 : 0);
        if (t) IDirectDrawSurface7_Release(t);
    }
}

static void modulate(void)
{
    static const DWORD half[4] = { 0x800000, 0x008000, 0x000080, 0x808080 };
    IDirectDrawSurface7 *t = texture(Argb8888, SystemMemory, primaries);
    textured("modulate", t, D3DTOP_MODULATE, 0xff808080, half, 2);
    if (t) IDirectDrawSurface7_Release(t);
}

static void alpha_test(void)
{
    static const DWORD texels[4] = { 0x00ff0000, 0xff00ff00, 0x000000ff, 0xffffffff };
    DWORD expect[4];
    SAMPLE s[5];
    IDirectDrawSurface7 *t = texture(Argb8888, SystemMemory, texels);
    expect[0] = expect[2] = CLEAR_COLOR; expect[1] = texels[1]; expect[3] = texels[3];
    defaults();
    stage(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    stage(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    stage(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    stage(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    set(D3DRENDERSTATE_ALPHATESTENABLE, TRUE);
    set(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER);
    set(D3DRENDERSTATE_ALPHAREF, 0x80);
    if (!t || !call("set texture", IDirect3DDevice7_SetTexture(device, 0, t)) || !begin() ||
        !quad(0xffffffff) || !end()) { ++failed; puts("TPIX case=alphatest setup failed"); }
    else { quadrant_samples(s, expect, 0); report("alphatest", s, 5); }
    if (t) IDirectDrawSurface7_Release(t);
}

static void blend(const char *name, DWORD src, DWORD dst, DWORD diffuse, DWORD expect)
{
    DWORD e[4];
    SAMPLE s[5];
    defaults();
    set(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    set(D3DRENDERSTATE_SRCBLEND, src);
    set(D3DRENDERSTATE_DESTBLEND, dst);
    if (!begin() || !quad(diffuse) || !end()) { ++failed; printf("TPIX case=%s setup failed\n", name); return; }
    e[0] = e[1] = e[2] = e[3] = expect;
    quadrant_samples(s, e, 3);
    report(name, s, 5);
}

/* A clockwise green triangle at the left and a counter-clockwise red one at the right. */
static void cull(const char *name, DWORD mode, BOOL showCw, BOOL showCcw)
{
    VERTEX v[6];
    SAMPLE s[3] = { {30, 40, 0, 0}, {98, 40, 0, 0}, {4, 4, CLEAR_COLOR & 0xffffff, 0} };
    defaults();
    set(D3DRENDERSTATE_CULLMODE, mode);
    v[0] = vertex(8, 8, 0xff00ff00, 0, 0); v[1] = vertex(60, 8, 0xff00ff00, 0, 0);
    v[2] = vertex(8, 100, 0xff00ff00, 0, 0);
    v[3] = vertex(68, 8, 0xffff0000, 0, 0); v[4] = vertex(68, 100, 0xffff0000, 0, 0);
    v[5] = vertex(120, 8, 0xffff0000, 0, 0);
    s[0].rgb = showCw ? 0x00ff00 : CLEAR_COLOR & 0xffffff;
    s[1].rgb = showCcw ? 0xff0000 : CLEAR_COLOR & 0xffffff;
    if (!begin() || !call("cull draw", triangles(v, 6)) ||
        !end()) { ++failed; printf("TPIX case=%s setup failed\n", name); return; }
    report(name, s, 3);
}

static void flat(void)
{
    VERTEX v[3];
    SAMPLE s[2] = { {40, 40, 0xff0000, 0}, {4, 4, CLEAR_COLOR & 0xffffff, 0} };
    defaults();
    set(D3DRENDERSTATE_SHADEMODE, D3DSHADE_FLAT);
    v[0] = vertex(16, 16, 0xffff0000, 0, 0); v[1] = vertex(112, 16, 0xff00ff00, 0, 0);
    v[2] = vertex(16, 112, 0xff0000ff, 0, 0);
    if (!begin() || !call("flat draw", triangles(v, 3)) ||
        !end()) { ++failed; puts("TPIX case=flat setup failed"); return; }
    report("flat", s, 2);
}

/* Rewrite one quadrant between scenes; the next scene must show it. */
/* The top-left quadrant of a texture becomes yellow. */
static const DWORD after[4] = { 0xffffff00, 0xff00ff00, 0xff0000ff, 0xffffffff };
static void repaint(IDirectDrawSurface7 *t)
{
    DDSURFACEDESC2 d = {0};
    int x, y;
    d.dwSize = sizeof(d);
    if (call("relock texture", IDirectDrawSurface7_Lock(t, NULL, &d, DDLOCK_WAIT, NULL))) {
        for (y = 0; y < 4; ++y)
            for (x = 0; x < 4; ++x) ((DWORD *)((BYTE *)d.lpSurface + y * d.lPitch))[x] = after[0];
        IDirectDrawSurface7_Unlock(t, NULL);
    }
}

static void rewrite(void)
{
    IDirectDrawSurface7 *t = texture(Argb8888, SystemMemory, primaries);
    textured("rewrite-before", t, D3DTOP_SELECTARG1, 0xffffffff, primaries, 0);
    if (!t) return;
    repaint(t);
    textured("rewrite-after", t, D3DTOP_SELECTARG1, 0xffffffff, after, 0);
    IDirectDrawSurface7_Release(t);
}

/* Video memory and the runtime's texture manager need texture hardware; the
 * RGB device answers DDERR_NOTEXTUREHW by design, so only the HAL runs these. */
static void hardware_memory(BOOL hal)
{
    static const int kinds[] = { VideoMemory, Managed };
    static const char *const names[] = { "video-argb8888", "managed-argb8888" };
    int i;
    for (i = 0; i < 2; ++i) {
        IDirectDrawSurface7 *t;
        if (!hal) { printf("TPIX case=%s not applicable to the RGB device\n", names[i]); continue; }
        t = texture(Argb8888, kinds[i], primaries);
        textured(names[i], t, D3DTOP_SELECTARG1, 0xffffffff, primaries, 0);
        if (t) {
            /* A later rewrite must reach the next scene. */
            repaint(t);
            textured(i ? "managed-rewrite" : "video-rewrite", t, D3DTOP_SELECTARG1, 0xffffffff, after, 0);
            IDirectDrawSurface7_Release(t);
        }
    }
}

typedef struct { const GUID *wanted; BOOL found; } ENUM_DEVICE;
static HRESULT CALLBACK device_info(LPSTR description, LPSTR name, D3DDEVICEDESC7 *d, LPVOID user)
{
    ENUM_DEVICE *e = user;
    (void)description; (void)name;
    if (IsEqualGUID(&d->deviceGUID, e->wanted)) e->found = TRUE;
    return D3DENUMRET_OK;
}

int main(int argc, char **argv)
{
    BOOL hal;
    ENUM_DEVICE e;
    IDirect3D7 *d3d = NULL;
    DDSURFACEDESC2 d = {0};
    D3DVIEWPORT7 vp = {0, 0, Side, Side, 0, 1};
    D3DDEVICEDESC7 caps;
    HWND window;
    BOOL tnl;
    if (argc < 2 || argc > 3 || (strcmp(argv[1], "hal") && strcmp(argv[1], "rgb") && strcmp(argv[1], "tnl")) ||
        (argc == 3 && strcmp(argv[2], "xyz") && strcmp(argv[2], "persp"))) {
        puts("Usage: tpixels hal|rgb|tnl [xyz|persp]");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    tnl = !strcmp(argv[1], "tnl");
    hal = tnl || !strcmp(argv[1], "hal");
    xyz = argc == 3;
    perspective = xyz && !strcmp(argv[2], "persp");
    e.wanted = tnl ? &IID_IDirect3DTnLHalDevice : hal ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice;
    e.found = FALSE;
    window = CreateWindowExA(0, "STATIC", "Direct3D 7 texture test", WS_POPUP, 0, 0, Side, Side,
                             NULL, NULL, GetModuleHandleA(NULL), NULL);
    displayDc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    printf("TPIX begin path=%s%s\n", argv[1], perspective ? " persp" : xyz ? " xyz" : "");
    if (!window || !call("DirectDrawCreateEx", DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) ||
        !call("cooperative level", IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL)) ||
        !call("IDirect3D7", IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d)) ||
        !call("enumerate", IDirect3D7_EnumDevices(d3d, device_info, &e)) || !e.found) goto done;
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = Side;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hal ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xff0000; d.ddpfPixelFormat.dwGBitMask = 0xff00; d.ddpfPixelFormat.dwBBitMask = 0xff;
    if (!call("render target", IDirectDraw7_CreateSurface(dd, &d, &target, NULL)) ||
        !call("CreateDevice", IDirect3D7_CreateDevice(d3d, e.wanted, target, &device)) ||
        !call("device caps", IDirect3DDevice7_GetCaps(device, &caps)) ||
        !call("viewport", IDirect3DDevice7_SetViewport(device, &vp))) goto done;
    /* The automatic checks (drvcheck.h): the "rgb" run on Microsoft's software device keeps its
     * pictures as the reference; the hardware runs compare with them and watch what the driver skips. */
    refFolder = !xyz ? "refpix7" : perspective ? "refpix7p" : "refpix7x";
    refMode = hal ? RefCompare : RefWrite;
    if (!hal) skipsOff = 1;
    skips_start();
    if (xyz) {
        /* Object x and y in pixels: clip x = x/64 - 1, clip y = 1 - y/64. */
        D3DMATRIX identity = {0}, projection = {0};
        identity._11 = identity._22 = identity._33 = identity._44 = 1;
        if (perspective) {
            /* Right-handed, as Quake uses: near 1, far 100, w = -z. */
            projection._11 = projection._22 = 1;
            projection._33 = projection._43 = 100.0f / (1 - 100);
            projection._34 = -1;
        } else {
            projection._11 = 1.0f / 64; projection._22 = -1.0f / 64; projection._33 = 1;
            projection._41 = -1; projection._42 = 1; projection._44 = 1;
        }
        if (!call("world", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_WORLD, &identity)) ||
            !call("view", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_VIEW, &identity)) ||
            !call("projection", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_PROJECTION, &projection)))
            goto done;
    }
    printf("TPIX device guid=%08lx caps=%08lx texture_caps=%08lx filter=%08lx\n", caps.deviceGUID.Data1,
           caps.dwDevCaps, caps.dpcTriCaps.dwTextureCaps, caps.dpcTriCaps.dwTextureFilterCaps);
    formats();
    modulate();
    alpha_test();
    blend("blend-alpha", D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, 0x80ff0000, 0x800033);
    blend("blend-add", D3DBLEND_ONE, D3DBLEND_ONE, 0xff404040, 0x4040a6);
    cull("cull-none", D3DCULL_NONE, TRUE, TRUE);
    cull("cull-ccw", D3DCULL_CCW, TRUE, FALSE);
    cull("cull-cw", D3DCULL_CW, FALSE, TRUE);
    flat();
    rewrite();
    hardware_memory(hal);
done:
    if (device) IDirect3DDevice7_Release(device);
    if (target) IDirectDrawSurface7_Release(target);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) IDirectDraw7_Release(dd);
    if (displayDc) DeleteDC(displayDc);
    DestroyWindow(window);
    {
        char refText[40];
        refText[0] = 0;
        if (refMode == RefCompare) {
            if (refCompared) sprintf(refText, " ref_compared=%d ref_failed=%d", refCompared, refFailed);
            else strcpy(refText, " ref=unavailable");
        }
        printf("TPIX end path=%s%s pass=%d fail=%d%s%s result=%s\n", argv[1], perspective ? " persp" : xyz ? " xyz" : "", passed, failed,
               skips_summary(), refText, refMode == RefWrite ? "REFERENCE" : passed && !failed ? "PASS" : "FAIL");
    }
    return passed && !failed ? 0 : 1;
}
