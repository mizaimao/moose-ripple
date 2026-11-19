/* D3D9 per-stage constant transitions across queued draws. MIT. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { \
    printf("FAIL %s = %08lx\n", #call, (unsigned long)hr_); goto done; } } while (0)

/* VS passes clip-space position to o0 and a test color to o1. PS adds a
 * second color. The two stages deliberately use the same register, c17. */
#define VS_DECLS D3DVS_VERSION(3,0), \
    0x0200001f,0x80000000,0x900f0000, /* dcl_position v0 */ \
    0x0200001f,0x80000000,0xe00f0000, /* dcl_position o0 */ \
    0x0200001f,0x8000000a,0xe00f0001  /* dcl_color o1 */
#define MOVE_POS 0x02000001,0xe00f0000,0x90e40000        /* mov o0, v0 */
#define VS_COLOR_C17 0x02000001,0xe00f0001,0xa0e40011    /* mov o1, c17 */
#define PS_DECLS D3DPS_VERSION(3,0),0x0200001f,0x8000000a,0x900f0000 /* dcl_color v0 */
#define PS_ADD_C17 0x03000002,0x800f0800,0x90e40000,0xa0e40011      /* add oC0, v0, c17 */

enum { keep = -1, global_shader, local_shader, no_constants };
static const DWORD vs_code[][32] = {
    {VS_DECLS, MOVE_POS, VS_COLOR_C17, 0x0000ffff},
    {VS_DECLS, 0x05000051,0xa00f0011,0,0x3f800000,0,0x3f800000, /* def c17, 0,1,0,1 */
     MOVE_POS, VS_COLOR_C17, 0x0000ffff},
    {VS_DECLS, 0x0200001f,0x8000000a,0x900f0001, /* dcl_color v1 */
     MOVE_POS, 0x02000001,0xe00f0001,0x90e40001, /* mov o1, v1 */
     0x0000ffff}
};
static const DWORD ps_code[][32] = {
    {PS_DECLS, PS_ADD_C17, 0x0000ffff},
    {PS_DECLS, 0x05000051,0xa00f0011,0,0,0x3f800000,0, /* def c17, 0,0,1,0 */
     PS_ADD_C17, 0x0000ffff},
    {PS_DECLS, 0x02000001,0x800f0800,0x90e40000, /* mov oC0, v0 */
     0x0000ffff}
};

enum { red, green, blue };
static const float rgb[][4] = {{1,0,0,1}, {0,1,0,1}, {0,0,1,1}};
static const struct {
    const char *name;
    int vs, ps, vc, pc;
    DWORD expected;
} cases[] = {
    {"initial",            global_shader, global_shader, red,  green, 0xffff00},
    {"unchanged",          keep,          keep,          keep, keep,  0xffff00},
    {"change VS only",     keep,          keep,          blue, keep,  0x00ffff},
    {"repeat equal PS",    keep,          keep,          keep, green, 0x00ffff},
    {"change PS only",     keep,          keep,          keep, red,   0xff00ff},
    {"VS local DEF",       local_shader,  keep,          keep, keep,  0xffff00},
    {"shadowed VS write",  keep,          keep,          red,  keep,  0xffff00},
    {"restore API VS",     global_shader, keep,          keep, keep,  0xff0000},
    {"PS local DEF",       keep,          local_shader,  keep, keep,  0xff00ff},
    {"shadowed PS write",  keep,          keep,          keep, green, 0xff00ff},
    {"PS no constants",    keep,          no_constants,  keep, keep,  0xff0000},
    {"VS no constants",    no_constants,  keep,          keep, keep,  0x0000ff},
    {"restore API PS",     keep,          global_shader, keep, keep,  0x00ffff},
    {"restore API VS/PS",  global_shader, keep,          keep, keep,  0xffff00},
    {"both local DEF",     local_shader,  local_shader,  keep, keep,  0x00ffff},
    {"both global again",  global_shader, global_shader, keep, keep,  0xffff00}
};
enum { tile = 8, case_count = sizeof(cases)/sizeof(cases[0]), width = tile*case_count };

int main(int argc, char **argv)
{
    static const D3DVERTEXELEMENT9 elements[] = {
        {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},
        D3DDECL_END()
    };
    struct vertex { float x,y,z,w; DWORD color; };
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexShader9 *vs[3] = {0};
    IDirect3DPixelShader9 *ps[3] = {0};
    IDirect3DSurface9 *target = NULL, *copy = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    D3DLOCKED_RECT map;
    HWND w = NULL;
    int rc = 1, failures = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2) { puts("usage: d3d9const [expected-full-UMD-path]"); return 2; }
    w = CreateWindowA("STATIC", "D3D9 constant check", WS_OVERLAPPEDWINDOW,
                      0, 0, width+32, tile+64, NULL, NULL, GetModuleHandleA(NULL), NULL);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!w || !d3d) goto done;
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = tile;
    pp.hDeviceWindow = w;
    CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    HMODULE module = GetModuleHandleA("viogpu_d3d10.dll");
    char path[MAX_PATH] = "";
    DWORD length = module ? GetModuleFileNameA(module, path, sizeof(path)) : 0;
    if (length >= sizeof(path)) goto done;
    printf("process=%u-bit UMD=%s\n", (unsigned)(sizeof(void*)*8), length ? path : "(other adapter)");
    if (argc == 2 && (!length || _stricmp(path, argv[1]))) {
        printf("FAIL expected UMD=%s\n", argv[1]);
        goto done;
    }
    CHECK(IDirect3DDevice9_CreateVertexDeclaration(device, elements, &decl));
    CHECK(IDirect3DDevice9_SetVertexDeclaration(device, decl));
    for (unsigned i=0; i<3; ++i) {
        CHECK(IDirect3DDevice9_CreateVertexShader(device, vs_code[i], &vs[i]));
        CHECK(IDirect3DDevice9_CreatePixelShader(device, ps_code[i], &ps[i]));
    }
    CHECK(IDirect3DDevice9_GetRenderTarget(device, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, width, tile, pp.BackBufferFormat,
                                                       D3DPOOL_SYSTEMMEM, &copy, NULL));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE));
    CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff204060, 1, 0));
    CHECK(IDirect3DDevice9_BeginScene(device));
    for (unsigned i=0; i<case_count; ++i) {
        float left = -1.0f + 2.0f*i/case_count, right = -1.0f + 2.0f*(i+1)/case_count;
        const struct vertex quad[] = {
            {left,1,0.5f,1,0xff0000ff}, {right,1,0.5f,1,0xff0000ff},
            {left,-1,0.5f,1,0xff0000ff}, {right,-1,0.5f,1,0xff0000ff}
        };
        if (cases[i].vs != keep) CHECK(IDirect3DDevice9_SetVertexShader(device, vs[cases[i].vs]));
        if (cases[i].ps != keep) CHECK(IDirect3DDevice9_SetPixelShader(device, ps[cases[i].ps]));
        if (cases[i].vc != keep) CHECK(IDirect3DDevice9_SetVertexShaderConstantF(device, 17, rgb[cases[i].vc], 1));
        if (cases[i].pc != keep) CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 17, rgb[cases[i].pc], 1));
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
    }
    CHECK(IDirect3DDevice9_EndScene(device));
    CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, copy));
    CHECK(IDirect3DSurface9_LockRect(copy, &map, NULL, D3DLOCK_READONLY));
    for (unsigned i=0; i<case_count; ++i) {
        unsigned bad = 0;
        DWORD seen = 0;
        for (unsigned y=2; y<tile-2; ++y) {
            const DWORD *row = (const DWORD *)((const BYTE *)map.pBits + y*map.Pitch);
            for (unsigned x=2; x<tile-2; ++x) {
                seen = row[tile*i+x] & 0xffffff;
                bad += seen != cases[i].expected;
            }
        }
        printf("%s expected=%06lx observed=%06lx mismatches=%u/16\n",
               cases[i].name, (unsigned long)cases[i].expected, (unsigned long)seen, bad);
        failures += bad != 0;
    }
    CHECK(IDirect3DSurface9_UnlockRect(copy));
    rc = failures != 0;
done:
    if (copy) IDirect3DSurface9_Release(copy);
    if (target) IDirect3DSurface9_Release(target);
    for (unsigned i=0; i<3; ++i) {
        if (vs[i]) IDirect3DVertexShader9_Release(vs[i]);
        if (ps[i]) IDirect3DPixelShader9_Release(ps[i]);
    }
    if (decl) IDirect3DVertexDeclaration9_Release(decl);
    if (device) IDirect3DDevice9_Release(device);
    if (d3d) IDirect3D9_Release(d3d);
    if (w) DestroyWindow(w);
    puts(rc ? "FAIL" : "PASS");
    return rc;
}
