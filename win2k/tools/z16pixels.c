/* Test independently owned colour and 16-bit depth surfaces with the stock
 * Direct3D 7 hardware or RGB device. Only the offscreen colour image is read. */
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <string.h>

enum { Side = 64, Black = 0, Red = 0x00ff0000, Blue = 0x000000ff };
typedef struct {
    float x, y, z, rhw;
    DWORD color;
} TL_VERTEX;

static BOOL checked(const char *name, HRESULT hr)
{
    if (FAILED(hr))
        printf("Z16 %s hr=%08lx\n", name, (DWORD)hr);
    return SUCCEEDED(hr);
}

static HRESULT CALLBACK select_z16(DDPIXELFORMAT *f, void *out)
{
    if ((f->dwFlags & (DDPF_ZBUFFER | DDPF_STENCILBUFFER)) == DDPF_ZBUFFER &&
        f->dwZBufferBitDepth == 16) {
        *(DDPIXELFORMAT *)out = *f;
        return D3DENUMRET_CANCEL;
    }
    return D3DENUMRET_OK;
}

static BOOL triangle(IDirect3DDevice7 *device, BOOL nearer)
{
    float origin = nearer ? 8.f : 2.f, end = nearer ? 44.f : 62.f, z = nearer ? .25f : .75f;
    DWORD color = 0xff000000UL | (nearer ? Red : Blue);
    TL_VERTEX v[] = {
        {origin, origin, z, 1, color}, {end, origin, z, 1, color}, {origin, end, z, 1, color}};
    return checked("draw", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
                                                          D3DFVF_XYZRHW | D3DFVF_DIFFUSE, v, 3, 0));
}

static BOOL samples(IDirectDrawSurface7 *target, const char *name, DWORD overlap)
{
    static const unsigned x[] = {16, 4, 58}, y[] = {16, 4, 58};
    const DWORD expected[] = {overlap, Blue, Black};
    DDSURFACEDESC2 desc = {0};
    unsigned i, match = 0;
    desc.dwSize = sizeof(desc);
    if (!checked("lock",
                 IDirectDrawSurface7_Lock(target, NULL, &desc, DDLOCK_WAIT | DDLOCK_READONLY, NULL)))
        return FALSE;
    if (desc.lpSurface && desc.dwWidth == Side && desc.dwHeight == Side && desc.lPitch >= 4 * Side &&
        desc.ddpfPixelFormat.dwRGBBitCount == 32 && desc.ddpfPixelFormat.dwRBitMask == 0xff0000 &&
        desc.ddpfPixelFormat.dwGBitMask == 0xff00 && desc.ddpfPixelFormat.dwBBitMask == 0xff) {
        for (i = 0; i < 3; ++i) {
            const DWORD *row = (const DWORD *)((const BYTE *)desc.lpSurface + y[i] * desc.lPitch);
            DWORD got = row[x[i]] & 0xffffff;
            printf("Z16 case=%s xy=%u,%u expected=%06lx actual=%06lx\n", name, x[i], y[i], expected[i],
                   got);
            match += got == expected[i];
        }
    }
    return checked("unlock", IDirectDrawSurface7_Unlock(target, NULL)) && match == 3;
}

static BOOL pair(IDirect3DDevice7 *device, IDirectDrawSurface7 *target, BOOL nearFirst, BOOL writes)
{
    BOOL ok;
    const char *label = writes ? (nearFirst ? "near-first" : "far-first") : "writes-off";
    if (!checked("clear both",
                 IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000,
                                        1.f, 0)) ||
        !checked("depth write",
                 IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZWRITEENABLE, writes)) ||
        !checked("begin", IDirect3DDevice7_BeginScene(device)))
        return FALSE;
    ok = triangle(device, nearFirst) && triangle(device, !nearFirst);
    return checked("end", IDirect3DDevice7_EndScene(device)) && ok &&
           samples(target, label, writes ? Red : Blue);
}

int main(int argc, char **argv)
{
    BOOL hal, ok = FALSE;
    const GUID *id;
    HWND window;
    IDirectDraw7 *dd = NULL;
    IDirect3D7 *d3d = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDrawSurface7 *target = NULL, *depth = NULL;
    DDPIXELFORMAT format = {0};
    DDSURFACEDESC2 d = {0};
    D3DDEVICEDESC7 caps = {0};
    D3DVIEWPORT7 view = {0, 0, Side, Side, 0.f, 1.f};
    if (argc != 2 || (strcmp(argv[1], "hal") && strcmp(argv[1], "rgb")))
        return 2;
    hal = !strcmp(argv[1], "hal");
    id = hal ? &IID_IDirect3DHALDevice : &IID_IDirect3DRGBDevice;
    window = CreateWindowA("STATIC", "D3D7 depth pixel test", WS_POPUP, 0, 0, Side, Side, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!window ||
        !checked("DirectDrawCreateEx",
                 DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) ||
        !checked("cooperative", IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL)) ||
        !checked("Direct3D7", IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d)) ||
        !checked("enumerate depth", IDirect3D7_EnumZBufferFormats(d3d, id, select_z16, &format)))
        goto done;
    if (!format.dwSize) {
        puts("Z16 16-bit depth format missing");
        goto done;
    }
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = Side;
    d.ddsCaps.dwCaps =
        DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hal ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xff0000;
    d.ddpfPixelFormat.dwGBitMask = 0xff00;
    d.ddpfPixelFormat.dwBBitMask = 0xff;
    if (!checked("create target", IDirectDraw7_CreateSurface(dd, &d, &target, NULL)))
        goto done;
    d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | (hal ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    d.ddpfPixelFormat = format;
    if (!checked("create depth", IDirectDraw7_CreateSurface(dd, &d, &depth, NULL)) ||
        !checked("attach depth", IDirectDrawSurface7_AddAttachedSurface(target, depth)) ||
        !checked("create device", IDirect3D7_CreateDevice(d3d, id, target, &device)) ||
        !checked("get caps", IDirect3DDevice7_GetCaps(device, &caps)))
        goto done;
    printf("Z16 selected=%08lx hw=%u\n", caps.deviceGUID.Data1,
           !!(caps.dwDevCaps & D3DDEVCAPS_HWRASTERIZATION));
    if (!IsEqualGUID(id, &caps.deviceGUID) || !!(caps.dwDevCaps & D3DDEVCAPS_HWRASTERIZATION) != hal ||
        !checked("viewport", IDirect3DDevice7_SetViewport(device, &view)) ||
        !checked("cull",
                 IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE)) ||
        !checked("colour op",
                 IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1)) ||
        !checked("colour arg",
                 IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE)) ||
        !checked("enable depth",
                 IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE)) ||
        !checked("depth function",
                 IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL)))
        goto done;
    ok = pair(device, target, TRUE, TRUE) && pair(device, target, FALSE, TRUE) &&
         pair(device, target, TRUE, FALSE);
done:
    if (device)
        IDirect3DDevice7_Release(device);
    if (target)
        IDirectDrawSurface7_Release(target);
    if (depth)
        IDirectDrawSurface7_Release(depth);
    if (d3d)
        IDirect3D7_Release(d3d);
    if (dd)
        IDirectDraw7_Release(dd);
    if (window)
        DestroyWindow(window);
    printf("Z16 path=%s result=%s\n", argv[1], ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
