/* Exercise the public Direct3D 8 or 9 API as an unmodified game would.
 * Check two textured frames on a HAL device, including the untouched border.
 * Build once per API with build-probes.sh. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#if RIPPLE_D3D == 8
#include <d3d8.h>
typedef IDirect3D8 Api;
typedef IDirect3DDevice8 Device;
typedef IDirect3DTexture8 Texture;
typedef IDirect3DSurface8 Surface;
typedef D3DADAPTER_IDENTIFIER8 Adapter;
typedef D3DCAPS8 Caps;
#elif RIPPLE_D3D == 9
#include <d3d9.h>
typedef IDirect3D9 Api;
typedef IDirect3DDevice9 Device;
typedef IDirect3DTexture9 Texture;
typedef IDirect3DSurface9 Surface;
typedef D3DADAPTER_IDENTIFIER9 Adapter;
typedef D3DCAPS9 Caps;
#else
#error Select RIPPLE_D3D 8 or 9
#endif

static void report(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    int size = _vsnprintf(text, sizeof(text) - 2, format, args);
    va_end(args);
    if (size < 0 || (unsigned)size > sizeof(text) - 2) size = sizeof(text) - 2;
    text[size++] = '\r';
    text[size++] = '\n';
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, size, &written, NULL);
}

static void module_path(const char *name)
{
    HMODULE module = GetModuleHandleA(name);
    char file[MAX_PATH] = {};
    if (module && GetModuleFileNameA(module, file, sizeof(file)))
        report("module %s", file);
}

static bool ok(HRESULT result, const char *name)
{
    if (SUCCEEDED(result)) return true;
    report("%s failed 0x%08lx", name, (unsigned long)result);
    return false;
}

static DWORD colour(unsigned x, unsigned y, unsigned frame)
{
    return D3DCOLOR_XRGB(27 + x * 51, 41 + y * 47, 12 + (x + y + frame) % 4 * 59);
}

static bool scene(Device *device, Texture *texture, Surface *back, unsigned frame)
{
    static const struct Vertex { float x, y, z, rhw, u, v; } quad[] = {
        {7.5f, 7.5f, .5f, 1, 0, 0}, {55.5f, 7.5f, .5f, 1, 1, 0},
        {7.5f, 55.5f, .5f, 1, 0, 1}, {55.5f, 55.5f, .5f, 1, 1, 1}
    };
    const DWORD background = D3DCOLOR_XRGB(5, 9, 19);
    D3DLOCKED_RECT pixels;
    if (!ok(texture->LockRect(0, &pixels, NULL, 0), "texture lock")) return false;
    for (unsigned y = 0; y < 4; ++y) {
        DWORD *row = (DWORD *)((BYTE *)pixels.pBits + y * pixels.Pitch);
        for (unsigned x = 0; x < 4; ++x) row[x] = colour(x, y, frame);
    }
    if (!ok(texture->UnlockRect(0), "texture unlock") ||
        !ok(device->Clear(0, NULL, D3DCLEAR_TARGET, background, 1, 0), "clear") ||
        !ok(device->BeginScene(), "begin")) return false;
    HRESULT draw = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    HRESULT end = device->EndScene();
    if (!ok(draw, "draw") || !ok(end, "end") ||
        !ok(back->LockRect(&pixels, NULL, D3DLOCK_READONLY), "back buffer lock")) return false;
    unsigned wrong = 0;
    for (unsigned y = 0; y < 64; ++y) {
        const DWORD *row = (const DWORD *)((const BYTE *)pixels.pBits + y * pixels.Pitch);
        for (unsigned x = 0; x < 64; ++x) {
            DWORD expected = background;
            if (x >= 8 && x < 56 && y >= 8 && y < 56)
                expected = colour((x - 8) / 12, (y - 8) / 12, frame);
            if ((row[x] ^ expected) & 0xffffff) {
                if (wrong == 0) report("pixel (%u,%u) got %06lx expected %06lx",
                    x, y, row[x] & 0xffffff, expected & 0xffffff);
                ++wrong;
            }
        }
    }
    report("frame %u pixels=4096 mismatches=%u", frame, wrong);
    if (!ok(back->UnlockRect(), "back buffer unlock") ||
        !ok(device->Present(NULL, NULL, NULL, NULL), "present")) return false;
    return wrong == 0;
}

#if RIPPLE_D3D == 9
static bool clear_conversion(Device *device, Surface *back, bool srgb)
{
    const DWORD linear = D3DCOLOR_XRGB(5, 9, 19);
    const DWORD expected = srgb ? D3DCOLOR_XRGB(38, 53, 77) : linear;
    D3DLOCKED_RECT pixels;
    if (!ok(device->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb), "sRGB write state") ||
        !ok(device->Clear(0, NULL, D3DCLEAR_TARGET, linear, 1, 0), "colour clear") ||
        !ok(back->LockRect(&pixels, NULL, D3DLOCK_READONLY), "clear readback")) return false;
    unsigned wrong = 0;
    for (unsigned y = 0; y < 64; ++y) {
        const DWORD *row = (const DWORD *)((const BYTE *)pixels.pBits + y * pixels.Pitch);
        for (unsigned x = 0; x < 64; ++x) {
            for (unsigned shift = 0; shift < 24; shift += 8) {
                int diff = int((row[x] >> shift) & 255) - int((expected >> shift) & 255);
                int tolerance = srgb ? 1 : 0;
                if (diff < -tolerance || diff > tolerance) {
                    if (!wrong) report("clear got %06lx expected %06lx", row[x] & 0xffffff, expected & 0xffffff);
                    ++wrong;
                    break;
                }
            }
        }
    }
    report("sRGB write %s clear pixels=4096 mismatches=%u", srgb ? "enabled" : "disabled", wrong);
    return ok(back->UnlockRect(), "clear unlock") && !wrong;
}
#endif

static bool render(Device *device, bool srgb_write)
{
    Texture *texture = NULL;
    Surface *back = NULL;
    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
    bool pass = false;
#if RIPPLE_D3D == 8
    if (!ok(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back), "back buffer") ||
        !ok(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture), "texture") ||
        !ok(device->SetVertexShader(fvf), "vertex format") ||
        !ok(device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT), "min filter") ||
        !ok(device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT), "mag filter")) goto done;
#else
    if (!ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back), "back buffer") ||
        !ok(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL), "texture") ||
        !ok(device->SetFVF(fvf), "vertex format") ||
        !ok(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), "min filter") ||
        !ok(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT), "mag filter")) goto done;
#endif
    if (!ok(device->SetRenderState(D3DRS_LIGHTING, FALSE), "lighting") ||
        !ok(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "culling") ||
        !ok(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "texture operation") ||
        !ok(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), "texture argument") ||
        !ok(device->SetTexture(0, texture), "set texture")) goto done;
    pass = scene(device, texture, back, 0) && scene(device, texture, back, 1);
#if RIPPLE_D3D == 9
    if (pass && srgb_write)
        pass = clear_conversion(device, back, true) && clear_conversion(device, back, false);
#else
    (void)srgb_write;
#endif
done:
    device->SetTexture(0, NULL);
    if (texture) texture->Release();
    if (back) back->Release();
    return pass;
}

static bool test(Api *api, HWND window)
{
    Adapter adapter = {};
    Caps caps = {};
    if (!ok(api->GetAdapterIdentifier(0, 0, &adapter), "adapter") ||
        !ok(api->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps), "hardware capabilities")) return false;
    report("adapter %s driver=%s PCI=%04lx:%04lx texture=%lux%lu VS=%04lx PS=%04lx",
        adapter.Description, adapter.Driver, adapter.VendorId, adapter.DeviceId,
        (DWORD)caps.MaxTextureWidth, (DWORD)caps.MaxTextureHeight,
        caps.VertexShaderVersion & 0xffff, caps.PixelShaderVersion & 0xffff);
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    Device *device = NULL;
    DWORD flags = caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT ?
        D3DCREATE_HARDWARE_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    if (!ok(api->CreateDevice(0, D3DDEVTYPE_HAL, window, flags, &pp, &device), "create hardware device"))
        return false;
    report("HAL vertex processing %s", flags == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software");
    module_path("wined3d.dll");
    module_path("ripplegl.dll");
    bool srgb_write = false;
#if RIPPLE_D3D == 9
    srgb_write = SUCCEEDED(api->CheckDeviceFormat(0, D3DDEVTYPE_HAL, pp.BackBufferFormat,
        D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_TEXTURE, pp.BackBufferFormat));
    report("sRGB writes for X8R8G8B8 %s", srgb_write ? "supported" : "unavailable");
#endif
    bool pass = render(device, srgb_write);
    device->Release();
    return pass;
}

extern "C" void mainCRTStartup()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const char *dll = RIPPLE_D3D == 8 ? "d3d8.dll" : "d3d9.dll";
    const char *entry = RIPPLE_D3D == 8 ? "Direct3DCreate8" : "Direct3DCreate9";
    report("Direct3D %d textured scene", RIPPLE_D3D);
    HMODULE module = LoadLibraryA(dll);
    if (!module) {
        report("LoadLibrary %s error %lu", dll, GetLastError());
        ExitProcess(1);
    }
    module_path(dll);
    typedef Api *(WINAPI *Create)(UINT);
    Create create = (Create)GetProcAddress(module, entry);
    HWND window = CreateWindowA("STATIC", "Ripple Direct3D", WS_POPUP | WS_VISIBLE,
        20, 20, 64, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
    Api *api = create && window ? create(D3D_SDK_VERSION) : NULL;
    bool pass = api && test(api, window);
    if (api) api->Release();
    if (window) DestroyWindow(window);
    FreeLibrary(module);
    report("Direct3D %d %s", RIPPLE_D3D, pass ? "PASS" : "FAIL");
    ExitProcess(pass ? 0 : 1);
}
