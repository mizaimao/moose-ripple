/* Screen-space triangle fans with an application pixel shader and no vertex
 * shader. Build with llvm-mingw clang positiont.c -O2 -ld3d9 -luser32.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "%s failed (%08lx)\n", #call, (unsigned long)hr); goto done; } } while (0)

/* def c0,1,0,0,1; mov oC0,c0. No interpolated inputs. */
static const DWORD solid2[] = { D3DPS_VERSION(2,0), 0x05000051, 0xa00f0000,
    0x3f800000, 0, 0, 0x3f800000, 0x02000001, 0x800f0800, 0xa0e40000, 0xffff };
static const DWORD solid3[] = { D3DPS_VERSION(3,0), 0x05000051, 0xa00f0000,
    0x3f800000, 0, 0, 0x3f800000, 0x02000001, 0x800f0800, 0xa0e40000, 0xffff };
/* dcl t0.xy; dcl_2d s0; texld r0,t0,s0; mov oC0,r0. */
static const DWORD sample2[] = { D3DPS_VERSION(2,0), 0x0200001f, 0x80000000, 0xb0030000,
    0x0200001f, 0x90000000, 0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800,
    0x02000001, 0x800f0800, 0x80e40000, 0xffff };

static const DWORD texels[2][2] = {
    {0xff235688, 0xffa7821b}, {0xffcc3344, 0xff2adc56}
};
static const DWORD background = 0xff112233;
struct vertex { float x, y, z, rhw, u, v; };
static const struct vertex quad[] = {
    {1.5f,1.5f,0.5f,1,0,0}, {13.5f,1.5f,0.5f,1,1,0},
    {13.5f,13.5f,0.5f,1,1,1}, {1.5f,13.5f,0.5f,1,0,1}
};

int main(int argc, char **argv)
{
    static const struct { const char *name; const DWORD *code; DWORD fvf; int gather; } tests[] = {
        {"ps_2_0 constant, position only", solid2, D3DFVF_XYZRHW, 0},
        {"ps_2_0 sampled texture", sample2, D3DFVF_XYZRHW | D3DFVF_TEX1, 0},
        {"ps_3_0 constant, position only", solid3, D3DFVF_XYZRHW, 0},
        {"ps_2_0 Fetch4, clamp", sample2, D3DFVF_XYZRHW | D3DFVF_TEX1, 1}
    };
    const int test_fetch4 = argc == 2 && !strcmp(argv[1], "--fetch4");
    HWND window = CreateWindowA("STATIC", "D3D9 screen-space probe", WS_OVERLAPPEDWINDOW,
                                0, 0, 64, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9 *device = NULL;
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DPixelShader9 *shader = NULL;
    IDirect3DSurface9 *target = NULL, *copy = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    D3DLOCKED_RECT map;
    void *data;
    int rc = 1;
    unsigned failures = 0;
    if (!window || !d3d) goto done;
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = pp.BackBufferHeight = 16;
    pp.hDeviceWindow = window;
    CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    HMODULE driver = GetModuleHandleA("viogpu_d3d10.dll");
    char path[MAX_PATH];
    if (driver && GetModuleFileNameA(driver, path, sizeof(path))) printf("Driver %s\n", path);
    CHECK(IDirect3DDevice9_CreateVertexBuffer(device, sizeof(quad), D3DUSAGE_WRITEONLY,
                                              0, D3DPOOL_DEFAULT, &vb, NULL));
    CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &data, 0));
    memcpy(data, quad, sizeof(quad));
    CHECK(IDirect3DVertexBuffer9_Unlock(vb));
    CHECK(IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(quad[0])));
    CHECK(IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_MANAGED, &texture, NULL));
    CHECK(IDirect3DTexture9_LockRect(texture, 0, &map, NULL, 0));
    for (int y = 0; y < 2; ++y)
        memcpy((BYTE *)map.pBits + y * map.Pitch, texels[y], sizeof(texels[y]));
    CHECK(IDirect3DTexture9_UnlockRect(texture, 0));
    CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_GetRenderTarget(device, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 16, 16, pp.BackBufferFormat,
                                                       D3DPOOL_SYSTEMMEM, &copy, NULL));

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].gather) {
            if (!test_fetch4) continue;
            CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
            CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPMAPLODBIAS,
                                                   MAKEFOURCC('G','E','T','4')));
        }
        CHECK(IDirect3DDevice9_SetFVF(device, tests[i].fvf));
        CHECK(IDirect3DDevice9_CreatePixelShader(device, tests[i].code, &shader));
        CHECK(IDirect3DDevice9_SetPixelShader(device, shader));
        IDirect3DPixelShader9_Release(shader);
        shader = NULL;
        CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, background, 1, 0));
        CHECK(IDirect3DDevice9_BeginScene(device));
        HRESULT draw = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLEFAN, 0, 2);
        CHECK(IDirect3DDevice9_EndScene(device));
        CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, copy));
        CHECK(IDirect3DSurface9_LockRect(copy, &map, NULL, D3DLOCK_READONLY));
        unsigned bad = 0;
        for (int y = 0; y < 16; ++y) {
            const DWORD *row = (const DWORD *)((const BYTE *)map.pBits + y * map.Pitch);
            for (int x = 0; x < 16; ++x) {
                DWORD expected = background;
                if (x >= 2 && x < 14 && y >= 2 && y < 14) {
                    if (tests[i].gather) {
                        /* Fetch4 returns four neighbours' red channels. Clamp
                         * repeats the last column and row at the two edges. */
                        DWORD right = (texels[y >= 8][1] >> 16) & 0xff;
                        DWORD below = (texels[1][x >= 8] >> 16) & 0xff;
                        DWORD diagonal = (texels[1][1] >> 16) & 0xff;
                        expected = (right << 16) | (below << 8) | diagonal;
                    } else {
                        expected = tests[i].code == sample2 ? texels[y >= 8][x >= 8] : 0xffff0000;
                    }
                }
                if ((row[x] ^ expected) & 0x00ffffff) {
                    if (!bad) printf("first mismatch (%d,%d) expected %06lx read %06lx\n", x, y,
                                     expected & 0xffffffUL, row[x] & 0xffffffUL);
                    ++bad;
                }
            }
        }
        CHECK(IDirect3DSurface9_UnlockRect(copy));
        printf("%s draw=%08lx mismatches=%u/256\n", tests[i].name, (unsigned long)draw, bad);
        failures += FAILED(draw) || bad;
    }
    rc = failures != 0;
done:
    if (copy) IDirect3DSurface9_Release(copy);
    if (target) IDirect3DSurface9_Release(target);
    if (shader) IDirect3DPixelShader9_Release(shader);
    if (texture) IDirect3DTexture9_Release(texture);
    if (vb) IDirect3DVertexBuffer9_Release(vb);
    if (device) IDirect3DDevice9_Release(device);
    if (d3d) IDirect3D9_Release(d3d);
    if (window) DestroyWindow(window);
    return rc;
}
