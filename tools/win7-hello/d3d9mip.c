/* Check D3DSAMP_MAXMIPLEVEL with implicit derivatives and shader-specified LOD.
 * Run: d3d9mip.exe [expected-full-UMD-path]
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    printf("FAIL line %d: %s = 0x%08lx\n", __LINE__, #call, (unsigned long)hr); \
    return 1; } } while (0)

enum { target_size = 32, texture_size = 128, mip_count = 4 };
static const DWORD colors[mip_count] = { 0xffde4334, 0xff36b96e, 0xff3464d8, 0xffeab340 };

/* ps_3_0. v0.xy supplies interpolated UV and c0.w the explicit LOD. */
static const DWORD sample_lod_shader[] = {
    D3DPS_VERSION(3, 0),
    0x0200001f, 0x80000005, 0x90030000,             /* dcl_texcoord0 v0.xy */
    0x0200001f, 0x90000000, 0xa00f0800,             /* dcl_2d s0 */
    0x02000001, 0x80030001, 0x90e40000,             /* mov r1.xy, v0 */
    0x02000001, 0x800c0001, 0xa0e40000,             /* mov r1.zw, c0 */
    0x0300005f, 0x800f0000, 0x80e40001, 0xa0e40800, /* texldl r0, r1, s0 */
    0x02000001, 0x800f0800, 0x80e40000,             /* mov oC0, r0 */
    0x0000ffff
};

static int fill_texture(IDirect3DDevice9 *device, IDirect3DTexture9 *texture)
{
    IDirect3DTexture9 *source;
    CHECK(IDirect3DDevice9_CreateTexture(device, texture_size, texture_size, mip_count,
            0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &source, NULL));
    for (UINT level = 0; level < mip_count; ++level) {
        D3DLOCKED_RECT lock;
        const UINT size = texture_size >> level;
        CHECK(IDirect3DTexture9_LockRect(source, level, &lock, NULL, 0));
        for (UINT y = 0; y < size; ++y) {
            DWORD *row = (DWORD *)((BYTE *)lock.pBits + y * lock.Pitch);
            for (UINT x = 0; x < size; ++x)
                row[x] = colors[level];
        }
        CHECK(IDirect3DTexture9_UnlockRect(source, level));
    }
    CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)source,
                                         (IDirect3DBaseTexture9 *)texture));
    IDirect3DTexture9_Release(source);
    return 0;
}

struct mip_case {
    const char *name;
    D3DTEXTUREFILTERTYPE filter;
    DWORD clamp;
    float uv_extent;
    int shader_lod; /* -1 uses fixed-function sampling */
    UINT expected_level;
};

static int check_case(IDirect3DDevice9 *device, IDirect3DSurface9 *target,
                      IDirect3DSurface9 *readback, IDirect3DPixelShader9 *shader,
                      const struct mip_case *test)
{
    const float uv = test->uv_extent, end = target_size - 0.5f;
    const struct { float x, y, z, rhw, u, v; } quad[] = {
        {-0.5f, -0.5f, 0, 1, 0, 0}, {end, -0.5f, 0, 1, uv, 0},
        {-0.5f, end, 0, 1, 0, uv}, {end, end, 0, 1, uv, uv}
    };
    const float coord[4] = {0.0f, 0.0f, 0.0f, (float)test->shader_lod};
    const DWORD expected = colors[test->expected_level] & 0xffffff;
    D3DLOCKED_RECT lock;
    unsigned mismatches = 0;
    DWORD sample;
    CHECK(IDirect3DDevice9_SetPixelShader(device, test->shader_lod >= 0 ? shader : NULL));
    if (test->shader_lod >= 0)
        CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, coord, 1));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAXMIPLEVEL, test->clamp));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, test->filter));
    CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0));
    CHECK(IDirect3DDevice9_BeginScene(device));
    HRESULT draw_result = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2,
                                                           quad, sizeof(quad[0]));
    HRESULT end_result = IDirect3DDevice9_EndScene(device);
    if (FAILED(draw_result) || FAILED(end_result)) {
        printf("%s: FAIL DrawPrimitiveUP=0x%08lx EndScene=0x%08lx\n", test->name,
               (unsigned long)draw_result, (unsigned long)end_result);
        return 1;
    }
    CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, readback));
    CHECK(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY));
    sample = *(const DWORD *)((const BYTE *)lock.pBits + 16 * lock.Pitch + 16 * sizeof(DWORD));
    for (int y = 0; y < target_size; ++y) {
        const DWORD *row = (const DWORD *)((const BYTE *)lock.pBits + y * lock.Pitch);
        for (int x = 0; x < target_size; ++x)
            mismatches += ((row[x] & 0xffffff) != expected);
    }
    CHECK(IDirect3DSurface9_UnlockRect(readback));
    printf("%s: expected mip %u (%06lx), center %06lx, mismatches %u/%u\n",
           test->name, test->expected_level, (unsigned long)expected,
           (unsigned long)(sample & 0xffffff), mismatches, (unsigned)(target_size * target_size));
    return mismatches != 0;
}

int main(int argc, char **argv)
{
    /* 128 texels across 32 pixels selects mip 2. A quarter of a texel per
     * pixel magnifies the texture. Explicit LODs use the original mip indices. */
    static const struct mip_case cases[] = {
        {"unclamped magnify", D3DTEXF_POINT, 0, 0.0625f, -1, 0},
        {"unclamped minify", D3DTEXF_POINT, 0, 1.0f, -1, 2},
        {"clamped magnify", D3DTEXF_POINT, 1, 0.0625f, -1, 1},
        {"clamped minify", D3DTEXF_POINT, 1, 1.0f, -1, 2},
        {"clamped linear mipfilter", D3DTEXF_LINEAR, 1, 1.0f, -1, 2},
        {"last mip magnify", D3DTEXF_POINT, 3, 0.0625f, -1, 3},
        {"clamp past last mip", D3DTEXF_POINT, 8, 0.0625f, -1, 3},
        {"disabled mipfilter", D3DTEXF_NONE, 1, 1.0f, -1, 0},
        {"unclamped explicit LOD", D3DTEXF_POINT, 0, 1.0f, 2, 2},
        {"explicit LOD below clamp", D3DTEXF_POINT, 1, 1.0f, 0, 1},
        {"explicit LOD above clamp", D3DTEXF_POINT, 1, 1.0f, 2, 2},
        {"restored minify", D3DTEXF_POINT, 0, 1.0f, -1, 2},
    };
    IDirect3D9 *d3d;
    IDirect3DDevice9 *device;
    IDirect3DTexture9 *texture;
    IDirect3DSurface9 *target, *readback;
    IDirect3DPixelShader9 *shader;
    D3DPRESENT_PARAMETERS pp = {0};
    D3DADAPTER_IDENTIFIER9 adapter;
    HMODULE module;
    char path[MAX_PATH];
    DWORD path_length;
    HWND window;
    int failures = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2) {
        puts("Usage: d3d9mip.exe [expected-full-UMD-path]");
        return 1;
    }
    window = CreateWindowA("STATIC", "D3D9 mip check", WS_OVERLAPPEDWINDOW,
                           0, 0, 120, 100, NULL, NULL, GetModuleHandleA(NULL), NULL);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) {
        puts("FAIL creating window or Direct3D9");
        return 1;
    }
    CHECK(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &adapter));
    printf("Adapter=%s (%u-bit)\n", adapter.Description, (unsigned)(8 * sizeof(void *)));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = pp.BackBufferHeight = target_size;
    pp.hDeviceWindow = window;
    CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    module = GetModuleHandleA("viogpu_d3d10.dll");
    path_length = module ? GetModuleFileNameA(module, path, sizeof(path)) : 0;
    if (module && (!path_length || path_length >= sizeof(path))) {
        puts("FAIL locating loaded viogpu_d3d10.dll");
        return 1;
    }
    if (module)
        printf("UMD=%s\n", path);
    if (argc == 2 && (!module || _stricmp(path, argv[1]))) {
        printf("FAIL expected UMD=%s\n", argv[1]);
        return 1;
    }
    CHECK(IDirect3DDevice9_CreateTexture(device, texture_size, texture_size, mip_count,
            0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL));
    if (fill_texture(device, texture))
        return 1;
    CHECK(IDirect3DDevice9_GetRenderTarget(device, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, target_size, target_size,
                                        pp.BackBufferFormat, D3DPOOL_SYSTEMMEM, &readback, NULL));
    CHECK(IDirect3DDevice9_CreatePixelShader(device, sample_lod_shader, &shader));
    CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE));
    CHECK(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1));
    for (UINT i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        failures += check_case(device, target, readback, shader, &cases[i]);
    IDirect3DPixelShader9_Release(shader);
    IDirect3DSurface9_Release(readback);
    IDirect3DSurface9_Release(target);
    IDirect3DTexture9_Release(texture);
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    printf("%s (%d failed cases)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
