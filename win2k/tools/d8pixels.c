/* Check what a Direct3D 8 program draws on the hardware device: a clear,
 * transformed triangles from user memory, a vertex buffer, an index buffer
 * with and without a base vertex, a strip, a managed texture and untransformed
 * vertices, and fifteen pixel shaders of versions 1.1 and 1.2, including
 * fog and lit untransformed vertices. Each case
 * draws on a 128x128 lockable back buffer and inspects a few pixels well
 * inside the shapes. "sw" asks for software vertex
 * processing. Built against the Microsoft headers; d3d8.dll is loaded at
 * run time. */
#define COBJMACROS
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drvcheck.h"

enum { Side = 128 };
enum { RefLimit = Side * Side / 1000 };   /* pixels of a case that may differ from the reference picture (as in d9pixels) */
#define CLEAR_COLOR 0xff000066UL
typedef struct { float x, y, z, rhw; DWORD diffuse; } TLVERTEX;
#define TLFVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } TLTEXVERTEX;
#define TLTEXFVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
typedef struct { float x, y, z; DWORD diffuse; } XYZVERTEX;
#define XYZFVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)
typedef struct { int x, y; DWORD rgb; } SAMPLE;

static IDirect3D8 *d3d;
static IDirect3DDevice8 *device;
static int passes, failures;

static void fail_hr(const char *name, const char *what, HRESULT hr)
{
    printf("D8PIX case=%s %s hr=%08lx\n", name, what, (unsigned long)hr);
    ++failures;
}

/* A quad from (x0,y0) to (x1,y1) as two triangles of a list. */
static void quad(TLVERTEX *v, float x0, float y0, float x1, float y1, DWORD color)
{
    const float xs[6] = { x0, x1, x0, x0, x1, x1 }, ys[6] = { y0, y0, y1, y1, y0, y1 };
    int i;
    for (i = 0; i < 6; ++i) {
        v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 0.5f; v[i].rhw = 1.0f; v[i].diffuse = color;
    }
}

static BOOL begin(const char *name)
{
    HRESULT hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1.0f, 0);
    if (FAILED(hr)) { fail_hr(name, "clear", hr); return FALSE; }
    hr = IDirect3DDevice8_BeginScene(device);
    if (FAILED(hr)) { fail_hr(name, "begin", hr); return FALSE; }
    return TRUE;
}

/* End the scene, read the back buffer and compare the samples. */
static void check(const char *name, const SAMPLE *s, int count)
{
    IDirect3DSurface8 *back = NULL;
    D3DLOCKED_RECT r;
    HRESULT hr = IDirect3DDevice8_EndScene(device);
    int i, bad = 0, worst = 0;
    long differ;
    unsigned long skipped;
    if (FAILED(hr)) { fail_hr(name, "end", hr); return; }
    hr = IDirect3DDevice8_GetBackBuffer(device, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (FAILED(hr)) { fail_hr(name, "backbuffer", hr); return; }
    hr = IDirect3DSurface8_LockRect(back, &r, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) { fail_hr(name, "lock", hr); IDirect3DSurface8_Release(back); return; }
    for (i = 0; i < count; ++i) {
        DWORD got = ((const DWORD *)((const BYTE *)r.pBits + s[i].y * r.Pitch))[s[i].x] & 0xffffff;
        DWORD want = s[i].rgb & 0xffffff;
        int k, off = 0;
        for (k = 0; k < 24; k += 8) {
            int a = (got >> k) & 255, b = (want >> k) & 255;
            if (a - b > 2 || b - a > 2) off = 1;
        }
        if (off) {
            printf("D8PIX case=%s sample=%d at=%d,%d got=%06lx want=%06lx\n", name, i, s[i].x, s[i].y,
                   (unsigned long)got, (unsigned long)want);
            ++bad;
        }
    }
    /* The automatic checks (drvcheck.h), as in d9pixels. */
    differ = ref_case(name, r.pBits, r.Pitch, Side, &worst);
    IDirect3DSurface8_UnlockRect(back);
    IDirect3DSurface8_Release(back);
    skipped = skips_case("D8PIX", name);
    if (refMode == RefCompare && differ >= 0) {
        printf("D8PIX case=%s samples=%d mismatches=%d ref_differ=%ld ref_worst=%d\n", name, count, bad, differ, worst);
        if (differ > RefLimit) {
            ++refFailed;
            printf("D8PIX case=%s differs first at=%d,%d ours=%06lx reference=%06lx\n", name, refAtX, refAtY, refMine, refTheirs);
        }
    } else printf("D8PIX case=%s samples=%d mismatches=%d\n", name, count, bad);
    if (bad || skipped || (refMode == RefCompare && differ > RefLimit)) ++failures; else ++passes;
}

static void case_clear(void)
{
    static const SAMPLE s[] = { { 4, 4, CLEAR_COLOR }, { 64, 64, CLEAR_COLOR }, { 123, 123, CLEAR_COLOR } };
    if (begin("clear")) check("clear", s, 3);
}

static void case_up(void)
{
    static const SAMPLE s[] = { { 64, 64, 0xff0000 }, { 40, 40, 0xff0000 }, { 8, 8, CLEAR_COLOR } };
    TLVERTEX v[6];
    HRESULT hr;
    quad(v, 32, 32, 96, 96, 0xffff0000);
    if (!begin("up")) return;
    IDirect3DDevice8_SetVertexShader(device, TLFVF);
    hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
    if (FAILED(hr)) { fail_hr("up", "draw", hr); IDirect3DDevice8_EndScene(device); return; }
    check("up", s, 3);
}

static IDirect3DVertexBuffer8 *vertex_buffer(const void *data, UINT bytes, DWORD fvf)
{
    IDirect3DVertexBuffer8 *vb = NULL;
    BYTE *p;
    if (FAILED(IDirect3DDevice8_CreateVertexBuffer(device, bytes, D3DUSAGE_WRITEONLY, fvf, D3DPOOL_DEFAULT, &vb)))
        return NULL;
    if (FAILED(IDirect3DVertexBuffer8_Lock(vb, 0, 0, &p, 0))) { IDirect3DVertexBuffer8_Release(vb); return NULL; }
    memcpy(p, data, bytes);
    IDirect3DVertexBuffer8_Unlock(vb);
    return vb;
}

static void case_vb(void)
{
    static const SAMPLE s[] = { { 64, 64, 0x00ff00 }, { 90, 90, 0x00ff00 }, { 120, 8, CLEAR_COLOR } };
    TLVERTEX v[12];
    IDirect3DVertexBuffer8 *vb;
    HRESULT hr;
    quad(v, 0, 0, 1, 1, 0xff0000ff);            /* six unused vertices first */
    quad(v + 6, 32, 32, 96, 96, 0xff00ff00);
    vb = vertex_buffer(v, sizeof(v), TLFVF);
    if (!vb) { fail_hr("vb", "create", E_FAIL); return; }
    if (begin("vb")) {
        IDirect3DDevice8_SetVertexShader(device, TLFVF);
        IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(v[0]));
        hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 6, 2);
        if (FAILED(hr)) { fail_hr("vb", "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check("vb", s, 3);
    }
    IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
    IDirect3DVertexBuffer8_Release(vb);
}

static void case_strip(void)
{
    static const SAMPLE s[] = { { 64, 64, 0xffff00 }, { 34, 94, 0xffff00 }, { 94, 34, 0xffff00 }, { 8, 64, CLEAR_COLOR } };
    TLVERTEX v[4] = {
        { 32, 32, 0.5f, 1, 0xffffff00 }, { 96, 32, 0.5f, 1, 0xffffff00 },
        { 32, 96, 0.5f, 1, 0xffffff00 }, { 96, 96, 0.5f, 1, 0xffffff00 } };
    IDirect3DVertexBuffer8 *vb = vertex_buffer(v, sizeof(v), TLFVF);
    HRESULT hr;
    if (!vb) { fail_hr("strip", "create", E_FAIL); return; }
    if (begin("strip")) {
        IDirect3DDevice8_SetVertexShader(device, TLFVF);
        IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(v[0]));
        hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, 0, 2);
        if (FAILED(hr)) { fail_hr("strip", "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check("strip", s, 4);
    }
    IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
    IDirect3DVertexBuffer8_Release(vb);
}

/* Four corners after base unused vertices, drawn through 16-bit indices. */
static void indexed(const char *name, UINT base, DWORD color)
{
    SAMPLE s[] = { { 64, 64, 0 }, { 36, 92, 0 }, { 120, 120, CLEAR_COLOR } };
    TLVERTEX v[8];
    static const WORD idx[6] = { 0, 1, 2, 2, 1, 3 };
    IDirect3DVertexBuffer8 *vb;
    IDirect3DIndexBuffer8 *ib = NULL;
    BYTE *p;
    HRESULT hr;
    UINT i;
    s[0].rgb = s[1].rgb = color;
    for (i = 0; i < 8; ++i) {
        UINT k = i >= base ? i - base : 0;
        v[i].x = (k & 1) ? 96.0f : 32.0f; v[i].y = (k & 2) ? 96.0f : 32.0f;
        v[i].z = 0.5f; v[i].rhw = 1.0f; v[i].diffuse = i >= base ? (0xff000000 | color) : 0xffffffff;
    }
    vb = vertex_buffer(v, sizeof(v), TLFVF);
    if (!vb) { fail_hr(name, "create", E_FAIL); return; }
    hr = IDirect3DDevice8_CreateIndexBuffer(device, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                            D3DPOOL_DEFAULT, &ib);
    if (FAILED(hr)) { fail_hr(name, "index buffer", hr); IDirect3DVertexBuffer8_Release(vb); return; }
    if (SUCCEEDED(IDirect3DIndexBuffer8_Lock(ib, 0, 0, &p, 0))) {
        memcpy(p, idx, sizeof(idx));
        IDirect3DIndexBuffer8_Unlock(ib);
    }
    if (begin(name)) {
        IDirect3DDevice8_SetVertexShader(device, TLFVF);
        IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(v[0]));
        IDirect3DDevice8_SetIndices(device, ib, base);
        hr = IDirect3DDevice8_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, 4, 0, 2);
        if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check(name, s, 3);
    }
    IDirect3DDevice8_SetIndices(device, NULL, 0);
    IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
    IDirect3DIndexBuffer8_Release(ib);
    IDirect3DVertexBuffer8_Release(vb);
}

/* A 2x2 managed texture with four colours, point sampled over a quad. */
static void case_texture(void)
{
    static const SAMPLE s[] = { { 48, 48, 0xff0000 }, { 80, 48, 0x00ff00 }, { 48, 80, 0x0000ff },
                                { 80, 80, 0xffffff }, { 8, 8, CLEAR_COLOR } };
    static const DWORD texels[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
    TLTEXVERTEX v[4] = {
        { 32, 32, 0.5f, 1, 0xffffffff, 0, 0 }, { 96, 32, 0.5f, 1, 0xffffffff, 1, 0 },
        { 32, 96, 0.5f, 1, 0xffffffff, 0, 1 }, { 96, 96, 0.5f, 1, 0xffffffff, 1, 1 } };
    IDirect3DTexture8 *t = NULL;
    D3DLOCKED_RECT r;
    HRESULT hr = IDirect3DDevice8_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
    int y;
    if (FAILED(hr)) { fail_hr("texture", "create", hr); return; }
    if (SUCCEEDED(IDirect3DTexture8_LockRect(t, 0, &r, NULL, 0))) {
        for (y = 0; y < 2; ++y) memcpy((BYTE *)r.pBits + y * r.Pitch, texels + 2 * y, 8);
        IDirect3DTexture8_UnlockRect(t, 0);
    }
    if (begin("texture")) {
        IDirect3DDevice8_SetTexture(device, 0, (IDirect3DBaseTexture8 *)t);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice8_SetVertexShader(device, TLTEXFVF);
        hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr("texture", "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check("texture", s, 5);
        IDirect3DDevice8_SetTexture(device, 0, NULL);
    }
    IDirect3DTexture8_Release(t);
}

/* Object-space vertices through identity world and view matrices and an
 * orthographic projection onto the whole target. */
static void case_transform(void)
{
    static const SAMPLE s[] = { { 64, 64, 0xff00ff }, { 40, 88, 0xff00ff }, { 8, 8, CLEAR_COLOR } };
    D3DMATRIX m;
    XYZVERTEX v[4] = {
        { -0.5f, 0.5f, 0.5f, 0xffff00ff }, { 0.5f, 0.5f, 0.5f, 0xffff00ff },
        { -0.5f, -0.5f, 0.5f, 0xffff00ff }, { 0.5f, -0.5f, 0.5f, 0xffff00ff } };
    HRESULT hr;
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &m);
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &m);
    IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &m);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (!begin("transform")) return;
    IDirect3DDevice8_SetVertexShader(device, XYZFVF);
    hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
    if (FAILED(hr)) { fail_hr("transform", "draw", hr); IDirect3DDevice8_EndScene(device); return; }
    check("transform", s, 3);
}

/* ---- pixel shaders ----
 * Token streams are assembled by hand: version, then each instruction's
 * opcode followed by its destination and source parameters. */
#define PSV(minor) (0xffff0100UL | (minor))
#define PSEND 0x0000ffffUL
#define PD(type, num, mask, mod, shift) (0x80000000UL | ((DWORD)(type) << 28) | ((DWORD)(mask) << 16) | \
                                         ((DWORD)(mod) << 20) | ((DWORD)(shift) << 24) | (num))
#define PS_(type, num, swz, mod) (0x80000000UL | ((DWORD)(type) << 28) | ((DWORD)(swz) << 16) | ((DWORD)(mod) << 24) | (num))
enum { RR = 0, RV = 1, RC = 2, RT = 3 };
enum { SwXYZW = 0xe4, SwA = 0xff };
enum { MNone, MNeg, MBias, MBiasNeg, MBx2, MBx2Neg, MComp };
enum { OMov = 1, OAdd = 2, OSub = 3, OMad = 4, OMul = 5, ODp3 = 8, OLrp = 18, OTexcoord = 64,
       OTexkill = 65, OTex = 66, OCnd = 80, ODef = 81, OCmp = 88 };
#define CO 0x40000000UL
/* IEEE single bit patterns for def. */
enum { F0 = 0, F01 = 0x3dcccccd, F02 = 0x3e4ccccd, F025 = 0x3e800000, F05 = 0x3f000000, F06 = 0x3f19999a,
       F075 = 0x3f400000, F1 = 0x3f800000 };
#define DEF(c, a, b, d, e) ODef, PD(RC, c, 15, 0, 0), (DWORD)(a), (DWORD)(b), (DWORD)(d), (DWORD)(e)
#define DST(r, n) PD(r, n, 15, 0, 0)
#define SRC(r, n) PS_(r, n, SwXYZW, MNone)

typedef struct { float x, y, z, rhw; DWORD diffuse, specular; float u0, v0, u1, v1; } PSVERTEX;
#define PSFVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2)

/* A quad as two triangles of a list; the coordinates run from (u0, v0)
 * at the top left to (u1, v1) at the bottom right, in both sets. */
static void ps_quad(PSVERTEX *v, float x0, float y0, float x1, float y1, DWORD diffuse,
                    float u0, float v0, float u1, float v1)
{
    const float xs[6] = { x0, x1, x0, x0, x1, x1 }, ys[6] = { y0, y0, y1, y1, y0, y1 };
    int i;
    for (i = 0; i < 6; ++i) {
        v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 0.5f; v[i].rhw = 1.0f;
        v[i].diffuse = diffuse; v[i].specular = 0;
        v[i].u0 = v[i].u1 = xs[i] == x0 ? u0 : u1;
        v[i].v0 = v[i].v1 = ys[i] == y0 ? v0 : v1;
    }
}

static DWORD rgb(float r, float g, float b)
{
    float c[3];
    DWORD out = 0;
    int k;
    c[0] = r; c[1] = g; c[2] = b;
    for (k = 0; k < 3; ++k) {
        float f = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
        out = (out << 8) | (DWORD)(f * 255.0f + 0.5f);
    }
    return out;
}

static float clampu(float f) { return f < -1 ? -1 : f > 1 ? 1 : f; }
static float unit(DWORD channel) { return (float)(channel & 255) / 255.0f; }

static IDirect3DTexture8 *ps_textures[2];

static IDirect3DTexture8 *texture2x2(const DWORD *texels)
{
    IDirect3DTexture8 *t = NULL;
    D3DLOCKED_RECT r;
    int y;
    if (FAILED(IDirect3DDevice8_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t))) return NULL;
    if (SUCCEEDED(IDirect3DTexture8_LockRect(t, 0, &r, NULL, 0))) {
        for (y = 0; y < 2; ++y) memcpy((BYTE *)r.pBits + y * r.Pitch, texels + 2 * y, 8);
        IDirect3DTexture8_UnlockRect(t, 0);
    }
    return t;
}

/* Draw quads with a pixel shader and compare the samples. */
static void ps_run(const char *name, const DWORD *tokens, const PSVERTEX *v, UINT quads, const SAMPLE *s, int samples)
{
    DWORD handle = 0;
    HRESULT hr = IDirect3DDevice8_CreatePixelShader(device, tokens, &handle);
    if (FAILED(hr)) { fail_hr(name, "create", hr); return; }
    if (begin(name)) {
        IDirect3DDevice8_SetPixelShader(device, handle);
        IDirect3DDevice8_SetVertexShader(device, PSFVF);
        hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2 * quads, v, sizeof(v[0]));
        IDirect3DDevice8_SetPixelShader(device, 0);
        if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check(name, s, samples);
    }
    IDirect3DDevice8_DeletePixelShader(device, handle);
}

static const DWORD texels0[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
static const DWORD texels1[4] = { 0xffffff00, 0xffffff00, 0xffffff00, 0xffffff00 };
/* The four texel centres of a 2x2 texture over the quad 32..96. */
static const int corners[4][2] = { { 48, 48 }, { 80, 48 }, { 48, 80 }, { 80, 80 } };

static void quad_samples(SAMPLE *s, const float (*colour)[3])
{
    int i;
    for (i = 0; i < 4; ++i) {
        s[i].x = corners[i][0]; s[i].y = corners[i][1];
        s[i].rgb = rgb(colour[i][0], colour[i][1], colour[i][2]);
    }
    s[4].x = 8; s[4].y = 8; s[4].rgb = CLEAR_COLOR;
}

static void texel(int i, float *c)
{
    c[0] = unit(texels0[i] >> 16); c[1] = unit(texels0[i] >> 8); c[2] = unit(texels0[i]);
}

/* A vertex program (vs.1.1, or Direct3D 8.0s vs.1.0 that 3DMark 2001 SE uses): mov oPos, v0; mov oD0, v5,
 * an orange quad over clip -0.5..0.5, the target's 32..96. */
static void vs_case(const char *name, DWORD version, BOOL offered, BOOL buffer)
{
    static const DWORD decl[] = { D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
                                  D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END() };
    DWORD code[] = { 0,
        0x00000001, 0xc00f0000, 0x90e40000,     /* mov oPos, v0 */
        0x00000001, 0xd00f0000, 0x90e40005,     /* mov oD0, v5 */
        0x0000ffff };
    static const SAMPLE s[] = { { 64, 64, 0xff8000 }, { 40, 88, 0xff8000 }, { 8, 8, CLEAR_COLOR } };
    struct { float x, y, z; DWORD color; } v[4] = {
        { -0.5f, 0.5f, 0.5f, 0xffff8000 }, { 0.5f, 0.5f, 0.5f, 0xffff8000 },
        { -0.5f, -0.5f, 0.5f, 0xffff8000 }, { 0.5f, -0.5f, 0.5f, 0xffff8000 } };
    DWORD shader = 0;
    HRESULT hr;
    if (!offered) { printf("D8PIX vertex shaders not offered\n"); return; }
    code[0] = version;
    hr = IDirect3DDevice8_CreateVertexShader(device, decl, code, &shader, 0);
    if (FAILED(hr)) { fail_hr(name, "create", hr); return; }
    if (begin(name)) {
        IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice8_SetTexture(device, 0, NULL);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice8_SetVertexShader(device, shader);
        if (buffer) {
            IDirect3DVertexBuffer8 *vb = vertex_buffer(v, sizeof(v), 0);
            hr = vb ? IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(v[0])) : E_FAIL;
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, 0, 2);
            IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
            if (vb) IDirect3DVertexBuffer8_Release(vb);
        } else hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check(name, s, 3);
        IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
    }
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice8_DeleteVertexShader(device, shader);
}

/* A lit fixed-function mesh of many vertices (a 12 by 12 grid of quads facing a white
 * directional light, material diffuse (0.5, 0.25, 1)): drawn as a list of 864 vertices from a
 * vertex buffer (lit_grid), then as 169 vertices with 864 indices (lit_grid_indexed). The other
 * lit cases draw a single quad. */
static void lit_grid_case(void)
{
    enum { N = 12 };
    typedef struct { float x, y, z, nx, ny, nz; } LITVERTEX;
    static const SAMPLE s[] = { { 40, 40, 0x8040ff }, { 64, 64, 0x8040ff }, { 88, 88, 0x8040ff }, { 60, 90, 0x8040ff },
                                { 8, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR } };
    static LITVERTEX list[N * N * 6], grid[(N + 1) * (N + 1)];
    static WORD index[N * N * 6];
    IDirect3DVertexBuffer8 *vb = NULL;
    IDirect3DIndexBuffer8 *ib = NULL;
    D3DMATERIAL8 m;
    D3DLIGHT8 l;
    D3DMATRIX identity;
    BYTE *p;
    HRESULT hr;
    int x, y, k, pass;
    for (y = 0; y <= N; ++y)
        for (x = 0; x <= N; ++x) {
            LITVERTEX *v = &grid[y * (N + 1) + x];
            v->x = -0.5f + (float)x / N; v->y = 0.5f - (float)y / N; v->z = 0.5f;
            v->nx = v->ny = 0; v->nz = -1;
        }
    for (y = 0, k = 0; y < N; ++y)
        for (x = 0; x < N; ++x) {
            const WORD a = (WORD)(y * (N + 1) + x), b = (WORD)(a + 1), c = (WORD)(a + N + 1), d = (WORD)(c + 1);
            const WORD six[6] = { a, b, c, c, b, d };
            int j;
            for (j = 0; j < 6; ++j, ++k) { index[k] = six[j]; list[k] = grid[six[j]]; }
        }
    memset(&m, 0, sizeof(m));
    m.Diffuse.r = 0.5f; m.Diffuse.g = 0.25f; m.Diffuse.b = 1.0f; m.Diffuse.a = 1.0f;
    memset(&l, 0, sizeof(l));
    l.Type = D3DLIGHT_DIRECTIONAL;
    l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = l.Diffuse.a = 1.0f;
    l.Direction.z = 1.0f;
    memset(&identity, 0, sizeof(identity));
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    for (pass = 0; pass < 2; ++pass) {
        const char *name = pass ? "lit_grid_indexed" : "lit_grid";
        vb = pass ? vertex_buffer(grid, sizeof(grid), D3DFVF_XYZ | D3DFVF_NORMAL)
                  : vertex_buffer(list, sizeof(list), D3DFVF_XYZ | D3DFVF_NORMAL);
        ib = NULL;
        hr = vb ? D3D_OK : E_FAIL;
        if (SUCCEEDED(hr) && pass) {
            hr = IDirect3DDevice8_CreateIndexBuffer(device, sizeof(index), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib);
            if (SUCCEEDED(hr) && SUCCEEDED(hr = IDirect3DIndexBuffer8_Lock(ib, 0, 0, &p, 0))) {
                memcpy(p, index, sizeof(index));
                IDirect3DIndexBuffer8_Unlock(ib);
            }
        }
        if (FAILED(hr)) fail_hr(name, "buffers", hr);
        else {
            IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
            IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &identity);
            IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &identity);
            IDirect3DDevice8_SetMaterial(device, &m);
            IDirect3DDevice8_SetLight(device, 0, &l);
            IDirect3DDevice8_LightEnable(device, 0, TRUE);
            IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, TRUE);
            IDirect3DDevice8_SetRenderState(device, D3DRS_AMBIENT, 0);
            IDirect3DDevice8_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
            if (begin(name)) {
                IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
                IDirect3DDevice8_SetTexture(device, 0, NULL);
                IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_NORMAL);
                IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(LITVERTEX));
                if (pass) {
                    IDirect3DDevice8_SetIndices(device, ib, 0);
                    hr = IDirect3DDevice8_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, (N + 1) * (N + 1), 0, N * N * 2);
                } else hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, N * N * 2);
                IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
                if (pass) IDirect3DDevice8_SetIndices(device, NULL, 0);
                if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice8_EndScene(device); }
                else check(name, s, 6);
                IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
            }
            IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice8_LightEnable(device, 0, FALSE);
        }
        if (ib) IDirect3DIndexBuffer8_Release(ib);
        if (vb) IDirect3DVertexBuffer8_Release(vb);
    }
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

/* The lit mesh of lit_grid_case with the material's emissive colour only (its other colours
 * are black, so no light adds anything and the result is (0.5, 0.25, 1) whatever the lights
 * are), under the lighting set-ups a game uses: no light enabled, a point light, a spot light,
 * eight lights of all kinds, and the same eight with specular, normalized normals and a local
 * viewer. What is checked is the geometry: the lit vertex shader runs its light loop and must
 * leave the positions alone. */
static void lit_variants_case(void)
{
    enum { N = 12 };
    typedef struct { float x, y, z, nx, ny, nz; } LITVERTEX;
    static const SAMPLE s[] = { { 40, 40, 0x8040ff }, { 64, 64, 0x8040ff }, { 88, 88, 0x8040ff }, { 60, 90, 0x8040ff },
                                { 8, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR } };
    static const char *const names[5] = { "lit_no_lights", "lit_point", "lit_spot", "lit_eight", "lit_eight_specular" };
    static LITVERTEX list[N * N * 6];
    IDirect3DVertexBuffer8 *vb;
    D3DMATERIAL8 m;
    D3DMATRIX identity;
    HRESULT hr;
    int x, y, k, pass, i;
    for (y = 0, k = 0; y < N; ++y)
        for (x = 0; x < N; ++x) {
            static const int cx[6] = { 0, 1, 0, 0, 1, 1 }, cy[6] = { 0, 0, 1, 1, 0, 1 };
            int j;
            for (j = 0; j < 6; ++j, ++k) {
                list[k].x = -0.5f + (float)(x + cx[j]) / N; list[k].y = 0.5f - (float)(y + cy[j]) / N; list[k].z = 0.5f;
                list[k].nx = list[k].ny = 0; list[k].nz = -1;
            }
        }
    vb = vertex_buffer(list, sizeof(list), D3DFVF_XYZ | D3DFVF_NORMAL);
    if (!vb) { fail_hr("lit_variants", "buffer", E_FAIL); return; }
    memset(&m, 0, sizeof(m));
    m.Emissive.r = 0.5f; m.Emissive.g = 0.25f; m.Emissive.b = 1.0f; m.Emissive.a = 1.0f;
    m.Diffuse.a = 1.0f; m.Power = 20.0f;
    memset(&identity, 0, sizeof(identity));
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    for (pass = 0; pass < 5; ++pass) {
        const int count = pass == 0 ? 0 : pass < 3 ? 1 : 8;
        for (i = 0; i < 8; ++i) {
            D3DLIGHT8 l;
            int kind = pass == 1 ? 0 : pass == 2 ? 1 : i % 3;      /* point, spot, directional */
            memset(&l, 0, sizeof(l));
            l.Type = kind == 0 ? D3DLIGHT_POINT : kind == 1 ? D3DLIGHT_SPOT : D3DLIGHT_DIRECTIONAL;
            l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = l.Diffuse.a = 1.0f;
            l.Specular = l.Diffuse; l.Ambient = l.Diffuse;
            l.Position.x = 0.1f * i; l.Position.y = 0.2f; l.Position.z = -1.0f;
            l.Direction.x = 0.1f; l.Direction.z = 1.0f;
            l.Range = 100.0f; l.Falloff = 1.0f;
            l.Attenuation0 = 0.0f; l.Attenuation1 = 0.5f; l.Attenuation2 = 0.01f * i;
            l.Theta = 0.3f; l.Phi = 0.9f;
            IDirect3DDevice8_SetLight(device, i, &l);
            IDirect3DDevice8_LightEnable(device, i, i < count);
        }
        IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
        IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &identity);
        IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &identity);
        IDirect3DDevice8_SetMaterial(device, &m);
        IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_AMBIENT, 0);
        IDirect3DDevice8_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_SPECULARENABLE, pass == 4);
        IDirect3DDevice8_SetRenderState(device, D3DRS_NORMALIZENORMALS, pass == 4);
        IDirect3DDevice8_SetRenderState(device, D3DRS_LOCALVIEWER, pass == 4);
        if (begin(names[pass])) {
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice8_SetTexture(device, 0, NULL);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_NORMAL);
            IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(LITVERTEX));
            hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, N * N * 2);
            IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
            if (FAILED(hr)) { fail_hr(names[pass], "draw", hr); IDirect3DDevice8_EndScene(device); }
            else check(names[pass], s, 6);
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
        }
    }
    for (i = 0; i < 8; ++i) IDirect3DDevice8_LightEnable(device, i, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_SPECULARENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_NORMALIZENORMALS, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LOCALVIEWER, TRUE);
    IDirect3DVertexBuffer8_Release(vb);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

/* The same mesh through a world, a view and a perspective projection matrix that are not the
 * identity (scaled by 3, moved 3 back, a 90 degree perspective: it covers the same pixels as
 * the cases above): unlit with the texture factor as its colour (persp_unlit), then lit with
 * the emissive material and no light (persp_lit) and with eight lights (persp_lit_eight). */
static void lit_perspective_case(void)
{
    enum { N = 12 };
    typedef struct { float x, y, z, nx, ny, nz; } LITVERTEX;
    static const SAMPLE s[] = { { 40, 40, 0x8040ff }, { 64, 64, 0x8040ff }, { 88, 88, 0x8040ff }, { 60, 90, 0x8040ff },
                                { 8, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR } };
    static const char *const names[3] = { "persp_unlit", "persp_lit", "persp_lit_eight" };
    static LITVERTEX list[N * N * 6];
    IDirect3DVertexBuffer8 *vb;
    D3DMATERIAL8 m;
    D3DMATRIX world, view, proj, identity;
    HRESULT hr;
    int x, y, k, pass, i;
    for (y = 0, k = 0; y < N; ++y)
        for (x = 0; x < N; ++x) {
            static const int cx[6] = { 0, 1, 0, 0, 1, 1 }, cy[6] = { 0, 0, 1, 1, 0, 1 };
            int j;
            for (j = 0; j < 6; ++j, ++k) {
                list[k].x = (-0.5f + (float)(x + cx[j]) / N) / 3.0f * 3.0f;
                list[k].y = (0.5f - (float)(y + cy[j]) / N) / 3.0f * 3.0f;
                list[k].z = 0.0f;
                list[k].nx = list[k].ny = 0; list[k].nz = -1;
            }
        }
    vb = vertex_buffer(list, sizeof(list), D3DFVF_XYZ | D3DFVF_NORMAL);
    if (!vb) { fail_hr("lit_perspective", "buffer", E_FAIL); return; }
    memset(&identity, 0, sizeof(identity));
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    world = identity; world._11 = world._22 = world._33 = 3.0f;        /* x and y from -1.5 to 1.5 */
    view = identity; view._43 = 3.0f;                                  /* 3 in front of the camera */
    memset(&proj, 0, sizeof(proj));                                    /* 90 degrees, near 1, far 100 */
    proj._11 = proj._22 = 1.0f; proj._33 = 100.0f / 99.0f; proj._34 = 1.0f; proj._43 = -100.0f / 99.0f;
    memset(&m, 0, sizeof(m));
    m.Emissive.r = 0.5f; m.Emissive.g = 0.25f; m.Emissive.b = 1.0f; m.Emissive.a = 1.0f;
    m.Diffuse.a = 1.0f; m.Power = 20.0f;
    for (pass = 0; pass < 3; ++pass) {
        for (i = 0; i < 8; ++i) {
            D3DLIGHT8 l;
            memset(&l, 0, sizeof(l));
            l.Type = i % 3 == 0 ? D3DLIGHT_POINT : i % 3 == 1 ? D3DLIGHT_SPOT : D3DLIGHT_DIRECTIONAL;
            l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = l.Diffuse.a = 1.0f;
            l.Position.x = 0.1f * i; l.Position.y = 0.2f; l.Position.z = -1.0f;
            l.Direction.x = 0.1f; l.Direction.z = 1.0f;
            l.Range = 100.0f; l.Falloff = 1.0f; l.Attenuation1 = 0.5f; l.Theta = 0.3f; l.Phi = 0.9f;
            IDirect3DDevice8_SetLight(device, i, &l);
            IDirect3DDevice8_LightEnable(device, i, pass == 2);
        }
        IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &view);
        IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &proj);
        IDirect3DDevice8_SetMaterial(device, &m);
        IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, pass != 0);
        IDirect3DDevice8_SetRenderState(device, D3DRS_AMBIENT, 0);
        IDirect3DDevice8_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_TEXTUREFACTOR, 0xff8040ff);
        if (begin(names[pass])) {
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice8_SetTexture(device, 0, NULL);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, pass ? D3DTA_DIFFUSE : D3DTA_TFACTOR);
            /* The alpha comes from the same place: no argument of the stage names its (missing) texture. */
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, pass ? D3DTA_DIFFUSE : D3DTA_TFACTOR);
            IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_NORMAL);
            IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(LITVERTEX));
            hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, N * N * 2);
            IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            if (FAILED(hr)) { fail_hr(names[pass], "draw", hr); IDirect3DDevice8_EndScene(device); }
            else check(names[pass], s, 6);
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
        }
    }
    for (i = 0; i < 8; ++i) IDirect3DDevice8_LightEnable(device, i, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &identity);
    IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &identity);
    IDirect3DVertexBuffer8_Release(vb);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

/* Coverage check: a sample whose colour is CLEAR_COLOR must still be the clear colour, any
 * other sample must have been drawn over (whatever the colour). */
static void check_covered(const char *name, const SAMPLE *s, int count)
{
    IDirect3DSurface8 *back = NULL;
    D3DLOCKED_RECT r;
    HRESULT hr = IDirect3DDevice8_EndScene(device);
    int i, bad = 0, worst = 0;
    long differ;
    unsigned long skipped;
    if (FAILED(hr)) { fail_hr(name, "end", hr); return; }
    hr = IDirect3DDevice8_GetBackBuffer(device, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (FAILED(hr)) { fail_hr(name, "backbuffer", hr); return; }
    hr = IDirect3DSurface8_LockRect(back, &r, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) { fail_hr(name, "lock", hr); IDirect3DSurface8_Release(back); return; }
    for (i = 0; i < count; ++i) {
        DWORD got = ((const DWORD *)((const BYTE *)r.pBits + s[i].y * r.Pitch))[s[i].x] & 0xffffff;
        BOOL clear = got == (CLEAR_COLOR & 0xffffff), wantClear = (s[i].rgb & 0xffffff) == (CLEAR_COLOR & 0xffffff);
        if (clear != wantClear) {
            printf("D8PIX case=%s sample=%d at=%d,%d got=%06lx want=%s\n", name, i, s[i].x, s[i].y,
                   (unsigned long)got, wantClear ? "clear" : "drawn");
            ++bad;
        }
    }
    /* The automatic checks (drvcheck.h), as in d9pixels. */
    differ = ref_case(name, r.pBits, r.Pitch, Side, &worst);
    IDirect3DSurface8_UnlockRect(back);
    IDirect3DSurface8_Release(back);
    skipped = skips_case("D8PIX", name);
    if (refMode == RefCompare && differ >= 0) {
        printf("D8PIX case=%s samples=%d mismatches=%d ref_differ=%ld ref_worst=%d\n", name, count, bad, differ, worst);
        if (differ > RefLimit) {
            ++refFailed;
            printf("D8PIX case=%s differs first at=%d,%d ours=%06lx reference=%06lx\n", name, refAtX, refAtY, refMine, refTheirs);
        }
    } else printf("D8PIX case=%s samples=%d mismatches=%d\n", name, count, bad);
    if (bad || skipped || (refMode == RefCompare && differ > RefLimit)) ++failures; else ++passes;
}

/* A lit draw as 3DMark 2001 SE's Car Chase makes them, with the matrices, light, material, fog
 * and texture stage of one of its draws: a mesh that covers the middle of the screen (x and y
 * from a quarter to three quarters), unlit (replay_unlit) and lit (replay_lit). */
static void lit_replay_case(void)
{
    enum { N = 12 };
    typedef struct { float x, y, z, nx, ny, nz, u, v; } TEXLIT;
    static const SAMPLE s[] = { { 40, 40, 0x123456 }, { 64, 64, 0x123456 }, { 88, 88, 0x123456 }, { 60, 90, 0x123456 },
                                { 8, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR }, { 8, 120, CLEAR_COLOR }, { 120, 8, CLEAR_COLOR } };
    static const float worldm[16] = { -3.3717e-06f, -4.3711e-08f, 1, 0,  -1, -4.3711e-08f, -3.3717e-06f, 0,
                                      4.3711e-08f, -1, -4.3711e-08f, 0,  -61.495f, 19.041f, 58.149f, 1 };
    static const float viewm[16] = { 0, -0.25882f, -0.96593f, 0,  0, 0.96593f, -0.25882f, 0,  1, 0, 0, 0,
                                     -74.012f, -4.1603f, 80, 1 };
    static const float projm[16] = { 1.3032f, 0, 0, 0,  0, 1.7376f, 0, 0,  0, -7.9473e-08f, 1.0019f, 1,  0, 0, -0.50096f, 0 };
    static const DWORD white[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
    static TEXLIT list[N * N * 6];
    IDirect3DVertexBuffer8 *vb;
    IDirect3DTexture8 *t = NULL;
    D3DLOCKED_RECT lr;
    D3DMATERIAL8 m;
    D3DLIGHT8 l;
    D3DMATRIX identity;
    HRESULT hr;
    float fogStart = 125.0f, fogEnd = 250.0f;
    int x, y, k, pass;
    /* In the camera's space the mesh is the plane z = 142.55 with x and y from -a to a and -b to b. */
    for (y = 0, k = 0; y < N; ++y)
        for (x = 0; x < N; ++x) {
            static const int cx[6] = { 0, 1, 0, 0, 1, 1 }, cy[6] = { 0, 0, 1, 1, 0, 1 };
            int j;
            for (j = 0; j < 6; ++j, ++k) {
                float a = (-0.5f + (float)(x + cx[j]) / N) * 2.0f * 54.69f;
                float b = (0.5f - (float)(y + cy[j]) / N) * 2.0f * 41.02f;
                list[k].x = 15.863f + a;
                list[k].y = 0.25882f * b;
                list[k].z = 31.211f - 0.96593f * b;
                list[k].nx = -1; list[k].ny = 0; list[k].nz = 0;
                list[k].u = (float)(x + cx[j]) / N; list[k].v = (float)(y + cy[j]) / N;
            }
        }
    vb = vertex_buffer(list, sizeof(list), D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    if (vb && SUCCEEDED(IDirect3DDevice8_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) &&
        SUCCEEDED(IDirect3DTexture8_LockRect(t, 0, &lr, NULL, 0))) {
        memcpy(lr.pBits, white, 8);
        memcpy((BYTE *)lr.pBits + lr.Pitch, white + 2, 8);
        IDirect3DTexture8_UnlockRect(t, 0);
    }
    if (!vb || !t) { fail_hr("lit_replay", "create", E_FAIL); if (vb) IDirect3DVertexBuffer8_Release(vb); return; }
    memset(&m, 0, sizeof(m));
    m.Diffuse.r = m.Diffuse.g = m.Diffuse.b = m.Diffuse.a = 1.0f;
    m.Ambient = m.Diffuse; m.Specular = m.Diffuse; m.Emissive.a = 1.0f; m.Power = 8.0f;
    memset(&l, 0, sizeof(l));
    l.Type = D3DLIGHT_DIRECTIONAL;
    l.Diffuse.r = l.Diffuse.g = 0.698f; l.Diffuse.b = 0.7529f; l.Diffuse.a = 1.0f;
    l.Specular.r = l.Specular.g = l.Specular.b = l.Specular.a = 1.0f;
    l.Direction.x = -0.6109f; l.Direction.y = -0.506f; l.Direction.z = -0.609f;
    l.Range = 1.0e19f; l.Attenuation0 = 1.0f;
    memset(&identity, 0, sizeof(identity));
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    for (pass = 0; pass < 2; ++pass) {
        const char *name = pass ? "replay_lit" : "replay_unlit";
        IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, (const D3DMATRIX *)worldm);
        IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, (const D3DMATRIX *)viewm);
        IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, (const D3DMATRIX *)projm);
        IDirect3DDevice8_SetMaterial(device, &m);
        IDirect3DDevice8_SetLight(device, 0, &l);
        IDirect3DDevice8_LightEnable(device, 0, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, pass);
        IDirect3DDevice8_SetRenderState(device, D3DRS_AMBIENT, 0x00202020);
        IDirect3DDevice8_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_NORMALIZENORMALS, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_LOCALVIEWER, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGCOLOR, 0x00081010);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGSTART, *(DWORD *)&fogStart);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGEND, *(DWORD *)&fogEnd);
        if (begin(name)) {
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice8_SetTexture(device, 0, (IDirect3DBaseTexture8 *)t);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
            IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(TEXLIT));
            hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, N * N * 2);
            IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
            IDirect3DDevice8_SetTexture(device, 0, NULL);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice8_EndScene(device); }
            else check_covered(name, s, 8);
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
        }
    }
    IDirect3DDevice8_LightEnable(device, 0, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_NORMALIZENORMALS, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_AMBIENT, 0);
    IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &identity);
    IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &identity);
    IDirect3DTexture8_Release(t);
    IDirect3DVertexBuffer8_Release(vb);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

/* Lit and unlit draws with a depth buffer that moves between two render targets, as in a
 * full-screen game whose back buffers take turns: a lit mesh on the left; then the depth buffer
 * goes to a render target texture and comes back; then an unlit quad and a lit mesh on the
 * right. (The depth buffer's view and the lit vertex shader once had the same host object
 * number: taking the view away took the shader with it, and the next lit vertices went through
 * the unlit shader. 3DMark 2001 SE's Car Chase, Nature and High Polygon Count were garbage.) */
static void lit_depth_switch_case(void)
{
    enum { N = 10 };
    typedef struct { float x, y, z, nx, ny, nz; } LITVERTEX;
    typedef struct { float x, y, z; DWORD diffuse; } FLATVERTEX;
    static const SAMPLE s[] = { { 40, 40, 0x8040ff }, { 48, 64, 0x8040ff }, { 80, 64, 0x8040ff }, { 88, 88, 0x8040ff },
                                { 9, 118, 0x00ff00 }, { 120, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR }, { 8, 8, CLEAR_COLOR } };
    static const FLATVERTEX flat[6] = {
        { -0.9f, -0.8f, 0.5f, 0xff00ff00 }, { -0.8f, -0.8f, 0.5f, 0xff00ff00 }, { -0.9f, -0.9f, 0.5f, 0xff00ff00 },
        { -0.9f, -0.9f, 0.5f, 0xff00ff00 }, { -0.8f, -0.8f, 0.5f, 0xff00ff00 }, { -0.8f, -0.9f, 0.5f, 0xff00ff00 } };
    static LITVERTEX half[2][N * N * 6];
    IDirect3DVertexBuffer8 *vb[2] = { NULL, NULL }, *fvb = NULL;
    IDirect3DTexture8 *rt = NULL;
    IDirect3DSurface8 *rts = NULL, *back = NULL, *depth = NULL, *oldDepth = NULL;
    D3DMATERIAL8 m;
    D3DLIGHT8 l;
    D3DMATRIX identity;
    HRESULT hr;
    int x, y, k, h;
    for (h = 0; h < 2; ++h)
        for (y = 0, k = 0; y < N; ++y)
            for (x = 0; x < N; ++x) {
                static const int cx[6] = { 0, 1, 0, 0, 1, 1 }, cy[6] = { 0, 0, 1, 1, 0, 1 };
                int j;
                for (j = 0; j < 6; ++j, ++k) {
                    half[h][k].x = (h ? 0.0f : -0.5f) + 0.5f * (float)(x + cx[j]) / N;
                    half[h][k].y = 0.5f - (float)(y + cy[j]) / N;
                    half[h][k].z = 0.5f;
                    half[h][k].nx = half[h][k].ny = 0; half[h][k].nz = -1;
                }
            }
    vb[0] = vertex_buffer(half[0], sizeof(half[0]), D3DFVF_XYZ | D3DFVF_NORMAL);
    vb[1] = vertex_buffer(half[1], sizeof(half[1]), D3DFVF_XYZ | D3DFVF_NORMAL);
    fvb = vertex_buffer(flat, sizeof(flat), D3DFVF_XYZ | D3DFVF_DIFFUSE);
    hr = vb[0] && vb[1] && fvb ? D3D_OK : E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateDepthStencilSurface(device, Side, Side, D3DFMT_D16, D3DMULTISAMPLE_NONE, &depth);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateTexture(device, Side, Side, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                           D3DPOOL_DEFAULT, &rt);
    if (SUCCEEDED(hr)) hr = IDirect3DTexture8_GetSurfaceLevel(rt, 0, &rts);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetRenderTarget(device, &back);
    if (FAILED(hr)) { fail_hr("lit_depth_switch", "create", hr); goto done; }
    IDirect3DDevice8_GetDepthStencilSurface(device, &oldDepth);
    memset(&m, 0, sizeof(m));
    m.Emissive.r = 0.5f; m.Emissive.g = 0.25f; m.Emissive.b = 1.0f; m.Emissive.a = 1.0f; m.Diffuse.a = 1.0f;
    memset(&l, 0, sizeof(l));
    l.Type = D3DLIGHT_DIRECTIONAL;
    l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = l.Diffuse.a = 1.0f;
    l.Direction.z = 1.0f;
    memset(&identity, 0, sizeof(identity));
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &identity);
    IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &identity);
    IDirect3DDevice8_SetMaterial(device, &m);
    IDirect3DDevice8_SetLight(device, 0, &l);
    IDirect3DDevice8_LightEnable(device, 0, TRUE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_AMBIENT, 0);
    IDirect3DDevice8_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetTexture(device, 0, NULL);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    hr = IDirect3DDevice8_SetRenderTarget(device, back, depth);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    if (FAILED(hr)) fail_hr("lit_depth_switch", "target", hr);
    else if (begin("lit_depth_switch")) {
        IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, TRUE);
        IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_NORMAL);
        IDirect3DDevice8_SetStreamSource(device, 0, vb[0], sizeof(LITVERTEX));
        hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, N * N * 2);
        IDirect3DDevice8_EndScene(device);
        /* The depth buffer serves the texture, then the back buffer again. */
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderTarget(device, rts, depth);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderTarget(device, back, depth);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);
        if (SUCCEEDED(hr)) {
            IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
            IDirect3DDevice8_SetStreamSource(device, 0, fvb, sizeof(FLATVERTEX));
            hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 2);
            IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, TRUE);
            IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_NORMAL);
            IDirect3DDevice8_SetStreamSource(device, 0, vb[1], sizeof(LITVERTEX));
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, N * N * 2);
            IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
            if (FAILED(hr)) { fail_hr("lit_depth_switch", "draw", hr); IDirect3DDevice8_EndScene(device); }
            else check("lit_depth_switch", s, 8);
        } else fail_hr("lit_depth_switch", "second scene", hr);
    }
    IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_LightEnable(device, 0, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice8_SetRenderTarget(device, back, oldDepth);
done:
    if (oldDepth) IDirect3DSurface8_Release(oldDepth);
    if (back) IDirect3DSurface8_Release(back);
    if (rts) IDirect3DSurface8_Release(rts);
    if (rt) IDirect3DTexture8_Release(rt);
    if (depth) IDirect3DSurface8_Release(depth);
    for (h = 0; h < 2; ++h) if (vb[h]) IDirect3DVertexBuffer8_Release(vb[h]);
    if (fvb) IDirect3DVertexBuffer8_Release(fvb);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

/* The fixed-function pipeline through a declaration with two streams and no shader code
 * (Direct3D 8's way to several streams): positions in stream 0, colours in stream 1, six unused
 * vertices first in both, drawn from vertex 6: a red quad on the left, a green one on the right. */
static void ff_streams_case(BOOL offered)
{
    static const DWORD decl[] = { D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
                                  D3DVSD_STREAM(1), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END() };
    static const SAMPLE s[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR },
                                { 120, 120, CLEAR_COLOR } };
    static const float rect[3][4] = { { 1, 1, 1, 1 }, { -0.5f, 0.5f, 0, -0.5f }, { 0, 0.5f, 0.5f, -0.5f } };
    static const DWORD colour[3] = { 0xff0000ff, 0xffff0000, 0xff00ff00 };
    static const int cx[6] = { 0, 2, 0, 0, 2, 2 }, cy[6] = { 1, 1, 3, 3, 1, 3 };
    float pos[18][3];
    DWORD col[18];
    IDirect3DVertexBuffer8 *pvb, *cvb;
    D3DMATRIX m;
    DWORD shader = 0;
    HRESULT hr;
    int k, q;
    if (!offered) { printf("D8PIX fixed-function streams not offered\n"); return; }
    for (q = 0; q < 3; ++q)
        for (k = 0; k < 6; ++k) {
            pos[q * 6 + k][0] = rect[q][cx[k]];
            pos[q * 6 + k][1] = rect[q][cy[k]];
            pos[q * 6 + k][2] = 0.5f;
            col[q * 6 + k] = colour[q];
        }
    pvb = vertex_buffer(pos, sizeof(pos), 0);
    cvb = vertex_buffer(col, sizeof(col), 0);
    hr = pvb && cvb ? IDirect3DDevice8_CreateVertexShader(device, decl, NULL, &shader, 0) : E_FAIL;
    if (FAILED(hr)) fail_hr("ff_streams", "create", hr);
    else {
        memset(&m, 0, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &m);
        IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &m);
        IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &m);
        IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
        if (begin("ff_streams")) {
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice8_SetTexture(device, 0, NULL);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            IDirect3DDevice8_SetVertexShader(device, shader);
            IDirect3DDevice8_SetStreamSource(device, 0, pvb, sizeof(pos[0]));
            IDirect3DDevice8_SetStreamSource(device, 1, cvb, sizeof(col[0]));
            hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 6, 4);
            IDirect3DDevice8_SetStreamSource(device, 0, NULL, 0);
            IDirect3DDevice8_SetStreamSource(device, 1, NULL, 0);
            if (FAILED(hr)) { fail_hr("ff_streams", "draw", hr); IDirect3DDevice8_EndScene(device); }
            else check("ff_streams", s, 4);
            IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
        }
        IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        IDirect3DDevice8_DeleteVertexShader(device, shader);
    }
    if (pvb) IDirect3DVertexBuffer8_Release(pvb);
    if (cvb) IDirect3DVertexBuffer8_Release(cvb);
}

/* One quad of a user clip plane case: red, over clip x -0.5..0.5 (the target's 32..96), with
 * plane 0 set and switched on or left off. x0: the left edge in the vertices' own space. */
static void clip_draw(const char *name, DWORD shader, const float *plane, BOOL on, float x0)
{
    static const SAMPLE cut[] = { { 40, 64, CLEAR_COLOR }, { 56, 64, CLEAR_COLOR }, { 72, 64, 0xff0000 },
                                  { 8, 8, CLEAR_COLOR } };
    static const SAMPLE whole[] = { { 40, 64, 0xff0000 }, { 56, 64, 0xff0000 }, { 72, 64, 0xff0000 },
                                    { 8, 8, CLEAR_COLOR } };
    struct { float x, y, z; DWORD color; } v[4];
    HRESULT hr;
    int i;
    for (i = 0; i < 4; ++i) {
        v[i].x = (i & 1) ? 0.5f : x0;
        v[i].y = (i & 2) ? -0.5f : 0.5f;
        v[i].z = 0.5f;
        v[i].color = 0xffff0000;
    }
    if (!begin(name)) return;
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetTexture(device, 0, NULL);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetVertexShader(device, shader);
    IDirect3DDevice8_SetClipPlane(device, 0, plane);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CLIPPLANEENABLE, on ? D3DCLIPPLANE0 : 0);
    hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
    IDirect3DDevice8_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0);
    if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice8_EndScene(device); }
    else check(name, on ? cut : whole, 4);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

/* User clip planes.
 * clip_ff: fixed-function vertices, whose plane is in world space. The view moves the world right
 * by 0.5 and the projection halves x, so the quad over world x -1.5..0.5 covers clip -0.5..0.5,
 * and the plane (1, 0, 0, 0.5), world x >= -0.5, keeps its right half. A driver that takes the
 * plane for one in clip space draws the whole quad.
 * clip_off: the same with the plane switched off again: the whole quad.
 * clip_vs: through a vertex shader (mov oPos, v0; mov oD0, v5) the plane is in clip space:
 * (1, 0, 0, 0) keeps the right half. The matrices stay set, so a driver that takes it for a
 * world space plane cuts elsewhere. */
static void clip_case(BOOL offered, BOOL shaders)
{
    static const DWORD decl[] = { D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
                                  D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END() };
    static const DWORD code[] = { 0xfffe0101,
        0x00000001, 0xc00f0000, 0x90e40000,     /* mov oPos, v0 */
        0x00000001, 0xd00f0000, 0x90e40005,     /* mov oD0, v5 */
        0x0000ffff };
    static const float world_plane[4] = { 1, 0, 0, 0.5f }, clip_plane[4] = { 1, 0, 0, 0 };
    D3DMATRIX m;
    DWORD shader = 0;
    HRESULT hr;
    if (!offered) { printf("D8PIX user clip planes not offered\n"); return; }
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &m);
    m._41 = 0.5f;
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &m);
    m._41 = 0.0f;
    m._11 = 0.5f;
    IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &m);
    clip_draw("clip_ff", D3DFVF_XYZ | D3DFVF_DIFFUSE, world_plane, TRUE, -1.5f);
    clip_draw("clip_off", D3DFVF_XYZ | D3DFVF_DIFFUSE, world_plane, FALSE, -1.5f);
    if (!shaders) printf("D8PIX clip_vs: vertex shaders not offered\n");
    else {
        hr = IDirect3DDevice8_CreateVertexShader(device, decl, code, &shader, 0);
        if (FAILED(hr)) fail_hr("clip_vs", "create", hr);
        else {
            clip_draw("clip_vs", shader, clip_plane, TRUE, -0.5f);
            IDirect3DDevice8_DeleteVertexShader(device, shader);
        }
    }
    m._11 = 1.0f;
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &m);
    IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &m);
}

/* only: run that case alone (numbered from 1), or all when zero. */
static void pixel_shaders(int only)
{
    PSVERTEX v[12];
    SAMPLE s[5];
    float want[4][3], t[3], d;
    int i, k;
    ps_textures[0] = texture2x2(texels0);
    ps_textures[1] = texture2x2(texels1);
    for (k = 0; k < 2; ++k) {
        IDirect3DDevice8_SetTexture(device, k, (IDirect3DBaseTexture8 *)ps_textures[k]);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    if (!only || only == 1) {   /* mov r0, v0 */
        static const DWORD ps[] = { PSV(1), OMov, DST(RR, 0), SRC(RV, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) { want[i][0] = unit(0x40); want[i][1] = unit(0x80); want[i][2] = unit(0xc0); }
        quad_samples(s, want);
        ps_run("ps_mov", ps, v, 1, s, 5);
    }
    if (!only || only == 97) {
        /* tex t0; mov r0, t0 with no texture on stage 0: a pixel shader reads black from an empty
         * stage (the reference rasterizers of Direct3D 8 and 9 both say so), not the white of a
         * fixed-function stage without a texture. */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OMov, DST(RR, 0), SRC(RT, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) want[i][0] = want[i][1] = want[i][2] = 0.0f;
        quad_samples(s, want);
        IDirect3DDevice8_SetTexture(device, 0, NULL);
        ps_run("ps_empty_stage", ps, v, 1, s, 5);
        IDirect3DDevice8_SetTexture(device, 0, (IDirect3DBaseTexture8 *)ps_textures[0]);
    }
    if (!only || only == 2) {   /* tex t0; mul r0, t0, v0 */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OMul, DST(RR, 0), SRC(RT, 0), SRC(RV, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0x80808080, 0, 0, 1, 1);
        d = unit(0x80);
        for (i = 0; i < 4; ++i) { texel(i, t); for (k = 0; k < 3; ++k) want[i][k] = t[k] * d; }
        quad_samples(s, want);
        ps_run("ps_texture", ps, v, 1, s, 5);
    }
    if (!only || only == 104) {
        /* tex t0 with transformed vertices while stage 0 asks for generated coordinates and a
         * texture transform. Both belong to Direct3D's own vertex processing, which transformed
         * vertices (a program's, or those the runtime shaded in software) do not go through: the
         * texture shows as the vertices map it. */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OMov, DST(RR, 0), SRC(RT, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) texel(i, want[i]);
        quad_samples(s, want);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        ps_run("ps_rhw_texgen", ps, v, 1, s, 5);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, 0);
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    if (!only || only == 3) {   /* def c0; mad r0, v0, c1, c0 with c1 set by the program; the blue sum clamps */
        static const DWORD ps[] = { PSV(1), DEF(0, F025, F05, F075, F1),
            OMad, DST(RR, 0), SRC(RV, 0), SRC(RC, 1), SRC(RC, 0), PSEND };
        static const float c1[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
        IDirect3DDevice8_SetPixelShaderConstant(device, 1, c1, 1);
        ps_quad(v, 32, 32, 96, 96, 0xff663399, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) {
            want[i][0] = 0.25f + unit(0x66) * 0.5f; want[i][1] = 0.5f + unit(0x33) * 0.5f;
            want[i][2] = 0.75f + unit(0x99) * 0.5f;
        }
        quad_samples(s, want);
        ps_run("ps_constants", ps, v, 1, s, 5);
    }
    if (!only || only == 4) {   /* tex t0; mul_x2 r0, t0_bias, 1-v0; sub_sat_d2 r0, r0, -c0: the negative
         * channels of the first result must survive until the second. */
        static const DWORD ps[] = { PSV(1), DEF(0, F02, F02, F02, F0), OTex, DST(RT, 0),
            OMul, PD(RR, 0, 15, 0, 1), PS_(RT, 0, SwXYZW, MBias), PS_(RV, 0, SwXYZW, MComp),
            OSub, PD(RR, 0, 15, 1, 15), SRC(RR, 0), PS_(RC, 0, SwXYZW, MNeg), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0x40404040, 0, 0, 1, 1);
        d = 1.0f - unit(0x40);
        for (i = 0; i < 4; ++i) {
            texel(i, t);
            for (k = 0; k < 3; ++k) want[i][k] = (clampu((t[k] - 0.5f) * d * 2.0f) + 0.2f) * 0.5f;
        }
        quad_samples(s, want);
        ps_run("ps_modifiers", ps, v, 1, s, 5);
    }
    if (!only || only == 5) {   /* tex t0; mul r0.rgb, t0, v0 + mov r0.a, c0.a, blended by that alpha */
        static const DWORD ps[] = { PSV(1), DEF(0, F0, F0, F0, F025), OTex, DST(RT, 0),
            OMul, PD(RR, 0, 7, 0, 0), SRC(RT, 0), SRC(RV, 0),
            OMov | CO, PD(RR, 0, 8, 0, 0), PS_(RC, 0, SwA, MNone), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) {
            texel(i, t);
            want[i][0] = t[0] * 0.25f; want[i][1] = t[1] * 0.25f; want[i][2] = t[2] * 0.25f + unit(0x66) * 0.75f;
        }
        quad_samples(s, want);
        IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice8_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        ps_run("ps_coissue", ps, v, 1, s, 5);
        IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (!only || only == 6) {   /* mov r0, v0; cnd r0, r0.a, c0, c1: red where alpha is over a half */
        static const DWORD ps[] = { PSV(1), DEF(0, F1, F0, F0, F1), DEF(1, F0, F1, F0, F1),
            OMov, DST(RR, 0), SRC(RV, 0), OCnd, DST(RR, 0), PS_(RR, 0, SwA, MNone), SRC(RC, 0), SRC(RC, 1), PSEND };
        static const SAMPLE cs[] = { { 40, 64, 0xff0000 }, { 88, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR } };
        ps_quad(v, 32, 32, 64, 96, 0xc0ffffff, 0, 0, 1, 1);
        ps_quad(v + 6, 64, 32, 96, 96, 0x40ffffff, 0, 0, 1, 1);
        ps_run("ps_cnd", ps, v, 2, cs, 3);
    }
    if (!only || only == 7) {   /* ps.1.2: cmp r0, v0_bias, c0, c1, per channel */
        static const DWORD ps[] = { PSV(2), DEF(0, F1, F0, F0, F1), DEF(1, F0, F1, F0, F1),
            OCmp, DST(RR, 0), PS_(RV, 0, SwXYZW, MBias), SRC(RC, 0), SRC(RC, 1), PSEND };
        static const SAMPLE cs[] = { { 64, 64, 0xffff00 }, { 8, 8, CLEAR_COLOR } };
        ps_quad(v, 32, 32, 96, 96, 0xffc040c0, 0, 0, 1, 1);
        ps_run("ps_cmp", ps, v, 1, cs, 2);
    }
    if (!only || only == 8) {   /* dp3 r0, v0_bx2, c0 */
        static const DWORD ps[] = { PSV(1), DEF(0, F06, F02, F01, F0),
            ODp3, DST(RR, 0), PS_(RV, 0, SwXYZW, MBx2), SRC(RC, 0), PSEND };
        SAMPLE cs[2];
        float x = 2 * unit(0xff) - 1, y = 2 * unit(0x80) - 1, z = 2 * unit(0x80) - 1;
        float dot = 0.6f * x + 0.2f * y + 0.1f * z;
        cs[0].x = 64; cs[0].y = 64; cs[0].rgb = rgb(dot, dot, dot);
        cs[1].x = 8; cs[1].y = 8; cs[1].rgb = CLEAR_COLOR;
        ps_quad(v, 32, 32, 96, 96, 0xffff8080, 0, 0, 1, 1);
        ps_run("ps_dp3", ps, v, 1, cs, 2);
    }
    if (!only || only == 9) {   /* texcoord t0; mov r0, t0 */
        static const DWORD ps[] = { PSV(1), OTexcoord, DST(RT, 0), OMov, DST(RR, 0), SRC(RT, 0), PSEND };
        SAMPLE cs[2];
        cs[0].x = 64; cs[0].y = 64; cs[0].rgb = rgb(0.25f, 0.75f, 0.0f);
        cs[1].x = 8; cs[1].y = 8; cs[1].rgb = CLEAR_COLOR;
        ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0.25f, 0.75f, 0.25f, 0.75f);
        ps_run("ps_texcoord", ps, v, 1, cs, 2);
    }
    if (!only || only == 10) {   /* texkill t0; mov r0, v0: the left half has negative coordinates */
        static const DWORD ps[] = { PSV(1), OTexkill, DST(RT, 0), OMov, DST(RR, 0), SRC(RV, 0), PSEND };
        static const SAMPLE cs[] = { { 40, 64, CLEAR_COLOR }, { 88, 64, 0x20e020 } };
        ps_quad(v, 32, 32, 96, 96, 0xff20e020, -1.0f, 0.5f, 1.0f, 0.5f);
        ps_run("ps_texkill", ps, v, 1, cs, 2);
    }
    if (!only || only == 11) {   /* tex t0; tex t1; lrp r0, v0.a, t0, t1 */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OTex, DST(RT, 1),
            OLrp, DST(RR, 0), PS_(RV, 0, SwA, MNone), SRC(RT, 0), SRC(RT, 1), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0x40ffffff, 0, 0, 1, 1);
        d = unit(0x40);
        for (i = 0; i < 4; ++i) {
            texel(i, t);
            want[i][0] = d * t[0] + (1 - d); want[i][1] = d * t[1] + (1 - d); want[i][2] = d * t[2];
        }
        quad_samples(s, want);
        ps_run("ps_two_textures", ps, v, 1, s, 5);
    }
    if (!only || only == 17) {   /* mov r0, v0 with fog from the specular alpha: a quarter of the colour */
        static const DWORD ps[] = { PSV(1), OMov, DST(RR, 0), SRC(RV, 0), PSEND };
        float fog = unit(0x40);
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 6; ++i) v[i].specular = 0x40000000;
        for (i = 0; i < 4; ++i) {
            want[i][0] = fog * unit(0x40); want[i][1] = fog * unit(0x80) + (1 - fog); want[i][2] = fog * unit(0xc0);
        }
        quad_samples(s, want);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGCOLOR, 0x0000ff00);
        ps_run("ps_fog_alpha", ps, v, 1, s, 5);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
    }
    if (!only || only == 18) {   /* the same with linear table fog over z 0..1, the quad at z 0.5 */
        static const DWORD ps[] = { PSV(1), OMov, DST(RR, 0), SRC(RV, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) {
            want[i][0] = 0.5f * unit(0x40); want[i][1] = 0.5f * unit(0x80) + 0.5f; want[i][2] = 0.5f * unit(0xc0);
        }
        quad_samples(s, want);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGCOLOR, 0x0000ff00);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGSTART, F0);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGEND, F1);
        ps_run("ps_fog_table", ps, v, 1, s, 5);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
    }
    if (!only || only == 19 || only == 20) {
        /* Untransformed quad facing a white directional light, lit from the
         * material: mov r0, v0 gives the material diffuse colour; 20 adds
         * linear vertex fog over camera z 0..1 with the quad at 0.5. */
        static const DWORD ps[] = { PSV(1), OMov, DST(RR, 0), SRC(RV, 0), PSEND };
        typedef struct { float x, y, z, nx, ny, nz; } LITVERTEX;
        LITVERTEX q[6] = {
            { -0.5f, 0.5f, 0.5f, 0, 0, -1 }, { 0.5f, 0.5f, 0.5f, 0, 0, -1 }, { -0.5f, -0.5f, 0.5f, 0, 0, -1 },
            { -0.5f, -0.5f, 0.5f, 0, 0, -1 }, { 0.5f, 0.5f, 0.5f, 0, 0, -1 }, { 0.5f, -0.5f, 0.5f, 0, 0, -1 } };
        D3DMATERIAL8 m;
        D3DLIGHT8 l;
        D3DMATRIX identity;
        DWORD handle = 0;
        int fogged;
        memset(&m, 0, sizeof(m));
        m.Diffuse.r = 0.5f; m.Diffuse.g = 0.25f; m.Diffuse.b = 1.0f; m.Diffuse.a = 1.0f;
        memset(&l, 0, sizeof(l));
        l.Type = D3DLIGHT_DIRECTIONAL;
        l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = l.Diffuse.a = 1.0f;
        l.Direction.z = 1.0f;
        memset(&identity, 0, sizeof(identity));
        identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
        for (fogged = 0; fogged < 2; ++fogged) {
            const char *name = fogged ? "ps_lit_fog" : "ps_lit";
            float f = fogged ? 0.5f : 1.0f;
            HRESULT hr;
            if (only && only != 19 + fogged) continue;
            for (i = 0; i < 4; ++i) { want[i][0] = 0.5f * f; want[i][1] = 0.25f * f + (1 - f); want[i][2] = f; }
            quad_samples(s, want);
            IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
            IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &identity);
            IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &identity);
            IDirect3DDevice8_SetMaterial(device, &m);
            IDirect3DDevice8_SetLight(device, 0, &l);
            IDirect3DDevice8_LightEnable(device, 0, TRUE);
            IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, TRUE);
            IDirect3DDevice8_SetRenderState(device, D3DRS_AMBIENT, 0);
            IDirect3DDevice8_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
            if (fogged) {
                IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
                IDirect3DDevice8_SetRenderState(device, D3DRS_FOGCOLOR, 0x0000ff00);
                IDirect3DDevice8_SetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
                IDirect3DDevice8_SetRenderState(device, D3DRS_FOGSTART, F0);
                IDirect3DDevice8_SetRenderState(device, D3DRS_FOGEND, F1);
            }
            hr = IDirect3DDevice8_CreatePixelShader(device, ps, &handle);
            if (FAILED(hr)) fail_hr(name, "create", hr);
            else if (begin(name)) {
                IDirect3DDevice8_SetPixelShader(device, handle);
                IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_NORMAL);
                hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, q, sizeof(q[0]));
                IDirect3DDevice8_SetPixelShader(device, 0);
                if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice8_EndScene(device); }
                else check(name, s, 5);
            }
            if (handle) IDirect3DDevice8_DeletePixelShader(device, handle);
            handle = 0;
            IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice8_SetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
            IDirect3DDevice8_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
        }
    }
    if (!only || only == 21) {   /* tex t1; add r0, t1, v0 with no texture on stage 1: an unbound stage
         * samples (0,0,0,1), so the quad is the vertex colour (the draw must not be dropped) */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 1), OAdd, DST(RR, 0), SRC(RT, 1), SRC(RV, 0), PSEND };
        IDirect3DDevice8_SetTexture(device, 1, NULL);
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) { want[i][0] = unit(0x40); want[i][1] = unit(0x80); want[i][2] = unit(0xc0); }
        quad_samples(s, want);
        ps_run("ps_unbound_stage", ps, v, 1, s, 5);
        IDirect3DDevice8_SetTexture(device, 1, (IDirect3DBaseTexture8 *)ps_textures[1]);
    }
    if (only == 12) {   /* tex t0; add r0, t0_bias, c0 */
        static const DWORD ps[] = { PSV(1), DEF(0, F05, F05, F05, F05), OTex, DST(RT, 0),
            OAdd, DST(RR, 0), PS_(RT, 0, SwXYZW, MBias), SRC(RC, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) { texel(i, t); for (k = 0; k < 3; ++k) want[i][k] = t[k]; }
        quad_samples(s, want);
        ps_run("ps_bias", ps, v, 1, s, 5);
    }
    if (only == 13) {   /* mov r0, 1-v0 */
        static const DWORD ps[] = { PSV(1), OMov, DST(RR, 0), PS_(RV, 0, SwXYZW, MComp), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) { want[i][0] = 1 - unit(0x40); want[i][1] = 1 - unit(0x80); want[i][2] = 1 - unit(0xc0); }
        quad_samples(s, want);
        ps_run("ps_complement", ps, v, 1, s, 5);
    }
    if (only == 14) {   /* tex t0; mul_x2 r0, t0, v0 */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OMul, PD(RR, 0, 15, 0, 1), SRC(RT, 0), SRC(RV, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0x40404040, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) { texel(i, t); for (k = 0; k < 3; ++k) want[i][k] = t[k] * unit(0x40) * 2; }
        quad_samples(s, want);
        ps_run("ps_x2", ps, v, 1, s, 5);
    }
    if (only == 15) {   /* tex t0; mov_sat_d2 r0, t0 */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OMov, PD(RR, 0, 15, 1, 15), SRC(RT, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) { texel(i, t); for (k = 0; k < 3; ++k) want[i][k] = t[k] * 0.5f; }
        quad_samples(s, want);
        ps_run("ps_sat_d2", ps, v, 1, s, 5);
    }
    if (only == 16) {   /* def c0; add r0, v0, -c0 */
        static const DWORD ps[] = { PSV(1), DEF(0, F02, F02, F02, F0), OAdd, DST(RR, 0), SRC(RV, 0),
            PS_(RC, 0, SwXYZW, MNeg), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) { want[i][0] = unit(0x40) - 0.2f; want[i][1] = unit(0x80) - 0.2f; want[i][2] = unit(0xc0) - 0.2f; }
        quad_samples(s, want);
        ps_run("ps_negative_constant", ps, v, 1, s, 5);
    }
    for (k = 0; k < 2; ++k) {
        IDirect3DDevice8_SetTexture(device, k, NULL);
        if (ps_textures[k]) IDirect3DTexture8_Release(ps_textures[k]);
    }
}

/* Version 1.4 pixel shaders on the Direct3D 8 interface. They read six stages, two more than the
 * four fixed-function stages a Direct3D 8 program is offered. */
typedef struct { float x, y, z, rhw, t0[2], t1[4], t2[3], t3[2], t4[2]; } WATERVERTEX;
#define WATERFVF (D3DFVF_XYZRHW | D3DFVF_TEX5 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE4(1) | \
                  D3DFVF_TEXCOORDSIZE3(2) | D3DFVF_TEXCOORDSIZE2(3) | D3DFVF_TEXCOORDSIZE2(4))

static IDirect3DTexture8 *plain2x2(DWORD colour)
{
    DWORD texels[4];
    texels[0] = texels[1] = texels[2] = texels[3] = colour;
    return texture2x2(texels);
}

static void ps14_cases(int only)
{
    static const SAMPLE inside[5] = { { 48, 48, 0 }, { 80, 48, 0 }, { 48, 80, 0 }, { 80, 80, 0 }, { 64, 64, 0 } };
    IDirect3DTexture8 *t[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    SAMPLE s[6];
    HRESULT hr, set4, set5;
    DWORD handle;
    int i, k;
    for (k = 0; k < 6; ++k) {
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    if (!only || only == 98) {
        /* texld r0, t0; texld r4, t0; texld r5, t0; add r0, r0, r4; add r0, r0, r5: stage 0 holds
         * red 0x40, stage 4 green 0x80 and stage 5 blue 0xc0. */
        static const DWORD ps[] = { PSV(4), OTex, DST(RR, 0), SRC(RT, 0), OTex, DST(RR, 4), SRC(RT, 0),
            OTex, DST(RR, 5), SRC(RT, 0), OAdd, DST(RR, 0), SRC(RR, 0), SRC(RR, 4),
            OAdd, DST(RR, 0), SRC(RR, 0), SRC(RR, 5), PSEND };
        PSVERTEX v[6];
        t[0] = plain2x2(0xff400000); t[4] = plain2x2(0xff008000); t[5] = plain2x2(0xff0000c0);
        ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
        for (i = 0; i < 5; ++i) { s[i] = inside[i]; s[i].rgb = 0x4080c0; }
        s[5].x = s[5].y = 8; s[5].rgb = CLEAR_COLOR;
        IDirect3DDevice8_SetTexture(device, 0, (IDirect3DBaseTexture8 *)t[0]);
        set4 = IDirect3DDevice8_SetTexture(device, 4, (IDirect3DBaseTexture8 *)t[4]);
        set5 = IDirect3DDevice8_SetTexture(device, 5, (IDirect3DBaseTexture8 *)t[5]);
        if (FAILED(set4) || FAILED(set5))
            printf("D8PIX case=ps14_six_stages set_texture stage4=%08lx stage5=%08lx\n", (unsigned long)set4, (unsigned long)set5);
        ps_run("ps14_six_stages", ps, v, 1, s, 6);
        for (k = 0; k < 6; ++k) {
            IDirect3DDevice8_SetTexture(device, k, NULL);
            if (t[k]) { IDirect3DTexture8_Release(t[k]); t[k] = NULL; }
        }
    }
    if (!only || only == 99) {
        /* The water of 3DMark 2001 SE's Advanced Pixel Shader test, token for token:
         *   texld r0, t0; texcrd r1.xy, t1_dw.xyw; texcrd r2.xyz, t2; bem r1.xy, r1, r0; mov r1.z, r2.z;
         *   add r2.xyz, r1, r2; phase; texld r1, r1; texld r2, r2; texld r3, t3; texld r4, t4;
         *   mul r3, r3, r4; lrp r0, r3, r2, r1; mul r0, r0, c0
         * Stage 0 holds red, so the bump matrix of stage 1 (0.5, 0, 0, 0) moves r1 = (0.6, 0.6) / 2 to
         * (0.8, 0.3): the green texel of stage 1. r2 = r1 + (0, 0.5) reads the blue texel of stage 2.
         * Stage 3 holds 0x80 and stage 4 white, so the result is half blue, half green, times
         * c0 = (1, 1, 0.5, 1). */
        static const DWORD ps[] = { 0xffff0104UL, 0x42, 0x800f0000UL, 0xb0e40000UL, 0x40, 0x80030001UL, 0xbaf40001UL,
            0x40, 0x80070002UL, 0xb0e40002UL, 0x59, 0x80030001UL, 0x80e40001UL, 0x80e40000UL,
            0x1, 0x80040001UL, 0x80aa0002UL, 0x2, 0x80070002UL, 0x80e40001UL, 0x80e40002UL, 0xfffd,
            0x42, 0x800f0001UL, 0x80e40001UL, 0x42, 0x800f0002UL, 0x80e40002UL, 0x42, 0x800f0003UL, 0xb0e40003UL,
            0x42, 0x800f0004UL, 0xb0e40004UL, 0x5, 0x800f0003UL, 0x80e40003UL, 0x80e40004UL,
            0x12, 0x800f0000UL, 0x80e40003UL, 0x80e40002UL, 0x80e40001UL, 0x5, 0x800f0000UL, 0x80e40000UL, 0xa0e40000UL,
            0xffff };
        static const DWORD corner[4] = { 0xff000000, 0xff000000, 0xff000000, 0xff0000ff };
        static const float c0[4] = { 1.0f, 1.0f, 0.5f, 1.0f };
        static const float xs[6] = { 32, 96, 32, 32, 96, 96 }, ys[6] = { 32, 32, 96, 96, 32, 96 };
        const float half = 0.5f, zero = 0.0f;
        WATERVERTEX v[6];
        memset(v, 0, sizeof(v));
        for (i = 0; i < 6; ++i) {
            v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 0.5f; v[i].rhw = 1.0f;
            v[i].t0[0] = v[i].t0[1] = 0.5f;
            v[i].t1[0] = v[i].t1[1] = 0.6f; v[i].t1[3] = 2.0f;
            v[i].t2[1] = 0.5f;
            v[i].t3[0] = v[i].t3[1] = v[i].t4[0] = v[i].t4[1] = 0.5f;
        }
        t[0] = plain2x2(0xffff0000); t[1] = texture2x2(texels0); t[2] = texture2x2(corner);
        t[3] = plain2x2(0xff808080); t[4] = plain2x2(0xffffffff);
        for (i = 0; i < 5; ++i) { s[i] = inside[i]; s[i].rgb = 0x007f40; }
        s[5].x = s[5].y = 8; s[5].rgb = CLEAR_COLOR;
        for (k = 0; k < 5; ++k) IDirect3DDevice8_SetTexture(device, k, (IDirect3DBaseTexture8 *)t[k]);
        IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT00, *(const DWORD *)&half);
        IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT01, *(const DWORD *)&zero);
        IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT10, *(const DWORD *)&zero);
        IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT11, *(const DWORD *)&zero);
        handle = 0;
        hr = IDirect3DDevice8_CreatePixelShader(device, ps, &handle);
        if (FAILED(hr)) fail_hr("ps14_water", "create", hr);
        else {
            if (begin("ps14_water")) {
                IDirect3DDevice8_SetPixelShader(device, handle);
                IDirect3DDevice8_SetPixelShaderConstant(device, 0, c0, 1);
                IDirect3DDevice8_SetVertexShader(device, WATERFVF);
                hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
                IDirect3DDevice8_SetPixelShader(device, 0);
                if (FAILED(hr)) { fail_hr("ps14_water", "draw", hr); IDirect3DDevice8_EndScene(device); }
                else check("ps14_water", s, 6);
            }
            IDirect3DDevice8_DeletePixelShader(device, handle);
        }
        IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT00, *(const DWORD *)&zero);
        for (k = 0; k < 6; ++k) {
            IDirect3DDevice8_SetTexture(device, k, NULL);
            if (t[k]) { IDirect3DTexture8_Release(t[k]); t[k] = NULL; }
        }
    }
}

/* 100: eight fixed-function stages on the Direct3D 8 interface. Stages 0 to 6 add a black texture
 * each (their coordinate sets are all 0), stage 7 modulates the diffuse white by a texture red on
 * its left and green on its right, read with coordinate set 7. */
static void stages8_case(int only, BOOL offered)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float st[8][2]; } V8;
    static const DWORD black[4] = { 0xff000000, 0xff000000, 0xff000000, 0xff000000 };
    static const DWORD halves[4] = { 0xffff0000, 0xff00ff00, 0xffff0000, 0xff00ff00 };
    static const SAMPLE s[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR } };
    IDirect3DTexture8 *dark, *last;
    V8 v[4];
    HRESULT hr;
    int i, k;
    if (only && only != 100) return;
    if (!offered) { printf("D8PIX eight texture stages not offered\n"); return; }
    dark = texture2x2(black);
    last = texture2x2(halves);
    if (!dark || !last) { printf("D8PIX case=stages8 textures failed\n"); ++failures; goto done; }
    memset(v, 0, sizeof(v));
    for (i = 0; i < 4; ++i) {
        v[i].x = i & 1 ? 96.0f : 32.0f;
        v[i].y = i & 2 ? 96.0f : 32.0f;
        v[i].z = 0.5f; v[i].rhw = 1; v[i].diffuse = 0xffffffff;
        v[i].st[7][0] = i & 1 ? 1.0f : 0.0f;
        v[i].st[7][1] = i & 2 ? 1.0f : 0.0f;
    }
    for (k = 0; k < 8; ++k) {
        IDirect3DDevice8_SetTexture(device, k, (IDirect3DBaseTexture8 *)(k < 7 ? dark : last));
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_COLOROP, k < 7 ? D3DTOP_ADD : D3DTOP_MODULATE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_COLORARG2, k ? D3DTA_CURRENT : D3DTA_DIFFUSE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ALPHAARG1, k ? D3DTA_CURRENT : D3DTA_DIFFUSE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_TEXCOORDINDEX, k);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_MINFILTER, D3DTEXF_POINT);
    }
    if (begin("stages8")) {
        IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX8);
        hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr("stages8", "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check("stages8", s, 3);
    }
done:
    for (k = 7; k >= 0; --k) {
        IDirect3DDevice8_SetTexture(device, k, NULL);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_COLOROP, k ? D3DTOP_DISABLE : D3DTOP_MODULATE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_COLORARG2, D3DTA_CURRENT);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ALPHAOP, k ? D3DTOP_DISABLE : D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(device, k, D3DTSS_TEXCOORDINDEX, k);
    }
    if (dark) IDirect3DTexture8_Release(dark);
    if (last) IDirect3DTexture8_Release(last);
}

/* 101 to 103: transformed vertices on a stage that asks for generated texture coordinates
 * (D3DTSS_TCI_CAMERASPACEPOSITION in D3DTSS_TEXCOORDINDEX). Direct3D generates coordinates while
 * it transforms vertices; a transformed vertex keeps its own.
 * rhw_texgen: the vertex's own coordinates read a texture red on its left and green on its right.
 * rhw_texgen_diffuse: the stage reads no texture at all, the vertex has no coordinates.
 * rhw_texgen_no_coords: the stage reads the texture, the vertex has no coordinates. */
static void rhw_texgen_cases(int only)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } VT;
    typedef struct { float x, y, z, rhw; DWORD diffuse; } VD;
    static const DWORD halves[4] = { 0xffff0000, 0xff00ff00, 0xffff0000, 0xff00ff00 };
    static const SAMPLE split[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR } };
    static const SAMPLE plain[] = { { 48, 64, 0x4080c0 }, { 80, 64, 0x4080c0 }, { 8, 8, CLEAR_COLOR } };
    static const SAMPLE first[] = { { 48, 64, 0xff0000 }, { 80, 64, 0xff0000 }, { 8, 8, CLEAR_COLOR } };
    static const char *const names[3] = { "rhw_texgen", "rhw_texgen_diffuse", "rhw_texgen_no_coords" };
    const SAMPLE *const want[3] = { split, plain, first };
    IDirect3DTexture8 *t;
    VT vt[4];
    VD vd[4];
    HRESULT hr;
    int i, k;
    if (only && (only < 101 || only > 103)) return;
    t = texture2x2(halves);
    if (!t) { printf("D8PIX case=rhw_texgen texture failed\n"); ++failures; return; }
    for (i = 0; i < 4; ++i) {
        vt[i].x = vd[i].x = i & 1 ? 96.0f : 32.0f;
        vt[i].y = vd[i].y = i & 2 ? 96.0f : 32.0f;
        vt[i].z = vd[i].z = 0.5f; vt[i].rhw = vd[i].rhw = 1.0f;
        vt[i].diffuse = vd[i].diffuse = 0xff4080c0;
        vt[i].u = i & 1 ? 1.0f : 0.0f;
        vt[i].v = i & 2 ? 1.0f : 0.0f;
    }
    IDirect3DDevice8_SetTexture(device, 0, (IDirect3DBaseTexture8 *)t);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
    for (k = 0; k < 3; ++k) {
        if (only && only != 101 + k) continue;
        IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, k == 1 ? D3DTOP_SELECTARG2 : D3DTOP_SELECTARG1);
        if (!begin(names[k])) continue;
        if (k == 0) {
            IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, vt, sizeof(vt[0]));
        } else {
            IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, vd, sizeof(vd[0]));
        }
        if (FAILED(hr)) { fail_hr(names[k], "draw", hr); IDirect3DDevice8_EndScene(device); }
        else check(names[k], want[k], 3);
    }
    IDirect3DDevice8_SetTexture(device, 0, NULL);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DTexture8_Release(t);
}

int main(int argc, char **argv)
{
    IDirect3D8 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d8.dll");
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    D3DCAPS8 caps;
    HWND window;
    DWORD flags;
    BOOL software = FALSE, reference = FALSE, noref = FALSE;
    char refText[40];
    int only = 0, a;
    HRESULT hr;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* "sw": software vertex processing; "ps=N": only pixel shader case N. */
    for (a = 1; a < argc; ++a) {
        if (!lstrcmpiA(argv[a], "sw")) software = TRUE;
        /* "ref", "-noref", "-noskips": the automatic checks, as in d9pixels (drvcheck.h). */
        else if (!lstrcmpiA(argv[a], "ref")) reference = TRUE;
        else if (!lstrcmpiA(argv[a], "-noref")) noref = TRUE;
        else if (!lstrcmpiA(argv[a], "-noskips")) skipsOff = 1;
        else if (!strncmp(argv[a], "ps=", 3)) only = atoi(argv[a] + 3);
    }
    create = lib ? (IDirect3D8 *(WINAPI *)(UINT))GetProcAddress(lib, "Direct3DCreate8") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d && create) {
        /* A DirectX 8.0 runtime (the last for Windows 95) refuses the 8.1 headers' version; its
         * own is 120, and the interfaces used here are the same. */
        d3d = create(120);
        if (d3d) printf("D8PIX runtime=8.0 (SDK version 120)\n");
    }
    if (!d3d) { printf("D8PIX no Direct3D 8 runtime\nD8PIX end result=FAIL\n"); return 1; }
    {
        D3DADAPTER_IDENTIFIER8 id;
        D3DDISPLAYMODE now;
        if (SUCCEEDED(IDirect3D8_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &id)))
            printf("D8PIX adapter driver=%s version=%08lx%08lx\n", id.Driver,
                   (unsigned long)id.DriverVersion.HighPart, (unsigned long)id.DriverVersion.LowPart);
        IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &now);
        printf("D8PIX mode=%lux%lu format=%lu modes=%lu check_type=%08lx\n", (unsigned long)now.Width,
               (unsigned long)now.Height, (unsigned long)now.Format,
               (unsigned long)IDirect3D8_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT),
               (unsigned long)IDirect3D8_CheckDeviceType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, now.Format,
                                                         now.Format, TRUE));
    }
    hr = IDirect3D8_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr)) { printf("D8PIX caps hr=%08lx\nD8PIX end result=UNAVAILABLE\n", (unsigned long)hr); return 2; }
    {
        /* Every word of the record, for comparison with what the driver reports. */
        const DWORD *w = (const DWORD *)&caps;
        UINT i;
        for (i = 0; i < sizeof(caps) / 4; ++i)
            printf("%s%08lx%s", i % 14 ? " " : "D8CAPS ", (unsigned long)w[i], i % 14 == 13 || i + 1 == sizeof(caps) / 4 ? "\n" : "");
    }
    printf("D8PIX caps devcaps=%08lx vs=%08lx ps=%08lx streams=%lu stride=%lu primitives=%lu index=%lx stages=%lu textures=%lu\n",
           (unsigned long)caps.DevCaps, (unsigned long)caps.VertexShaderVersion,
           (unsigned long)caps.PixelShaderVersion, (unsigned long)caps.MaxStreams,
           (unsigned long)caps.MaxStreamStride, (unsigned long)caps.MaxPrimitiveCount,
           (unsigned long)caps.MaxVertexIndex, (unsigned long)caps.MaxTextureBlendStages,
           (unsigned long)caps.MaxSimultaneousTextures);
    IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    window = CreateWindowA("STATIC", "d8pixels", WS_POPUP | WS_VISIBLE, 0, 0, Side, Side, NULL, NULL, NULL, NULL);
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = pp.BackBufferHeight = Side;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = window;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    flags = !software && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ?
            D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, reference ? D3DDEVTYPE_REF : D3DDEVTYPE_HAL, window, flags, &pp, &device);
    if (reference && FAILED(hr)) {
        printf("D8PIX reference not available hr=%08lx\nD8PIX end result=NOREFERENCE\n", (unsigned long)hr);
        return 0;
    }
    printf("D8PIX device hr=%08lx vertex_processing=%s format=%lu\n", (unsigned long)hr,
           flags == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software", (unsigned long)mode.Format);
    if (FAILED(hr)) { printf("D8PIX end result=UNAVAILABLE\n"); return 2; }
    if (reference) {
        /* A runtime without the reference rasterizer still makes the device, and it draws nothing:
         * clear to a colour of our own and read it back before trusting its pictures. */
        IDirect3DSurface8 *probe = NULL;
        D3DLOCKED_RECT lr;
        DWORD seen = 0;
        IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff123456UL, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice8_GetBackBuffer(device, 0, D3DBACKBUFFER_TYPE_MONO, &probe))) {
            if (SUCCEEDED(IDirect3DSurface8_LockRect(probe, &lr, NULL, D3DLOCK_READONLY))) {
                seen = *(const DWORD *)lr.pBits & 0xffffff;
                IDirect3DSurface8_UnlockRect(probe);
            }
            IDirect3DSurface8_Release(probe);
        }
        if (seen != 0x123456) {
            printf("D8PIX reference not available: its device draws nothing (%06lx)\nD8PIX end result=NOREFERENCE\n", (unsigned long)seen);
            return 0;
        }
        printf("D8PIX reference rasterizer: pictures kept in refpix8\n");
    }
    refFolder = "refpix8";
    refMode = reference ? RefWrite : noref ? RefOff : RefCompare;
    if (reference) skipsOff = 1;
    skips_start();
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (!only) {
        case_clear();
        case_up();
        case_vb();
        case_strip();
        indexed("indexed", 0, 0x00ffff);
        indexed("base", 3, 0xff8000);
        case_texture();
        case_transform();
        vs_case("vs11", 0xfffe0101, caps.VertexShaderVersion >= D3DVS_VERSION(1, 1) && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT), FALSE);
        vs_case("vs11_buffer", 0xfffe0101, caps.VertexShaderVersion >= D3DVS_VERSION(1, 1) && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT), TRUE);
        vs_case("vs10", 0xfffe0100, caps.VertexShaderVersion >= D3DVS_VERSION(1, 0) && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT), FALSE);
        ff_streams_case(caps.MaxStreams >= 2 && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT));
        if (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) { lit_grid_case(); lit_variants_case(); lit_perspective_case(); lit_replay_case(); lit_depth_switch_case(); }
        clip_case(caps.MaxUserClipPlanes >= 1, caps.VertexShaderVersion >= D3DVS_VERSION(1, 1) && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT));
    }
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 2)) pixel_shaders(only);
    else printf("D8PIX pixel shaders not offered\n");
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 4)) ps14_cases(only);
    stages8_case(only, caps.MaxTextureBlendStages >= 8 && caps.MaxSimultaneousTextures >= 8);
    rhw_texgen_cases(only);
    IDirect3DDevice8_Release(device);
    IDirect3D8_Release(d3d);
    DestroyWindow(window);
    refText[0] = 0;
    if (refMode == RefCompare) {
        if (refCompared) sprintf(refText, " ref_compared=%d ref_failed=%d", refCompared, refFailed);
        else strcpy(refText, " ref=unavailable");
    }
    printf("D8PIX end pass=%d fail=%d%s%s result=%s\n", passes, failures, skips_summary(), refText,
           reference ? "REFERENCE" : failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
