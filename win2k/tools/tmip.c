/* Check Direct3D 7 mipmaps on Microsoft's RGB device, the HAL or the T&L
 * device. A 64x64 texture has seven levels of distinct flat colours; a
 * transformed quad drawn at 64 / 2^(k + 1/4) pixels puts the level of detail
 * a quarter level past k, where rounding down (Microsoft's RGB device) and to
 * the nearest level (hardware) both pick level k. The centre pixel shows
 * that level's colour, a mix of levels 2 and 3 with linear mipmap filtering
 * at 2.5, and level zero with no mipmap filter. The HAL and T&L devices also run a texture managed by the
 * runtime, whose video-memory copy is a separate mipmap chain. */
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Side = 128, Levels = 7 };
typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } VERTEX;
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
static const DWORD colours[Levels] = {
    0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff, 0xffffffff
};

static IDirectDraw7 *dd;
static IDirect3DDevice7 *device;
static IDirectDrawSurface7 *target;
static int passed, failed;

static BOOL call(const char *what, HRESULT hr)
{
    if (FAILED(hr)) printf("TMIP %s hr=%08lx\n", what, (DWORD)hr);
    return SUCCEEDED(hr);
}

static void stage(D3DTEXTURESTAGESTATETYPE s, DWORD v) { IDirect3DDevice7_SetTextureStageState(device, 0, s, v); }

/* A 64x64 mipmap chain, level k filled with colours[k]. */
static IDirectDrawSurface7 *mipmaps(BOOL managed)
{
    DDSURFACEDESC2 d = {0};
    IDirectDrawSurface7 *top = NULL, *level;
    DDSCAPS2 caps = {0};
    int k;
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT;
    d.dwWidth = d.dwHeight = 64;
    d.dwMipMapCount = Levels;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | (managed ? 0 : DDSCAPS_SYSTEMMEMORY);
    if (managed) d.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xff0000; d.ddpfPixelFormat.dwGBitMask = 0xff00;
    d.ddpfPixelFormat.dwBBitMask = 0xff; d.ddpfPixelFormat.dwRGBAlphaBitMask = 0xff000000;
    if (!call("mipmap texture", IDirectDraw7_CreateSurface(dd, &d, &top, NULL))) return NULL;
    caps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
    level = top;
    IDirectDrawSurface7_AddRef(level);
    for (k = 0; k < Levels && level; ++k) {
        IDirectDrawSurface7 *next = NULL;
        DDSURFACEDESC2 l = {0};
        DWORD x, y;
        l.dwSize = sizeof(l);
        if (call("lock level", IDirectDrawSurface7_Lock(level, NULL, &l, DDLOCK_WAIT, NULL))) {
            for (y = 0; y < l.dwHeight; ++y)
                for (x = 0; x < l.dwWidth; ++x)
                    ((DWORD *)((BYTE *)l.lpSurface + y * l.lPitch))[x] = colours[k];
            IDirectDrawSurface7_Unlock(level, NULL);
        }
        if (k + 1 < Levels) IDirectDrawSurface7_GetAttachedSurface(level, &caps, &next);
        IDirectDrawSurface7_Release(level);
        level = next;
    }
    if (level) IDirectDrawSurface7_Release(level);
    return top;
}

static DWORD average(DWORD a, DWORD b)
{
    return (((a >> 16 & 255) + (b >> 16 & 255) + 1) / 2) << 16 | (((a >> 8 & 255) + (b >> 8 & 255) + 1) / 2) << 8 |
           ((a & 255) + (b & 255) + 1) / 2;
}

/* Draw the texture on a centred square of the given side and check its centre. */
static void run(const char *name, IDirectDrawSurface7 *t, DWORD mip, float size, DWORD want, int tolerance)
{
    VERTEX v[4];
    DDSURFACEDESC2 d = {0};
    DWORD got = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        v[i].x = 64 + (i & 1 ? size : -size) / 2; v[i].y = 64 + (i & 2 ? size : -size) / 2;
        v[i].z = 0.5f; v[i].rhw = 1; v[i].diffuse = 0xffffffff;
        v[i].u = i & 1 ? 1.0f : 0.0f; v[i].v = i & 2 ? 1.0f : 0.0f;
    }
    IDirect3DDevice7_SetTexture(device, 0, t);
    stage(D3DTSS_MIPFILTER, mip);
    if (!call("clear", IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000066, 1, 0)) ||
        !call("begin", IDirect3DDevice7_BeginScene(device)) ||
        !call("draw", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, FVF, v, 4, 0)) ||
        !call("end", IDirect3DDevice7_EndScene(device))) { ++failed; return; }
    d.dwSize = sizeof(d);
    if (call("lock", IDirectDrawSurface7_Lock(target, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) {
        got = ((const DWORD *)((const BYTE *)d.lpSurface + 64 * d.lPitch))[64] & 0xffffff;
        IDirectDrawSurface7_Unlock(target, NULL);
    }
    want &= 0xffffff;
    i = abs((int)(got >> 16) - (int)(want >> 16)) <= tolerance &&
        abs((int)((got >> 8) & 255) - (int)((want >> 8) & 255)) <= tolerance &&
        abs((int)(got & 255) - (int)(want & 255)) <= tolerance;
    printf("TMIP case=%s expected=%06lx got=%06lx %s\n", name, want, got, i ? "ok" : "wrong");
    if (i) ++passed; else ++failed;
}

static void cases(IDirectDrawSurface7 *t, const char *prefix)
{
    char name[64];
    sprintf(name, "%s-none", prefix);
    run(name, t, D3DTFP_NONE, 8, colours[0], 3);
    sprintf(name, "%s-point-level0", prefix);
    run(name, t, D3DTFP_POINT, 64, colours[0], 3);
    sprintf(name, "%s-point-level1", prefix);
    run(name, t, D3DTFP_POINT, 64 / 2.3784142f, colours[1], 3);
    sprintf(name, "%s-point-level3", prefix);
    run(name, t, D3DTFP_POINT, 64 / 9.5136569f, colours[3], 3);
    sprintf(name, "%s-linear-level2.5", prefix);
    /* Implementations differ by a few hundredths in where 2.5 falls. */
    run(name, t, D3DTFP_LINEAR, 64 / 5.656854f, average(colours[2], colours[3]), 32);
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
    IDirectDrawSurface7 *t = NULL, *managed = NULL;
    DDSURFACEDESC2 d = {0};
    D3DVIEWPORT7 vp = {0, 0, Side, Side, 0, 1};
    D3DDEVICEDESC7 caps;
    HWND window;
    BOOL hal, tnl;
    if (argc != 2 || (strcmp(argv[1], "rgb") && strcmp(argv[1], "hal") && strcmp(argv[1], "tnl"))) {
        puts("Usage: tmip rgb|hal|tnl");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    tnl = !strcmp(argv[1], "tnl");
    hal = tnl || !strcmp(argv[1], "hal");
    e.wanted = tnl ? &IID_IDirect3DTnLHalDevice : hal ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice;
    e.found = FALSE;
    window = CreateWindowExA(0, "STATIC", "Direct3D 7 mipmap test", WS_POPUP, 0, 0, Side, Side,
                             NULL, NULL, GetModuleHandleA(NULL), NULL);
    printf("TMIP begin path=%s\n", argv[1]);
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
    printf("TMIP device guid=%08lx filter=%08lx\n", caps.deviceGUID.Data1, caps.dpcTriCaps.dwTextureFilterCaps);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_DITHERENABLE, FALSE);
    stage(D3DTSS_COLOROP, D3DTOP_SELECTARG1); stage(D3DTSS_COLORARG1, D3DTA_TEXTURE);
    stage(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); stage(D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    stage(D3DTSS_MAGFILTER, D3DTFG_POINT); stage(D3DTSS_MINFILTER, D3DTFN_POINT);
    stage(D3DTSS_ADDRESS, D3DTADDRESS_CLAMP);
    IDirect3DDevice7_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    t = mipmaps(FALSE);
    if (!t) { ++failed; goto done; }
    cases(t, "system");
    if (hal) {
        managed = mipmaps(TRUE);
        if (managed) cases(managed, "managed"); else ++failed;
    }
    IDirect3DDevice7_SetTexture(device, 0, NULL);
done:
    if (t) IDirectDrawSurface7_Release(t);
    if (managed) IDirectDrawSurface7_Release(managed);
    if (device) IDirect3DDevice7_Release(device);
    if (target) IDirectDrawSurface7_Release(target);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) IDirectDraw7_Release(dd);
    DestroyWindow(window);
    printf("TMIP end path=%s pass=%d fail=%d result=%s\n", argv[1], passed, failed,
           passed && !failed ? "PASS" : "FAIL");
    return passed && !failed ? 0 : 1;
}
