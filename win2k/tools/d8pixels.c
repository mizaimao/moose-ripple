/* Check what a Direct3D 8 program draws on the hardware device: a clear,
 * transformed triangles from user memory, a vertex buffer, an index buffer
 * with and without a base vertex, a strip, a managed texture and untransformed
 * vertices. Each case draws on a 128x128 lockable back buffer and inspects
 * a few pixels well inside the shapes. "sw" asks for software vertex
 * processing. Built against the Microsoft headers; d3d8.dll is loaded at
 * run time. */
#define COBJMACROS
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
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

int main(int argc, char **argv)
{
    IDirect3D8 *(WINAPI *create)(UINT);
    HMODULE lib = LoadLibraryA("d3d8.dll");
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    D3DCAPS8 caps;
    HWND window;
    DWORD flags;
    BOOL software = argc > 1 && !lstrcmpiA(argv[1], "sw");
    HRESULT hr;
    setvbuf(stdout, NULL, _IONBF, 0);
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
    case_clear();
    case_up();
    case_vb();
    case_strip();
    indexed("indexed", 0, 0x00ffff);
    indexed("base", 3, 0xff8000);
    case_texture();
    case_transform();
    IDirect3DDevice8_Release(device);
    IDirect3D8_Release(d3d);
    DestroyWindow(window);
    printf("D8PIX end pass=%d fail=%d result=%s\n", passes, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
