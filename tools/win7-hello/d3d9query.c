/* Compare an occlusion query with the pixels a hardware D3D9 device drew. MIT. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

#define CHECK(call) do { HRESULT status = (call); if (FAILED(status)) { \
    printf("%s failed: %08lx\n", #call, (unsigned long)status); goto done; } } while (0)

static HRESULT wait_for_result(IDirect3DQuery9 *query)
{
    DWORD start = GetTickCount();
    HRESULT hr;
    do {
        hr = IDirect3DQuery9_GetData(query, NULL, 0, D3DGETDATA_FLUSH);
        if (hr != S_FALSE)
            return hr;
        Sleep(1);
    } while (GetTickCount() - start < 10000);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

struct vertex { float x, y, z, rhw; DWORD color; };

static HRESULT draw_rectangle(IDirect3DDevice9 *device, float x, float width, float z, DWORD color)
{
    const struct vertex quad[] = {
        {x, 8, z, 1, color}, {x + width, 8, z, 1, color},
        {x, 24, z, 1, color}, {x + width, 24, z, 1, color}
    };
    return IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

struct pixels { unsigned red, green, blue; };

static HRESULT read_pixels(IDirect3DDevice9 *device, IDirect3DSurface9 *target,
                           IDirect3DSurface9 *copy, struct pixels *pixels)
{
    D3DLOCKED_RECT lock;
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(device, target, copy);
    if (FAILED(hr)) return hr;
    hr = IDirect3DSurface9_LockRect(copy, &lock, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) return hr;
    memset(pixels, 0, sizeof(*pixels));
    for (unsigned y = 0; y < 32; ++y) {
        const DWORD *row = (const DWORD *)((const BYTE *)lock.pBits + y * lock.Pitch);
        for (unsigned x = 0; x < 32; ++x) {
            const DWORD rgb = row[x] & 0xffffff;
            pixels->red += rgb == 0xff0000;
            pixels->green += rgb == 0x00ff00;
            pixels->blue += rgb == 0x0000ff;
        }
    }
    return IDirect3DSurface9_UnlockRect(copy);
}

static HRESULT check_samples(IDirect3DQuery9 *query, const char *name, DWORD pixels, BOOL visibility)
{
    const DWORD sentinel = 0xdeadbeef;
    const DWORD expected = visibility ? (pixels != 0) : pixels;
    DWORD count = sentinel;
    HRESULT hr = IDirect3DQuery9_GetData(query, &count, sizeof(count), 0);
    if (hr == S_FALSE) {
        /* The runtime may satisfy the first poll immediately. */
        printf("%s pending GetData left buffer %s\n", name, count == sentinel ? "intact" : "changed");
        hr = wait_for_result(query);
        if (hr == S_OK) hr = IDirect3DQuery9_GetData(query, &count, sizeof(count), 0);
    }
    printf("%s expected=%lu samples=%lu GetData=%08lx\n", name, (unsigned long)expected,
           (unsigned long)count, (unsigned long)hr);
    return hr == S_OK && count == expected ? S_OK : E_FAIL;
}

static int check_overlap(IDirect3DDevice9 *device, IDirect3DSurface9 *target, IDirect3DSurface9 *copy,
                         IDirect3DQuery9 *a, BOOL visibility)
{
    IDirect3DQuery9 *b = NULL;
    struct pixels pixels;
    int failed = 1;
    CHECK(IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_OCCLUSION, &b));
    CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                 0xff000000, 1, 0));
    CHECK(IDirect3DDevice9_BeginScene(device));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_BEGIN));
    CHECK(draw_rectangle(device, 0, 16, .5f, 0xffff0000));
    CHECK(IDirect3DQuery9_Issue(b, D3DISSUE_BEGIN));
    CHECK(draw_rectangle(device, 16, 8, .5f, 0xff00ff00));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_END));
    CHECK(draw_rectangle(device, 24, 4, .5f, 0xff0000ff));
    CHECK(IDirect3DQuery9_Issue(b, D3DISSUE_END));
    CHECK(IDirect3DDevice9_EndScene(device));
    CHECK(read_pixels(device, target, copy, &pixels));
    printf("overlap drawn_pixels=%u/%u/%u\n", pixels.red, pixels.green, pixels.blue);
    if (pixels.red != 256 || pixels.green != 128 || pixels.blue != 64) goto done;
    CHECK(check_samples(a, "overlap A", pixels.red + pixels.green, visibility));
    CHECK(check_samples(b, "overlap B", pixels.green + pixels.blue, visibility));

    /* Only A contains a visible draw. A shared visibility result would make
     * B report true even though all its fragments failed the depth test. */
    CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                 0xff000000, 1, 0));
    CHECK(IDirect3DDevice9_BeginScene(device));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_BEGIN));
    CHECK(draw_rectangle(device, 0, 16, .25f, 0xffff0000));
    CHECK(IDirect3DQuery9_Issue(b, D3DISSUE_BEGIN));
    CHECK(draw_rectangle(device, 0, 16, .5f, 0xff00ff00));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_END));
    CHECK(draw_rectangle(device, 0, 16, .5f, 0xff0000ff));
    CHECK(IDirect3DQuery9_Issue(b, D3DISSUE_END));
    CHECK(IDirect3DDevice9_EndScene(device));
    CHECK(read_pixels(device, target, copy, &pixels));
    printf("hidden overlap drawn_pixels=%u/%u/%u\n", pixels.red, pixels.green, pixels.blue);
    if (pixels.red != 256 || pixels.green || pixels.blue) goto done;
    CHECK(check_samples(a, "visible overlap A", pixels.red, visibility));
    CHECK(check_samples(b, "hidden overlap B", 0, visibility));

    /* Reissue A before reading it. Releasing B mid-bracket must leave A
     * counting both the green draw and the subsequent blue draw. */
    CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                 0xff000000, 1, 0));
    CHECK(IDirect3DDevice9_BeginScene(device));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_BEGIN));
    CHECK(draw_rectangle(device, 0, 16, .5f, 0xffff0000));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_END));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_BEGIN));
    CHECK(IDirect3DQuery9_Issue(b, D3DISSUE_BEGIN));
    CHECK(draw_rectangle(device, 16, 8, .5f, 0xff00ff00));
    IDirect3DQuery9_Release(b);
    b = NULL;
    CHECK(draw_rectangle(device, 24, 4, .5f, 0xff0000ff));
    CHECK(IDirect3DQuery9_Issue(a, D3DISSUE_END));
    CHECK(IDirect3DDevice9_EndScene(device));
    CHECK(read_pixels(device, target, copy, &pixels));
    printf("reissue drawn_pixels=%u/%u/%u\n", pixels.red, pixels.green, pixels.blue);
    if (pixels.red != 256 || pixels.green != 128 || pixels.blue != 64) goto done;
    CHECK(check_samples(a, "reissued A after active B released", pixels.green + pixels.blue, visibility));
    failed = 0;
done:
    if (b) IDirect3DQuery9_Release(b);
    return failed;
}

int main(int argc, char **argv)
{
    static const struct { const char *name; UINT cover; BOOL draw; UINT red; } cases[] = {
        {"visible", 0, TRUE, 256}, {"hidden", 16, TRUE, 0}, {"partial", 8, TRUE, 128},
        {"empty", 0, FALSE, 0}, {"reused", 0, TRUE, 256}
    };
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DQuery9 *query = NULL, *event = NULL;
    IDirect3DSurface9 *target = NULL, *copy = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    int failures = 0, rc = 1;
    BOOL aero = FALSE;
    BOOL unavailable = argc >= 2 && !strcmp(argv[1], "--expect-unavailable");
    BOOL visibility = argc >= 2 && !strcmp(argv[1], "--expect-visibility");
    if (argc > 3 || (argc >= 2 && !unavailable && !visibility && strcmp(argv[1], "--expect-supported"))) {
        puts("usage: d3d9query [--expect-supported | --expect-visibility | --expect-unavailable] [expected-full-UMD-path]");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    HWND window = CreateWindowA("STATIC", "D3D9 query check", WS_OVERLAPPEDWINDOW,
                                40, 40, 128, 128, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!window) return 1;
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = pp.BackBufferHeight = 32;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    pp.hDeviceWindow = window;
    HRESULT dwm = DwmIsCompositionEnabled(&aero);
    printf("process=%u-bit composition_hr=%08lx enabled=%d expected=%s\n",
           (unsigned)(8 * sizeof(void *)), (unsigned long)dwm, (int)aero,
           unavailable ? "unavailable" : (visibility ? "visibility (0/1)" : "sample count"));
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
          window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    HMODULE umd = GetModuleHandleA("viogpu_d3d10.dll");
    char path[MAX_PATH] = "";
    DWORD length = umd ? GetModuleFileNameA(umd, path, sizeof(path)) : 0;
    if (length >= sizeof(path)) goto done;
    printf("UMD=%s\n", length ? path : "(other adapter)");
    if (argc == 3 && (!length || _stricmp(path, argv[2]))) {
        printf("FAIL expected UMD=%s\n", argv[2]);
        goto done;
    }
    CHECK(IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_EVENT, NULL));
    CHECK(IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_EVENT, &event));
    if (!event || IDirect3DQuery9_GetDataSize(event) != sizeof(BOOL)) goto done;
    HRESULT support = IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_OCCLUSION, NULL);
    HRESULT create = IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_OCCLUSION, &query);
    printf("occlusion support=%08lx create=%08lx\n", (unsigned long)support, (unsigned long)create);
    if (unavailable) {
        if (support != D3DERR_NOTAVAILABLE || create != D3DERR_NOTAVAILABLE || query) goto done;
    } else if (support != S_OK || create != S_OK || !query ||
               IDirect3DQuery9_GetDataSize(query) != sizeof(DWORD)) goto done;
    CHECK(IDirect3DDevice9_GetRenderTarget(device, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32, D3DFMT_X8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &copy, NULL));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, TRUE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESS));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    for (unsigned pass = 0; pass < sizeof(cases) / sizeof(cases[0]); ++pass) {
        BOOL signaled = FALSE;
        struct pixels pixels;
        CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                     0xff000000, 1, 0));
        CHECK(IDirect3DDevice9_BeginScene(device));
        if (cases[pass].cover)
            CHECK(draw_rectangle(device, 8, cases[pass].cover, .25f, 0xff0000ff));
        if (query)
            CHECK(IDirect3DQuery9_Issue(query, D3DISSUE_BEGIN));
        if (cases[pass].draw)
            CHECK(draw_rectangle(device, 8, 16, .5f, 0xffff0000));
        if (query)
            CHECK(IDirect3DQuery9_Issue(query, D3DISSUE_END));
        CHECK(IDirect3DDevice9_EndScene(device));
        CHECK(IDirect3DQuery9_Issue(event, D3DISSUE_END));
        CHECK(wait_for_result(event));
        if (IDirect3DQuery9_GetData(event, &signaled, sizeof(signaled), 0) != S_OK ||
            !signaled) { puts("FAIL event did not signal"); goto done; }
        CHECK(read_pixels(device, target, copy, &pixels));
        printf("%s drawn_pixels=%u occluder_pixels=%u event=%d\n", cases[pass].name,
               pixels.red, pixels.blue, (int)signaled);
        failures += pixels.red != cases[pass].red || pixels.blue != cases[pass].cover * 16;
        if (query) failures += FAILED(check_samples(query, cases[pass].name, pixels.red, visibility));
    }
    if (query) failures += check_overlap(device, target, copy, query, visibility);
    rc = failures != 0;
done:
    if (copy) IDirect3DSurface9_Release(copy);
    if (target) IDirect3DSurface9_Release(target);
    if (query) IDirect3DQuery9_Release(query);
    if (event) IDirect3DQuery9_Release(event);
    if (device) IDirect3DDevice9_Release(device);
    if (d3d) IDirect3D9_Release(d3d);
    DestroyWindow(window);
    puts(rc ? "FAIL" : "PASS");
    return rc;
}
