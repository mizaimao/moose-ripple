/* Check what a Direct3D 9 program draws on the hardware device: a clear,
 * transformed triangles from user memory, a vertex buffer, an index buffer
 * with and without a base vertex, a strip, a managed texture and untransformed
 * vertices, fourteen pixel shaders of versions 1.1 and 1.2, including fog
 * and lit untransformed vertices, nine of shader model 2.0, seven with
 * vertex shaders 1.1 and 2.0 (hardware ones when offered) or blending
 * after shaders, two each of cube maps, render target textures and volumes,
 * luminance textures, a bump map read by texbem, a colour write mask, DOT3 and texture
 * transforms. Each case
 * draws on a 128x128 lockable back buffer and inspects a few pixels well
 * inside the shapes. "sw" asks for software vertex
 * processing. Built against the Microsoft headers; d3d9.dll is loaded at
 * run time. The Direct3D 9 version of d8pixels. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
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

static IDirect3D9 *d3d;
static IDirect3DDevice9 *device;
static int passes, failures;

static void fail_hr(const char *name, const char *what, HRESULT hr)
{
    printf("D9PIX case=%s %s hr=%08lx\n", name, what, (unsigned long)hr);
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
    HRESULT hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1.0f, 0);
    if (FAILED(hr)) { fail_hr(name, "clear", hr); return FALSE; }
    hr = IDirect3DDevice9_BeginScene(device);
    if (FAILED(hr)) { fail_hr(name, "begin", hr); return FALSE; }
    return TRUE;
}

/* End the scene, read the back buffer and compare the samples. */
static void check(const char *name, const SAMPLE *s, int count)
{
    IDirect3DSurface9 *back = NULL;
    D3DLOCKED_RECT r;
    HRESULT hr = IDirect3DDevice9_EndScene(device);
    int i, bad = 0;
    if (FAILED(hr)) { fail_hr(name, "end", hr); return; }
    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (FAILED(hr)) { fail_hr(name, "backbuffer", hr); return; }
    hr = IDirect3DSurface9_LockRect(back, &r, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) { fail_hr(name, "lock", hr); IDirect3DSurface9_Release(back); return; }
    for (i = 0; i < count; ++i) {
        DWORD got = ((const DWORD *)((const BYTE *)r.pBits + s[i].y * r.Pitch))[s[i].x] & 0xffffff;
        DWORD want = s[i].rgb & 0xffffff;
        int k, off = 0;
        for (k = 0; k < 24; k += 8) {
            int a = (got >> k) & 255, b = (want >> k) & 255;
            if (a - b > 2 || b - a > 2) off = 1;
        }
        if (off) {
            printf("D9PIX case=%s sample=%d at=%d,%d got=%06lx want=%06lx\n", name, i, s[i].x, s[i].y,
                   (unsigned long)got, (unsigned long)want);
            ++bad;
        }
    }
    IDirect3DSurface9_UnlockRect(back);
    IDirect3DSurface9_Release(back);
    printf("D9PIX case=%s samples=%d mismatches=%d\n", name, count, bad);
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
    IDirect3DDevice9_SetFVF(device, TLFVF);
    hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
    if (FAILED(hr)) { fail_hr("up", "draw", hr); IDirect3DDevice9_EndScene(device); return; }
    check("up", s, 3);
}

static IDirect3DVertexBuffer9 *vertex_buffer(const void *data, UINT bytes, DWORD fvf)
{
    IDirect3DVertexBuffer9 *vb = NULL;
    BYTE *p;
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(device, bytes, D3DUSAGE_WRITEONLY, fvf, D3DPOOL_DEFAULT, &vb, NULL)))
        return NULL;
    if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&p, 0))) { IDirect3DVertexBuffer9_Release(vb); return NULL; }
    memcpy(p, data, bytes);
    IDirect3DVertexBuffer9_Unlock(vb);
    return vb;
}

static void case_vb(void)
{
    static const SAMPLE s[] = { { 64, 64, 0x00ff00 }, { 90, 90, 0x00ff00 }, { 120, 8, CLEAR_COLOR } };
    TLVERTEX v[12];
    IDirect3DVertexBuffer9 *vb;
    HRESULT hr;
    quad(v, 0, 0, 1, 1, 0xff0000ff);            /* six unused vertices first */
    quad(v + 6, 32, 32, 96, 96, 0xff00ff00);
    vb = vertex_buffer(v, sizeof(v), TLFVF);
    if (!vb) { fail_hr("vb", "create", E_FAIL); return; }
    if (begin("vb")) {
        IDirect3DDevice9_SetFVF(device, TLFVF);
        IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(v[0]));
        hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 6, 2);
        if (FAILED(hr)) { fail_hr("vb", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("vb", s, 3);
    }
    IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    IDirect3DVertexBuffer9_Release(vb);
}

static void case_strip(void)
{
    static const SAMPLE s[] = { { 64, 64, 0xffff00 }, { 34, 94, 0xffff00 }, { 94, 34, 0xffff00 }, { 8, 64, CLEAR_COLOR } };
    TLVERTEX v[4] = {
        { 32, 32, 0.5f, 1, 0xffffff00 }, { 96, 32, 0.5f, 1, 0xffffff00 },
        { 32, 96, 0.5f, 1, 0xffffff00 }, { 96, 96, 0.5f, 1, 0xffffff00 } };
    IDirect3DVertexBuffer9 *vb = vertex_buffer(v, sizeof(v), TLFVF);
    HRESULT hr;
    if (!vb) { fail_hr("strip", "create", E_FAIL); return; }
    if (begin("strip")) {
        IDirect3DDevice9_SetFVF(device, TLFVF);
        IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(v[0]));
        hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, 0, 2);
        if (FAILED(hr)) { fail_hr("strip", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("strip", s, 4);
    }
    IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    IDirect3DVertexBuffer9_Release(vb);
}

/* Four corners after base unused vertices, drawn through 16-bit indices. */
static void indexed(const char *name, UINT base, DWORD color)
{
    SAMPLE s[] = { { 64, 64, 0 }, { 36, 92, 0 }, { 120, 120, CLEAR_COLOR } };
    TLVERTEX v[8];
    static const WORD idx[6] = { 0, 1, 2, 2, 1, 3 };
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9 *ib = NULL;
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
    hr = IDirect3DDevice9_CreateIndexBuffer(device, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                            D3DPOOL_DEFAULT, &ib, NULL);
    if (FAILED(hr)) { fail_hr(name, "index buffer", hr); IDirect3DVertexBuffer9_Release(vb); return; }
    if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&p, 0))) {
        memcpy(p, idx, sizeof(idx));
        IDirect3DIndexBuffer9_Unlock(ib);
    }
    if (begin(name)) {
        IDirect3DDevice9_SetFVF(device, TLFVF);
        IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(v[0]));
        IDirect3DDevice9_SetIndices(device, ib);
        hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, base, 0, 4, 0, 2);
        if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check(name, s, 3);
    }
    IDirect3DDevice9_SetIndices(device, NULL);
    IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    IDirect3DIndexBuffer9_Release(ib);
    IDirect3DVertexBuffer9_Release(vb);
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
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT r;
    HRESULT hr = IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL);
    int y;
    if (FAILED(hr)) { fail_hr("texture", "create", hr); return; }
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &r, NULL, 0))) {
        for (y = 0; y < 2; ++y) memcpy((BYTE *)r.pBits + y * r.Pitch, texels + 2 * y, 8);
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    if (begin("texture")) {
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetFVF(device, TLTEXFVF);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr("texture", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("texture", s, 5);
        IDirect3DDevice9_SetTexture(device, 0, NULL);
    }
    IDirect3DTexture9_Release(t);
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
    IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (!begin("transform")) return;
    IDirect3DDevice9_SetFVF(device, XYZFVF);
    hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
    if (FAILED(hr)) { fail_hr("transform", "draw", hr); IDirect3DDevice9_EndScene(device); return; }
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
enum { OMov = 1, OAdd = 2, OSub = 3, OMad = 4, OMul = 5, ODp3 = 8, ODp4 = 9, OLrp = 18, OTexcoord = 64,
       OTexkill = 65, OTex = 66, OTexbem = 67, OCnd = 80, ODef = 81, OCmp = 88 };
#define CO 0x40000000UL
/* IEEE single bit patterns for def. */
enum { F0 = 0, F01 = 0x3dcccccd, F02 = 0x3e4ccccd, F025 = 0x3e800000, F05 = 0x3f000000, F06 = 0x3f19999a,
       F075 = 0x3f400000, F1 = 0x3f800000 };
#define DEF(c, a, b, d, e) ODef, PD(RC, c, 15, 0, 0), (DWORD)(a), (DWORD)(b), (DWORD)(d), (DWORD)(e)
#define DST(r, n) PD(r, n, 15, 0, 0)
#define SRC(r, n) PS_(r, n, SwXYZW, MNone)

/* Shader model 2 tokens: the instruction length is in bits 24-27, and a
 * register type above 7 continues in bits 11-12. */
#define P2I(op, len) ((DWORD)(op) | ((DWORD)(len) << 24))
#define P2REG(type, num) (0x80000000UL | (((DWORD)(type) & 7) << 28) | ((((DWORD)(type) >> 3) & 3) << 11) | (num))
#define P2D(type, num, mask) (P2REG(type, num) | ((DWORD)(mask) << 16))
#define P2DS(type, num, mask) (P2D(type, num, mask) | (1UL << 20))
#define P2S(type, num, swz, mod) (P2REG(type, num) | ((DWORD)(swz) << 16) | ((DWORD)(mod) << 24))
#define P2SRC(type, num) P2S(type, num, SwXYZW, MNone)
#define P2DCL_T(n, mask) P2I(31, 2), 0x80000000UL, P2D(RT, n, mask)
#define P2DCL_V(n) P2I(31, 2), 0x80000000UL, P2D(RV, n, 15)
#define P2DCL_S(n) P2I(31, 2), (0x80000000UL | (2UL << 27)), P2D(10, n, 15)
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

static IDirect3DTexture9 *ps_textures[2];

static IDirect3DTexture9 *texture2x2(const DWORD *texels)
{
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT r;
    int y;
    if (FAILED(IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) return NULL;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &r, NULL, 0))) {
        for (y = 0; y < 2; ++y) memcpy((BYTE *)r.pBits + y * r.Pitch, texels + 2 * y, 8);
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    return t;
}

/* Draw quads with a pixel shader and compare the samples. */
static void ps_run(const char *name, const DWORD *tokens, const PSVERTEX *v, UINT quads, const SAMPLE *s, int samples)
{
    IDirect3DPixelShader9 *handle = NULL;
    HRESULT hr = IDirect3DDevice9_CreatePixelShader(device, tokens, &handle);
    if (FAILED(hr)) { fail_hr(name, "create", hr); return; }
    if (begin(name)) {
        IDirect3DDevice9_SetPixelShader(device, handle);
        IDirect3DDevice9_SetFVF(device, PSFVF);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2 * quads, v, sizeof(v[0]));
        IDirect3DDevice9_SetPixelShader(device, NULL);
        if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check(name, s, samples);
    }
    IDirect3DPixelShader9_Release(handle);
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
static void pixel_shaders(int only, BOOL version2)
{
    PSVERTEX v[12];
    SAMPLE s[5];
    float want[4][3], t[3], d;
    int i, k;
    ps_textures[0] = texture2x2(texels0);
    ps_textures[1] = texture2x2(texels1);
    for (k = 0; k < 2; ++k) {
        IDirect3DDevice9_SetTexture(device, k, (IDirect3DBaseTexture9 *)ps_textures[k]);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
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
        IDirect3DDevice9_SetPixelShaderConstantF(device, 1, c1, 1);
        ps_quad(v, 32, 32, 96, 96, 0xff663399, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) {
            want[i][0] = 0.25f + unit(0x66) * 0.5f; want[i][1] = 0.5f + unit(0x33) * 0.5f;
            want[i][2] = 0.75f + unit(0x99) * 0.5f;
        }
        quad_samples(s, want);
        ps_run("ps_constants", ps, v, 1, s, 5);
    }
    /* Direct3D 9 refuses this Direct3D 8 shader when it is created; d8pixels
     * covers it, and ps=4 still runs it here. */
    if (only == 4) {   /* tex t0; mul_x2 r0, t0_bias, 1-v0; sub_sat_d2 r0, r0, -c0: the negative
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
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        ps_run("ps_coissue", ps, v, 1, s, 5);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
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
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGCOLOR, 0x0000ff00);
        ps_run("ps_fog_alpha", ps, v, 1, s, 5);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
    }
    if (!only || only == 18) {   /* the same with linear table fog over z 0..1, the quad at z 0.5 */
        static const DWORD ps[] = { PSV(1), OMov, DST(RR, 0), SRC(RV, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0xff4080c0, 0, 0, 1, 1);
        for (i = 0; i < 4; ++i) {
            want[i][0] = 0.5f * unit(0x40); want[i][1] = 0.5f * unit(0x80) + 0.5f; want[i][2] = 0.5f * unit(0xc0);
        }
        quad_samples(s, want);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGCOLOR, 0x0000ff00);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGSTART, F0);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGEND, F1);
        ps_run("ps_fog_table", ps, v, 1, s, 5);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
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
        D3DMATERIAL9 m;
        D3DLIGHT9 l;
        D3DMATRIX identity;
        IDirect3DPixelShader9 *handle = NULL;
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
            IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &identity);
            IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &identity);
            IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &identity);
            IDirect3DDevice9_SetMaterial(device, &m);
            IDirect3DDevice9_SetLight(device, 0, &l);
            IDirect3DDevice9_LightEnable(device, 0, TRUE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, TRUE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_AMBIENT, 0);
            IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
            if (fogged) {
                IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
                IDirect3DDevice9_SetRenderState(device, D3DRS_FOGCOLOR, 0x0000ff00);
                IDirect3DDevice9_SetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
                IDirect3DDevice9_SetRenderState(device, D3DRS_FOGSTART, F0);
                IDirect3DDevice9_SetRenderState(device, D3DRS_FOGEND, F1);
            }
            hr = IDirect3DDevice9_CreatePixelShader(device, ps, &handle);
            if (FAILED(hr)) fail_hr(name, "create", hr);
            else if (begin(name)) {
                IDirect3DDevice9_SetPixelShader(device, handle);
                IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_NORMAL);
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, q, sizeof(q[0]));
                IDirect3DDevice9_SetPixelShader(device, NULL);
                if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check(name, s, 5);
            }
            if (handle) IDirect3DPixelShader9_Release(handle);
            handle = NULL;
            IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
        }
    }
    if (!version2 && (!only || only >= 21)) printf("D9PIX pixel shader 2.0 not offered\n");
    else if (!only || only == 21) {   /* ps_2_0: texld, then multiply by the diffuse colour */
        static const DWORD ps[] = { 0xffff0200, P2DCL_T(0, 3), P2DCL_V(0), P2DCL_S(0),
            P2I(OTex, 3), P2D(RR, 0, 15), P2SRC(RT, 0), P2SRC(10, 0),
            P2I(OMul, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RV, 0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RR, 0), PSEND };
        ps_quad(v, 32, 32, 96, 96, 0x80808080, 0, 0, 1, 1);
        d = unit(0x80);
        for (i = 0; i < 4; ++i) { texel(i, t); for (k = 0; k < 3; ++k) want[i][k] = t[k] * d; }
        quad_samples(s, want);
        ps_run("ps2_texture", ps, v, 1, s, 5);
    }
    if (version2 && (!only || only == 22)) {   /* ps_2_0 arithmetic: pow, rcp, rsq, nrm, dp2add, unclamped values */
        static const DWORD ps[] = { 0xffff0200,
            P2I(ODef, 5), P2D(RC, 0, 15), F025, 0x40000000, F05, F1,
            P2I(ODef, 5), P2D(RC, 1, 15), 0x40400000, 0x40800000, F0, F0,
            P2I(OMov, 2), P2D(RR, 0, 8), P2S(RC, 0, 0xff, 0),                      /* r0.w = 1 */
            P2I(32, 3), P2D(RR, 0, 1), P2S(RC, 0, 0x00, 0), P2S(RC, 0, 0xaa, 0),     /* pow(0.25, 0.5) = 0.5 */
            P2I(6, 2), P2D(RR, 0, 2), P2S(RC, 0, 0x55, 0),                          /* 1/2 */
            P2I(7, 2), P2D(RR, 0, 4), P2S(RC, 0, 0x00, 0),                          /* 1/sqrt(0.25) = 2 */
            P2I(36, 2), P2D(RR, 1, 7), P2SRC(RC, 1),                                /* (0.6, 0.8, 0) */
            P2I(90, 4), P2D(RR, 2, 1), P2SRC(RR, 1), P2SRC(RR, 1), P2S(RC, 0, 0x00, 0),   /* 1 + 0.25 */
            P2I(OMul, 3), P2D(RR, 0, 4), P2SRC(RR, 0), P2S(RR, 2, 0x00, 0),        /* 2 * 1.25 = 2.5 */
            P2I(OMad, 4), P2D(RR, 0, 4), P2SRC(RR, 0), P2S(RC, 0, 0xaa, 0), P2S(RC, 0, 0xaa, 1),  /* 2.5/2 - 0.5 */
            P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RR, 0), PSEND };
        static const SAMPLE cs[] = { { 64, 64, 0x808000 }, { 8, 8, CLEAR_COLOR } };
        SAMPLE ws[2];
        memcpy(ws, cs, sizeof(ws));
        ws[0].rgb = rgb(0.5f, 0.5f, 0.75f);
        ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
        ps_run("ps2_arithmetic", ps, v, 1, ws, 2);
    }
    if (version2 && (!only || only == 23)) {   /* ps_2_0: texkill on the coordinates, colour from v0 */
        static const DWORD ps[] = { 0xffff0200, P2DCL_T(0, 15), P2DCL_V(0),
            P2I(OTexkill, 1), P2D(RT, 0, 15), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RV, 0), PSEND };
        static const SAMPLE cs[] = { { 40, 64, CLEAR_COLOR }, { 88, 64, 0x20e020 } };
        ps_quad(v, 32, 32, 96, 96, 0xff20e020, -1.0f, 0.5f, 1.0f, 0.5f);
        ps_run("ps2_texkill", ps, v, 1, cs, 2);
    }
    {
        /* Single shader model 2 instructions, each writing all of oC0 from
         * constants defined in the shader; the expected colour follows. */
        static const DWORD c0[] = { P2I(ODef, 5), P2D(RC, 0, 15), F025, 0x40000000, F05, F1 };
        static const DWORD c1[] = { P2I(ODef, 5), P2D(RC, 1, 15), 0x40400000, 0x40800000, F0, F0 };
        static const struct { int id; const char *name; DWORD body[16]; int words; float rgb[3]; } ops[] = {
            { 24, "ps2_pow", { P2I(32, 3), P2D(RR, 0, 15), P2S(RC, 0, 0x00, 0), P2S(RC, 0, 0xaa, 0) }, 4, { 0.5f, 0.5f, 0.5f } },
            { 25, "ps2_rcp", { P2I(6, 2), P2D(RR, 0, 15), P2S(RC, 0, 0x55, 0) }, 3, { 0.5f, 0.5f, 0.5f } },
            { 26, "ps2_rsq", { P2I(7, 2), P2D(RR, 0, 15), P2S(RC, 0, 0xaa, 0) }, 3, { 1.0f, 1.0f, 1.0f } },
            { 27, "ps2_nrm", { P2I(36, 2), P2D(RR, 0, 7), P2SRC(RC, 1), P2I(OMov, 2), P2D(RR, 0, 8), P2S(RC, 0, 0xff, 0) }, 6,
              { 0.6f, 0.8f, 0.0f } },
            { 28, "ps2_dp2add", { P2I(OMov, 2), P2D(RR, 1, 15), P2SRC(RC, 0), P2I(90, 4), P2D(RR, 0, 15), P2SRC(RR, 1), P2SRC(RR, 1),
              P2S(RC, 0, 0x00, 0) }, 8, { 0.25f + 0.0625f + 4.0f > 1 ? 1.0f : 0, 1.0f, 1.0f } }
        };
        int o;
        for (o = 0; o < (int)(sizeof(ops) / sizeof(ops[0])); ++o) {
            DWORD ps[40];
            int nps = 0, w;
            SAMPLE cs[2];
            if (!version2 || (only && only != ops[o].id)) continue;
            ps[nps++] = 0xffff0200;
            for (w = 0; w < 6; ++w) ps[nps++] = c0[w];
            for (w = 0; w < 6; ++w) ps[nps++] = c1[w];
            for (w = 0; w < ops[o].words; ++w) ps[nps++] = ops[o].body[w];
            /* The render target clamps; _sat on oC0 is refused by the runtime. */
            ps[nps++] = P2I(OMov, 2); ps[nps++] = P2D(8, 0, 15); ps[nps++] = P2SRC(RR, 0);
            ps[nps++] = PSEND;
            cs[0].x = 64; cs[0].y = 64; cs[0].rgb = rgb(ops[o].rgb[0], ops[o].rgb[1], ops[o].rgb[2]);
            cs[1].x = 8; cs[1].y = 8; cs[1].rgb = CLEAR_COLOR;
            ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
            ps_run(ops[o].name, ps, v, 1, cs, 2);
        }
    }
    if (version2 && (!only || only == 29)) {
        /* ps_2_0 with sets t5..t7 and samplers s6 and s7: texld r0, t7, s7;
         * texld r1, t5, s6; mul r0, r0, r1; add r0.z, r0, t6.x. Sets 0 to 4
         * run the other way, so taking a wrong set shows. */
        static const DWORD ps[] = { 0xffff0200, P2DCL_T(5, 3), P2DCL_T(6, 3), P2DCL_T(7, 3), P2DCL_S(6), P2DCL_S(7),
            P2I(OTex, 3), P2D(RR, 0, 15), P2SRC(RT, 7), P2SRC(10, 7),
            P2I(OTex, 3), P2D(RR, 1, 15), P2SRC(RT, 5), P2SRC(10, 6),
            P2I(OMul, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RR, 1),
            P2I(OAdd, 3), P2D(RR, 0, 4), P2SRC(RR, 0), P2S(RT, 6, 0x00, 0),
            P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RR, 0), PSEND };
        typedef struct { float x, y, z, rhw; DWORD diffuse; float tc[8][2]; } WIDEVERTEX;
        const float xs[6] = { 32, 96, 32, 32, 96, 96 }, ys[6] = { 32, 32, 96, 96, 32, 96 };
        WIDEVERTEX w[6];
        IDirect3DPixelShader9 *handle = NULL;
        HRESULT hr;
        int j;
        for (i = 0; i < 6; ++i) {
            float u = xs[i] == 32 ? 0.0f : 1.0f, vv = ys[i] == 32 ? 0.0f : 1.0f;
            w[i].x = xs[i]; w[i].y = ys[i]; w[i].z = 0.5f; w[i].rhw = 1.0f; w[i].diffuse = 0xffffffff;
            for (j = 0; j < 8; ++j) { w[i].tc[j][0] = 1.0f - u; w[i].tc[j][1] = 1.0f - vv; }
            w[i].tc[5][0] = u; w[i].tc[5][1] = vv;
            w[i].tc[6][0] = 0.25f; w[i].tc[6][1] = 0.0f;
            w[i].tc[7][0] = u; w[i].tc[7][1] = vv;
        }
        for (i = 0; i < 4; ++i) {
            float y[3] = { 1.0f, 1.0f, 0.0f };
            texel(i, t);
            for (k = 0; k < 3; ++k) want[i][k] = t[k] * y[k] + (k == 2 ? 0.25f : 0.0f);
        }
        quad_samples(s, want);
        IDirect3DDevice9_SetTexture(device, 6, (IDirect3DBaseTexture9 *)ps_textures[1]);
        IDirect3DDevice9_SetTexture(device, 7, (IDirect3DBaseTexture9 *)ps_textures[0]);
        for (k = 6; k < 8; ++k) {
            IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        hr = IDirect3DDevice9_CreatePixelShader(device, ps, &handle);
        if (FAILED(hr)) fail_hr("ps2_eight_sets", "create", hr);
        else {
            if (begin("ps2_eight_sets")) {
                IDirect3DDevice9_SetPixelShader(device, handle);
                IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX8);
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, w, sizeof(w[0]));
                IDirect3DDevice9_SetPixelShader(device, NULL);
                if (FAILED(hr)) { fail_hr("ps2_eight_sets", "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check("ps2_eight_sets", s, 5);
            }
            IDirect3DPixelShader9_Release(handle);
        }
        for (k = 6; k < 8; ++k) IDirect3DDevice9_SetTexture(device, k, NULL);
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
        IDirect3DDevice9_SetTexture(device, k, NULL);
        if (ps_textures[k]) IDirect3DTexture9_Release(ps_textures[k]);
    }
}

/* ---- vertex shaders ---- */

/* Register types and declaration usages of vertex shaders. */
enum { VPos = 4, VAttr = 5, VTex = 6, VAddr = 3 };
#define VDCL2(usage, reg) P2I(31, 2), (0x80000000UL | (usage)), P2D(RV, reg, 15)
#define VDCL1(usage, reg) 31, (0x80000000UL | (usage)), P2D(RV, reg, 15)
typedef struct { float x, y, z; DWORD color; } VSVERTEX;

/* A quad in clip space (y up), as two triangles of a list, times w. */
static void vs_quad(VSVERTEX *v, float x0, float y0, float x1, float y1, DWORD color)
{
    const float xs[6] = { x0, x1, x0, x0, x1, x1 }, ys[6] = { y0, y0, y1, y1, y0, y1 };
    int i;
    for (i = 0; i < 6; ++i) { v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 0.5f; v[i].color = color; }
}

static void vs_run(const char *name, const D3DVERTEXELEMENT9 *elements, const DWORD *vs, const DWORD *ps,
                   const void *vertices, UINT stride, UINT triangles, const SAMPLE *s, int samples)
{
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexShader9 *vsh = NULL;
    IDirect3DPixelShader9 *psh = NULL;
    HRESULT hr = IDirect3DDevice9_CreateVertexDeclaration(device, elements, &decl);
    if (FAILED(hr)) { fail_hr(name, "declaration", hr); return; }
    hr = IDirect3DDevice9_CreateVertexShader(device, vs, &vsh);
    if (FAILED(hr)) fail_hr(name, "vertex shader", hr);
    else {
        hr = ps ? IDirect3DDevice9_CreatePixelShader(device, ps, &psh) : D3D_OK;
        if (FAILED(hr)) fail_hr(name, "pixel shader", hr);
        else if (begin(name)) {
            IDirect3DDevice9_SetVertexDeclaration(device, decl);
            IDirect3DDevice9_SetVertexShader(device, vsh);
            IDirect3DDevice9_SetPixelShader(device, psh);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, triangles, vertices, stride);
            IDirect3DDevice9_SetVertexShader(device, NULL);
            IDirect3DDevice9_SetPixelShader(device, NULL);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check(name, s, samples);
        }
    }
    if (psh) IDirect3DPixelShader9_Release(psh);
    if (vsh) IDirect3DVertexShader9_Release(vsh);
    IDirect3DVertexDeclaration9_Release(decl);
}

/* only: run that case alone (30 and up), or all when zero. */
static void vertex_shaders(int only)
{
    static const D3DVERTEXELEMENT9 colored[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    /* ps_2_0: mov oC0, v0 */
    static const DWORD ps_color[] = { 0xffff0200, P2DCL_V(0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RV, 0), PSEND };
    VSVERTEX v[24];
    if (!only || only == 30) {
        /* vs_2_0: oPos = v0 times c0..c3, which doubles x, y, z and w; oD0 = v1. */
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(ODp4, 3), P2D(VPos, 0, 1), P2SRC(RV, 0), P2SRC(RC, 0),
            P2I(ODp4, 3), P2D(VPos, 0, 2), P2SRC(RV, 0), P2SRC(RC, 1),
            P2I(ODp4, 3), P2D(VPos, 0, 4), P2SRC(RV, 0), P2SRC(RC, 2),
            P2I(ODp4, 3), P2D(VPos, 0, 8), P2SRC(RV, 0), P2SRC(RC, 3),
            P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RV, 1), PSEND };
        static const float m[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2 };
        static const SAMPLE s[] = { { 48, 48, 0xff0000 }, { 80, 48, 0x00ff00 }, { 48, 80, 0x0000ff },
                                    { 80, 80, 0xffffff }, { 8, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR } };
        vs_quad(v, -0.5f, 0.5f, 0, 0, 0xffff0000);
        vs_quad(v + 6, 0, 0.5f, 0.5f, 0, 0xff00ff00);
        vs_quad(v + 12, -0.5f, 0, 0, -0.5f, 0xff0000ff);
        vs_quad(v + 18, 0, 0, 0.5f, -0.5f, 0xffffffff);
        IDirect3DDevice9_SetVertexShaderConstantF(device, 0, m, 4);
        vs_run("vs2_transform", colored, vs, ps_color, v, sizeof(v[0]), 8, s, 6);
    }
    if (!only || only == 31) {
        /* The same shader through a viewport on the top right quarter. */
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RV, 1), PSEND };
        static const SAMPLE s[] = { { 96, 32, 0xffff00 }, { 32, 32, CLEAR_COLOR }, { 96, 96, CLEAR_COLOR },
                                    { 32, 96, CLEAR_COLOR } };
        D3DVIEWPORT9 vp = { 64, 0, 64, 64, 0.0f, 1.0f }, full = { 0, 0, Side, Side, 0.0f, 1.0f };
        vs_quad(v, -1, 1, 1, -1, 0xffffff00);
        /* A clear covers only the viewport, so clear the whole target first. */
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, CLEAR_COLOR, 1.0f, 0);
        IDirect3DDevice9_SetViewport(device, &vp);
        vs_run("vs2_viewport", colored, vs, ps_color, v, sizeof(v[0]), 2, s, 4);
        IDirect3DDevice9_SetViewport(device, &full);
    }
    if (!only || only == 33) {
        /* Two streams: positions in stream 0, colours in stream 1, both vertex
         * buffers with six unused vertices first, drawn from vertex 6: a red
         * quad on the left and a green one on the right. */
        static const D3DVERTEXELEMENT9 split[] = {
            { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
            { 1, 0, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RV, 1), PSEND };
        static const SAMPLE s[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR },
                                    { 120, 120, CLEAR_COLOR } };
        float pos[18][3];
        DWORD col[18];
        IDirect3DVertexDeclaration9 *decl = NULL;
        IDirect3DVertexShader9 *vsh = NULL;
        IDirect3DPixelShader9 *psh = NULL;
        IDirect3DVertexBuffer9 *pvb = NULL, *cvb = NULL;
        D3DCAPS9 dc;
        HRESULT hr;
        int k;
        IDirect3DDevice9_GetDeviceCaps(device, &dc);
        if (dc.MaxStreams < 2) printf("D9PIX streams not offered (%lu)\n", (unsigned long)dc.MaxStreams);
        else {
            vs_quad(v, 1, 1, 1, 1, 0xff0000ff);                 /* unused */
            vs_quad(v + 6, -0.5f, 0.5f, 0, -0.5f, 0xffff0000);
            vs_quad(v + 12, 0, 0.5f, 0.5f, -0.5f, 0xff00ff00);
            for (k = 0; k < 18; ++k) { pos[k][0] = v[k].x; pos[k][1] = v[k].y; pos[k][2] = v[k].z; col[k] = v[k].color; }
            pvb = vertex_buffer(pos, sizeof(pos), 0);
            cvb = vertex_buffer(col, sizeof(col), 0);
            hr = pvb && cvb ? IDirect3DDevice9_CreateVertexDeclaration(device, split, &decl) : E_FAIL;
            if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateVertexShader(device, vs, &vsh);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreatePixelShader(device, ps_color, &psh);
            if (FAILED(hr)) fail_hr("vs2_streams", "create", hr);
            else if (begin("vs2_streams")) {
                IDirect3DDevice9_SetVertexDeclaration(device, decl);
                IDirect3DDevice9_SetVertexShader(device, vsh);
                IDirect3DDevice9_SetPixelShader(device, psh);
                IDirect3DDevice9_SetStreamSource(device, 0, pvb, 0, sizeof(pos[0]));
                IDirect3DDevice9_SetStreamSource(device, 1, cvb, 0, sizeof(col[0]));
                hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 6, 4);
                IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
                IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0);
                IDirect3DDevice9_SetVertexShader(device, NULL);
                IDirect3DDevice9_SetPixelShader(device, NULL);
                if (FAILED(hr)) { fail_hr("vs2_streams", "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check("vs2_streams", s, 4);
            }
            if (psh) IDirect3DPixelShader9_Release(psh);
            if (vsh) IDirect3DVertexShader9_Release(vsh);
            if (decl) IDirect3DVertexDeclaration9_Release(decl);
            if (pvb) IDirect3DVertexBuffer9_Release(pvb);
            if (cvb) IDirect3DVertexBuffer9_Release(cvb);
        }
    }
    if (!only || only == 32) {
        /* vs_1_1: oPos = v0 (two floats); a0.x = the first byte of v1;
         * oD0 = c[a0.x]; oT0 = v2 (two shorts) times c10. ps_2_0 adds them. */
        typedef struct { float x, y; BYTE index[4]; short s[2]; } V1;
        static const D3DVERTEXELEMENT9 decl[] = {
            { 0, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_POSITION, 0 },
            { 0, 8, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0 },
            { 0, 12, D3DDECLTYPE_SHORT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
        static const DWORD vs[] = { 0xfffe0101, VDCL1(0, 0), VDCL1(2, 1), VDCL1(5, 2),
            OMov, P2D(VPos, 0, 15), P2SRC(RV, 0),
            OMov, P2D(VAddr, 0, 1), P2S(RV, 1, 0x00, 0),
            OMov, P2D(VAttr, 0, 15), P2SRC(RC, 0) | 0x2000,
            OMul, P2D(VTex, 0, 15), P2SRC(RV, 2), P2SRC(RC, 10), PSEND };
        static const DWORD ps[] = { 0xffff0200, P2DCL_V(0), P2DCL_T(0, 15),
            P2I(OMov, 2), P2D(RR, 1, 15), P2SRC(RT, 0),
            P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RV, 0), P2SRC(RR, 1), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RR, 0), PSEND };
        static const float c[44] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.25f, 0.5f, 0, 1 };
        static const float c10[4] = { 1.0f / 256, 1.0f / 256, 0, 0 };
        const float xs[6] = { -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f }, ys[6] = { 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f };
        V1 w[6];
        SAMPLE s[2];
        int i;
        for (i = 0; i < 6; ++i) {
            w[i].x = xs[i]; w[i].y = ys[i];
            w[i].index[0] = 3; w[i].index[1] = w[i].index[2] = w[i].index[3] = 0;
            w[i].s[0] = 64; w[i].s[1] = -32;
        }
        s[0].x = 64; s[0].y = 64; s[0].rgb = rgb(0.5f, 0.375f, 0);
        s[1].x = 8; s[1].y = 8; s[1].rgb = CLEAR_COLOR;
        IDirect3DDevice9_SetVertexShaderConstantF(device, 0, c, 4);
        IDirect3DDevice9_SetVertexShaderConstantF(device, 10, c10, 1);
        vs_run("vs1_inputs", decl, vs, ps, w, sizeof(w[0]), 2, s, 2);
    }
    if (!only || only == 33) {
        /* vs_2_0 writing oFog = 0.25 with fog on: a quarter of the colour. */
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RV, 1),
            P2I(OMov, 2), P2D(VPos, 1, 15), P2S(RC, 0, 0x00, 0), PSEND };
        static const float c0[4] = { 0.25f, 0, 0, 0 };
        SAMPLE s[2];
        s[0].x = 64; s[0].y = 64; s[0].rgb = rgb(0.25f, 0, 0.75f);
        s[1].x = 8; s[1].y = 8; s[1].rgb = CLEAR_COLOR;
        vs_quad(v, -0.5f, 0.5f, 0.5f, -0.5f, 0xffff0000);
        IDirect3DDevice9_SetVertexShaderConstantF(device, 0, c0, 1);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGCOLOR, 0x000000ff);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        vs_run("vs2_fog", colored, vs, ps_color, v, sizeof(v[0]), 2, s, 2);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
    }
    if (!only || only == 34) {
        /* vs_2_0 without a pixel shader: oD0 = c0 (grey), oT0 = v1; stage 0
         * modulates the texture by the diffuse colour, stage 1 adds the
         * texture factor. */
        typedef struct { float x, y, z, u, v; } TV;
        static const D3DVERTEXELEMENT9 decl[] = {
            { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(5, 1),
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RC, 0),
            P2I(OMov, 2), P2D(VTex, 0, 15), P2SRC(RV, 1), PSEND };
        static const float grey[4] = { 128.0f / 255, 128.0f / 255, 128.0f / 255, 1 };
        const float xs[6] = { -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f }, ys[6] = { 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f };
        IDirect3DTexture9 *t = texture2x2(texels0);
        float want[4][3], c[3];
        SAMPLE s[5];
        TV w[6];
        int i, k;
        for (i = 0; i < 6; ++i) {
            w[i].x = xs[i]; w[i].y = ys[i]; w[i].z = 0.5f;
            w[i].u = xs[i] < 0 ? 0.0f : 1.0f; w[i].v = ys[i] > 0 ? 0.0f : 1.0f;
        }
        for (i = 0; i < 4; ++i) { texel(i, c); for (k = 0; k < 3; ++k) want[i][k] = c[k] * unit(0x80) + unit(0x20); }
        quad_samples(s, want);
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_ADD);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLORARG1, D3DTA_CURRENT);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
        IDirect3DDevice9_SetRenderState(device, D3DRS_TEXTUREFACTOR, 0xff202020);
        IDirect3DDevice9_SetVertexShaderConstantF(device, 0, grey, 1);
        vs_run("vs2_stages", decl, vs, NULL, w, sizeof(w[0]), 2, s, 5);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        if (t) IDirect3DTexture9_Release(t);
    }
    if (!only || only == 35 || only == 36) {
        /* Alpha blending after shaders: an opaque red quad, then a quad of
         * (0, 0, 1, 0.25) over it with SRCALPHA, INVSRCALPHA. 35 takes the
         * colour from a vertex shader constant, 36 from a pixel shader alone
         * on transformed vertices. */
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RC, 0), PSEND };
        static const float red[4] = { 1, 0, 0, 1 }, blue[4] = { 0, 0, 1, 0.25f };
        SAMPLE s[2];
        int pass;
        s[0].x = 64; s[0].y = 64; s[0].rgb = rgb(0.75f, 0, 0.25f);
        s[1].x = 8; s[1].y = 8; s[1].rgb = CLEAR_COLOR;
        for (pass = 35; pass <= 36; ++pass) {
            IDirect3DVertexDeclaration9 *decl = NULL;
            IDirect3DVertexShader9 *vsh = NULL;
            IDirect3DPixelShader9 *psh = NULL;
            const char *name = pass == 35 ? "vs2_blend" : "ps2_blend";
            HRESULT hr;
            if (only && only != pass) continue;
            IDirect3DDevice9_CreateVertexDeclaration(device, colored, &decl);
            IDirect3DDevice9_CreateVertexShader(device, vs, &vsh);
            hr = IDirect3DDevice9_CreatePixelShader(device, ps_color, &psh);
            if (FAILED(hr) || !decl || !vsh) { fail_hr(name, "create", hr); }
            else if (begin(name)) {
                int q;
                for (q = 0; q < 2; ++q) {
                    IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, q ? TRUE : FALSE);
                    IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                    IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                    IDirect3DDevice9_SetPixelShader(device, psh);
                    if (pass == 35) {
                        vs_quad(v, -0.5f, 0.5f, 0.5f, -0.5f, 0);
                        IDirect3DDevice9_SetVertexShaderConstantF(device, 0, q ? blue : red, 1);
                        IDirect3DDevice9_SetVertexDeclaration(device, decl);
                        IDirect3DDevice9_SetVertexShader(device, vsh);
                        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
                        IDirect3DDevice9_SetVertexShader(device, NULL);
                    } else {
                        TLVERTEX t[6];
                        quad(t, 32, 32, 96, 96, q ? 0x400000ff : 0xffff0000);
                        IDirect3DDevice9_SetFVF(device, TLFVF);
                        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, t, sizeof(t[0]));
                    }
                    IDirect3DDevice9_SetPixelShader(device, NULL);
                    if (FAILED(hr)) break;
                }
                IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check(name, s, 2);
            }
            if (psh) IDirect3DPixelShader9_Release(psh);
            if (vsh) IDirect3DVertexShader9_Release(vsh);
            if (decl) IDirect3DVertexDeclaration9_Release(decl);
        }
    }
}

/* ---- vertex shader static flow control ----
 * Register types beyond those above: integer and boolean constants, aL and labels. */
enum { RInt = 7, RBool = 14, RLoop = 15, RLabel = 18 };
enum { OCall = 25, OCallnz = 26, OLoop = 27, ORet = 28, OEndLoop = 29, OLabel = 30, ORep = 38, OEndRep = 39,
       OIf = 40, OElse = 42, OEndif = 43, ODefb = 47, ODefi = 48 };
/* 61: rep i1 (count 4 from SetVertexShaderConstantI) adds a quarter of red four times; loop aL, i0
 * (defi: 3 from 0 by 1) adds c[aL + 6] for green (a quarter, a quarter, a half). 62: if b0 (true)
 * adds red; if b2 (defb false) would add blue, its else adds half of green; callnz l0, b1 (false)
 * would add blue; call l1 adds the other half of green. Both are yellow when right. They need the
 * Direct3D 9 driver interface, which alone passes integer and boolean constants (its sign is
 * D3DDEVCAPS2_STREAMOFFSET). */
static void vs_flow_cases(int only, BOOL offered)
{
    static const D3DVERTEXELEMENT9 colored[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    static const DWORD ps_color[] = { 0xffff0200, P2DCL_V(0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RV, 0), PSEND };
    static const SAMPLE s[] = { { 64, 64, 0xffff00 }, { 8, 8, CLEAR_COLOR } };
    static const float c[8][4] = { { 0, 0, 0, 1 }, { 0.25f, 0, 0, 0 }, { 0, 0.25f, 0, 0 }, { 0, 0.25f, 0, 0 },
                                   { 0, 0.5f, 0, 0 }, { 1, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0.5f, 0, 0 } };
    static const int ints[2][4] = { { 0, 0, 0, 0 }, { 4, 0, 1, 0 } };
    static const BOOL bools[2] = { TRUE, FALSE };
    VSVERTEX v[6];
    if (only && only != 61 && only != 62) return;
    if (!offered) { printf("D9PIX vertex shader flow control not offered\n"); return; }
    vs_quad(v, -0.5f, 0.5f, 0.5f, -0.5f, 0xff000000);
    /* c4..c11 as above; i0, i1; b0, b1. */
    IDirect3DDevice9_SetVertexShaderConstantF(device, 4, &c[0][0], 8);
    IDirect3DDevice9_SetVertexShaderConstantI(device, 0, &ints[0][0], 2);
    IDirect3DDevice9_SetVertexShaderConstantB(device, 0, bools, 2);
    if (!only || only == 61) {
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(ODefi, 5), P2D(RInt, 0, 15), 3, 0, 1, 0,
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0),
            P2I(OMov, 2), P2D(RR, 0, 15), P2SRC(RC, 4),
            P2I(ORep, 1), P2SRC(RInt, 1),
                P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RC, 5),
            P2I(OEndRep, 0),
            P2I(OLoop, 2), P2SRC(RLoop, 0), P2SRC(RInt, 0),
                P2I(OAdd, 4), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RC, 6) | 0x2000, P2SRC(RLoop, 0),
            P2I(OEndLoop, 0),
            P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RR, 0), PSEND };
        vs_run("vs2_loops", colored, vs, ps_color, v, sizeof(v[0]), 2, s, 2);
    }
    if (!only || only == 62) {
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(ODefb, 2), P2D(RBool, 2, 15), 0,
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0),
            P2I(OMov, 2), P2D(RR, 0, 15), P2SRC(RC, 4),
            P2I(OIf, 1), P2SRC(RBool, 0),
                P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RC, 9),
            P2I(OEndif, 0),
            P2I(OIf, 1), P2SRC(RBool, 2),
                P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RC, 10),
            P2I(OElse, 0),
                P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RC, 11),
            P2I(OEndif, 0),
            P2I(OCallnz, 2), P2SRC(RLabel, 0), P2SRC(RBool, 1),
            P2I(OCall, 1), P2SRC(RLabel, 1),
            P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RR, 0),
            P2I(ORet, 0),
            P2I(OLabel, 1), P2SRC(RLabel, 0),
                P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RC, 10),
            P2I(ORet, 0),
            P2I(OLabel, 1), P2SRC(RLabel, 1),
                P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RC, 11),
            P2I(ORet, 0), PSEND };
        vs_run("vs2_branches", colored, vs, ps_color, v, sizeof(v[0]), 2, s, 2);
    }
}

/* ---- user clip planes ----
 * 64: a red quad over x -0.5..0.5 of untransformed vertices (identity matrices) with the plane
 * (1, 0, 0, 0) in world space: only its right half, x >= 0, is drawn. 65: the same through a
 * vertex shader, the plane in clip space.
 * 66: 64 lit (emissive red, no lights). 67 and 68: 64 and 66 with a pixel shader (mov oC0, v0),
 * whose fixed-function vertices take the object-space shaders of pixel shader draws. */
static void clip_cases(int only, BOOL offered, BOOL shaders)
{
    static const D3DVERTEXELEMENT9 colored[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    static const DWORD ps_color[] = { 0xffff0200, P2DCL_V(0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RV, 0), PSEND };
    static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
        P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RV, 1), PSEND };
    static const SAMPLE s[] = { { 48, 64, CLEAR_COLOR }, { 80, 64, 0xff0000 }, { 8, 8, CLEAR_COLOR } };
    static const float plane[4] = { 1, 0, 0, 0 };
    VSVERTEX v[6];
    HRESULT hr;
    if (only && (only < 64 || only > 68)) return;
    if (!offered) { printf("D9PIX user clip planes not offered\n"); return; }
    vs_quad(v, -0.5f, 0.5f, 0.5f, -0.5f, 0xffff0000);
    IDirect3DDevice9_SetClipPlane(device, 0, plane);
    if (!only || only == 64) {
        if (begin("clip_ff")) {
            IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 1);
            IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice9_SetTexture(device, 0, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
            IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0);
            if (FAILED(hr)) { fail_hr("clip_ff", "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check("clip_ff", s, 3);
        }
    }
    {
        static const DWORD ps_diffuse[] = { 0xffff0200, P2DCL_V(0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RV, 0), PSEND };
        IDirect3DPixelShader9 *psh = NULL;
        D3DMATERIAL9 m;
        int pass;
        memset(&m, 0, sizeof(m));
        m.Emissive.r = 1; m.Emissive.a = 1;
        if (shaders) IDirect3DDevice9_CreatePixelShader(device, ps_diffuse, &psh);
        for (pass = 66; pass <= 68; ++pass) {
            const char *name = pass == 66 ? "clip_lit" : pass == 67 ? "clip_ps" : "clip_ps_lit";
            if ((only && only != pass) || (pass > 66 && !psh)) continue;
            if (!begin(name)) continue;
            IDirect3DDevice9_SetMaterial(device, &m);
            IDirect3DDevice9_SetRenderState(device, D3DRS_AMBIENT, 0);
            IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, pass != 67);
            IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 1);
            IDirect3DDevice9_SetTexture(device, 0, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            if (pass > 66) IDirect3DDevice9_SetPixelShader(device, psh);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
            IDirect3DDevice9_SetPixelShader(device, NULL);
            IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0);
            IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX, TRUE);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check(name, s, 3);
        }
        if (psh) IDirect3DPixelShader9_Release(psh);
    }
    if ((!only || only == 65) && shaders) {
        IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 1);
        vs_run("clip_vs", colored, vs, ps_color, v, sizeof(v[0]), 2, s, 3);
        IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0);
    }
}

/* ---- compressed textures ---- */

/* A managed texture of the given format from raw blocks. */
static IDirect3DTexture9 *block_texture(D3DFORMAT format, UINT side, const void *blocks, UINT bytes)
{
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT r;
    UINT rowBytes = bytes / ((side + 3) / 4), y;
    if (FAILED(IDirect3DDevice9_CreateTexture(device, side, side, 1, 0, format, D3DPOOL_MANAGED, &t, NULL))) return NULL;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &r, NULL, 0))) {
        for (y = 0; y < (side + 3) / 4; ++y) memcpy((BYTE *)r.pBits + y * r.Pitch, (const BYTE *)blocks + y * rowBytes, rowBytes);
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    return t;
}

static void textured_quad(const char *name, IDirect3DTexture9 *t, DWORD arg, const SAMPLE *s, int samples)
{
    TLTEXVERTEX v[6];
    const float xs[6] = { 32, 96, 32, 32, 96, 96 }, ys[6] = { 32, 32, 96, 96, 32, 96 };
    HRESULT hr;
    int i;
    for (i = 0; i < 6; ++i) {
        v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 0.5f; v[i].rhw = 1; v[i].diffuse = 0xffffffff;
        v[i].u = xs[i] == 32 ? 0.0f : 1.0f; v[i].v = ys[i] == 32 ? 0.0f : 1.0f;
    }
    if (!t) { fail_hr(name, "texture", E_FAIL); return; }
    if (!begin(name)) return;
    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, arg);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetFVF(device, TLTEXFVF);
    hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
    else check(name, s, samples);
    IDirect3DTexture9_Release(t);
}

/* DXT1: four 4x4 blocks of red, green, blue, and the colour a third of the
 * way from red to blue; DXT3 and DXT5: one block whose alpha shows as grey
 * (8/15 of full, and the second of six steps from 200 to 40). */
static void case_dxt(void)
{
    static const BYTE dxt1[32] = {
        0x00, 0xf8, 0x00, 0xf8, 0, 0, 0, 0,  0xe0, 0x07, 0xe0, 0x07, 0, 0, 0, 0,
        0x1f, 0x00, 0x1f, 0x00, 0, 0, 0, 0,  0x00, 0xf8, 0x1f, 0x00, 0xaa, 0xaa, 0xaa, 0xaa };
    static const BYTE dxt3[16] = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0 };
    static const BYTE dxt5[16] = { 200, 40, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0 };
    static const SAMPLE s1[] = { { 48, 48, 0xff0000 }, { 80, 48, 0x00ff00 }, { 48, 80, 0x0000ff }, { 80, 80, 0xaa0055 },
                                 { 8, 8, CLEAR_COLOR } };
    static const SAMPLE s3[] = { { 64, 64, 0x888888 }, { 8, 8, CLEAR_COLOR } };
    static const SAMPLE s5[] = { { 64, 64, 0xb1b1b1 }, { 8, 8, CLEAR_COLOR } };
    if (FAILED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1))) {
        printf("D9PIX DXT textures not offered\n");
        return;
    }
    textured_quad("dxt1", block_texture(D3DFMT_DXT1, 8, dxt1, sizeof(dxt1)), D3DTA_TEXTURE, s1, 5);
    textured_quad("dxt3_alpha", block_texture(D3DFMT_DXT3, 4, dxt3, sizeof(dxt3)), D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE, s3, 2);
    textured_quad("dxt5_alpha", block_texture(D3DFMT_DXT5, 4, dxt5, sizeof(dxt5)), D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE, s5, 2);
}

/* ---- cube maps ----
 * A 4x4 cube map with a solid colour on each face (+X red, -X green, +Y
 * blue, -Y yellow, +Z cyan, -Z magenta), sampled in the six directions by
 * six small quads: 40 with ps_2_0 (dcl_cube), 41 with ps_1_1 (tex). */
static void cube_cases(int only, BOOL version2)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v, w; } CV;
    static const DWORD colors[6] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xff00ffff, 0xffff00ff };
    static const float dirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const DWORD ps2[] = { 0xffff0200, P2DCL_T(0, 7), P2I(31, 2), (0x80000000UL | (3UL << 27)), P2D(10, 0, 15),
        P2I(OTex, 3), P2D(RR, 0, 15), P2SRC(RT, 0), P2SRC(10, 0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RR, 0), PSEND };
    static const DWORD ps1[] = { PSV(1), OTex, DST(RT, 0), OMov, DST(RR, 0), SRC(RT, 0), PSEND };
    IDirect3DCubeTexture9 *cube = NULL;
    CV v[36];
    SAMPLE s[7];
    int face, i, pass;
    if (FAILED(IDirect3DDevice9_CreateCubeTexture(device, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL))) {
        printf("D9PIX cube textures not offered\n");
        return;
    }
    for (face = 0; face < 6; ++face) {
        D3DLOCKED_RECT r;
        int y, x;
        if (FAILED(IDirect3DCubeTexture9_LockRect(cube, (D3DCUBEMAP_FACES)face, 0, &r, NULL, 0))) continue;
        for (y = 0; y < 4; ++y) for (x = 0; x < 4; ++x) ((DWORD *)((BYTE *)r.pBits + y * r.Pitch))[x] = colors[face];
        IDirect3DCubeTexture9_UnlockRect(cube, (D3DCUBEMAP_FACES)face, 0);
    }
    for (face = 0; face < 6; ++face) {
        float x0 = 4.0f + face * 20, x1 = x0 + 16;
        const float xs[6] = { x0, x1, x0, x0, x1, x1 }, ys[6] = { 56, 56, 72, 72, 56, 72 };
        for (i = 0; i < 6; ++i) {
            CV *p = &v[face * 6 + i];
            p->x = xs[i]; p->y = ys[i]; p->z = 0.5f; p->rhw = 1; p->diffuse = 0xffffffff;
            p->u = dirs[face][0]; p->v = dirs[face][1]; p->w = dirs[face][2];
        }
        s[face].x = (int)x0 + 8; s[face].y = 64; s[face].rgb = colors[face] & 0xffffff;
    }
    s[6].x = 8; s[6].y = 8; s[6].rgb = CLEAR_COLOR;
    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)cube);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    for (pass = 40; pass <= 41; ++pass) {
        IDirect3DPixelShader9 *handle = NULL;
        const char *name = pass == 40 ? "ps2_cube" : "ps1_cube";
        HRESULT hr;
        if ((only && only != pass) || (pass == 40 && !version2)) continue;
        hr = IDirect3DDevice9_CreatePixelShader(device, pass == 40 ? ps2 : ps1, &handle);
        if (FAILED(hr)) { fail_hr(name, "create", hr); continue; }
        if (begin(name)) {
            IDirect3DDevice9_SetPixelShader(device, handle);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 12, v, sizeof(v[0]));
            IDirect3DDevice9_SetPixelShader(device, NULL);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check(name, s, 7);
        }
        IDirect3DPixelShader9_Release(handle);
    }
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DCubeTexture9_Release(cube);
}

/* ---- luminance and alpha formats ----
 * 47: an L8 texture of 0x80 over the left half and an A8L8 one of 0x40
 * (alpha 0xff) over the right, by a fixed-function stage. */
static void luminance_case(int only)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } TV;
    static const D3DFORMAT formats[2] = { D3DFMT_L8, D3DFMT_A8L8 };
    static const SAMPLE s[] = { { 32, 64, 0x808080 }, { 96, 64, 0x404040 } };
    IDirect3DTexture9 *tex[2] = { NULL, NULL };
    int k;
    if (only && only != 47) return;
    for (k = 0; k < 2; ++k) {
        D3DLOCKED_RECT r;
        int y;
        if (FAILED(IDirect3DDevice9_CreateTexture(device, 4, 4, 1, 0, formats[k], D3DPOOL_MANAGED, &tex[k], NULL))) {
            printf("D9PIX luminance format %d not offered\n", (int)formats[k]);
            if (tex[0]) IDirect3DTexture9_Release(tex[0]);
            return;
        }
        IDirect3DTexture9_LockRect(tex[k], 0, &r, NULL, 0);
        for (y = 0; y < 4; ++y) {
            if (k) { WORD *row = (WORD *)((BYTE *)r.pBits + y * r.Pitch); int x; for (x = 0; x < 4; ++x) row[x] = 0xff40; }
            else memset((BYTE *)r.pBits + y * r.Pitch, 0x80, 4);
        }
        IDirect3DTexture9_UnlockRect(tex[k], 0);
    }
    if (begin("luminance")) {
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        for (k = 0; k < 2; ++k) {
            float x0 = k * 64.0f, x1 = x0 + 64;
            TV v[4] = { { x0, 0, 0.5f, 1, 0xffffffff, 0, 0 }, { x1, 0, 0.5f, 1, 0xffffffff, 1, 0 },
                        { x0, Side, 0.5f, 1, 0xffffffff, 0, 1 }, { x1, Side, 0.5f, 1, 0xffffffff, 1, 1 } };
            IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)tex[k]);
            IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        }
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        check("luminance", s, 2);
    }
    for (k = 0; k < 2; ++k) IDirect3DTexture9_Release(tex[k]);
}

/* ---- bump maps ----
 * 48: tex t0 reads a V8U8 bump map whose texels are all du 64/127, dv 0;
 * texbem t1, t0 moves stage 1's lookup by du times BUMPENVMAT00 = 0.5, a
 * quarter of the 2x2 texture, so its left column shows its right one. */
static void bump_case(int only)
{
    union { float f; DWORD d; } half;
    IDirect3DTexture9 *bump = NULL, *colour;
    D3DLOCKED_RECT r;
    PSVERTEX v[6];
    SAMPLE s[5];
    float want[4][3];
    int i, y;
    static const DWORD ps[] = { PSV(1), OTex, DST(RT, 0), OTexbem, DST(RT, 1), SRC(RT, 0),
        OMov, DST(RR, 0), SRC(RT, 1), PSEND };
    if (only && only != 48) return;
    if (FAILED(IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_V8U8, D3DPOOL_MANAGED, &bump, NULL))) {
        printf("D9PIX bump map format V8U8 not offered\n");
        return;
    }
    IDirect3DTexture9_LockRect(bump, 0, &r, NULL, 0);
    for (y = 0; y < 2; ++y) { WORD *row = (WORD *)((BYTE *)r.pBits + y * r.Pitch); row[0] = row[1] = 0x0040; }
    IDirect3DTexture9_UnlockRect(bump, 0);
    colour = texture2x2(texels0);
    if (!colour) { IDirect3DTexture9_Release(bump); return; }
    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)bump);
    IDirect3DDevice9_SetTexture(device, 1, (IDirect3DBaseTexture9 *)colour);
    for (i = 0; i < 2; ++i) {
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    half.f = 0.5f;
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT00, half.d);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT01, 0);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT10, 0);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_BUMPENVMAT11, 0);
    ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
    for (i = 0; i < 4; ++i) texel(i | 1, want[i]);   /* the right column: green, white */
    quad_samples(s, want);
    ps_run("ps_texbem", ps, v, 1, s, 5);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTexture(device, 1, NULL);
    IDirect3DTexture9_Release(bump);
    IDirect3DTexture9_Release(colour);
}

/* ---- colour write mask ----
 * 50: white quads over the clear colour, the left one with D3DRS_COLORWRITEENABLE red
 * only, the right one with it zero (as for stencil shadow volumes): the left keeps the
 * clear colour's green and blue, the right all of it. */
static void colorwrite_case(int only)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; } CV;
    static const SAMPLE s[] = { { 32, 64, 0xff0066 }, { 96, 64, CLEAR_COLOR & 0xffffff } };
    int k;
    if (only && only != 50) return;
    if (!begin("colorwrite")) return;
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    for (k = 0; k < 2; ++k) {
        float x0 = k * 64.0f, x1 = x0 + 64;
        CV v[4] = { { x0, 0, 0.5f, 1, 0xffffffff }, { x1, 0, 0.5f, 1, 0xffffffff },
                    { x0, Side, 0.5f, 1, 0xffffffff }, { x1, Side, 0.5f, 1, 0xffffffff } };
        IDirect3DDevice9_SetRenderState(device, D3DRS_COLORWRITEENABLE, k ? 0 : D3DCOLORWRITEENABLE_RED);
        IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
    }
    IDirect3DDevice9_SetRenderState(device, D3DRS_COLORWRITEENABLE, 15);
    check("colorwrite", s, 2);
}

/* ---- DOT3 ----
 * 53: D3DTOP_DOTPRODUCT3 of the diffuse colour (1, 0.5, 0.5), a vector along x,
 * with the texture factor: along x as well on the left (white), along y on the
 * right (black). */
static void dot3_case(int only, BOOL offered)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; } DV;
    static const SAMPLE s[] = { { 32, 64, 0xffffff }, { 96, 64, 0x000000 } };
    int k;
    if (only && only != 53) return;
    if (!offered) { printf("D9PIX DOT3 not offered\n"); return; }
    if (!begin("dot3")) return;
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    for (k = 0; k < 2; ++k) {
        float x0 = k * 64.0f, x1 = x0 + 64;
        DV v[4] = { { x0, 0, 0.5f, 1, 0xffff8080 }, { x1, 0, 0.5f, 1, 0xffff8080 },
                    { x0, Side, 0.5f, 1, 0xffff8080 }, { x1, Side, 0.5f, 1, 0xffff8080 } };
        IDirect3DDevice9_SetRenderState(device, D3DRS_TEXTUREFACTOR, k ? 0xff80ff80 : 0xffff8080);
        IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
    }
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    check("dot3", s, 2);
}

/* ---- fixed-function bump mapping ----
 * 54: stage 0 is D3DTOP_BUMPENVMAP with the bump map of case 48 and its matrix
 * (on the bump stage itself, unlike texbem); stage 1 shows its texture at the
 * moved coordinates: the right column. */
static void embm_case(int only, BOOL offered)
{
    union { float f; DWORD d; } half;
    IDirect3DTexture9 *bump = NULL, *colour;
    D3DLOCKED_RECT r;
    PSVERTEX v[6];
    SAMPLE s[5];
    float want[4][3];
    int i, y;
    HRESULT hr;
    if (only && only != 54) return;
    if (!offered) { printf("D9PIX BUMPENVMAP not offered\n"); return; }
    if (FAILED(IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_V8U8, D3DPOOL_MANAGED, &bump, NULL))) {
        printf("D9PIX bump map format V8U8 not offered\n");
        return;
    }
    IDirect3DTexture9_LockRect(bump, 0, &r, NULL, 0);
    for (y = 0; y < 2; ++y) { WORD *row = (WORD *)((BYTE *)r.pBits + y * r.Pitch); row[0] = row[1] = 0x0040; }
    IDirect3DTexture9_UnlockRect(bump, 0);
    colour = texture2x2(texels0);
    if (!colour) { IDirect3DTexture9_Release(bump); return; }
    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)bump);
    IDirect3DDevice9_SetTexture(device, 1, (IDirect3DBaseTexture9 *)colour);
    for (i = 0; i < 2; ++i) {
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    half.f = 0.5f;
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_BUMPENVMAT00, half.d);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_BUMPENVMAT01, 0);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_BUMPENVMAT10, 0);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_BUMPENVMAT11, 0);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_BUMPENVMAP);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_TEXCOORDINDEX, 1);
    ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
    for (i = 0; i < 4; ++i) texel(i | 1, want[i]);   /* the right column: green, white */
    quad_samples(s, want);
    if (begin("embm")) {
        IDirect3DDevice9_SetFVF(device, PSFVF);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr("embm", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("embm", s, 5);
    }
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_TEXCOORDINDEX, 1);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTexture(device, 1, NULL);
    IDirect3DTexture9_Release(bump);
    IDirect3DTexture9_Release(colour);
}

/* ---- eight fixed-function stages ----
 * 58: stages 0 to 6 add a black texture each (their coordinate sets are all 0), stage 7 modulates
 * the diffuse white by a texture red on its left and green on its right, read with coordinate set 7. */
static void stages8_case(int only, BOOL offered)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float st[8][2]; } V8;
    static const DWORD black[4] = { 0xff000000, 0xff000000, 0xff000000, 0xff000000 };
    static const DWORD halves[4] = { 0xffff0000, 0xff00ff00, 0xffff0000, 0xff00ff00 };
    static const SAMPLE s[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR } };
    IDirect3DTexture9 *dark, *last;
    V8 v[4];
    HRESULT hr;
    int i, k;
    if (only && only != 58) return;
    if (!offered) { printf("D9PIX eight texture stages not offered\n"); return; }
    dark = texture2x2(black);
    last = texture2x2(halves);
    if (!dark || !last) { printf("D9PIX case=stages8 textures failed\n"); goto done; }
    memset(v, 0, sizeof(v));
    for (i = 0; i < 4; ++i) {
        v[i].x = i & 1 ? 96.0f : 32.0f;
        v[i].y = i & 2 ? 96.0f : 32.0f;
        v[i].z = 0.5f; v[i].rhw = 1; v[i].diffuse = 0xffffffff;
        v[i].st[7][0] = i & 1 ? 1.0f : 0.0f;
        v[i].st[7][1] = i & 2 ? 1.0f : 0.0f;
    }
    for (k = 0; k < 8; ++k) {
        IDirect3DDevice9_SetTexture(device, k, (IDirect3DBaseTexture9 *)(k < 7 ? dark : last));
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_COLOROP, k < 7 ? D3DTOP_ADD : D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_COLORARG2, k ? D3DTA_CURRENT : D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_ALPHAARG1, k ? D3DTA_CURRENT : D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_TEXCOORDINDEX, k);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (begin("stages8")) {
        IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX8);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr("stages8", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("stages8", s, 3);
    }
done:
    for (k = 7; k >= 0; --k) {
        IDirect3DDevice9_SetTexture(device, k, NULL);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_COLOROP, k ? D3DTOP_DISABLE : D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_COLORARG2, D3DTA_CURRENT);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_ALPHAOP, k ? D3DTOP_DISABLE : D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(device, k, D3DTSS_TEXCOORDINDEX, k);
    }
    if (dark) IDirect3DTexture9_Release(dark);
    if (last) IDirect3DTexture9_Release(last);
}

/* ---- texture transforms (fixed function) ----
 * 51: D3DTTFF_COUNT2 with a matrix whose third row moves u by a half (Direct3D
 * transforms (u, v, 1, 0)), on untransformed vertices with identity world, view and
 * projection: the 2x2 texture's left column shows its right one.
 * 52: pretransformed vertices with four coordinates (2u, 2v, 0, 2) and COUNT4 |
 * PROJECTED: Direct3D applies no texture matrix to such vertices, but divided by the
 * fourth they are the plain coordinates. */
static void transform_cases(int only)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float s, t, r, q; } TV;
    IDirect3DTexture9 *tex;
    D3DMATRIX m;
    SAMPLE s[5];
    float want[4][3];
    int i, pass;
    if (only && only != 51 && only != 52) return;
    if (!(tex = texture2x2(texels0))) return;
    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    for (pass = 51; pass <= 52; ++pass) {
        const char *name = pass == 51 ? "tt_count2" : "tt_projected";
        float k = pass == 51 ? 1.0f : 2.0f;
        TV v[4] = { { 32, 32, 0.5f, 1, 0xffffffff, 0, 0, 0, k }, { 96, 32, 0.5f, 1, 0xffffffff, k, 0, 0, k },
                    { 32, 96, 0.5f, 1, 0xffffffff, 0, k, 0, k }, { 96, 96, 0.5f, 1, 0xffffffff, k, k, 0, k } };
        if (only && only != pass) continue;
        memset(&m, 0, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        if (pass == 51) m._31 = 0.5f;
        IDirect3DDevice9_SetTransform(device, D3DTS_TEXTURE0, &m);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                              pass == 51 ? D3DTTFF_COUNT2 : D3DTTFF_COUNT4 | D3DTTFF_PROJECTED);
        for (i = 0; i < 4; ++i) texel(pass == 51 ? i | 1 : i, want[i]);
        quad_samples(s, want);
        if (begin(name)) {
            IDirect3DDevice9_SetFVF(device, pass == 51 ? D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE2(0) :
                                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
            if (pass == 51) {
                /* the same quad, untransformed (the viewport maps -1..1 to 0..Side), with two
                 * coordinates per vertex */
                struct { float x, y, z; DWORD diffuse; float u, v; } w[4];
                D3DMATRIX identity;
                memset(&identity, 0, sizeof(identity));
                identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
                IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &identity);
                IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &identity);
                IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &identity);
                for (i = 0; i < 4; ++i) {
                    w[i].x = v[i].x / (Side / 2) - 1; w[i].y = 1 - v[i].y / (Side / 2); w[i].z = v[i].z;
                    w[i].diffuse = v[i].diffuse; w[i].u = v[i].s; w[i].v = v[i].t;
                }
                IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, w, sizeof(w[0]));
            } else IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
            check(name, s, 5);
        }
    }
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DTexture9_Release(tex);
}

/* ---- volume textures ----
 * A 4x4x4 volume with one colour per slice (red, green, blue, yellow),
 * sampled at each slice's middle by four quads: 45 with ps_2_0
 * (dcl_volume), 46 with ps_1_1 (tex), 49 with ps_1_1 from a DXT1 volume. */
static void volume_cases(int only, BOOL version2)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v, w; } VV;
    static const DWORD colors[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00 };
    static const DWORD ps2[] = { 0xffff0200, P2DCL_T(0, 7), P2I(31, 2), (0x80000000UL | (4UL << 27)), P2D(10, 0, 15),
        P2I(OTex, 3), P2D(RR, 0, 15), P2SRC(RT, 0), P2SRC(10, 0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RR, 0), PSEND };
    static const DWORD ps1[] = { PSV(1), OTex, DST(RT, 0), OMov, DST(RR, 0), SRC(RT, 0), PSEND };
    IDirect3DVolumeTexture9 *volume = NULL;
    D3DLOCKED_BOX box;
    VV v[24];
    SAMPLE s[5];
    int slice, i, pass;
    if (FAILED(IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, NULL))) {
        printf("D9PIX volume textures not offered\n");
        return;
    }
    if (SUCCEEDED(IDirect3DVolumeTexture9_LockBox(volume, 0, &box, NULL, 0))) {
        int x, y;
        for (slice = 0; slice < 4; ++slice)
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x)
                    ((DWORD *)((BYTE *)box.pBits + slice * box.SlicePitch + y * box.RowPitch))[x] = colors[slice];
        IDirect3DVolumeTexture9_UnlockBox(volume, 0);
    }
    for (slice = 0; slice < 4; ++slice) {
        float x0 = 8.0f + slice * 28, x1 = x0 + 20, w = (slice + 0.5f) / 4;
        const float xs[6] = { x0, x1, x0, x0, x1, x1 }, ys[6] = { 56, 56, 76, 76, 56, 76 };
        for (i = 0; i < 6; ++i) {
            VV *p = &v[slice * 6 + i];
            p->x = xs[i]; p->y = ys[i]; p->z = 0.5f; p->rhw = 1; p->diffuse = 0xffffffff;
            p->u = 0.5f; p->v = 0.5f; p->w = w;
        }
        s[slice].x = (int)x0 + 10; s[slice].y = 66; s[slice].rgb = colors[slice] & 0xffffff;
    }
    s[4].x = 8; s[4].y = 8; s[4].rgb = CLEAR_COLOR & 0xffffff;
    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)volume);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    for (pass = 45; pass <= 46; ++pass) {
        IDirect3DPixelShader9 *handle = NULL;
        const char *name = pass == 45 ? "ps2_volume" : "ps1_volume";
        HRESULT hr;
        if ((only && only != pass) || (pass == 45 && !version2)) continue;
        hr = IDirect3DDevice9_CreatePixelShader(device, pass == 45 ? ps2 : ps1, &handle);
        if (FAILED(hr)) { fail_hr(name, "create", hr); continue; }
        if (begin(name)) {
            IDirect3DDevice9_SetPixelShader(device, handle);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 8, v, sizeof(v[0]));
            IDirect3DDevice9_SetPixelShader(device, NULL);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check(name, s, 5);
        }
        IDirect3DPixelShader9_Release(handle);
    }
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DVolumeTexture9_Release(volume);
    /* 49: the same slices in a DXT1 volume, one block each (565 colours, both ends alike). */
    if ((!only || only == 49) &&
        SUCCEEDED(IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &volume, NULL))) {
        static const WORD ends[4] = { 0xf800, 0x07e0, 0x001f, 0xffe0 };
        IDirect3DPixelShader9 *handle = NULL;
        if (SUCCEEDED(IDirect3DVolumeTexture9_LockBox(volume, 0, &box, NULL, 0))) {
            for (slice = 0; slice < 4; ++slice) {
                WORD *b = (WORD *)((BYTE *)box.pBits + slice * box.SlicePitch);
                b[0] = b[1] = ends[slice];
                b[2] = b[3] = 0;
            }
            IDirect3DVolumeTexture9_UnlockBox(volume, 0);
        }
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)volume);
        if (FAILED(IDirect3DDevice9_CreatePixelShader(device, ps1, &handle))) fail_hr("ps1_volume_dxt1", "create", E_FAIL);
        else {
            if (begin("ps1_volume_dxt1")) {
                HRESULT hr;
                IDirect3DDevice9_SetPixelShader(device, handle);
                IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 8, v, sizeof(v[0]));
                IDirect3DDevice9_SetPixelShader(device, NULL);
                if (FAILED(hr)) { fail_hr("ps1_volume_dxt1", "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check("ps1_volume_dxt1", s, 5);
            }
            IDirect3DPixelShader9_Release(handle);
        }
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DVolumeTexture9_Release(volume);
    } else if (!only || only == 49) printf("D9PIX DXT1 volume textures not offered\n");
}

/* ---- render target textures ----
 * A 64x64 render target texture is cleared blue with a red left half drawn
 * into it, then drawn over the back buffer: 43 by a fixed-function stage,
 * 44 by a ps_1_1 shader. */
/* ---- the scissor test ----
 * 60: a red quad over the whole back buffer with the scissor rectangle 32,32 to 96,96: red
 * inside it, the clear colour outside. */
static void scissor_case(int only, BOOL offered)
{
    static const SAMPLE s[] = { { 64, 64, 0xff0000 }, { 40, 88, 0xff0000 }, { 8, 8, CLEAR_COLOR }, { 120, 64, CLEAR_COLOR },
                                { 64, 120, CLEAR_COLOR } };
    RECT r = { 32, 32, 96, 96 };
    TLVERTEX v[6];
    HRESULT hr;
    if (only && only != 60) return;
    if (!offered) { printf("D9PIX scissor test not offered\n"); return; }
    if (!begin("scissor")) return;
    IDirect3DDevice9_SetScissorRect(device, &r);
    IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    quad(v, 0, 0, Side, Side, 0xffff0000);
    hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
    IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
    if (FAILED(hr)) { fail_hr("scissor", "draw", hr); IDirect3DDevice9_EndScene(device); }
    else check("scissor", s, 5);
}

static void rt_cases(int only)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } TV;
    static const DWORD ps1[] = { PSV(1), OTex, DST(RT, 0), OMov, DST(RR, 0), SRC(RT, 0), PSEND };
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL, *back = NULL;
    TLVERTEX half[6];
    TV v[4] = { { 0, 0, 0.5f, 1, 0xffffffff, 0, 0 }, { Side, 0, 0.5f, 1, 0xffffffff, 1, 0 },
                { 0, Side, 0.5f, 1, 0xffffffff, 0, 1 }, { Side, Side, 0.5f, 1, 0xffffffff, 1, 1 } };
    static const SAMPLE s[] = { { 32, 64, 0xff0000 }, { 96, 64, 0x0000ff }, { 8, 8, 0xff0000 }, { 120, 120, 0x0000ff } };
    int pass;
    HRESULT hr = IDirect3DDevice9_CreateTexture(device, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                D3DPOOL_DEFAULT, &texture, NULL);
    if (FAILED(hr)) { printf("D9PIX render target textures not offered hr=%08lx\n", (unsigned long)hr); return; }
    for (pass = 43; pass <= 44; ++pass) {
        const char *name = pass == 43 ? "rt_texture" : "rt_texture_ps1";
        IDirect3DPixelShader9 *handle = NULL;
        if (only && only != pass) continue;
        if (pass == 44 && FAILED(hr = IDirect3DDevice9_CreatePixelShader(device, ps1, &handle))) {
            fail_hr(name, "create", hr);
            continue;
        }
        /* Into the texture: blue, then a red left half. */
        IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        IDirect3DDevice9_GetRenderTarget(device, 0, &back);
        hr = IDirect3DDevice9_SetRenderTarget(device, 0, surface);
        if (FAILED(hr)) fail_hr(name, "target", hr);
        else {
            IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff0000ff, 1.0f, 0);
            IDirect3DDevice9_BeginScene(device);
            quad(half, 0, 0, 32, 64, 0xffff0000);
            IDirect3DDevice9_SetTexture(device, 0, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, half, sizeof(half[0]));
            IDirect3DDevice9_EndScene(device);
            IDirect3DDevice9_SetRenderTarget(device, 0, back);
            /* Then the texture over the whole back buffer. */
            if (begin(name)) {
                IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                if (handle) IDirect3DDevice9_SetPixelShader(device, handle);
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
                IDirect3DDevice9_SetPixelShader(device, NULL);
                IDirect3DDevice9_SetTexture(device, 0, NULL);
                IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check(name, s, 4);
            }
        }
        if (surface) { IDirect3DSurface9_Release(surface); surface = NULL; }
        if (back) { IDirect3DSurface9_Release(back); back = NULL; }
        if (handle) IDirect3DPixelShader9_Release(handle);
    }
    /* 56: ColorFill of the whole texture (green), drawn over the back buffer. 57: StretchRect of the
     * whole back buffer (cleared green) into a texture of its size, then drawn black-cleared back. */
    for (pass = 56; pass <= 57; ++pass) {
        static const SAMPLE g[] = { { 8, 8, 0x00ff00 }, { 120, 8, 0x00ff00 }, { 64, 64, 0x00ff00 }, { 120, 120, 0x00ff00 } };
        const char *name = pass == 56 ? "colorfill" : "stretchrect";
        IDirect3DTexture9 *t = texture, *whole = NULL;
        if (only && only != pass) continue;
        if (pass == 57) {
            if (FAILED(hr = IDirect3DDevice9_CreateTexture(device, Side, Side, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                           D3DPOOL_DEFAULT, &whole, NULL))) { fail_hr(name, "create", hr); continue; }
            t = whole;
        }
        IDirect3DTexture9_GetSurfaceLevel(t, 0, &surface);
        IDirect3DDevice9_GetRenderTarget(device, 0, &back);
        if (pass == 56) hr = IDirect3DDevice9_ColorFill(device, surface, NULL, 0xff00ff00);
        else {
            IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
            hr = IDirect3DDevice9_StretchRect(device, back, NULL, surface, NULL, D3DTEXF_NONE);
            IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
        }
        if (FAILED(hr)) fail_hr(name, pass == 56 ? "fill" : "stretch", hr);
        else if (begin(name)) {
            IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
            IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
            IDirect3DDevice9_SetTexture(device, 0, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check(name, g, 4);
        }
        if (surface) { IDirect3DSurface9_Release(surface); surface = NULL; }
        if (back) { IDirect3DSurface9_Release(back); back = NULL; }
        if (whole) IDirect3DTexture9_Release(whole);
    }
    /* 59: StretchRect of the whole back buffer (red left, green right) into the 64x64 texture with
     * linear filtering, then the texture drawn over the back buffer. */
    D3DCAPS9 dc;
    if (FAILED(IDirect3DDevice9_GetDeviceCaps(device, &dc)) || (~dc.StretchRectFilterCaps & (D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFLINEAR)))
        printf("D9PIX linear StretchRect not offered\n");
    else if (!only || only == 59) {
        static const SAMPLE h[] = { { 16, 64, 0xff0000 }, { 112, 64, 0x00ff00 }, { 8, 8, 0xff0000 }, { 120, 120, 0x00ff00 } };
        IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        IDirect3DDevice9_GetRenderTarget(device, 0, &back);
        if (begin("stretch_linear")) {
            IDirect3DDevice9_SetTexture(device, 0, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            quad(half, 0, 0, 64, Side, 0xffff0000);
            IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, half, sizeof(half[0]));
            quad(half, 64, 0, Side, Side, 0xff00ff00);
            IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, half, sizeof(half[0]));
            IDirect3DDevice9_EndScene(device);
            hr = IDirect3DDevice9_StretchRect(device, back, NULL, surface, NULL, D3DTEXF_LINEAR);
            if (FAILED(hr)) fail_hr("stretch_linear", "stretch", hr);
            else if (begin("stretch_linear")) {
                IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
                IDirect3DDevice9_SetTexture(device, 0, NULL);
                IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                if (FAILED(hr)) { fail_hr("stretch_linear", "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check("stretch_linear", h, 4);
            }
        }
        if (surface) { IDirect3DSurface9_Release(surface); surface = NULL; }
        if (back) { IDirect3DSurface9_Release(back); back = NULL; }
    }
    IDirect3DTexture9_Release(texture);
}

int main(int argc, char **argv)
{
    IDirect3D9 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d9.dll");
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    D3DCAPS9 caps;
    HWND window;
    DWORD flags;
    BOOL software = FALSE;
    int only = 0, a;
    HRESULT hr;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* "sw": software vertex processing; "ps=N": only case N of the shader
     * cases (pixel shaders below 30, vertex shaders from 30). */
    for (a = 1; a < argc; ++a) {
        if (!lstrcmpiA(argv[a], "sw")) software = TRUE;
        else if (!strncmp(argv[a], "ps=", 3)) only = atoi(argv[a] + 3);
    }
    create = lib ? (IDirect3D9 *(WINAPI *)(UINT))GetProcAddress(lib, "Direct3DCreate9") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { printf("D9PIX no Direct3D 9 runtime\nD9PIX end result=FAIL\n"); return 1; }
    {
        D3DADAPTER_IDENTIFIER9 id;
        D3DDISPLAYMODE now;
        if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id)))
            printf("D9PIX adapter driver=%s version=%08lx%08lx\n", id.Driver,
                   (unsigned long)id.DriverVersion.HighPart, (unsigned long)id.DriverVersion.LowPart);
        IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &now);
        printf("D9PIX mode=%lux%lu format=%lu modes=%lu check_type=%08lx\n", (unsigned long)now.Width,
               (unsigned long)now.Height, (unsigned long)now.Format,
               (unsigned long)IDirect3D9_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT, now.Format),
               (unsigned long)IDirect3D9_CheckDeviceType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, now.Format,
                                                         now.Format, TRUE));
    }
    hr = IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr)) { printf("D9PIX caps hr=%08lx\nD9PIX end result=UNAVAILABLE\n", (unsigned long)hr); return 2; }
    {
        /* Every word of the record, for comparison with what the driver reports. */
        const DWORD *w = (const DWORD *)&caps;
        UINT i;
        for (i = 0; i < sizeof(caps) / 4; ++i)
            printf("%s%08lx%s", i % 14 ? " " : "D8CAPS ", (unsigned long)w[i], i % 14 == 13 || i + 1 == sizeof(caps) / 4 ? "\n" : "");
    }
    printf("D9PIX caps devcaps=%08lx vs=%08lx ps=%08lx streams=%lu stride=%lu primitives=%lu index=%lx stages=%lu textures=%lu\n",
           (unsigned long)caps.DevCaps, (unsigned long)caps.VertexShaderVersion,
           (unsigned long)caps.PixelShaderVersion, (unsigned long)caps.MaxStreams,
           (unsigned long)caps.MaxStreamStride, (unsigned long)caps.MaxPrimitiveCount,
           (unsigned long)caps.MaxVertexIndex, (unsigned long)caps.MaxTextureBlendStages,
           (unsigned long)caps.MaxSimultaneousTextures);
    printf("D9PIX caps9 ps20=%d,%d,%d vs20=%d,%d executed=%lu,%lu rts=%lu decltypes=%lx stretch=%lx group=%lu\n",
           caps.PS20Caps.NumTemps, caps.PS20Caps.NumInstructionSlots, caps.PS20Caps.StaticFlowControlDepth,
           caps.VS20Caps.NumTemps, caps.VS20Caps.StaticFlowControlDepth,
           (unsigned long)caps.MaxPShaderInstructionsExecuted, (unsigned long)caps.MaxVShaderInstructionsExecuted,
           (unsigned long)caps.NumSimultaneousRTs, (unsigned long)caps.DeclTypes,
           (unsigned long)caps.StretchRectFilterCaps, (unsigned long)caps.NumberOfAdaptersInGroup);
    IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    window = CreateWindowA("STATIC", "d9pixels", WS_POPUP | WS_VISIBLE, 0, 0, Side, Side, NULL, NULL, NULL, NULL);
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
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &pp, &device);
    printf("D9PIX device hr=%08lx vertex_processing=%s format=%lu\n", (unsigned long)hr,
           flags == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software", (unsigned long)mode.Format);
    if (FAILED(hr)) { printf("D9PIX end result=UNAVAILABLE\n"); return 2; }
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (!only) {
        case_clear();
        case_up();
        case_vb();
        case_strip();
        indexed("indexed", 0, 0x00ffff);
        indexed("base", 3, 0xff8000);
        case_texture();
        case_transform();
        case_dxt();
    }
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 2) && (!only || only < 30))
        pixel_shaders(only, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0));
    else if (caps.PixelShaderVersion < D3DPS_VERSION(1, 2)) printf("D9PIX pixel shaders not offered\n");
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) && (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) && (!only || only >= 40))
        cube_cases(only, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0));
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) && (!only || only >= 43)) rt_cases(only);
    scissor_case(only, (caps.RasterCaps & D3DPRASTERCAPS_SCISSORTEST) != 0);
    luminance_case(only);
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1)) bump_case(only);
    colorwrite_case(only);
    dot3_case(only, (caps.TextureOpCaps & D3DTEXOPCAPS_DOTPRODUCT3) != 0);
    embm_case(only, (caps.TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAP) != 0);
    stages8_case(only, caps.MaxTextureBlendStages >= 8 && caps.MaxSimultaneousTextures >= 8);
    transform_cases(only);
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) && (caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) && (!only || only >= 45))
        volume_cases(only, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0));
    if (caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) printf("D9PIX vertex shader cases need pixel shader 2.0\n");
    else if (flags == D3DCREATE_SOFTWARE_VERTEXPROCESSING || caps.VertexShaderVersion >= D3DVS_VERSION(2, 0)) {
        if (!only || only >= 30) vertex_shaders(only);
        if (!only || only >= 61) vs_flow_cases(only, (caps.DevCaps2 & D3DDEVCAPS2_STREAMOFFSET) != 0);
    } else printf("D9PIX vertex shaders not offered\n");
    clip_cases(only, caps.MaxUserClipPlanes >= 1, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0) && (flags == D3DCREATE_SOFTWARE_VERTEXPROCESSING || caps.VertexShaderVersion >= D3DVS_VERSION(2, 0)));
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    printf("D9PIX end pass=%d fail=%d result=%s\n", passes, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
