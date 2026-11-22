/* Check Direct3D 7 fixed-function lighting on Microsoft's RGB device or the
 * driver's T&L device. Each case lights one quad facing the camera and
 * compares pixels inside it with Direct3D's lighting equations, evaluated
 * here for a corner vertex. The corners sit symmetrically around the view
 * axis, and every light in these cases lies on that axis, so all four
 * corners light alike and Gouraud shading leaves the quad one colour. */
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

enum { Side = 128, Lights = 4 };
/* Corners at x, y = +-Half and z = Depth, in camera space (view is identity). */
#define Half 48.0f
#define Depth 10.0f
typedef struct { float x, y, z, nx, ny, nz; DWORD diffuse, specular; } LIT_VERTEX;
#define LIT_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR)

typedef struct {
    const char *name;
    DWORD ambient;                     /* D3DRENDERSTATE_AMBIENT */
    BOOL colorVertex, normalize, localViewer, specular;
    DWORD diffuseSource;               /* D3DMCS_* */
    DWORD vertexDiffuse;
    float normalZ;                     /* the normal is (0, 0, normalZ) */
    D3DMATERIAL7 material;
    int lights;
    D3DLIGHT7 light[Lights];
} CASE;

static IDirectDraw7 *dd;
static IDirect3DDevice7 *device;
static IDirectDrawSurface7 *target;
static int passed, failed;

static BOOL call(const char *what, HRESULT hr)
{
    if (FAILED(hr)) printf("TLIT %s hr=%08lx\n", what, (DWORD)hr);
    return SUCCEEDED(hr);
}

static void set(D3DRENDERSTATETYPE s, DWORD v) { IDirect3DDevice7_SetRenderState(device, s, v); }

static float dot(const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void normalize(float *v)
{
    float l = (float)sqrt(dot(v, v));
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
static float channel(DWORD c, int shift) { return ((c >> shift) & 255) / 255.0f; }
static float saturate(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Direct3D's vertex lighting in camera space for the corner (-Half, -Half,
 * Depth); returns the lit diffuse plus specular colour as packed RGB. */
static DWORD expected(const CASE *k)
{
    float P[3] = { -Half, -Half, Depth }, N[3] = { 0, 0, 0 }, V[3], A[3], D[3] = {0,0,0}, S[3] = {0,0,0};
    float Cd[3], Ca[3], Cs[3], Ce[3], out[3];
    int i;
    N[2] = k->normalZ;
    if (k->normalize) normalize(N);
    if (k->localViewer) { V[0] = -P[0]; V[1] = -P[1]; V[2] = -P[2]; normalize(V); }
    else { V[0] = 0; V[1] = 0; V[2] = -1; }
    A[0] = channel(k->ambient, 16); A[1] = channel(k->ambient, 8); A[2] = channel(k->ambient, 0);
    Cd[0] = k->material.diffuse.r; Cd[1] = k->material.diffuse.g; Cd[2] = k->material.diffuse.b;
    if (k->colorVertex && k->diffuseSource == D3DMCS_COLOR1) {
        Cd[0] = channel(k->vertexDiffuse, 16); Cd[1] = channel(k->vertexDiffuse, 8); Cd[2] = channel(k->vertexDiffuse, 0);
    }
    Ca[0] = k->material.ambient.r; Ca[1] = k->material.ambient.g; Ca[2] = k->material.ambient.b;
    Cs[0] = k->material.specular.r; Cs[1] = k->material.specular.g; Cs[2] = k->material.specular.b;
    Ce[0] = k->material.emissive.r; Ce[1] = k->material.emissive.g; Ce[2] = k->material.emissive.b;
    for (i = 0; i < k->lights; ++i) {
        const D3DLIGHT7 *l = &k->light[i];
        float L[3], H[3], factor = 1, ndotl;
        if (l->dltType == D3DLIGHT_DIRECTIONAL) {
            L[0] = -l->dvDirection.x; L[1] = -l->dvDirection.y; L[2] = -l->dvDirection.z;
            normalize(L);
        } else {
            float d;
            L[0] = l->dvPosition.x - P[0]; L[1] = l->dvPosition.y - P[1]; L[2] = l->dvPosition.z - P[2];
            d = (float)sqrt(dot(L, L));
            normalize(L);
            factor = d > l->dvRange ? 0 : 1 / (l->dvAttenuation0 + l->dvAttenuation1 * d + l->dvAttenuation2 * d * d);
            if (l->dltType == D3DLIGHT_SPOT) {
                float dir[3], rho, inner = (float)cos(l->dvTheta / 2), outer = (float)cos(l->dvPhi / 2);
                dir[0] = l->dvDirection.x; dir[1] = l->dvDirection.y; dir[2] = l->dvDirection.z;
                normalize(dir);
                rho = -dot(L, dir);
                if (rho <= outer) factor = 0;
                else if (rho < inner) factor *= (float)pow((rho - outer) / (inner - outer), l->dvFalloff);
            }
        }
        A[0] += l->dcvAmbient.r * factor; A[1] += l->dcvAmbient.g * factor; A[2] += l->dcvAmbient.b * factor;
        ndotl = dot(N, L);
        if (ndotl > 0) {
            float ndoth;
            D[0] += l->dcvDiffuse.r * ndotl * factor;
            D[1] += l->dcvDiffuse.g * ndotl * factor;
            D[2] += l->dcvDiffuse.b * ndotl * factor;
            H[0] = L[0] + V[0]; H[1] = L[1] + V[1]; H[2] = L[2] + V[2];
            normalize(H);
            ndoth = dot(N, H);
            if (ndoth > 0) {
                float s = (float)pow(ndoth, k->material.power) * factor;
                S[0] += l->dcvSpecular.r * s; S[1] += l->dcvSpecular.g * s; S[2] += l->dcvSpecular.b * s;
            }
        }
    }
    for (i = 0; i < 3; ++i) {
        out[i] = saturate(Ca[i] * A[i] + Cd[i] * D[i] + Ce[i]);
        if (k->specular) out[i] = saturate(out[i] + saturate(Cs[i] * S[i]));
    }
    return (DWORD)(out[0] * 255 + 0.5f) << 16 | (DWORD)(out[1] * 255 + 0.5f) << 8 | (DWORD)(out[2] * 255 + 0.5f);
}

static void run(const CASE *k)
{
    static const int at[][2] = { {64, 64}, {40, 40}, {88, 88}, {40, 88} };
    LIT_VERTEX v[6], c[4];
    DDSURFACEDESC2 d = {0};
    DWORD want = expected(k), got = 0;
    int i, good = 0;
    for (i = 0; i < 4; ++i) {
        c[i].x = i & 1 ? Half : -Half; c[i].y = i & 2 ? -Half : Half; c[i].z = Depth;
        c[i].nx = 0; c[i].ny = 0; c[i].nz = k->normalZ;
        c[i].diffuse = k->vertexDiffuse; c[i].specular = 0;
    }
    v[0] = c[0]; v[1] = c[1]; v[2] = c[2]; v[3] = c[1]; v[4] = c[3]; v[5] = c[2];
    set(D3DRENDERSTATE_LIGHTING, TRUE);
    set(D3DRENDERSTATE_AMBIENT, k->ambient);
    set(D3DRENDERSTATE_COLORVERTEX, k->colorVertex);
    set(D3DRENDERSTATE_DIFFUSEMATERIALSOURCE, k->diffuseSource);
    set(D3DRENDERSTATE_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    set(D3DRENDERSTATE_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    set(D3DRENDERSTATE_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    set(D3DRENDERSTATE_NORMALIZENORMALS, k->normalize);
    set(D3DRENDERSTATE_LOCALVIEWER, k->localViewer);
    set(D3DRENDERSTATE_SPECULARENABLE, k->specular);
    for (i = 0; i < Lights; ++i) {
        if (i < k->lights) call("light", IDirect3DDevice7_SetLight(device, i, (D3DLIGHT7 *)&k->light[i]));
        IDirect3DDevice7_LightEnable(device, i, i < k->lights);
    }
    if (!call("material", IDirect3DDevice7_SetMaterial(device, (D3DMATERIAL7 *)&k->material)) ||
        !call("clear", IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000066, 1, 0)) ||
        !call("begin", IDirect3DDevice7_BeginScene(device)) ||
        !call("draw", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST, LIT_FVF, v, 6, 0)) ||
        !call("end", IDirect3DDevice7_EndScene(device))) { ++failed; return; }
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
    printf("TLIT case=%s expected=%06lx samples=%d/4", k->name, want, good);
    if (got) printf(" got=%06lx", got & 0xffffff);
    putchar('\n');
    if (good == 4) ++passed; else ++failed;
}

static D3DLIGHT7 light(D3DLIGHTTYPE type, float r, float g, float b)
{
    D3DLIGHT7 l;
    memset(&l, 0, sizeof(l));
    l.dltType = type;
    l.dcvDiffuse.r = r; l.dcvDiffuse.g = g; l.dcvDiffuse.b = b; l.dcvDiffuse.a = 1;
    l.dvDirection.z = 1;               /* into the screen */
    l.dvPosition.z = -Depth;           /* on the axis, Depth in front of the camera's plane */
    l.dvRange = 1000;
    l.dvAttenuation0 = 1;
    l.dvFalloff = 1;
    return l;
}

static CASE base(const char *name)
{
    CASE k;
    memset(&k, 0, sizeof(k));
    k.name = name;
    k.localViewer = TRUE;
    k.diffuseSource = D3DMCS_COLOR1;
    k.vertexDiffuse = 0xffffffff;
    k.normalZ = -1;
    k.material.diffuse.r = k.material.diffuse.g = k.material.diffuse.b = k.material.diffuse.a = 1;
    return k;
}

static void cases(void)
{
    CASE k;
    k = base("dir-diffuse"); k.lights = 1; k.light[0] = light(D3DLIGHT_DIRECTIONAL, 0.5f, 0.25f, 1); run(&k);
    k = base("ambient-global"); k.ambient = 0x00404040;
    k.material.ambient.r = 1; k.material.ambient.g = 0.5f; run(&k);
    k = base("emissive"); k.material.emissive.g = 0.5f; k.material.emissive.b = 1; run(&k);
    k = base("light-ambient"); k.lights = 1; k.light[0] = light(D3DLIGHT_DIRECTIONAL, 0, 0, 0);
    k.light[0].dcvAmbient.r = k.light[0].dcvAmbient.g = k.light[0].dcvAmbient.b = 0.5f;
    k.material.ambient.r = 1; run(&k);
    k = base("point"); k.lights = 1; k.light[0] = light(D3DLIGHT_POINT, 1, 1, 1);
    k.light[0].dvAttenuation0 = 0.5f; run(&k);
    k = base("point-range"); k.lights = 1; k.light[0] = light(D3DLIGHT_POINT, 1, 1, 1);
    k.light[0].dvRange = 50; run(&k);
    k = base("point-linear"); k.lights = 1; k.light[0] = light(D3DLIGHT_POINT, 1, 1, 1);
    k.light[0].dvAttenuation0 = 0; k.light[0].dvAttenuation1 = 0.02f; run(&k);
    k = base("spot"); k.lights = 1; k.light[0] = light(D3DLIGHT_SPOT, 4, 4, 4);
    k.light[0].dvTheta = 1.0471976f; k.light[0].dvPhi = 2.7925268f; run(&k);
    k = base("spot-outside"); k.lights = 1; k.light[0] = light(D3DLIGHT_SPOT, 4, 4, 4);
    k.light[0].dvTheta = 1.0471976f; k.light[0].dvPhi = 1.7453293f; run(&k);
    k = base("vertex-colour"); k.colorVertex = TRUE; k.vertexDiffuse = 0xff00ff00;
    k.lights = 1; k.light[0] = light(D3DLIGHT_DIRECTIONAL, 1, 1, 1); run(&k);
    k = base("specular-local"); k.specular = TRUE; k.material.diffuse.r = k.material.diffuse.g =
    k.material.diffuse.b = 0; k.material.specular.g = 0.5f; k.material.specular.b = 1; k.material.power = 4;
    k.lights = 1; k.light[0] = light(D3DLIGHT_DIRECTIONAL, 0, 0, 0);
    k.light[0].dcvSpecular.r = k.light[0].dcvSpecular.g = k.light[0].dcvSpecular.b = 1; run(&k);
    k.name = "specular-infinite"; k.localViewer = FALSE; k.material.power = 1; run(&k);
    k = base("normalize"); k.normalize = TRUE; k.normalZ = -2;
    k.lights = 1; k.light[0] = light(D3DLIGHT_DIRECTIONAL, 0.5f, 0.5f, 0.5f); run(&k);
    k.name = "unnormalized"; k.normalize = FALSE; run(&k);
    k = base("two-lights"); k.lights = 2; k.light[0] = light(D3DLIGHT_DIRECTIONAL, 0.25f, 0, 0);
    k.light[1] = light(D3DLIGHT_DIRECTIONAL, 0, 0.25f, 0); run(&k);
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
    D3DMATRIX identity, projection;
    HWND window;
    BOOL tnl;
    if (argc != 2 || (strcmp(argv[1], "rgb") && strcmp(argv[1], "tnl"))) {
        puts("Usage: tlight rgb|tnl");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    tnl = !strcmp(argv[1], "tnl");
    e.wanted = tnl ? &IID_IDirect3DTnLHalDevice : &IID_IDirect3DRGBDevice;
    e.found = FALSE;
    window = CreateWindowExA(0, "STATIC", "Direct3D 7 lighting test", WS_POPUP, 0, 0, Side, Side,
                             NULL, NULL, GetModuleHandleA(NULL), NULL);
    printf("TLIT begin path=%s\n", argv[1]);
    if (!window || !call("DirectDrawCreateEx", DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) ||
        !call("cooperative level", IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL)) ||
        !call("IDirect3D7", IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d)) ||
        !call("enumerate", IDirect3D7_EnumDevices(d3d, device_info, &e)) || !e.found) { ++failed; goto done; }
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = Side;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (tnl ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 32;
    d.ddpfPixelFormat.dwRBitMask = 0xff0000; d.ddpfPixelFormat.dwGBitMask = 0xff00; d.ddpfPixelFormat.dwBBitMask = 0xff;
    /* Camera-space x and y in pixels from the centre; z from 0 to 100. */
    memset(&identity, 0, sizeof(identity));
    identity._11 = identity._22 = identity._33 = identity._44 = 1;
    memset(&projection, 0, sizeof(projection));
    projection._11 = projection._22 = 1.0f / 64; projection._33 = 1.0f / 100; projection._44 = 1;
    if (!call("render target", IDirectDraw7_CreateSurface(dd, &d, &target, NULL)) ||
        !call("CreateDevice", IDirect3D7_CreateDevice(d3d, e.wanted, target, &device)) ||
        !call("device caps", IDirect3DDevice7_GetCaps(device, &caps)) ||
        !call("viewport", IDirect3DDevice7_SetViewport(device, &vp)) ||
        !call("world", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_WORLD, &identity)) ||
        !call("view", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_VIEW, &identity)) ||
        !call("projection", IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_PROJECTION, &projection))) {
        ++failed;
        goto done;
    }
    printf("TLIT device guid=%08lx caps=%08lx lights=%lu vertex_caps=%08lx\n", caps.deviceGUID.Data1,
           caps.dwDevCaps, caps.dwMaxActiveLights, caps.dwVertexProcessingCaps);
    set(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    set(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    set(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    set(D3DRENDERSTATE_DITHERENABLE, FALSE);
    set(D3DRENDERSTATE_FOGENABLE, FALSE);
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
    printf("TLIT end path=%s pass=%d fail=%d result=%s\n", argv[1], passed, failed,
           passed && !failed ? "PASS" : "FAIL");
    return passed && !failed ? 0 : 1;
}
