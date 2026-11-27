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

/* 77: a 32-bit index buffer naming vertices past 65535: a vertex buffer of 65540 vertices, white
 * but for a magenta quad in its last four, drawn through indices 65536 to 65539. */
static void index32_case(int only, BOOL offered)
{
    enum { Vertices = 65540 };
    static const SAMPLE s[] = { { 64, 64, 0xff00ff }, { 36, 92, 0xff00ff }, { 120, 120, CLEAR_COLOR } };
    static const DWORD idx[6] = { 65536, 65537, 65538, 65538, 65537, 65539 };
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    TLVERTEX *v;
    BYTE *p;
    HRESULT hr;
    UINT i;
    if (only && only != 77) return;
    if (!offered) { printf("D9PIX 32-bit indices not offered\n"); return; }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, Vertices * sizeof(TLVERTEX), D3DUSAGE_WRITEONLY, TLFVF,
                                             D3DPOOL_DEFAULT, &vb, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0);
    if (FAILED(hr)) { fail_hr("index32", "vertex buffer", hr); goto done; }
    for (i = 0; i < Vertices; ++i) {
        UINT k = i >= Vertices - 4 ? i - (Vertices - 4) : 0;
        v[i].x = (k & 1) ? 96.0f : 32.0f; v[i].y = (k & 2) ? 96.0f : 32.0f;
        v[i].z = 0.5f; v[i].rhw = 1.0f; v[i].diffuse = i >= Vertices - 4 ? 0xffff00ff : 0xffffffff;
    }
    IDirect3DVertexBuffer9_Unlock(vb);
    hr = IDirect3DDevice9_CreateIndexBuffer(device, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT,
                                            &ib, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&p, 0);
    if (FAILED(hr)) { fail_hr("index32", "index buffer", hr); goto done; }
    memcpy(p, idx, sizeof(idx));
    IDirect3DIndexBuffer9_Unlock(ib);
    if (begin("index32")) {
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetFVF(device, TLFVF);
        IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(TLVERTEX));
        IDirect3DDevice9_SetIndices(device, ib);
        hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, 65536, 4, 0, 2);
        if (FAILED(hr)) { fail_hr("index32", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("index32", s, 3);
        IDirect3DDevice9_SetIndices(device, NULL);
        IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    }
done:
    if (ib) IDirect3DIndexBuffer9_Release(ib);
    if (vb) IDirect3DVertexBuffer9_Release(vb);
}

/* 75: point sizes. A sprite of 16 pixels at 64, 64 textured with a 2x2 texture (red, green over
 * blue, white): each quarter shows its texel, top left red as in Direct3D. A point of 8 pixels
 * at 32, 100 without sprites: yellow across it. The clear colour beside both. */
static void point_case(int only)
{
    static const DWORD texels[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
    static const SAMPLE s[] = { { 60, 60, 0xff0000 }, { 68, 60, 0x00ff00 }, { 60, 68, 0x0000ff }, { 68, 68, 0xffffff },
                                { 50, 64, CLEAR_COLOR }, { 78, 64, CLEAR_COLOR },
                                { 30, 98, 0xffff00 }, { 34, 102, 0xffff00 }, { 40, 100, CLEAR_COLOR } };
    TLTEXVERTEX sprite = { 64.0f, 64.0f, 0.5f, 1, 0xffffffff, 0, 0 };
    TLVERTEX dot = { 32.0f, 100.0f, 0.5f, 1, 0xffffff00 };
    IDirect3DTexture9 *t;
    float size;
    HRESULT hr;
    if (only && only != 75) return;
    t = texture2x2(texels);
    if (!t) { printf("D9PIX case=points texture failed\n"); ++failures; return; }
    if (begin("points")) {
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        size = 16.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&size);
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSPRITEENABLE, TRUE);
        IDirect3DDevice9_SetFVF(device, TLTEXFVF);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1, &sprite, sizeof(sprite));
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        size = 8.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&size);
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSPRITEENABLE, FALSE);
        IDirect3DDevice9_SetFVF(device, TLFVF);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1, &dot, sizeof(dot));
        size = 1.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&size);
        if (FAILED(hr)) { fail_hr("points", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("points", s, 9);
    }
    IDirect3DTexture9_Release(t);
}

/* 76: a vs_1_1 program sets the point size: mov oPts, c0.x with c0.x 10, a green point at clip
 * (0.5, -0.5), the target's 96, 96. */
static void program_point_case(int only, BOOL offered)
{
    static const D3DVERTEXELEMENT9 elements[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    static const DWORD vs[] = { 0xfffe0101,
        0x0000001f, 0x80000000, 0x900f0000,     /* dcl_position v0 */
        0x0000001f, 0x8000000a, 0x900f0001,     /* dcl_color v1 */
        0x00000001, 0xc00f0000, 0x90e40000,     /* mov oPos, v0 */
        0x00000001, 0xd00f0000, 0x90e40001,     /* mov oD0, v1 */
        0x00000001, 0xc00f0002, 0xa0000000,     /* mov oPts, c0.x (scalar outputs take the full mask) */
        0x0000ffff };
    static const float c0[4] = { 10.0f, 0, 0, 0 };
    static const SAMPLE s[] = { { 93, 93, 0x00ff00 }, { 99, 99, 0x00ff00 }, { 106, 96, CLEAR_COLOR }, { 96, 86, CLEAR_COLOR } };
    struct { float x, y, z; DWORD color; } v = { 0.5f, -0.5f, 0.5f, 0xff00ff00 };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexShader9 *shader = NULL;
    HRESULT hr;
    if (only && only != 76) return;
    if (!offered) { printf("D9PIX vertex shaders 1.1 not offered\n"); return; }
    hr = IDirect3DDevice9_CreateVertexDeclaration(device, elements, &decl);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateVertexShader(device, vs, &shader);
    if (FAILED(hr)) { fail_hr("program_point", "create", hr); goto done; }
    if (begin("program_point")) {
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetVertexDeclaration(device, decl);
        IDirect3DDevice9_SetVertexShader(device, shader);
        IDirect3DDevice9_SetVertexShaderConstantF(device, 0, c0, 1);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1, &v, sizeof(v));
        IDirect3DDevice9_SetVertexShader(device, NULL);
        if (FAILED(hr)) { fail_hr("program_point", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("program_point", s, 4);
    }
done:
    if (shader) IDirect3DVertexShader9_Release(shader);
    if (decl) IDirect3DVertexDeclaration9_Release(decl);
}

/* 82: an indexed draw of untransformed vertices from a write-only vertex buffer and index buffer,
 * the indices from the buffer's start. With hardware vertex processing a driver may keep both on
 * the GPU, where the draw's index "pointer" is the offset zero. */
static void static_indexed_case(int only)
{
    static const SAMPLE s[] = { { 64, 64, 0x00ffff }, { 40, 88, 0x00ffff }, { 8, 8, CLEAR_COLOR } };
    static const XYZVERTEX v[4] = {
        { -0.5f, 0.5f, 0.5f, 0xff00ffff }, { 0.5f, 0.5f, 0.5f, 0xff00ffff },
        { -0.5f, -0.5f, 0.5f, 0xff00ffff }, { 0.5f, -0.5f, 0.5f, 0xff00ffff } };
    static const WORD idx[6] = { 0, 1, 2, 2, 1, 3 };
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9 *ib = NULL;
    D3DMATRIX m;
    BYTE *p;
    HRESULT hr;
    if (only && only != 82) return;
    vb = vertex_buffer(v, sizeof(v), XYZFVF);
    if (!vb) { fail_hr("static_indexed", "create", E_FAIL); return; }
    hr = IDirect3DDevice9_CreateIndexBuffer(device, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                            D3DPOOL_DEFAULT, &ib, NULL);
    if (FAILED(hr)) { fail_hr("static_indexed", "index buffer", hr); IDirect3DVertexBuffer9_Release(vb); return; }
    if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&p, 0))) {
        memcpy(p, idx, sizeof(idx));
        IDirect3DIndexBuffer9_Unlock(ib);
    }
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (begin("static_indexed")) {
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetFVF(device, XYZFVF);
        IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(v[0]));
        IDirect3DDevice9_SetIndices(device, ib);
        hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
        if (FAILED(hr)) { fail_hr("static_indexed", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("static_indexed", s, 3);
    }
    IDirect3DDevice9_SetIndices(device, NULL);
    IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    IDirect3DIndexBuffer9_Release(ib);
    IDirect3DVertexBuffer9_Release(vb);
}

/* 79: a point scaled by its distance from the eye: D3DRS_POINTSCALEENABLE with A = B = 0 and
 * C = 1, a size of 0.0625, the point 0.5 from the eye, a viewport 128 pixels high:
 * 128 x 0.0625 x sqrt(1 / 0.25) = 16 pixels around the centre. */
static void scaled_point_case(int only, BOOL offered)
{
    static const SAMPLE s[] = { { 64, 64, 0xffff00 }, { 58, 58, 0xffff00 }, { 70, 70, 0xffff00 },
                                { 76, 64, CLEAR_COLOR }, { 64, 52, CLEAR_COLOR } };
    static const XYZVERTEX v = { 0.0f, 0.0f, 0.5f, 0xffffff00 };
    D3DMATRIX m;
    HRESULT hr;
    float f;
    if (only && only != 79) return;
    if (!offered) { printf("D9PIX scaled points not offered\n"); return; }
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (begin("scaled_point")) {
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        f = 0.0625f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&f);
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALEENABLE, TRUE);
        f = 0.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_A, *(DWORD *)&f);
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_B, *(DWORD *)&f);
        f = 1.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_C, *(DWORD *)&f);
        IDirect3DDevice9_SetFVF(device, XYZFVF);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1, &v, sizeof(v));
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALEENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_A, *(DWORD *)&f);
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&f);
        f = 0.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_C, *(DWORD *)&f);
        if (FAILED(hr)) { fail_hr("scaled_point", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("scaled_point", s, 5);
    }
}

/* 81: points that bring their own sizes (D3DFVF_PSIZE on untransformed vertices): one of 16
 * pixels at the centre and one of 4 pixels at 32,32 in a single draw, while D3DRS_POINTSIZE says
 * 2. The vertex's size wins over the render state's, and each point has its own. */
static void vertex_point_case(int only, BOOL offered)
{
    static const SAMPLE s[] = { { 64, 64, 0xffff00 }, { 58, 58, 0xffff00 }, { 70, 70, 0xffff00 },
                                { 76, 64, CLEAR_COLOR }, { 64, 52, CLEAR_COLOR },
                                { 32, 32, 0xffff00 }, { 38, 32, CLEAR_COLOR }, { 32, 38, CLEAR_COLOR } };
    static const struct { float x, y, z, size; DWORD diffuse; } v[2] = {
        { 0.0f, 0.0f, 0.5f, 16.0f, 0xffffff00 }, { -0.5f, 0.5f, 0.5f, 4.0f, 0xffffff00 } };
    D3DMATRIX m;
    HRESULT hr;
    float f;
    if (only && only != 81) return;
    if (!offered) { printf("D9PIX points with their own sizes not offered\n"); return; }
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &m);
    IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (begin("vertex_point")) {
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        f = 2.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&f);
        IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 2, v, sizeof(v[0]));
        f = 1.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&f);
        if (FAILED(hr)) { fail_hr("vertex_point", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("vertex_point", s, 8);
    }
}

/* 93 to 95: the other vertex shaders that work point sizes out. 93: the scaled point of case 79
 * with lighting on (no lights, a yellow emissive material). 94: transformed vertices with their
 * own sizes, as case 81 (16 pixels at 64,64 and 4 pixels at 32,32). 95: the scaled point of
 * case 79 with a user clip plane that cuts nothing. */
static void point_variant_cases(int only, BOOL offered, BOOL planes)
{
    static const SAMPLE s[] = { { 64, 64, 0xffff00 }, { 58, 58, 0xffff00 }, { 70, 70, 0xffff00 },
                                { 76, 64, CLEAR_COLOR }, { 64, 52, CLEAR_COLOR },
                                { 32, 32, 0xffff00 }, { 38, 32, CLEAR_COLOR }, { 32, 38, CLEAR_COLOR } };
    static const struct { float x, y, z, nx, ny, nz; } lit = { 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, -1.0f };
    static const struct { float x, y, z, rhw, size; DWORD diffuse; } tl[2] = {
        { 64.0f, 64.0f, 0.5f, 1.0f, 16.0f, 0xffffff00 }, { 32.0f, 32.0f, 0.5f, 1.0f, 4.0f, 0xffffff00 } };
    static const XYZVERTEX v = { 0.0f, 0.0f, 0.5f, 0xffffff00 };
    static const float plane[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    D3DMATERIAL9 material;
    D3DMATRIX m;
    HRESULT hr;
    float f;
    int pass;
    if (only && (only < 93 || only > 95)) return;
    if (!offered) { printf("D9PIX point size variants not offered\n"); return; }
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    for (pass = 93; pass <= 95; ++pass) {
        const char *name = pass == 93 ? "lit_scaled_point" : pass == 94 ? "rhw_vertex_point" : "clip_scaled_point";
        if (only && only != pass) continue;
        if (pass == 95 && !planes) { printf("D9PIX clip planes for points not offered\n"); continue; }
        IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &m);
        IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &m);
        IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m);
        IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, pass == 93);
        if (!begin(name)) continue;
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        f = pass == 94 ? 2.0f : 0.0625f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&f);
        if (pass != 94) {
            IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALEENABLE, TRUE);
            f = 0.0f;
            IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_A, *(DWORD *)&f);
            IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_B, *(DWORD *)&f);
            f = 1.0f;
            IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_C, *(DWORD *)&f);
        }
        if (pass == 93) {
            memset(&material, 0, sizeof(material));
            material.Emissive.r = material.Emissive.g = material.Emissive.a = material.Diffuse.a = 1.0f;
            IDirect3DDevice9_SetMaterial(device, &material);
            IDirect3DDevice9_SetRenderState(device, D3DRS_AMBIENT, 0);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_NORMAL);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1, &lit, sizeof(lit));
        } else if (pass == 94) {
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 2, tl, sizeof(tl[0]));
        } else {
            IDirect3DDevice9_SetClipPlane(device, 0, plane);
            IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 1);
            IDirect3DDevice9_SetFVF(device, XYZFVF);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1, &v, sizeof(v));
            IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0);
        }
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALEENABLE, FALSE);
        f = 1.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_A, *(DWORD *)&f);
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE, *(DWORD *)&f);
        f = 0.0f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSCALE_C, *(DWORD *)&f);
        IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
        if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check(name, s, pass == 94 ? 8 : 5);
    }
}

/* 96: depth bias (D3DRS_DEPTHBIAS, a float added to a polygon's depth in units of the depth
 * range). A red quad; then a green one in the same plane with a bias of -0.0005, which the depth
 * test LESS would refuse everywhere without it; then a blue one a little nearer without bias,
 * which must still cover the green: the bias is small beside any real distance. */
static void depth_bias_case(int only, BOOL offered)
{
    static const SAMPLE s[] = { { 96, 64, 0x0000ff }, { 60, 64, 0x00ff00 }, { 28, 64, 0xff0000 }, { 8, 8, CLEAR_COLOR } };
    IDirect3DSurface9 *depth = NULL, *oldDepth = NULL;
    TLVERTEX v[6];
    HRESULT hr;
    float f;
    int i;
    if (only && only != 96) return;
    if (!offered) { printf("D9PIX depth bias not offered\n"); return; }
    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, Side, Side, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, NULL);
    if (FAILED(hr)) { fail_hr("depth_bias", "create", hr); return; }
    IDirect3DDevice9_GetDepthStencilSurface(device, &oldDepth);
    hr = IDirect3DDevice9_SetDepthStencilSurface(device, depth);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    if (FAILED(hr)) fail_hr("depth_bias", "depth", hr);
    else if (begin("depth_bias")) {
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESS);
        IDirect3DDevice9_SetFVF(device, TLFVF);
        quad(v, 16, 16, 112, 112, 0xffff0000);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
        f = -0.0005f;
        IDirect3DDevice9_SetRenderState(device, D3DRS_DEPTHBIAS, *(DWORD *)&f);
        quad(v, 44, 16, 112, 112, 0xff00ff00);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
        IDirect3DDevice9_SetRenderState(device, D3DRS_DEPTHBIAS, 0);
        quad(v, 80, 16, 112, 112, 0xff0000ff);
        for (i = 0; i < 6; ++i) v[i].z = 0.45f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
        if (FAILED(hr)) { fail_hr("depth_bias", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("depth_bias", s, 4);
    }
    IDirect3DDevice9_SetDepthStencilSurface(device, oldDepth);
    if (oldDepth) IDirect3DSurface9_Release(oldDepth);
    IDirect3DSurface9_Release(depth);
}

/* 97: blend operations (D3DRS_BLENDOP): over a quad of (c0, 40, 20) four strips of grey
 * (80, 80, 80) with both blend factors ONE: source minus destination, destination minus source,
 * the minimum and the maximum of the two. */
static void blend_op_case(int only, BOOL offered)
{
    static const SAMPLE s[] = { { 28, 64, 0x004060 }, { 52, 64, 0x400000 }, { 76, 64, 0x804020 }, { 100, 64, 0xc08080 },
                                { 64, 12, 0xc04020 }, { 4, 4, CLEAR_COLOR } };
    static const DWORD op[4] = { D3DBLENDOP_SUBTRACT, D3DBLENDOP_REVSUBTRACT, D3DBLENDOP_MIN, D3DBLENDOP_MAX };
    TLVERTEX v[6];
    HRESULT hr;
    int i;
    if (only && only != 97) return;
    if (!offered) { printf("D9PIX blend operations not offered\n"); return; }
    if (!begin("blend_op")) return;
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetFVF(device, TLFVF);
    quad(v, 8, 8, 120, 120, 0xffc04020);
    hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
    IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ONE);
    for (i = 0; i < 4 && SUCCEEDED(hr); ++i) {
        IDirect3DDevice9_SetRenderState(device, D3DRS_BLENDOP, op[i]);
        quad(v, 16.0f + 24.0f * i, 24, 40.0f + 24.0f * i, 104, 0xff808080);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
    }
    IDirect3DDevice9_SetRenderState(device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (FAILED(hr)) { fail_hr("blend_op", "draw", hr); IDirect3DDevice9_EndScene(device); }
    else check("blend_op", s, 6);
}

/* 80: the fixed-function pipeline through a declaration with two streams (no vertex shader):
 * positions in stream 0, colours in stream 1, six unused vertices first in both, drawn from
 * vertex 6: a red quad on the left and a green one on the right. */
static void ff_streams_case(int only, BOOL hardware)
{
    static const D3DVERTEXELEMENT9 split[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
        { 1, 0, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    static const SAMPLE s[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR },
                                { 120, 120, CLEAR_COLOR } };
    static const float quad[3][4] = { { 1, 1, 1, 1 }, { -0.5f, 0.5f, 0, -0.5f }, { 0, 0.5f, 0.5f, -0.5f } };
    static const DWORD colour[3] = { 0xff0000ff, 0xffff0000, 0xff00ff00 };
    float pos[18][3];
    DWORD col[18];
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexBuffer9 *pvb = NULL, *cvb = NULL;
    D3DCAPS9 dc;
    D3DMATRIX m;
    HRESULT hr;
    int k, q;
    if (only && only != 80 && only != 92) return;
    IDirect3DDevice9_GetDeviceCaps(device, &dc);
    if (dc.MaxStreams < 2) { printf("D9PIX fixed-function streams not offered (%lu)\n", (unsigned long)dc.MaxStreams); return; }
    for (q = 0; q < 3; ++q) {           /* x0, y0, x1, y1: two triangles each */
        static const int cx[6] = { 0, 2, 0, 0, 2, 2 }, cy[6] = { 1, 1, 3, 3, 1, 3 };
        for (k = 0; k < 6; ++k) {
            pos[q * 6 + k][0] = quad[q][cx[k]];
            pos[q * 6 + k][1] = quad[q][cy[k]];
            pos[q * 6 + k][2] = 0.5f;
            col[q * 6 + k] = colour[q];
        }
    }
    pvb = vertex_buffer(pos, sizeof(pos), 0);
    cvb = vertex_buffer(col, sizeof(col), 0);
    hr = pvb && cvb ? IDirect3DDevice9_CreateVertexDeclaration(device, split, &decl) : E_FAIL;
    if (only == 92) {}
    else if (FAILED(hr)) fail_hr("ff_streams", "create", hr);
    else {
        memset(&m, 0, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &m);
        IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &m);
        IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m);
        IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
        if (begin("ff_streams")) {
            IDirect3DDevice9_SetTexture(device, 0, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            IDirect3DDevice9_SetVertexShader(device, NULL);
            IDirect3DDevice9_SetVertexDeclaration(device, decl);
            IDirect3DDevice9_SetStreamSource(device, 0, pvb, 0, sizeof(pos[0]));
            IDirect3DDevice9_SetStreamSource(device, 1, cvb, 0, sizeof(col[0]));
            hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 6, 4);
            IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
            IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0);
            if (FAILED(hr)) { fail_hr("ff_streams", "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check("ff_streams", s, 4);
        }
    }
    /* 92: the same through an index buffer, with each colour 4 bytes into the vertices of the
     * second stream: base vertex 6 and indices 6 to 11 are the green quad alone. Hardware vertex
     * processing only: with software vertex processing the runtime refuses this draw itself
     * (D3DERR_INVALIDCALL), which was not looked into. */
    if (!hardware) printf("D9PIX indexed fixed-function streams not offered\n");
    else if (!only || only == 92) {
        static const D3DVERTEXELEMENT9 padded[] = {
            { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
            { 1, 4, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
        static const SAMPLE si[] = { { 80, 64, 0x00ff00 }, { 48, 64, CLEAR_COLOR }, { 8, 8, CLEAR_COLOR },
                                     { 120, 120, CLEAR_COLOR } };
        static const WORD idx[6] = { 6, 7, 8, 9, 10, 11 };
        DWORD wide[18][2];
        IDirect3DVertexDeclaration9 *padDecl = NULL;
        IDirect3DVertexBuffer9 *wvb;
        IDirect3DIndexBuffer9 *ib = NULL;
        void *to;
        for (k = 0; k < 18; ++k) { wide[k][0] = 0xff0000ff; wide[k][1] = col[k]; }
        wvb = vertex_buffer(wide, sizeof(wide), 0);
        hr = pvb && wvb ? IDirect3DDevice9_CreateVertexDeclaration(device, padded, &padDecl) : E_FAIL;
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_CreateIndexBuffer(device, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                    D3DPOOL_DEFAULT, &ib, NULL);
        if (SUCCEEDED(hr) && SUCCEEDED(hr = IDirect3DIndexBuffer9_Lock(ib, 0, 0, &to, 0))) {
            memcpy(to, idx, sizeof(idx));
            IDirect3DIndexBuffer9_Unlock(ib);
        }
        if (FAILED(hr)) fail_hr("ff_streams_indexed", "create", hr);
        else {
            memset(&m, 0, sizeof(m));
            m._11 = m._22 = m._33 = m._44 = 1.0f;
            IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &m);
            IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &m);
            IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m);
            IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            if (begin("ff_streams_indexed")) {
                IDirect3DDevice9_SetTexture(device, 0, NULL);
                IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                IDirect3DDevice9_SetVertexShader(device, NULL);
                IDirect3DDevice9_SetVertexDeclaration(device, padDecl);
                IDirect3DDevice9_SetStreamSource(device, 0, pvb, 0, sizeof(pos[0]));
                IDirect3DDevice9_SetStreamSource(device, 1, wvb, 0, sizeof(wide[0]));
                IDirect3DDevice9_SetIndices(device, ib);
                hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 6, 6, 6, 0, 2);
                IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
                IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0);
                IDirect3DDevice9_SetIndices(device, NULL);
                if (FAILED(hr)) { fail_hr("ff_streams_indexed", "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check("ff_streams_indexed", si, 4);
            }
        }
        if (padDecl) IDirect3DVertexDeclaration9_Release(padDecl);
        if (wvb) IDirect3DVertexBuffer9_Release(wvb);
        if (ib) IDirect3DIndexBuffer9_Release(ib);
    }
    if (decl) IDirect3DVertexDeclaration9_Release(decl);
    if (pvb) IDirect3DVertexBuffer9_Release(pvb);
    if (cvb) IDirect3DVertexBuffer9_Release(cvb);
}

/* 73: a line list (red lines at y 38 to 41 from x 8 to 120) and a point list (a 2x2 block of green
 * points at 95 and 96): pixels on them, and the clear colour beside. Then an indexed line list
 * (blue lines at y 86 to 89, the indices reversed in each line). The counts (8, 4 and 8
 * vertices) are not multiples of three. */
static void lines_points_case(int only)
{
    static const SAMPLE s[] = { { 64, 40, 0xff0000 }, { 64, 64, CLEAR_COLOR }, { 96, 96, 0x00ff00 }, { 104, 104, CLEAR_COLOR },
                                { 64, 88, 0x0000ff } };
    static const WORD order[8] = { 1, 0, 3, 2, 5, 4, 7, 6 };
    TLVERTEX lines[8], points[4], blue[8];
    HRESULT hr;
    int i;
    if (only && only != 73) return;
    for (i = 0; i < 8; ++i) {
        lines[i].x = i & 1 ? 120.0f : 8.0f; lines[i].y = 38.0f + i / 2; lines[i].z = 0.5f; lines[i].rhw = 1;
        lines[i].diffuse = 0xffff0000;
        blue[i] = lines[i]; blue[i].y += 48.0f; blue[i].diffuse = 0xff0000ff;
    }
    for (i = 0; i < 4; ++i) {
        points[i].x = 95.0f + i % 2; points[i].y = 95.0f + i / 2; points[i].z = 0.5f; points[i].rhw = 1;
        points[i].diffuse = 0xff00ff00;
    }
    if (!begin("lines_points")) return;
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_LINELIST, 4, lines, sizeof(lines[0]));
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 4, points, sizeof(points[0]));
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_DrawIndexedPrimitiveUP(device, D3DPT_LINELIST, 0, 8, 4, order, D3DFMT_INDEX16, blue, sizeof(blue[0]));
    if (FAILED(hr)) { fail_hr("lines_points", "draw", hr); IDirect3DDevice9_EndScene(device); }
    else check("lines_points", s, 5);
}

/* 72: ps_1_4. texcrd r1.rgb, t1 (0.75, 0.25 at every vertex); phase; texld r0, t0 (sampler 0: red
 * left, green right); texld r3, r1 (sampler 3, a dependent read: blue at the top right texel);
 * add r0, r0, r3: magenta on the left, cyan on the right. (Registers written before the phase
 * lose their alpha.) */
static void ps14_case(int only)
{
    static const DWORD halves[4] = { 0xffff0000, 0xff00ff00, 0xffff0000, 0xff00ff00 };
    static const DWORD corner[4] = { 0xff000000, 0xff0000ff, 0xff000000, 0xff000000 };
    static const DWORD ps[] = { PSV(4), OTexcoord, PD(RR, 1, 7, 0, 0), PS_(RT, 1, 0xa4, MNone), 0x0000fffdUL,
        OTex, DST(RR, 0), SRC(RT, 0), OTex, DST(RR, 3), SRC(RR, 1), OAdd, DST(RR, 0), SRC(RR, 0), SRC(RR, 3), PSEND };
    static const SAMPLE s[] = { { 48, 64, 0xff00ff }, { 80, 64, 0x00ffff }, { 8, 8, CLEAR_COLOR } };
    IDirect3DTexture9 *t0, *t3;
    PSVERTEX v[6];
    int i, k;
    if (only && only != 72) return;
    t0 = texture2x2(halves);
    t3 = texture2x2(corner);
    if (!t0 || !t3) { printf("D9PIX case=ps14 textures failed\n"); goto done; }
    ps_quad(v, 32, 32, 96, 96, 0xffffffff, 0, 0, 1, 1);
    for (i = 0; i < 6; ++i) { v[i].u1 = 0.75f; v[i].v1 = 0.25f; }
    for (k = 0; k < 4; ++k) {
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t0);
    IDirect3DDevice9_SetTexture(device, 3, (IDirect3DBaseTexture9 *)t3);
    ps_run("ps14", ps, v, 1, s, 3);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTexture(device, 3, NULL);
done:
    if (t0) IDirect3DTexture9_Release(t0);
    if (t3) IDirect3DTexture9_Release(t3);
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

/* Set for a second pass over the vertex shader cases: vs_run then draws from a vertex buffer
 * (with eight unused bytes before the vertices) instead of the program's memory, and the case
 * names end in "_vb". A driver that keeps vertex buffers on the GPU reads them there. */
static BOOL vsFromBuffer;
static UINT vsLead;     /* the unused bytes: 8 where stream offsets are offered, else none */

static void vs_run(const char *name, const D3DVERTEXELEMENT9 *elements, const DWORD *vs, const DWORD *ps,
                   const void *vertices, UINT stride, UINT triangles, const SAMPLE *s, int samples)
{
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexShader9 *vsh = NULL;
    IDirect3DPixelShader9 *psh = NULL;
    IDirect3DVertexBuffer9 *vb = NULL;
    char named[64];
    HRESULT hr = IDirect3DDevice9_CreateVertexDeclaration(device, elements, &decl);
    if (vsFromBuffer) {
        BYTE *p;
        sprintf(named, "%.50s_vb", name);
        name = named;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateVertexBuffer(device, vsLead + triangles * 3 * stride, D3DUSAGE_WRITEONLY, 0,
                                                                    D3DPOOL_DEFAULT, &vb, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&p, 0);
        if (SUCCEEDED(hr)) {
            memset(p, 0xee, vsLead);
            memcpy(p + vsLead, vertices, triangles * 3 * stride);
            IDirect3DVertexBuffer9_Unlock(vb);
        }
    }
    if (FAILED(hr)) { fail_hr(name, "declaration", hr); if (vb) IDirect3DVertexBuffer9_Release(vb); if (decl) IDirect3DVertexDeclaration9_Release(decl); return; }
    hr = IDirect3DDevice9_CreateVertexShader(device, vs, &vsh);
    if (FAILED(hr)) fail_hr(name, "vertex shader", hr);
    else {
        hr = ps ? IDirect3DDevice9_CreatePixelShader(device, ps, &psh) : D3D_OK;
        if (FAILED(hr)) fail_hr(name, "pixel shader", hr);
        else if (begin(name)) {
            IDirect3DDevice9_SetVertexDeclaration(device, decl);
            IDirect3DDevice9_SetVertexShader(device, vsh);
            IDirect3DDevice9_SetPixelShader(device, psh);
            if (vb) {
                IDirect3DDevice9_SetStreamSource(device, 0, vb, vsLead, stride);
                hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, triangles);
                IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
            } else hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, triangles, vertices, stride);
            IDirect3DDevice9_SetVertexShader(device, NULL);
            IDirect3DDevice9_SetPixelShader(device, NULL);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check(name, s, samples);
        }
    }
    if (psh) IDirect3DPixelShader9_Release(psh);
    if (vsh) IDirect3DVertexShader9_Release(vsh);
    if (vb) IDirect3DVertexBuffer9_Release(vb);
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
    if (!only || only == 110) {
        /* vs_2_0's sincos: the angle is the vertex colour's red times c4.x plus c4.y (-3 to 3),
         * the colour drawn is (cos / 2 + 0.5, sin / 2 + 0.5, 0). c5 and c6 are the constants
         * the instruction takes in vs_2_0 (D3DSINCOSCONST1 and 2).
         *   mad r1.x, v1.x, c4.x, c4.y;  sincos r0.xy, r1.x, c5, c6;  mad r0.xy, r0, c7.x, c7.x;
         *   mov r0.zw, c7.yyyz;  mov oD0, r0 */
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
            P2I(ODp4, 3), P2D(VPos, 0, 1), P2SRC(RV, 0), P2SRC(RC, 0),
            P2I(ODp4, 3), P2D(VPos, 0, 2), P2SRC(RV, 0), P2SRC(RC, 1),
            P2I(ODp4, 3), P2D(VPos, 0, 4), P2SRC(RV, 0), P2SRC(RC, 2),
            P2I(ODp4, 3), P2D(VPos, 0, 8), P2SRC(RV, 0), P2SRC(RC, 3),
            P2I(OMad, 4), P2D(RR, 1, 1), P2S(RV, 1, 0x00, MNone), P2S(RC, 4, 0x00, MNone), P2S(RC, 4, 0x55, MNone),
            P2I(37, 4), P2D(RR, 0, 3), P2S(RR, 1, 0x00, MNone), P2SRC(RC, 5), P2SRC(RC, 6),
            P2I(OMad, 4), P2D(RR, 0, 3), P2SRC(RR, 0), P2S(RC, 7, 0x00, MNone), P2S(RC, 7, 0x00, MNone),
            P2I(OMov, 2), P2D(RR, 0, 12), P2S(RC, 7, 0x95, MNone),
            P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RR, 0), PSEND };
        static const float m[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2 };
        static const float k[16] = { 6, -3, 0, 0,
                                     -1.5500992e-006f, -2.1701389e-005f, 0.0026041667f, 0.00026041668f,
                                     -0.020833334f, -0.12500000f, 1.0f, 0.50000000f,
                                     0.5f, 0, 1, 0 };
        /* red 255: 3 radians; 0: -3; 128: 0.012; 191: 1.494 */
        static const SAMPLE s[] = { { 48, 48, 0x019100 }, { 80, 48, 0x016e00 }, { 48, 80, 0xff8100 },
                                    { 80, 80, 0x89ff00 }, { 8, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR } };
        vs_quad(v, -0.5f, 0.5f, 0, 0, 0xffff0000);
        vs_quad(v + 6, 0, 0.5f, 0.5f, 0, 0xff000000);
        vs_quad(v + 12, -0.5f, 0, 0, -0.5f, 0xff800000);
        vs_quad(v + 18, 0, 0, 0.5f, -0.5f, 0xffbf0000);
        IDirect3DDevice9_SetVertexShaderConstantF(device, 0, m, 4);
        IDirect3DDevice9_SetVertexShaderConstantF(device, 4, k, 4);
        vs_run("vs2_sincos", colored, vs, ps_color, v, sizeof(v[0]), 8, s, 6);
    }
    if (!only || only == 38) {
        /* def statements before the dcl statements, as a compiled HLSL shader has them:
         * oPos = v0 + c5 (zero), oD0 = v1 * c6 (one). A driver that looks for the dcl statements
         * only at the start reads no vertex data, and nothing is drawn. */
        static const DWORD vs[] = { 0xfffe0200,
            P2I(ODef, 5), P2D(RC, 5, 15), 0, 0, 0, 0,
            P2I(ODef, 5), P2D(RC, 6, 15), 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
            VDCL2(0, 0), VDCL2(10, 1),
            P2I(OAdd, 3), P2D(VPos, 0, 15), P2SRC(RV, 0), P2SRC(RC, 5),
            P2I(OMul, 3), P2D(VAttr, 0, 15), P2SRC(RV, 1), P2SRC(RC, 6), PSEND };
        static const SAMPLE s[] = { { 64, 64, 0xffff00 }, { 40, 40, 0xffff00 }, { 8, 8, CLEAR_COLOR } };
        vs_quad(v, -0.5f, 0.5f, 0.5f, -0.5f, 0xffffff00);
        vs_run("vs2_def_first", colored, vs, ps_color, v, sizeof(v[0]), 2, s, 3);
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
    if ((!only || only == 33) && !vsFromBuffer) {   /* draws from vertex buffers in both passes */
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
    if ((!only || only == 35 || only == 36) && !vsFromBuffer) {
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

/* ---- vertex and index buffers through a vertex shader (cases 84 to 89) ----
 * What a driver that keeps the buffers on the GPU has to get right: a buffer rewritten between
 * two draws of one scene (each draw shows what the buffer held when it was drawn), vertices
 * appended without overwriting, indices rewritten, a base vertex (also a negative one) with a
 * start index, strips and fans, and a stream offset. */
static void four_corners(VSVERTEX *v, float x0, float y0, float x1, float y1, DWORD color, BOOL fan)
{
    const float xs[4] = { x0, x1, fan ? x1 : x0, fan ? x0 : x1 }, ys[4] = { y0, y0, y1, y1 };
    int i;
    for (i = 0; i < 4; ++i) { v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 0.5f; v[i].color = color; }
}

static void buffer_cases(int only, BOOL streamOffset)
{
    static const D3DVERTEXELEMENT9 colored[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(10, 1),
        P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RV, 1), PSEND };
    static const DWORD ps_color[] = { 0xffff0200, P2DCL_V(0), P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RV, 0), PSEND };
    /* Left and right halves of the middle, and the corners that stay clear. */
    static const SAMPLE two[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR }, { 120, 120, CLEAR_COLOR } };
    static const WORD list[6] = { 0, 1, 2, 2, 1, 3 };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexShader9 *vsh = NULL;
    IDirect3DPixelShader9 *psh = NULL;
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    VSVERTEX v[18], *p;
    WORD *x;
    DWORD cull = D3DCULL_CCW;
    HRESULT hr = IDirect3DDevice9_CreateVertexDeclaration(device, colored, &decl);
    int pass, k;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateVertexShader(device, vs, &vsh);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreatePixelShader(device, ps_color, &psh);
    if (FAILED(hr)) { fail_hr("buffers", "create", hr); goto done; }
    IDirect3DDevice9_GetRenderState(device, D3DRS_CULLMODE, &cull);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    for (pass = 84; pass <= 89; ++pass) {
        static const char *const names[6] = { "vb_rewrite", "vb_append", "ib_rewrite", "vb_base", "vb_strip_fan", "vb_offset" };
        const char *name = names[pass - 84];
        if (only && only != pass) continue;
        if (pass == 89 && !streamOffset) { printf("D9PIX stream offsets not offered\n"); continue; }
        vb = NULL; ib = NULL;
        hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(v), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL);
        if (SUCCEEDED(hr) && (pass == 86 || pass == 87))
            hr = IDirect3DDevice9_CreateIndexBuffer(device, 12 * sizeof(WORD), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                    D3DPOOL_DEFAULT, &ib, NULL);
        if (FAILED(hr)) { fail_hr(name, "buffer", hr); goto next; }
        if (!begin(name)) goto next;
        IDirect3DDevice9_SetVertexDeclaration(device, decl);
        IDirect3DDevice9_SetVertexShader(device, vsh);
        IDirect3DDevice9_SetPixelShader(device, psh);
        IDirect3DDevice9_SetStreamSource(device, 0, vb, pass == 89 ? 6 * sizeof(v[0]) : 0, sizeof(v[0]));
        if (ib) IDirect3DDevice9_SetIndices(device, ib);
        switch (pass) {
        case 84:
            /* The same six vertices twice: a red quad on the left, then a green one on the right. */
            for (k = 0; k < 2 && SUCCEEDED(hr); ++k) {
                hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&p, D3DLOCK_DISCARD);
                if (FAILED(hr)) break;
                if (k) vs_quad(p, 0, 0.5f, 0.5f, -0.5f, 0xff00ff00); else vs_quad(p, -0.5f, 0.5f, 0, -0.5f, 0xffff0000);
                IDirect3DVertexBuffer9_Unlock(vb);
                hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 2);
            }
            break;
        case 85:
            /* Six vertices, drawn; six more after them without overwriting, drawn from vertex 6. */
            for (k = 0; k < 2 && SUCCEEDED(hr); ++k) {
                hr = IDirect3DVertexBuffer9_Lock(vb, k * 6 * sizeof(v[0]), 6 * sizeof(v[0]), (void **)&p, k ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD);
                if (FAILED(hr)) break;
                if (k) vs_quad(p, 0, 0.5f, 0.5f, -0.5f, 0xff00ff00); else vs_quad(p, -0.5f, 0.5f, 0, -0.5f, 0xffff0000);
                IDirect3DVertexBuffer9_Unlock(vb);
                hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, k * 6, 2);
            }
            break;
        case 86:
            /* Eight corners that stay; the six indices name the left quad, then the right one. */
            four_corners(v, -0.5f, 0.5f, 0, -0.5f, 0xffff0000, FALSE);
            four_corners(v + 4, 0, 0.5f, 0.5f, -0.5f, 0xff00ff00, FALSE);
            hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&p, D3DLOCK_DISCARD);
            if (FAILED(hr)) break;
            memcpy(p, v, 8 * sizeof(v[0]));
            IDirect3DVertexBuffer9_Unlock(vb);
            for (k = 0; k < 2 && SUCCEEDED(hr); ++k) {
                int i;
                hr = IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&x, D3DLOCK_DISCARD);
                if (FAILED(hr)) break;
                for (i = 0; i < 6; ++i) x[i] = (WORD)(list[i] + 4 * k);
                IDirect3DIndexBuffer9_Unlock(ib);
                hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, 4 * k, 4, 0, 2);
            }
            break;
        case 87:
            /* Corners 4 to 7 are the left quad and 8 to 11 the right one. The left is drawn with
             * base vertex 4 from index 6 on (indices 0 to 3); the right with base vertex -4 from
             * index 0 on (indices 12 to 15). */
            four_corners(v, 1, 1, 1, 1, 0xff0000ff, FALSE);
            four_corners(v + 4, -0.5f, 0.5f, 0, -0.5f, 0xffff0000, FALSE);
            four_corners(v + 8, 0, 0.5f, 0.5f, -0.5f, 0xff00ff00, FALSE);
            hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&p, D3DLOCK_DISCARD);
            if (FAILED(hr)) break;
            memcpy(p, v, 12 * sizeof(v[0]));
            IDirect3DVertexBuffer9_Unlock(vb);
            hr = IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&x, D3DLOCK_DISCARD);
            if (FAILED(hr)) break;
            for (k = 0; k < 6; ++k) { x[k] = (WORD)(list[k] + 12); x[6 + k] = list[k]; }
            IDirect3DIndexBuffer9_Unlock(ib);
            hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 4, 0, 4, 6, 2);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, -4, 12, 4, 0, 2);
            break;
        case 88:
            /* The left quad as a strip of four corners, the right one as a fan. */
            four_corners(v, -0.5f, 0.5f, 0, -0.5f, 0xffff0000, FALSE);
            four_corners(v + 4, 0, 0.5f, 0.5f, -0.5f, 0xff00ff00, TRUE);
            hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&p, D3DLOCK_DISCARD);
            if (FAILED(hr)) break;
            memcpy(p, v, 8 * sizeof(v[0]));
            IDirect3DVertexBuffer9_Unlock(vb);
            hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, 0, 2);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLEFAN, 4, 2);
            break;
        case 89:
            /* The stream starts six vertices into the buffer: both quads follow them. */
            vs_quad(v, 1, 1, 1, 1, 0xff0000ff);
            hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&p, D3DLOCK_DISCARD);
            if (FAILED(hr)) break;
            memcpy(p, v, 6 * sizeof(v[0]));
            vs_quad(p + 6, -0.5f, 0.5f, 0, -0.5f, 0xffff0000);
            vs_quad(p + 12, 0, 0.5f, 0.5f, -0.5f, 0xff00ff00);
            IDirect3DVertexBuffer9_Unlock(vb);
            hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 4);
            break;
        }
        IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
        IDirect3DDevice9_SetIndices(device, NULL);
        IDirect3DDevice9_SetVertexShader(device, NULL);
        IDirect3DDevice9_SetPixelShader(device, NULL);
        if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check(name, two, 4);
    next:
        if (ib) IDirect3DIndexBuffer9_Release(ib);
        if (vb) IDirect3DVertexBuffer9_Release(vb);
    }
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, cull);
done:
    if (psh) IDirect3DPixelShader9_Release(psh);
    if (vsh) IDirect3DVertexShader9_Release(vsh);
    if (decl) IDirect3DVertexDeclaration9_Release(decl);
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
    /* 90: the same six quads through the fixed-function stage alone (no pixel shader): the stage
     * program samples the cube map. 91: the stage after a vertex shader that passes the three
     * coordinates on, as 3DMark03 draws the reflections on its planes. */
    for (pass = 90; pass <= 91; ++pass) {
        static const D3DVERTEXELEMENT9 elements[] = {
            { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
        static const DWORD vs[] = { 0xfffe0200, VDCL2(0, 0), VDCL2(5, 1),
            P2I(OMov, 2), P2D(VPos, 0, 15), P2SRC(RV, 0), P2I(OMov, 2), P2D(VAttr, 0, 15), P2SRC(RC, 0),
            P2I(OMov, 2), P2D(VTex, 0, 15), P2SRC(RV, 1), PSEND };
        static const float white[4] = { 1, 1, 1, 1 };
        typedef struct { float x, y, z, u, v, w; } VV;
        const char *name = pass == 90 ? "ff_cube" : "vs_ff_cube";
        IDirect3DVertexDeclaration9 *decl = NULL;
        IDirect3DVertexShader9 *vsh = NULL;
        VV vv[36];
        HRESULT hr = D3D_OK;
        if (only && only != pass) continue;
        if (pass == 91) {
            if (!version2) continue;
            for (i = 0; i < 36; ++i) {
                vv[i].x = v[i].x / 64.0f - 1.0f; vv[i].y = 1.0f - v[i].y / 64.0f; vv[i].z = 0.5f;
                vv[i].u = v[i].u; vv[i].v = v[i].v; vv[i].w = v[i].w;
            }
            hr = IDirect3DDevice9_CreateVertexDeclaration(device, elements, &decl);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateVertexShader(device, vs, &vsh);
            if (FAILED(hr)) { fail_hr(name, "create", hr); if (decl) IDirect3DVertexDeclaration9_Release(decl); continue; }
        }
        if (begin(name)) {
            IDirect3DDevice9_SetPixelShader(device, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, 0);
            if (pass == 90) {
                IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 12, v, sizeof(v[0]));
            } else {
                DWORD cull = D3DCULL_CCW;
                IDirect3DDevice9_GetRenderState(device, D3DRS_CULLMODE, &cull);
                IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
                IDirect3DDevice9_SetVertexDeclaration(device, decl);
                IDirect3DDevice9_SetVertexShader(device, vsh);
                IDirect3DDevice9_SetVertexShaderConstantF(device, 0, white, 1);
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 12, vv, sizeof(vv[0]));
                IDirect3DDevice9_SetVertexShader(device, NULL);
                IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, cull);
            }
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            if (FAILED(hr)) { fail_hr(name, "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check(name, s, 7);
        }
        if (vsh) IDirect3DVertexShader9_Release(vsh);
        if (decl) IDirect3DVertexDeclaration9_Release(decl);
    }
    /* 71: a render target cube map (64x64), each face cleared to its colour as a render target, then
     * sampled the same way (3DMark05 draws its reflections into one). */
    if ((!only || only == 71) && version2) {
        IDirect3DCubeTexture9 *rt = NULL;
        IDirect3DPixelShader9 *handle = NULL;
        IDirect3DSurface9 *back = NULL;
        HRESULT hr = IDirect3DDevice9_CreateCubeTexture(device, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                        D3DPOOL_DEFAULT, &rt, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreatePixelShader(device, ps2, &handle);
        if (FAILED(hr)) fail_hr("cube_rt", "create", hr);
        else {
            IDirect3DDevice9_GetRenderTarget(device, 0, &back);
            for (face = 0; face < 6 && SUCCEEDED(hr); ++face) {
                IDirect3DSurface9 *f = NULL;
                hr = IDirect3DCubeTexture9_GetCubeMapSurface(rt, (D3DCUBEMAP_FACES)face, 0, &f);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderTarget(device, 0, f);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, colors[face], 1.0f, 0);
                if (f) IDirect3DSurface9_Release(f);
            }
            IDirect3DDevice9_SetRenderTarget(device, 0, back);
            if (FAILED(hr)) fail_hr("cube_rt", "face", hr);
            else if (begin("cube_rt")) {
                IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)rt);
                IDirect3DDevice9_SetPixelShader(device, handle);
                IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
                hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 12, v, sizeof(v[0]));
                IDirect3DDevice9_SetPixelShader(device, NULL);
                if (FAILED(hr)) { fail_hr("cube_rt", "draw", hr); IDirect3DDevice9_EndScene(device); }
                else check("cube_rt", s, 7);
            }
            if (back) IDirect3DSurface9_Release(back);
        }
        if (handle) IDirect3DPixelShader9_Release(handle);
        if (rt) { IDirect3DDevice9_SetTexture(device, 0, NULL); IDirect3DCubeTexture9_Release(rt); }
    }
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DCubeTexture9_Release(cube);
}

/* ---- luminance and alpha formats ----
 * 47: an L8 texture of 0x80 over the left half and an A8L8 one of 0x40
 * (alpha 0xff) over the right, by a fixed-function stage. */
/* 63: a 2x2 L16 texture, white on the left and half grey (0x8000) on the right; and a 128x128x128 L16
 * volume texture, which 3DMark03 makes for its procedural volumes. */
static void l16_case(int only)
{
    static const SAMPLE s[] = { { 48, 64, 0xffffff }, { 80, 64, 0x808080 }, { 8, 8, CLEAR_COLOR } };
    IDirect3DTexture9 *t = NULL;
    IDirect3DVolumeTexture9 *vol = NULL;
    D3DLOCKED_RECT r;
    TLTEXVERTEX v[4] = {
        { 32, 32, 0.5f, 1, 0xffffffff, 0, 0 }, { 96, 32, 0.5f, 1, 0xffffffff, 1, 0 },
        { 32, 96, 0.5f, 1, 0xffffffff, 0, 1 }, { 96, 96, 0.5f, 1, 0xffffffff, 1, 1 } };
    HRESULT hr;
    int y;
    if (only && only != 63) return;
    if (FAILED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_L16))) {
        printf("D9PIX L16 textures not offered\n");
        return;
    }
    hr = IDirect3DDevice9_CreateVolumeTexture(device, 128, 128, 128, 1, 0, D3DFMT_L16, D3DPOOL_MANAGED, &vol, NULL);
    printf("D9PIX case=l16 volume128 hr=%08lx\n", (unsigned long)hr);
    if (vol) IDirect3DVolumeTexture9_Release(vol);
    if (FAILED(hr = IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_L16, D3DPOOL_MANAGED, &t, NULL))) {
        fail_hr("l16", "create", hr);
        return;
    }
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &r, NULL, 0))) {
        for (y = 0; y < 2; ++y) { USHORT *row = (USHORT *)((BYTE *)r.pBits + y * r.Pitch); row[0] = 0xffff; row[1] = 0x8000; }
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    if (begin("l16")) {
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetFVF(device, TLTEXFVF);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr("l16", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("l16", s, 3);
    }
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DTexture9_Release(t);
}

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
/* 74: more texture memory than the driver keeps on the host at once: 24
 * textures of 1024x1024 ARGB (96 MB, past its 64 MB), each its own colour,
 * a quad each in a 6x4 grid. Two scenes, so the second draws textures the
 * first one's later textures pushed out. */
static void eviction_case(int only)
{
    enum { Count = 24, TexSide = 1024 };
    IDirect3DTexture9 *tex[Count];
    SAMPLE s[Count];
    int i, scene, made = 0;
    if (only && only != 74) return;
    for (i = 0; i < Count; ++i) {
        DWORD color = 0xff000000UL | (DWORD)(10 + 10 * i) << 16 | (DWORD)(250 - 10 * i) << 8 | (i & 1 ? 0x40 : 0xc0);
        D3DLOCKED_RECT r;
        int x, y;
        tex[i] = NULL;
        if (FAILED(IDirect3DDevice9_CreateTexture(device, TexSide, TexSide, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                  &tex[i], NULL))) break;
        ++made;
        if (FAILED(IDirect3DTexture9_LockRect(tex[i], 0, &r, NULL, 0))) break;
        for (y = 0; y < TexSide; ++y)
            for (x = 0; x < TexSide; ++x) ((DWORD *)((BYTE *)r.pBits + y * r.Pitch))[x] = color;
        IDirect3DTexture9_UnlockRect(tex[i], 0);
        s[i].x = 8 + 20 * (i % 6);
        s[i].y = 8 + 20 * (i / 6);
        s[i].rgb = color;
    }
    if (made < Count || i < Count) fail_hr("eviction", "create", E_OUTOFMEMORY);
    /* The third scene: the sixteen textures the host still holds (the last sixteen drawn) and then
     * the first one, which needs the room of the one drawn first in this scene. That one is freed
     * while its draw has not gone to the host yet; a driver that frees it there too early loses
     * the draws that follow it. The eight textures not drawn leave the clear colour. */
    else for (scene = 0; scene < 3; ++scene) {
        HRESULT hr = S_OK;
        if (!begin("eviction")) break;
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetFVF(device, TLTEXFVF);
        for (i = 0; SUCCEEDED(hr) && i < (scene == 2 ? 17 : Count); ++i) {
            int t = scene == 2 ? (i < 16 ? Count - 16 + i : 0) : i;
            float x0 = 2.0f + 20 * (t % 6), y0 = 2.0f + 20 * (t / 6);
            TLTEXVERTEX v[4] = {
                { x0, y0, 0.5f, 1, 0xffffffff, 0, 0 }, { x0 + 14, y0, 0.5f, 1, 0xffffffff, 1, 0 },
                { x0, y0 + 14, 0.5f, 1, 0xffffffff, 0, 1 }, { x0 + 14, y0 + 14, 0.5f, 1, 0xffffffff, 1, 1 } };
            IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)tex[t]);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        }
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        if (FAILED(hr)) { fail_hr("eviction", "draw", hr); IDirect3DDevice9_EndScene(device); break; }
        if (scene == 2) {
            SAMPLE in[Count];
            for (i = 0; i < Count; ++i) { in[i] = s[i]; if (i >= 1 && i < Count - 16) in[i].rgb = CLEAR_COLOR; }
            check("eviction_in_scene", in, Count);
        } else check(scene ? "eviction_again" : "eviction", s, Count);
    }
    for (i = 0; i < made; ++i) IDirect3DTexture9_Release(tex[i]);
}

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

/* ---- large textures ----
 * 55: a 2048x2048 texture, red on its left half and green on its right, drawn
 * over the quad 32..96 (3DMark 2001 SE creates 2048x2048 textures). */
static void large_texture_case(int only, DWORD side)
{
    static const SAMPLE s[] = { { 48, 64, 0xff0000 }, { 80, 64, 0x00ff00 }, { 8, 8, CLEAR_COLOR } };
    TLTEXVERTEX v[4] = {
        { 32, 32, 0.5f, 1, 0xffffffff, 0, 0 }, { 96, 32, 0.5f, 1, 0xffffffff, 1, 0 },
        { 32, 96, 0.5f, 1, 0xffffffff, 0, 1 }, { 96, 96, 0.5f, 1, 0xffffffff, 1, 1 } };
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT r;
    HRESULT hr;
    int x, y;
    if (only && only != 55) return;
    if (side < 2048) { printf("D9PIX textures of 2048 not offered (%lu)\n", (unsigned long)side); return; }
    hr = IDirect3DDevice9_CreateTexture(device, 2048, 2048, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &t, NULL);
    if (FAILED(hr)) { fail_hr("texture2048", "create", hr); return; }
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &r, NULL, 0))) {
        for (y = 0; y < 2048; ++y) {
            DWORD *row = (DWORD *)((BYTE *)r.pBits + y * r.Pitch);
            for (x = 0; x < 2048; ++x) row[x] = x < 1024 ? 0xffff0000 : 0xff00ff00;
        }
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    if (begin("texture2048")) {
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetFVF(device, TLTEXFVF);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        if (FAILED(hr)) { fail_hr("texture2048", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("texture2048", s, 3);
    }
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DTexture9_Release(t);
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
/* 70: a 2048x2048 render target texture with a depth buffer of its size (3DMark05's shadow maps),
 * cleared green, then drawn over the back buffer. */
static void rt2048_case(int only)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } TV;
    static const SAMPLE s[] = { { 32, 64, 0x00ff00 }, { 96, 64, 0x00ff00 } };
    TV v[4] = { { 0, 0, 0.5f, 1, 0xffffffff, 0, 0 }, { Side, 0, 0.5f, 1, 0xffffffff, 1, 0 },
                { 0, Side, 0.5f, 1, 0xffffffff, 0, 1 }, { Side, Side, 0.5f, 1, 0xffffffff, 1, 1 } };
    IDirect3DTexture9 *t = NULL;
    IDirect3DSurface9 *surface = NULL, *back = NULL, *depth = NULL, *oldDepth = NULL;
    HRESULT hr;
    if (only && only != 70) return;
    hr = IDirect3DDevice9_CreateTexture(device, 2048, 2048, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &t, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 2048, 2048, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, NULL);
    if (FAILED(hr)) { fail_hr("rt2048", "create", hr); if (t) IDirect3DTexture9_Release(t); return; }
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &surface);
    IDirect3DDevice9_GetRenderTarget(device, 0, &back);
    IDirect3DDevice9_GetDepthStencilSurface(device, &oldDepth);
    hr = IDirect3DDevice9_SetRenderTarget(device, 0, surface);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetDepthStencilSurface(device, depth);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff00ff00, 1.0f, 0);
    IDirect3DDevice9_SetRenderTarget(device, 0, back);
    IDirect3DDevice9_SetDepthStencilSurface(device, oldDepth);
    if (FAILED(hr)) fail_hr("rt2048", "target", hr);
    else if (begin("rt2048")) {
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        if (FAILED(hr)) { fail_hr("rt2048", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("rt2048", s, 2);
    }
    if (oldDepth) IDirect3DSurface9_Release(oldDepth);
    IDirect3DSurface9_Release(depth);
    IDirect3DSurface9_Release(surface);
    if (back) IDirect3DSurface9_Release(back);
    IDirect3DTexture9_Release(t);
}

/* 78: a render target texture larger than the depth buffer that stays set, drawn into with the
 * depth test off (3DMark05 fills its 2048x2048 shadow textures this way while the 1024x768
 * depth buffer of the scene is set). In one scene: the texture is cleared blue, a red quad
 * covers its left half, and it is drawn over the back buffer. */
static void rt_small_depth_case(int only)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } TV;
    enum { Big = 512 };
    static const SAMPLE s[] = { { 32, 64, 0xff0000 }, { 96, 64, 0x0000ff }, { 16, 16, 0xff0000 }, { 112, 112, 0x0000ff } };
    TV q[4] = { { 0, 0, 0.5f, 1, 0xffff0000, 0, 0 }, { Big / 2, 0, 0.5f, 1, 0xffff0000, 0, 0 },
                { 0, Big, 0.5f, 1, 0xffff0000, 0, 0 }, { Big / 2, Big, 0.5f, 1, 0xffff0000, 0, 0 } };
    TV v[4] = { { 0, 0, 0.5f, 1, 0xffffffff, 0, 0 }, { Side, 0, 0.5f, 1, 0xffffffff, 1, 0 },
                { 0, Side, 0.5f, 1, 0xffffffff, 0, 1 }, { Side, Side, 0.5f, 1, 0xffffffff, 1, 1 } };
    IDirect3DTexture9 *t = NULL;
    IDirect3DSurface9 *surface = NULL, *back = NULL, *depth = NULL, *oldDepth = NULL;
    DWORD oldZ = D3DZB_FALSE;
    HRESULT hr;
    if (only && only != 78) return;
    hr = IDirect3DDevice9_CreateTexture(device, Big, Big, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &t, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateDepthStencilSurface(device, Side, Side, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, NULL);
    if (FAILED(hr)) { fail_hr("rt_small_depth", "create", hr); if (t) IDirect3DTexture9_Release(t); return; }
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &surface);
    IDirect3DDevice9_GetRenderTarget(device, 0, &back);
    IDirect3DDevice9_GetDepthStencilSurface(device, &oldDepth);
    IDirect3DDevice9_GetRenderState(device, D3DRS_ZENABLE, &oldZ);
    hr = IDirect3DDevice9_SetDepthStencilSurface(device, depth);    /* the back buffer's size */
    if (FAILED(hr)) fail_hr("rt_small_depth", "depth", hr);
    else if (begin("rt_small_depth")) {
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
        hr = IDirect3DDevice9_SetRenderTarget(device, 0, surface);  /* larger than the depth buffer */
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff0000ff, 1.0f, 0);
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
        IDirect3DDevice9_SetRenderTarget(device, 0, back);
        if (SUCCEEDED(hr)) {
            IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)t);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
            IDirect3DDevice9_SetTexture(device, 0, NULL);
            IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        }
        if (FAILED(hr)) { fail_hr("rt_small_depth", "draw", hr); IDirect3DDevice9_EndScene(device); }
        else check("rt_small_depth", s, 4);
    }
    IDirect3DDevice9_SetRenderTarget(device, 0, back);
    IDirect3DDevice9_SetDepthStencilSurface(device, oldDepth);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, oldZ);
    if (oldDepth) IDirect3DSurface9_Release(oldDepth);
    IDirect3DSurface9_Release(depth);
    IDirect3DSurface9_Release(surface);
    if (back) IDirect3DSurface9_Release(back);
    IDirect3DTexture9_Release(t);
}

/* 83: a ps_2_0 shader that reads samplers 9 and 15 (shader model 2 has sixteen; 3DMark05's third
 * game test draws its water with a ninth one): texld r0, t0, s9; texld r1, t0, s15;
 * add r0, r0, r1. A green texture on sampler 9 and a blue one on sampler 15 give cyan. */
static void ps2_sixteen_samplers_case(int only, BOOL version2)
{
    typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } TV;
    static const DWORD ps[] = { 0xffff0200, P2DCL_T(0, 3), P2DCL_S(9), P2DCL_S(15),
        P2I(OTex, 3), P2D(RR, 0, 15), P2SRC(RT, 0), P2SRC(10, 9),
        P2I(OTex, 3), P2D(RR, 1, 15), P2SRC(RT, 0), P2SRC(10, 15),
        P2I(OAdd, 3), P2D(RR, 0, 15), P2SRC(RR, 0), P2SRC(RR, 1),
        P2I(OMov, 2), P2D(8, 0, 15), P2SRC(RR, 0), PSEND };
    static const DWORD green[4] = { 0xff00ff00, 0xff00ff00, 0xff00ff00, 0xff00ff00 };
    static const DWORD blue[4] = { 0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff };
    static const SAMPLE s[] = { { 16, 16, 0x00ffff }, { 96, 32, 0x00ffff }, { 32, 96, 0x00ffff }, { 112, 112, 0x00ffff } };
    TV v[4] = { { 0, 0, 0.5f, 1, 0xffffffff, 0, 0 }, { Side, 0, 0.5f, 1, 0xffffffff, 1, 0 },
                { 0, Side, 0.5f, 1, 0xffffffff, 0, 1 }, { Side, Side, 0.5f, 1, 0xffffffff, 1, 1 } };
    IDirect3DTexture9 *g, *b;
    IDirect3DPixelShader9 *handle = NULL;
    HRESULT hr;
    int k;
    if (!version2 || (only && only != 83)) return;
    g = texture2x2(green);
    b = texture2x2(blue);
    if (!g || !b) { fail_hr("ps2_sixteen_samplers", "texture", E_FAIL); if (g) IDirect3DTexture9_Release(g); if (b) IDirect3DTexture9_Release(b); return; }
    IDirect3DDevice9_SetTexture(device, 9, (IDirect3DBaseTexture9 *)g);
    IDirect3DDevice9_SetTexture(device, 15, (IDirect3DBaseTexture9 *)b);
    for (k = 9; k < 16; k += 6) {
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(device, k, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    hr = IDirect3DDevice9_CreatePixelShader(device, ps, &handle);
    if (FAILED(hr)) fail_hr("ps2_sixteen_samplers", "create", hr);
    else {
        if (begin("ps2_sixteen_samplers")) {
            IDirect3DDevice9_SetPixelShader(device, handle);
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
            IDirect3DDevice9_SetPixelShader(device, NULL);
            if (FAILED(hr)) { fail_hr("ps2_sixteen_samplers", "draw", hr); IDirect3DDevice9_EndScene(device); }
            else check("ps2_sixteen_samplers", s, 4);
        }
        IDirect3DPixelShader9_Release(handle);
    }
    IDirect3DDevice9_SetTexture(device, 9, NULL);
    IDirect3DDevice9_SetTexture(device, 15, NULL);
    IDirect3DTexture9_Release(g);
    IDirect3DTexture9_Release(b);
}

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
    D3DCAPS9 dc;                        /* declared here: Visual C++ 6 (the Windows 9x toolchain) is C89 */
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
    /* 69: the immediate present interval (the driver's D3DCAPS2_PRESENT_INTERVAL_IMMEDIATE). */
    printf("D9PIX case=interval_immediate samples=1 mismatches=%d\n",
           caps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : 1);
    if (caps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE) ++passes; else ++failures;
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
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 4)) ps14_case(only);
    else if (caps.PixelShaderVersion < D3DPS_VERSION(1, 2)) printf("D9PIX pixel shaders not offered\n");
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) && (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) && (!only || only >= 40))
        cube_cases(only, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0));
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) && (!only || only >= 43)) rt_cases(only);
    rt2048_case(only);
    rt_small_depth_case(only);
    ps2_sixteen_samplers_case(only, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0));
    scissor_case(only, (caps.RasterCaps & D3DPRASTERCAPS_SCISSORTEST) != 0);
    luminance_case(only);
    lines_points_case(only);
    index32_case(only, caps.MaxVertexIndex > 0xffff);
    printf("D9PIX max_point_size=%d\n", (int)caps.MaxPointSize);
    point_case(only);
    program_point_case(only, caps.VertexShaderVersion >= D3DVS_VERSION(1, 1));
    static_indexed_case(only);
    scaled_point_case(only, caps.MaxPointSize >= 16.0f);
    vertex_point_case(only, caps.MaxPointSize >= 16.0f && flags == D3DCREATE_HARDWARE_VERTEXPROCESSING);
    point_variant_cases(only, caps.MaxPointSize >= 16.0f, caps.MaxUserClipPlanes >= 1);
    ff_streams_case(only, flags == D3DCREATE_HARDWARE_VERTEXPROCESSING);
    depth_bias_case(only, (caps.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS) != 0);
    blend_op_case(only, (caps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP) != 0);
    l16_case(only);
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1)) bump_case(only);
    colorwrite_case(only);
    eviction_case(only);
    dot3_case(only, (caps.TextureOpCaps & D3DTEXOPCAPS_DOTPRODUCT3) != 0);
    large_texture_case(only, caps.MaxTextureWidth < caps.MaxTextureHeight ? caps.MaxTextureWidth : caps.MaxTextureHeight);
    embm_case(only, (caps.TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAP) != 0);
    stages8_case(only, caps.MaxTextureBlendStages >= 8 && caps.MaxSimultaneousTextures >= 8);
    transform_cases(only);
    if (caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) && (caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) && (!only || only >= 45))
        volume_cases(only, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0));
    if (caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) printf("D9PIX vertex shader cases need pixel shader 2.0\n");
    else if (flags == D3DCREATE_SOFTWARE_VERTEXPROCESSING || caps.VertexShaderVersion >= D3DVS_VERSION(2, 0)) {
        BOOL offsets = (caps.DevCaps2 & D3DDEVCAPS2_STREAMOFFSET) != 0;
        if (!only || only >= 30) vertex_shaders(only);
        if (!only || only >= 61) vs_flow_cases(only, offsets);
        if (!only || only >= 84) buffer_cases(only, offsets);
        if (!only) {
            /* The vertex shader cases once more, drawn from vertex buffers ("_vb"). */
            vsFromBuffer = TRUE;
            vsLead = offsets ? 8 : 0;
            vertex_shaders(0);
            vs_flow_cases(0, offsets);
            if (caps.MaxUserClipPlanes >= 1) clip_cases(65, TRUE, TRUE);
            vsFromBuffer = FALSE;
        }
    } else printf("D9PIX vertex shaders not offered\n");
    clip_cases(only, caps.MaxUserClipPlanes >= 1, caps.PixelShaderVersion >= D3DPS_VERSION(2, 0) && (flags == D3DCREATE_SOFTWARE_VERTEXPROCESSING || caps.VertexShaderVersion >= D3DVS_VERSION(2, 0)));
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    printf("D9PIX end pass=%d fail=%d result=%s\n", passes, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
