/*
 * Texture updates the way Windows 7's compositor makes them (docs/windows/kmd-port-notes.md):
 * a texture as wide as the screen and as high as the taskbar, filled once and then updated
 * rectangle by rectangle with ID3D10Device::UpdateSubresource, the source pointer inside a
 * larger bitmap, as the compositor uploads what changed in a window. After every update the
 * texture is copied to a staging texture and compared with what it should hold.
 * Three rounds for each of the two formats the compositor uses: updates alone, updates with a
 * draw between them that has the texture bound (so the texture is in use when the next update
 * comes), and all updates first with one comparison at the end.
 * Writes d3d10tex.txt next to the program. On the display adapter's own driver.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>
#include <string.h>

#include "tri_vs.h"   /* tri.hlsl compiled to DXBC by hlsl2dxbc.c */
#include "tri_ps.h"

#define W 1024
#define H 40
#define BW 1280          /* the source bitmap is larger than the texture */
#define BH 64

static UINT32 bitmap[BH][BW];   /* what the "window" holds, the source of every update */
static UINT32 model[H][W];      /* what the texture should hold */

static const RECT rects[] = {
    { 28, 0, 55, 40 }, { 887, 0, 912, 40 }, { 0, 0, 54, 40 }, { 283, 0, 305, 40 }, { 1, 1, 2, 2 },
    { 1000, 39, 1024, 40 }, { 5, 3, 300, 37 }, { 28, 0, 55, 40 }, { 511, 7, 513, 9 }, { 0, 0, 1024, 40 },
    { 100, 0, 101, 40 }, { 862, 0, 888, 40 }, { 3, 0, 1021, 1 }, { 640, 20, 1024, 40 },
};
#define NRECTS (sizeof(rects) / sizeof(rects[0]))

static FILE *f;
static ID3D10Device *dev;
static ID3D10Texture2D *tex, *staging, *rt;
static ID3D10ShaderResourceView *srv;
static ID3D10RenderTargetView *rtv;

/* A new value for every pixel of the bitmap: its place and the number of the step */
static void paint(unsigned step)
{
    unsigned x, y;
    for (y = 0; y < BH; y++)
        for (x = 0; x < BW; x++)
            bitmap[y][x] = 0xff000000u | ((step * 16 + 15) & 0xff) << 16 | (y & 0xff) << 8 | ((x * 7) & 0xff);
}

static void update(const RECT *r)
{
    D3D10_BOX box;
    LONG x, y;
    box.left = r->left; box.top = r->top; box.front = 0;
    box.right = r->right; box.bottom = r->bottom; box.back = 1;
    ID3D10Device_UpdateSubresource(dev, (ID3D10Resource *)tex, 0, &box, &bitmap[r->top][r->left], BW * 4, 0);
    for (y = r->top; y < r->bottom; y++)
        for (x = r->left; x < r->right; x++)
            model[y][x] = bitmap[y][x];
}

/* Compare the texture with the model; returns the number of pixels that differ */
static unsigned compare(UINT32 mask, const char *what)
{
    D3D10_MAPPED_TEXTURE2D map;
    unsigned x, y, bad = 0, x0 = W, y0 = H, x1 = 0, y1 = 0;
    UINT32 got0 = 0, want0 = 0;
    HRESULT hr;

    ID3D10Device_CopyResource(dev, (ID3D10Resource *)staging, (ID3D10Resource *)tex);
    hr = ID3D10Texture2D_Map(staging, 0, D3D10_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        fprintf(f, "  %s: Map of the staging texture failed, 0x%08lx\r\n", what, (unsigned long)hr);
        return 1;
    }
    for (y = 0; y < H; y++) {
        const UINT32 *row = (const UINT32 *)((const unsigned char *)map.pData + y * map.RowPitch);
        for (x = 0; x < W; x++) {
            if ((row[x] & mask) == (model[y][x] & mask))
                continue;
            if (!bad) { got0 = row[x]; want0 = model[y][x]; }
            bad++;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x + 1 > x1) x1 = x + 1;
            if (y + 1 > y1) y1 = y + 1;
        }
    }
    ID3D10Texture2D_Unmap(staging, 0);
    if (bad)
        fprintf(f, "  %s: %u pixels differ, inside (%u,%u)-(%u,%u); the first holds %08lx, should hold %08lx\r\n",
                what, bad, x0, y0, x1, y1, (unsigned long)got0, (unsigned long)want0);
    return bad;
}

static void draw_with_texture(void)
{
    ID3D10Device_PSSetShaderResources(dev, 0, 1, &srv);
    ID3D10Device_Draw(dev, 3, 0);
}

static unsigned round_of(DXGI_FORMAT format, const char *name, int mode)
{
    static const char *const modes[] = {
        "compared after every update", "a draw with the texture bound after every update",
        "all updates, then one comparison",
    };
    D3D10_TEXTURE2D_DESC td;
    UINT32 mask = format == DXGI_FORMAT_B8G8R8X8_UNORM ? 0x00ffffffu : 0xffffffffu;
    RECT all = { 0, 0, W, H };
    unsigned i, bad = 0;
    char what[96];
    HRESULT hr;

    ZeroMemory(&td, sizeof(td));
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = format; td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT; td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    hr = ID3D10Device_CreateTexture2D(dev, &td, NULL, &tex);
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateShaderResourceView(dev, (ID3D10Resource *)tex, NULL, &srv);
    td.Usage = D3D10_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateTexture2D(dev, &td, NULL, &staging);
    fprintf(f, "%s, %s:\r\n", name, modes[mode]);
    if (FAILED(hr)) {
        fprintf(f, "  creating the textures failed, 0x%08lx\r\n", (unsigned long)hr);
        return 1;
    }
    paint(0);
    update(&all);
    if (mode != 2)
        bad += compare(mask, "after the first fill");
    for (i = 0; i < NRECTS; i++) {
        paint(i + 1);
        update(&rects[i]);
        if (mode == 1)
            draw_with_texture();
        if (mode != 2) {
            snprintf(what, sizeof(what), "update %u, (%ld,%ld)-(%ld,%ld)", i + 1, rects[i].left, rects[i].top,
                     rects[i].right, rects[i].bottom);
            bad += compare(mask, what);
        }
    }
    if (mode == 2)
        bad += compare(mask, "at the end");
    if (!bad)
        fprintf(f, "  all %u updates arrived whole\r\n", (unsigned)NRECTS + 1);
    {
        ID3D10ShaderResourceView *none = NULL;
        ID3D10Device_PSSetShaderResources(dev, 0, 1, &none);
    }
    ID3D10ShaderResourceView_Release(srv);
    ID3D10Texture2D_Release(staging);
    ID3D10Texture2D_Release(tex);
    return bad;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    static const D3D10_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0 },
    };
    static const float corners[3][4] = {
        { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, -1.0f, 0.0f, 1.0f },
    };
    D3D10_VIEWPORT vp = { 0, 0, 64, 64, 0.0f, 1.0f };
    UINT stride = 4 * sizeof(float), offset = 0;
    D3D10_TEXTURE2D_DESC td;
    D3D10_BUFFER_DESC bd;
    D3D10_SUBRESOURCE_DATA init;
    ID3D10VertexShader *vs = 0;
    ID3D10PixelShader *ps = 0;
    ID3D10InputLayout *layout = 0;
    ID3D10Buffer *vb = 0;
    unsigned bad = 0;
    int mode;
    HRESULT hr;
    (void)inst; (void)prev; (void)cmd; (void)show;

    f = fopen("d3d10tex.txt", "wb");
    if (!f)
        return 1;
    setvbuf(f, NULL, _IONBF, 0);   /* every line reaches the file, also if the next call crashes */
    hr = D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &dev);
    if (FAILED(hr)) {
        fprintf(f, "no Direct3D 10 device on the adapter's driver: 0x%08lx\r\n", (unsigned long)hr);
        return 1;
    }
    /* Something to draw with, for the rounds that keep the texture in use */
    ZeroMemory(&td, sizeof(td));
    td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT; td.BindFlags = D3D10_BIND_RENDER_TARGET;
    hr = ID3D10Device_CreateTexture2D(dev, &td, NULL, &rt);
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateRenderTargetView(dev, (ID3D10Resource *)rt, NULL, &rtv);
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateVertexShader(dev, tri_vs, sizeof(tri_vs), &vs);
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreatePixelShader(dev, tri_ps, sizeof(tri_ps), &ps);
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateInputLayout(dev, elements, 1, tri_vs, sizeof(tri_vs), &layout);
    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = sizeof(corners); bd.Usage = D3D10_USAGE_DEFAULT; bd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    ZeroMemory(&init, sizeof(init));
    init.pSysMem = corners;
    if (SUCCEEDED(hr))
        hr = ID3D10Device_CreateBuffer(dev, &bd, &init, &vb);
    if (FAILED(hr)) {
        fprintf(f, "setting up the draw failed: 0x%08lx\r\n", (unsigned long)hr);
        return 1;
    }
    ID3D10Device_IASetInputLayout(dev, layout);
    ID3D10Device_IASetVertexBuffers(dev, 0, 1, &vb, &stride, &offset);
    ID3D10Device_OMSetRenderTargets(dev, 1, &rtv, NULL);
    ID3D10Device_RSSetViewports(dev, 1, &vp);
    ID3D10Device_IASetPrimitiveTopology(dev, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Device_VSSetShader(dev, vs);
    ID3D10Device_PSSetShader(dev, ps);

    for (mode = 0; mode < 3; mode++) {
        bad += round_of(DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8", mode);
        bad += round_of(DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8", mode);
    }
    fprintf(f, bad ? "FAILED: %u pixels wrong in all\r\n" : "passed\r\n", bad);
    fclose(f);
    return bad != 0;
}
