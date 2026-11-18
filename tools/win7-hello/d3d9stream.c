/* Exercise the dynamic, write-only vertex buffer used by the D3D9 compositor.
 * Check full and ranged NOOVERWRITE locks, stream offsets, indexed draws, and
 * DISCARD while earlier draws may still be queued. MIT licence (LICENSE).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define CHECK(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "line %d: %s failed (%08lx)\n", __LINE__, #call, (unsigned long)hr); \
    return 1; } } while (0)

struct vertex { float x, y, z, rhw; DWORD color; };

static HRESULT write_quad(IDirect3DVertexBuffer9 *buffer, UINT first,
                          DWORD flags, BOOL whole, UINT tile, DWORD color)
{
    const float x = tile * 16.0f - 0.5f;
    const struct vertex quad[] = {
        {x, -0.5f, 0.5f, 1, color}, {x + 16, -0.5f, 0.5f, 1, color},
        {x, 15.5f, 0.5f, 1, color}, {x + 16, 15.5f, 0.5f, 1, color}
    };
    void *data;
    HRESULT hr = IDirect3DVertexBuffer9_Lock(buffer, whole ? 0 : first * sizeof(*quad),
                                             whole ? 0 : sizeof(quad), &data, flags);
    if (FAILED(hr)) return hr;
    if (whole && (flags & D3DLOCK_DISCARD)) {
        /* Exercise a large upload as well as the small appended quads. */
        memset(data, 0, 640032);
    }
    memcpy((struct vertex *)data + (whole ? first : 0), quad, sizeof(quad));
    return IDirect3DVertexBuffer9_Unlock(buffer);
}

int main(void)
{
    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    const DWORD colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffff8000};
    const WORD order[] = {0, 1, 2, 3};
    IDirect3DDevice9 *dev = NULL;
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    IDirect3DSurface9 *target = NULL, *readback = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    D3DLOCKED_RECT lock;
    int failed = 0;
    void *indices;
    HWND window = CreateWindowA("STATIC", "D3D9 stream check", WS_OVERLAPPEDWINDOW,
                                0, 0, 96, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) return 1;
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 16;
    CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
    CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, 640032, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                              fvf, D3DPOOL_DEFAULT, &vb, NULL));
    CHECK(IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(order), D3DUSAGE_WRITEONLY,
                                             D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL));
    CHECK(IDirect3DIndexBuffer9_Lock(ib, 0, 0, &indices, 0));
    memcpy(indices, order, sizeof(order));
    CHECK(IDirect3DIndexBuffer9_Unlock(ib));
    CHECK(IDirect3DDevice9_SetFVF(dev, fvf));
    CHECK(IDirect3DDevice9_SetIndices(dev, ib));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1, 0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct vertex)));
    CHECK(write_quad(vb, 20, D3DLOCK_DISCARD, TRUE, 0, colors[0]));
    CHECK(write_quad(vb, 1016, D3DLOCK_NOOVERWRITE, TRUE, 1, colors[1]));
    CHECK(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 1016, 2));
    CHECK(write_quad(vb, 16004, D3DLOCK_NOOVERWRITE, FALSE, 2, colors[2]));
    CHECK(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 20, 2));
    CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 16000 * sizeof(struct vertex),
                                          sizeof(struct vertex)));
    CHECK(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLESTRIP, 4, 0, 4, 0, 2));
    CHECK(write_quad(vb, 31997, D3DLOCK_DISCARD, TRUE, 3, colors[3]));
    CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct vertex)));
    CHECK(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 31997, 2));
    CHECK(IDirect3DDevice9_EndScene(dev));
    CHECK(IDirect3DDevice9_GetRenderTarget(dev, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 16, D3DFMT_X8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &readback, NULL));
    CHECK(IDirect3DDevice9_GetRenderTargetData(dev, target, readback));
    CHECK(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY));
    for (UINT tile = 0; tile < 4; ++tile) {
        const DWORD pixel = ((const DWORD *)((const BYTE *)lock.pBits + 8 * lock.Pitch))[tile * 16 + 8];
        printf("tile %u: %06lx expected %06lx\n", tile, pixel & 0xffffff, colors[tile] & 0xffffff);
        if ((pixel & 0xffffff) != (colors[tile] & 0xffffff)) failed = 1;
    }
    CHECK(IDirect3DSurface9_UnlockRect(readback));
    IDirect3DSurface9_Release(readback);
    IDirect3DSurface9_Release(target);
    IDirect3DIndexBuffer9_Release(ib);
    IDirect3DVertexBuffer9_Release(vb);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    return failed;
}
