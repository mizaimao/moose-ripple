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

enum { Side = 128 };
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
    int i, bad = 0;
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
    IDirect3DSurface8_UnlockRect(back);
    IDirect3DSurface8_Release(back);
    printf("D8PIX case=%s samples=%d mismatches=%d\n", name, count, bad);
    if (bad) ++failures; else ++passes;
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
    if (!only || only == 2) {   /* tex t0; mul r0, t0, v0 */
        static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OMul, DST(RR, 0), SRC(RT, 0), SRC(RV, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0x80808080, 0, 0, 1, 1);
        d = unit(0x80);
        for (i = 0; i < 4; ++i) { texel(i, t); for (k = 0; k < 3; ++k) want[i][k] = t[k] * d; }
        quad_samples(s, want);
        ps_run("ps_texture", ps, v, 1, s, 5);
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

int main(int argc, char **argv)
{
    IDirect3D8 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d8.dll");
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    D3DCAPS8 caps;
    HWND window;
    DWORD flags;
    BOOL software = FALSE;
    int only = 0, a;
    HRESULT hr;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* "sw": software vertex processing; "ps=N": only pixel shader case N. */
    for (a = 1; a < argc; ++a) {
        if (!lstrcmpiA(argv[a], "sw")) software = TRUE;
        else if (!strncmp(argv[a], "ps=", 3)) only = atoi(argv[a] + 3);
    }
    create = lib ? (IDirect3D8 *(WINAPI *)(UINT))GetProcAddress(lib, "Direct3DCreate8") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
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
    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &pp, &device);
    printf("D8PIX device hr=%08lx vertex_processing=%s format=%lu\n", (unsigned long)hr,
           flags == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software", (unsigned long)mode.Format);
    if (FAILED(hr)) { printf("D8PIX end result=UNAVAILABLE\n"); return 2; }
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
    }
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 2)) pixel_shaders(only);
    else printf("D8PIX pixel shaders not offered\n");
    IDirect3DDevice8_Release(device);
    IDirect3D8_Release(d3d);
    DestroyWindow(window);
    printf("D8PIX end pass=%d fail=%d result=%s\n", passes, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
