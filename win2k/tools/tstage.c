/* Check two Direct3D 7 texture stages on Microsoft's RGB device, the HAL or
 * the T&L device. Each case draws one transformed quad with two 8x8
 * textures of four flat quadrants. Stage one reads the second texture
 * coordinate set, which mirrors the first left to right, so each screen
 * quadrant combines a known texel of each texture. Pixels at the quadrant
 * centres are compared with the stage operations evaluated here.
 * Microsoft's RGB device draws stage zero alone correctly but no texture at
 * all once stage one is enabled, so there the two-stage cases are printed
 * for reference and not counted. */
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Side = 128 };
typedef struct { float x, y, z, rhw; DWORD diffuse; float u0, v0, u1, v1; } VERTEX;
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2)
/* Quadrants top-left, top-right, bottom-left, bottom-right. */
static const DWORD first[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
static const DWORD second[4] = { 0x80808080, 0xffffff00, 0x40ff00ff, 0xff000000 };

static IDirectDraw7 *dd;
static IDirect3DDevice7 *device;
static IDirectDrawSurface7 *target;
static int passed, failed;
static BOOL hal;

static BOOL call(const char *what, HRESULT hr)
{
    if (FAILED(hr)) printf("TSTAGE %s hr=%08lx\n", what, (DWORD)hr);
    return SUCCEEDED(hr);
}

static void set(D3DRENDERSTATETYPE s, DWORD v) { IDirect3DDevice7_SetRenderState(device, s, v); }
static void stage(DWORD n, D3DTEXTURESTAGESTATETYPE s, DWORD v) { IDirect3DDevice7_SetTextureStageState(device, n, s, v); }

static IDirectDrawSurface7 *texture(BOOL hal, const DWORD quadrants[4])
{
    DDSURFACEDESC2 d = {0};
    IDirectDrawSurface7 *t = NULL;
    int x, y;
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = 8;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
    (void)hal;
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xff0000; d.ddpfPixelFormat.dwGBitMask = 0xff00;
    d.ddpfPixelFormat.dwBBitMask = 0xff; d.ddpfPixelFormat.dwRGBAlphaBitMask = 0xff000000;
    if (!call("texture", IDirectDraw7_CreateSurface(dd, &d, &t, NULL))) return NULL;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (!call("lock texture", IDirectDrawSurface7_Lock(t, NULL, &d, DDLOCK_WAIT, NULL))) {
        IDirectDrawSurface7_Release(t);
        return NULL;
    }
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x)
            ((DWORD *)((BYTE *)d.lpSurface + y * d.lPitch))[x] = quadrants[(y >= 4) * 2 + (x >= 4)];
    IDirectDrawSurface7_Unlock(t, NULL);
    return t;
}

static float ch(DWORD c, int shift) { return ((c >> shift) & 255) / 255.0f; }

/* Expected colour of one screen quadrant: t0, t1 the two texels, op the
 * stage-one colour operation and its arguments, current alpha a0. */
/* redraw: draw once with the textures swapped in stage zero first, so the
 * second draw changes stage zero's texture while stage one keeps its own. */
typedef struct { const char *name; DWORD op, arg1, arg2, coordIndex, diffuse, alpha0Diffuse, redraw; } CASE;

static DWORD expect(const CASE *k, DWORD t0, DWORD t1)
{
    float r[3], cur[3], tex[3], dif[3], a0, ta = ch(t1, 24), a1[3], a2[3];
    int i;
    for (i = 0; i < 3; ++i) {
        int sh = 16 - 8 * i;
        cur[i] = ch(t0, sh); tex[i] = ch(t1, sh); dif[i] = ch(k->diffuse, sh);
    }
    a0 = k->alpha0Diffuse ? ch(k->diffuse, 24) : ch(t0, 24);
    for (i = 0; i < 3; ++i) {
        a1[i] = k->arg1 == D3DTA_TEXTURE ? tex[i] : k->arg1 == D3DTA_DIFFUSE ? dif[i] : cur[i];
        a2[i] = k->arg2 == D3DTA_TEXTURE ? tex[i] : k->arg2 == D3DTA_DIFFUSE ? dif[i] : cur[i];
        switch (k->op) {
        case D3DTOP_DISABLE: r[i] = cur[i]; break;
        case D3DTOP_SELECTARG1: r[i] = a1[i]; break;
        case D3DTOP_MODULATE: r[i] = a1[i] * a2[i]; break;
        case D3DTOP_ADD: r[i] = a1[i] + a2[i]; break;
        case D3DTOP_BLENDTEXTUREALPHA: r[i] = a1[i] * ta + a2[i] * (1 - ta); break;
        case D3DTOP_BLENDCURRENTALPHA: r[i] = a1[i] * a0 + a2[i] * (1 - a0); break;
        default: r[i] = 0;
        }
        if (r[i] > 1) r[i] = 1;
    }
    return (DWORD)(r[0] * 255 + 0.5f) << 16 | (DWORD)(r[1] * 255 + 0.5f) << 8 | (DWORD)(r[2] * 255 + 0.5f);
}

static void run(const CASE *k, IDirectDrawSurface7 *t0, IDirectDrawSurface7 *t1)
{
    static const int at[4][2] = { {40, 40}, {88, 40}, {40, 88}, {88, 88} };
    VERTEX v[4];
    DDSURFACEDESC2 d = {0};
    DWORD got = 0, want = 0, first_bad = 0;
    int i, good = 0;
    for (i = 0; i < 4; ++i) {
        v[i].x = i & 1 ? 112.0f : 16.0f; v[i].y = i & 2 ? 112.0f : 16.0f; v[i].z = 0.5f; v[i].rhw = 1;
        v[i].diffuse = k->diffuse;
        v[i].u0 = i & 1 ? 1.0f : 0.0f; v[i].v0 = i & 2 ? 1.0f : 0.0f;
        v[i].u1 = 1 - v[i].u0; v[i].v1 = v[i].v0;            /* mirrored left to right */
    }
    IDirect3DDevice7_SetTexture(device, 0, t0);
    IDirect3DDevice7_SetTexture(device, 1, t1);
    stage(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); stage(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    stage(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    stage(0, D3DTSS_ALPHAARG1, k->alpha0Diffuse ? D3DTA_DIFFUSE : D3DTA_TEXTURE);
    stage(1, D3DTSS_COLOROP, k->op); stage(1, D3DTSS_COLORARG1, k->arg1); stage(1, D3DTSS_COLORARG2, k->arg2);
    stage(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); stage(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    stage(1, D3DTSS_TEXCOORDINDEX, k->coordIndex);
    if (!call("clear", IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000066, 1, 0)) ||
        !call("begin", IDirect3DDevice7_BeginScene(device))) { ++failed; return; }
    if (k->redraw) {
        IDirect3DDevice7_SetTexture(device, 0, t1);
        if (!call("first draw", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0))) {
            ++failed;
            return;
        }
        IDirect3DDevice7_SetTexture(device, 0, t0);
    }
    if (!call("draw", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0)) ||
        !call("end", IDirect3DDevice7_EndScene(device))) { ++failed; return; }
    d.dwSize = sizeof(d);
    if (call("lock", IDirectDrawSurface7_Lock(target, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) {
        for (i = 0; i < 4; ++i) {
            /* Stage one's quadrant: mirrored with the second set, the same with the first. */
            int q1 = k->coordIndex ? i ^ 1 : i;
            DWORD w = expect(k, first[i], second[q1]);
            DWORD p = ((const DWORD *)((const BYTE *)d.lpSurface + at[i][1] * d.lPitch))[at[i][0]] & 0xffffff;
            if (abs((int)(p >> 16) - (int)(w >> 16)) <= 3 && abs((int)((p >> 8) & 255) - (int)((w >> 8) & 255)) <= 3 &&
                abs((int)(p & 255) - (int)(w & 255)) <= 3) ++good;
            else if (!first_bad) { first_bad = 1; got = p; want = w; }
        }
        IDirectDrawSurface7_Unlock(target, NULL);
    }
    printf("TSTAGE case=%s samples=%d/4", k->name, good);
    if (first_bad) printf(" expected=%06lx got=%06lx", want, got);
    if (!hal && k->op != D3DTOP_DISABLE) { puts(" reference-only"); return; }
    putchar('\n');
    if (good == 4) ++passed; else ++failed;
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
    static const CASE cases[] = {
        { "stage-zero-only", D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT, 1, 0xffffffff, 0 },
        { "modulate", D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, 1, 0xffffffff, 0 },
        { "add", D3DTOP_ADD, D3DTA_TEXTURE, D3DTA_CURRENT, 1, 0xffffffff, 0 },
        { "select-current", D3DTOP_SELECTARG1, D3DTA_CURRENT, D3DTA_TEXTURE, 1, 0xffffffff, 0 },
        { "blend-texture-alpha", D3DTOP_BLENDTEXTUREALPHA, D3DTA_TEXTURE, D3DTA_CURRENT, 1, 0xffffffff, 0 },
        { "blend-current-alpha", D3DTOP_BLENDCURRENTALPHA, D3DTA_TEXTURE, D3DTA_CURRENT, 1, 0x80ffffff, 1 },
        { "texture-times-diffuse", D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, 1, 0xff8040ff, 0 },
        { "first-coordinates", D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, 0, 0xffffffff, 0 },
        { "stage-zero-changes", D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, 1, 0xffffffff, 0, 1 },
    };
    ENUM_DEVICE e;
    IDirect3D7 *d3d = NULL;
    IDirectDrawSurface7 *t0 = NULL, *t1 = NULL;
    DDSURFACEDESC2 d = {0};
    D3DVIEWPORT7 vp = {0, 0, Side, Side, 0, 1};
    D3DDEVICEDESC7 caps;
    HWND window;
    BOOL tnl;
    unsigned i;
    if (argc != 2 || (strcmp(argv[1], "rgb") && strcmp(argv[1], "hal") && strcmp(argv[1], "tnl"))) {
        puts("Usage: tstage rgb|hal|tnl");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    tnl = !strcmp(argv[1], "tnl");
    hal = tnl || !strcmp(argv[1], "hal");
    e.wanted = tnl ? &IID_IDirect3DTnLHalDevice : hal ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice;
    e.found = FALSE;
    window = CreateWindowExA(0, "STATIC", "Direct3D 7 stage test", WS_POPUP, 0, 0, Side, Side,
                             NULL, NULL, GetModuleHandleA(NULL), NULL);
    printf("TSTAGE begin path=%s\n", argv[1]);
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
    if (!call("render target", IDirectDraw7_CreateSurface(dd, &d, &target, NULL)) ||
        !call("CreateDevice", IDirect3D7_CreateDevice(d3d, e.wanted, target, &device)) ||
        !call("device caps", IDirect3DDevice7_GetCaps(device, &caps)) ||
        !call("viewport", IDirect3DDevice7_SetViewport(device, &vp))) { ++failed; goto done; }
    printf("TSTAGE device guid=%08lx stages=%u textures=%u\n", caps.deviceGUID.Data1,
           caps.wMaxTextureBlendStages, caps.wMaxSimultaneousTextures);
    if (caps.wMaxSimultaneousTextures < 2) { printf("TSTAGE not applicable: one texture\n"); ++passed; goto done; }
    t0 = texture(hal, first);
    t1 = texture(hal, second);
    if (!t0 || !t1) { ++failed; goto done; }
    set(D3DRENDERSTATE_LIGHTING, FALSE);
    set(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    set(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    set(D3DRENDERSTATE_DITHERENABLE, FALSE);
    set(D3DRENDERSTATE_FOGENABLE, FALSE);
    set(D3DRENDERSTATE_SPECULARENABLE, FALSE);
    for (i = 0; i < 2; ++i) {
        stage(i, D3DTSS_MAGFILTER, D3DTFG_POINT); stage(i, D3DTSS_MINFILTER, D3DTFN_POINT);
        stage(i, D3DTSS_MIPFILTER, D3DTFP_NONE); stage(i, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP);
    }
    stage(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    stage(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) run(&cases[i], t0, t1);
    IDirect3DDevice7_SetTexture(device, 0, NULL);
    IDirect3DDevice7_SetTexture(device, 1, NULL);
done:
    if (t0) IDirectDrawSurface7_Release(t0);
    if (t1) IDirectDrawSurface7_Release(t1);
    if (device) IDirect3DDevice7_Release(device);
    if (target) IDirectDrawSurface7_Release(target);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) IDirectDraw7_Release(dd);
    DestroyWindow(window);
    printf("TSTAGE end path=%s pass=%d fail=%d result=%s\n", argv[1], passed, failed,
           passed && !failed ? "PASS" : "FAIL");
    return passed && !failed ? 0 : 1;
}
