/* Check Direct3D 7 fog on Microsoft's RGB device, the HAL device or the
 * driver's T&L device. Each case fogs a blue quad toward red at one depth
 * and compares pixels inside it with the fog factor Direct3D's formulas
 * give: transformed vertices with the factor in the specular alpha or with
 * table fog from z, and object-space vertices with vertex fog (linear,
 * exponential, range) or table fog from w under a perspective projection.
 * Every vertex of a quad has the same depth, so the quad is one colour. */
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Side = 128 };
#define FOG_COLOR 0xffff0000UL
#define QUAD_COLOR 0xff0000ffUL
typedef struct { float x, y, z, rhw; DWORD diffuse, specular; } TL_VERTEX;
#define TL_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR)
typedef struct { float x, y, z; DWORD diffuse, specular; } OBJECT_VERTEX;
#define OBJECT_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR)

static IDirectDraw7 *dd;
static IDirect3DDevice7 *device;
static IDirectDrawSurface7 *target;
static int passed, failed;
static D3DMATRIX ortho, perspective;
static DWORD rasterCaps;
static BOOL tnl;

static BOOL call(const char *what, HRESULT hr)
{
    if (FAILED(hr)) printf("TFOG %s hr=%08lx\n", what, (DWORD)hr);
    return SUCCEEDED(hr);
}

static void set(D3DRENDERSTATETYPE s, DWORD v) { IDirect3DDevice7_SetRenderState(device, s, v); }
static void setf(D3DRENDERSTATETYPE s, float v) { DWORD d; memcpy(&d, &v, sizeof(d)); set(s, d); }

/* Blend the quad colour toward the fog colour by one minus the factor. */
static DWORD fogged(float f)
{
    DWORD r = (DWORD)((1 - f) * 255 + 0.5f), b = (DWORD)(f * 255 + 0.5f);
    return r << 16 | b;
}

static float factor(D3DFOGMODE mode, float d, float start, float end, float density)
{
    float f;
    switch (mode) {
    case D3DFOG_LINEAR: f = (end - d) / (end - start); break;
    case D3DFOG_EXP: f = (float)exp(-density * d); break;
    default: f = (float)exp(-(density * d) * (density * d)); break;
    }
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void check(const char *name, DWORD want)
{
    static const int at[][2] = { {64, 64}, {40, 40}, {88, 88}, {40, 88} };
    DDSURFACEDESC2 d = {0};
    DWORD got = 0;
    int i, good = 0;
    d.dwSize = sizeof(d);
    if (call("lock", IDirectDrawSurface7_Lock(target, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) {
        for (i = 0; i < 4; ++i) {
            DWORD p = ((const DWORD *)((const BYTE *)d.lpSurface + at[i][1] * d.lPitch))[at[i][0]] & 0xffffff;
            if (abs((int)(p >> 16) - (int)(want >> 16)) <= 3 &&
                abs((int)((p >> 8) & 255) - (int)((want >> 8) & 255)) <= 3 &&
                abs((int)(p & 255) - (int)(want & 255)) <= 3) ++good;
            else if (!got) got = p | 0x1000000;
        }
        IDirectDrawSurface7_Unlock(target, NULL);
    }
    printf("TFOG case=%s expected=%06lx samples=%d/4", name, want, good);
    if (got) printf(" got=%06lx", got & 0xffffff);
    putchar('\n');
    if (good == 4) ++passed; else ++failed;
}

static BOOL scene(D3DPRIMITIVETYPE type, DWORD fvf, void *v, DWORD n)
{
    return call("clear", IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000066, 1, 0)) &&
           call("begin", IDirect3DDevice7_BeginScene(device)) &&
           call("draw", IDirect3DDevice7_DrawPrimitive(device, type, fvf, v, n, 0)) &&
           call("end", IDirect3DDevice7_EndScene(device));
}

static void fog_states(D3DFOGMODE table, D3DFOGMODE vertex, BOOL range, float start, float end, float density)
{
    set(D3DRENDERSTATE_FOGENABLE, TRUE);
    set(D3DRENDERSTATE_FOGCOLOR, FOG_COLOR);
    set(D3DRENDERSTATE_FOGTABLEMODE, table);
    set(D3DRENDERSTATE_FOGVERTEXMODE, vertex);
    set(D3DRENDERSTATE_RANGEFOGENABLE, range);
    setf(D3DRENDERSTATE_FOGSTART, start);
    setf(D3DRENDERSTATE_FOGEND, end);
    setf(D3DRENDERSTATE_FOGDENSITY, density);
}

/* A transformed quad covering 16..112 at depth z with the given specular alpha. */
static void transformed(const char *name, float z, DWORD specular, DWORD want)
{
    TL_VERTEX v[4];
    int i;
    for (i = 0; i < 4; ++i) {
        v[i].x = i & 1 ? 112.0f : 16.0f; v[i].y = i & 2 ? 112.0f : 16.0f;
        v[i].z = z; v[i].rhw = 1;
        v[i].diffuse = QUAD_COLOR; v[i].specular = specular;
    }
    if (scene(D3DPT_TRIANGLESTRIP, TL_FVF, v, 4)) check(name, want); else ++failed;
}

/* An object-space quad at camera depth z, of half width h. */
static void object(const char *name, const D3DMATRIX *projection, float z, float h, DWORD want)
{
    OBJECT_VERTEX v[4];
    int i;
    if (!call("projection", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_PROJECTION,
                                                         (D3DMATRIX *)projection))) { ++failed; return; }
    for (i = 0; i < 4; ++i) {
        v[i].x = i & 1 ? h : -h; v[i].y = i & 2 ? -h : h; v[i].z = z;
        v[i].diffuse = QUAD_COLOR; v[i].specular = 0xff000000;
    }
    if (scene(D3DPT_TRIANGLESTRIP, OBJECT_FVF, v, 4)) check(name, want); else ++failed;
}

static void cases(void)
{
    float d;
    fog_states(D3DFOG_NONE, D3DFOG_NONE, FALSE, 0, 1, 1);
    transformed("alpha", 0.5f, 0x40000000, fogged(0x40 / 255.0f));
    fog_states(D3DFOG_LINEAR, D3DFOG_NONE, FALSE, 0.2f, 0.8f, 1);
    transformed("table-linear-z", 0.5f, 0, fogged(factor(D3DFOG_LINEAR, 0.5f, 0.2f, 0.8f, 1)));
    fog_states(D3DFOG_EXP, D3DFOG_NONE, FALSE, 0, 1, 2);
    transformed("table-exp-z", 0.5f, 0, fogged(factor(D3DFOG_EXP, 0.5f, 0, 1, 2)));
    fog_states(D3DFOG_EXP2, D3DFOG_NONE, FALSE, 0, 1, 1);
    transformed("table-exp2-z", 0.5f, 0, fogged(factor(D3DFOG_EXP2, 0.5f, 0, 1, 1)));
    /* Object space: camera z 10, corners 48 off the axis under ortho. */
    fog_states(D3DFOG_NONE, D3DFOG_LINEAR, FALSE, 5, 15, 1);
    object("vertex-linear", &ortho, 10, 48, fogged(factor(D3DFOG_LINEAR, 10, 5, 15, 1)));
    fog_states(D3DFOG_NONE, D3DFOG_EXP, FALSE, 0, 1, 0.1f);
    object("vertex-exp", &ortho, 10, 48, fogged(factor(D3DFOG_EXP, 10, 0, 1, 0.1f)));
    /* Range fog uses the distance to each vertex. Microsoft's software vertex
     * path ignores it, so only a device doing its own vertex fog checks it. */
    d = (float)sqrt(48.0 * 48 * 2 + 10.0 * 10);
    fog_states(D3DFOG_NONE, D3DFOG_LINEAR, TRUE, 0, 2 * d, 1);
    if (tnl && (rasterCaps & D3DPRASTERCAPS_FOGRANGE)) object("vertex-range", &ortho, 10, 48, fogged(0.5f));
    else printf("TFOG case=vertex-range not applicable: vertex fog in software\n");
    fog_states(D3DFOG_LINEAR, D3DFOG_NONE, FALSE, 0, 0.2f, 1);
    object("table-linear-ortho-z", &ortho, 10, 48, fogged(factor(D3DFOG_LINEAR, 0.1f, 0, 0.2f, 1)));
    /* With a perspective projection, a device offering w-based fog uses w. */
    fog_states(D3DFOG_LINEAR, D3DFOG_NONE, FALSE, 5, 15, 1);
    if (rasterCaps & D3DPRASTERCAPS_WFOG)
        object("table-linear-w", &perspective, 10, 6, fogged(factor(D3DFOG_LINEAR, 10, 5, 15, 1)));
    else printf("TFOG case=table-linear-w not applicable: no w-based fog\n");
    set(D3DRENDERSTATE_FOGENABLE, FALSE);
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
    ENUM_DEVICE e;
    IDirect3D7 *d3d = NULL;
    DDSURFACEDESC2 d = {0};
    D3DVIEWPORT7 vp = {0, 0, Side, Side, 0, 1};
    D3DDEVICEDESC7 caps;
    D3DMATRIX identity;
    HWND window;
    BOOL hal;
    if (argc != 2 || (strcmp(argv[1], "rgb") && strcmp(argv[1], "hal") && strcmp(argv[1], "tnl"))) {
        puts("Usage: tfog rgb|hal|tnl");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    tnl = !strcmp(argv[1], "tnl");
    hal = tnl || !strcmp(argv[1], "hal");
    e.wanted = tnl ? &IID_IDirect3DTnLHalDevice : hal ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice;
    e.found = FALSE;
    window = CreateWindowExA(0, "STATIC", "Direct3D 7 fog test", WS_POPUP, 0, 0, Side, Side,
                             NULL, NULL, GetModuleHandleA(NULL), NULL);
    printf("TFOG begin path=%s\n", argv[1]);
    if (!window || !call("DirectDrawCreateEx", DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) ||
        !call("cooperative level", IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL)) ||
        !call("IDirect3D7", IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d)) ||
        !call("enumerate", IDirect3D7_EnumDevices(d3d, device_info, &e)) || !e.found) { ++failed; goto done; }
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = Side;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hal ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xff0000; d.ddpfPixelFormat.dwGBitMask = 0xff00; d.ddpfPixelFormat.dwBBitMask = 0xff;
    memset(&identity, 0, sizeof(identity));
    identity._11 = identity._22 = identity._33 = identity._44 = 1;
    /* Ortho: x and y in pixels from the centre, z 0..100 to 0..1. Perspective:
     * left-handed, 90 degrees, near 1, far 100, w = z. */
    memset(&ortho, 0, sizeof(ortho));
    ortho._11 = ortho._22 = 1.0f / 64; ortho._33 = 1.0f / 100; ortho._44 = 1;
    memset(&perspective, 0, sizeof(perspective));
    perspective._11 = perspective._22 = 1;
    perspective._33 = 100.0f / 99; perspective._34 = 1; perspective._43 = -100.0f / 99;
    if (!call("render target", IDirectDraw7_CreateSurface(dd, &d, &target, NULL)) ||
        !call("CreateDevice", IDirect3D7_CreateDevice(d3d, e.wanted, target, &device)) ||
        !call("device caps", IDirect3DDevice7_GetCaps(device, &caps)) ||
        !call("viewport", IDirect3DDevice7_SetViewport(device, &vp)) ||
        !call("world", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_WORLD, &identity)) ||
        !call("view", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_VIEW, &identity)) ||
        !call("projection", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_PROJECTION, &ortho))) {
        ++failed;
        goto done;
    }
    printf("TFOG device guid=%08lx raster_caps=%08lx vertex_caps=%08lx\n", caps.deviceGUID.Data1,
           caps.dpcTriCaps.dwRasterCaps, caps.dwVertexProcessingCaps);
    rasterCaps = caps.dpcTriCaps.dwRasterCaps;
    set(D3DRENDERSTATE_LIGHTING, FALSE);
    set(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    set(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    set(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    set(D3DRENDERSTATE_DITHERENABLE, FALSE);
    set(D3DRENDERSTATE_SPECULARENABLE, FALSE);
    IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice7_SetTexture(device, 0, NULL);
    cases();
done:
    if (device) IDirect3DDevice7_Release(device);
    if (target) IDirectDrawSurface7_Release(target);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) IDirectDraw7_Release(dd);
    DestroyWindow(window);
    printf("TFOG end path=%s pass=%d fail=%d result=%s\n", argv[1], passed, failed,
           passed && !failed ? "PASS" : "FAIL");
    return passed && !failed ? 0 : 1;
}
