/* Partial NOOVERWRITE ranges on a dynamic Direct3D 9 vertex buffer. MIT. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "line %d: %s returned %08lx\n", __LINE__, #call, (unsigned long)hr); \
    return 1; } } while (0)

struct vertex { float x, y, z, rhw; DWORD color; };
typedef struct vertex quad[4];

static void make_quad(quad q, unsigned tile, DWORD color)
{
    for (unsigned i = 0; i < 4; ++i) {
        q[i].x = (float)(tile * 16 + (i & 1) * 16) - 0.5f;
        q[i].y = (float)((i >> 1) * 16) - 0.5f;
        q[i].z = 0.5f;
        q[i].rhw = 1.0f;
        q[i].color = color;
    }
}

static int exercise(IDirect3DDevice9 *dev, DWORD flags)
{
    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    DWORD colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff};
    const char *kind = flags & D3DUSAGE_WRITEONLY ? "write-only" : "readable";
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DSurface9 *target = NULL, *readback = NULL;
    D3DLOCKED_RECT mapped;
    quad reference[3];
    void *data = NULL;
    unsigned failures = 0;
    CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, 786432, flags, fvf, D3DPOOL_DEFAULT, &vb, NULL));
    for (unsigned tile = 0; tile < 3; ++tile) {
        make_quad(reference[tile], tile, colors[tile]);
        CHECK(IDirect3DVertexBuffer9_Lock(vb, (1 + tile) * sizeof(quad), sizeof(quad), &data, D3DLOCK_NOOVERWRITE));
        memcpy(data, reference[tile], sizeof(quad));
        CHECK(IDirect3DVertexBuffer9_Unlock(vb));
    }
    /* A lock spans two quads. The application only overwrites four colour
     * fields in the first. Both its positions and the following quad must survive. */
    CHECK(IDirect3DVertexBuffer9_Lock(vb, sizeof(quad), 2 * sizeof(quad), &data, D3DLOCK_NOOVERWRITE));
    colors[0] = 0xffffff00;
    for (unsigned i = 0; i < 4; ++i) {
        ((struct vertex *)data)[i].color = colors[0];
        reference[0][i].color = colors[0];
    }
    CHECK(IDirect3DVertexBuffer9_Unlock(vb));
    if (!(flags & D3DUSAGE_WRITEONLY)) {
        CHECK(IDirect3DVertexBuffer9_Lock(vb, sizeof(quad), sizeof(reference), &data, D3DLOCK_READONLY));
        unsigned mismatch = 0;
        const BYTE *actual = (const BYTE *)data, *expected = (const BYTE *)reference;
        for (unsigned i = 0; i < sizeof(reference); ++i)
            mismatch += actual[i] != expected[i];
        failures += mismatch;
        printf("%s CPU read mismatches=%u/%u bytes\n", kind, mismatch, (unsigned)sizeof(reference));
        CHECK(IDirect3DVertexBuffer9_Unlock(vb));
    }
    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1, 0));
    CHECK(IDirect3DDevice9_SetFVF(dev, fvf));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct vertex)));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    for (unsigned tile = 0; tile < 3; ++tile)
        CHECK(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 4 * (tile + 1), 2));
    CHECK(IDirect3DDevice9_EndScene(dev));
    CHECK(IDirect3DDevice9_GetRenderTarget(dev, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 48, 16, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL));
    CHECK(IDirect3DDevice9_GetRenderTargetData(dev, target, readback));
    CHECK(IDirect3DSurface9_LockRect(readback, &mapped, NULL, D3DLOCK_READONLY));
    for (unsigned tile = 0; tile < 3; ++tile) {
        unsigned wrong = 0;
        for (unsigned y = 2; y < 14; ++y) {
            const DWORD *row = (const DWORD *)((const BYTE *)mapped.pBits + y * mapped.Pitch);
            for (unsigned x = 2; x < 14; ++x)
                wrong += ((row[16 * tile + x] ^ colors[tile]) & 0xffffff) != 0;
        }
        printf("%s tile %u expected=%06lx mismatches=%u/144\n", kind, tile, colors[tile] & 0xffffff, wrong);
        failures += wrong;
    }
    CHECK(IDirect3DSurface9_UnlockRect(readback));
    CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0));
    IDirect3DSurface9_Release(readback);
    IDirect3DSurface9_Release(target);
    IDirect3DVertexBuffer9_Release(vb);
    return failures > 0;
}

int main(void)
{
    HWND window = CreateWindowA("STATIC", "D3D9 ranged buffer check", WS_OVERLAPPEDWINDOW,
                                0, 0, 96, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    if (!d3d || !window) return 1;
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 48;
    pp.BackBufferHeight = 16;
    CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
    int bad = exercise(dev, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY);
    bad |= exercise(dev, D3DUSAGE_DYNAMIC);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    return bad;
}
