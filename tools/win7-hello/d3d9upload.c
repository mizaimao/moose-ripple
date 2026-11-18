/* Regression for reading an unrelated system-memory texture during CreateTexture.
 * The source is guarded only while an unrelated default-pool texture is created,
 * then restored before any operation on the source. Also checks explicit uploads
 * and managed textures by rendering and reading back the result.
 * Build with llvm-mingw: clang -O2 -Wall -Wextra d3d9upload.c -ld3d9 -lgdi32
 * Run outside the package directory: d3d9upload.exe > d3d9upload.txt
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG guarded;
static LONG CALLBACK fault(EXCEPTION_POINTERS *exception)
{
    if (guarded && exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        printf("FAIL: unrelated CreateTexture accessed guarded memory (%llu at %p)\n",
               (unsigned long long)exception->ExceptionRecord->ExceptionInformation[0],
               (void *)exception->ExceptionRecord->ExceptionInformation[1]);
        fflush(stdout);
        ExitProcess(1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#define CHECK(call) do { HRESULT h = (call); if (FAILED(h)) { \
    printf("FAIL line %d: %s = 0x%08lx\n", __LINE__, #call, (unsigned long)h); \
    return 1; } } while (0)

static int picture_region(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, DWORD expected,
                          const RECT *region, DWORD outside)
{
    struct vertex { float x, y, z, rhw, u, v; } vertices[] = {
        {-0.5f, -0.5f, 0, 1, 0, 0}, {31.5f, -0.5f, 0, 1, 1, 0},
        {-0.5f, 31.5f, 0, 1, 0, 1}, {31.5f, 31.5f, 0, 1, 1, 1}
    };
    IDirect3DSurface9 *target = NULL, *readback = NULL;
    D3DLOCKED_RECT lock;
    DWORD pixel;
    unsigned failures = 0;
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    CHECK(IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture));
    CHECK(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1));
    CHECK(IDirect3DDevice9_BeginScene(device));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0])));
    CHECK(IDirect3DDevice9_EndScene(device));
    CHECK(IDirect3DDevice9_GetRenderTarget(device, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32, D3DFMT_X8R8G8B8,
                                                     D3DPOOL_SYSTEMMEM, &readback, NULL));
    CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, readback));
    CHECK(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY));
    pixel = *(DWORD *)((BYTE *)lock.pBits + 16 * lock.Pitch + 16 * 4) & 0xffffff;
    if (region) {
        for (int y = 0; y < 32; ++y) {
            const DWORD *row = (const DWORD *)((const BYTE *)lock.pBits + y * lock.Pitch);
            for (int x = 0; x < 32; ++x) {
                POINT point = {x, y};
                DWORD color = PtInRect(region, point) ? expected : outside;
                if ((row[x] & 0xffffff) != (color & 0xffffff)) ++failures;
            }
        }
    }
    IDirect3DSurface9_UnlockRect(readback);
    IDirect3DSurface9_Release(readback);
    IDirect3DSurface9_Release(target);
    if (region) {
        printf("Sampled texture: %u mismatches across 1024 pixels\n", failures);
        return failures != 0;
    }
    printf("pixel=%06lx expected=%06lx: %s\n", (unsigned long)pixel,
           (unsigned long)(expected & 0xffffff), pixel == (expected & 0xffffff) ? "PASS" : "FAIL");
    return pixel != (expected & 0xffffff);
}

static int picture(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, DWORD expected)
{
    return picture_region(device, texture, expected, NULL, 0);
}

/* Queue updates of two private sampled textures before reading either one.
 * Each partial upload comes from rows wider than the copied rectangle. */
static int bitmap_uploads(IDirect3DDevice9 *device)
{
    enum { width = 640, pitch = 673 };
    IDirect3DTexture9 *source = NULL, *dest[2] = {NULL, NULL};
    IDirect3DSurface9 *src = NULL, *surface = NULL;
    DWORD *pixels = malloc(pitch * width * sizeof(*pixels));
    HANDLE memory = pixels;
    const DWORD background = 0xff123456, colors[2] = {0xffd84925, 0xff16b572};
    const RECT full = {0, 0, width, width}, part = {16, 20, 528, 532};
    const POINT offset = {64, 64};
    const RECT sampled = {3, 3, 29, 29};
    if (!pixels) return 1;
    for (int j = 0; j < pitch * width; ++j) pixels[j] = background;
    CHECK(IDirect3DDevice9_CreateTexture(device, pitch, width, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_SYSTEMMEM, &source, &memory));
    CHECK(IDirect3DTexture9_GetSurfaceLevel(source, 0, &src));
    for (int i = 0; i < 2; ++i) {
        CHECK(IDirect3DDevice9_CreateTexture(device, width, width, 1, 0, D3DFMT_A8R8G8B8,
                                             D3DPOOL_DEFAULT, &dest[i], NULL));
        CHECK(IDirect3DTexture9_GetSurfaceLevel(dest[i], 0, &surface));
        CHECK(IDirect3DDevice9_UpdateSurface(device, src, &full, surface, NULL));
        IDirect3DSurface9_Release(surface);
    }
    for (int i = 0; i < 2; ++i) {
        for (int y = part.top; y < part.bottom; ++y)
            for (int x = part.left; x < part.right; ++x)
                pixels[y * pitch + x] = colors[i];
        CHECK(IDirect3DTexture9_GetSurfaceLevel(dest[i], 0, &surface));
        CHECK(IDirect3DDevice9_UpdateSurface(device, src, &part, surface, &offset));
        IDirect3DSurface9_Release(surface);
    }
    int failures = 0;
    for (int i = 0; i < 2; ++i) {
        failures += picture_region(device, dest[i], colors[i], &sampled, background);
        IDirect3DTexture9_Release(dest[i]);
    }
    IDirect3DSurface9_Release(src);
    IDirect3DTexture9_Release(source);
    free(pixels);
    return failures != 0;
}

/* A subrectangle of an image retains the pitch of the full source. Check
 * every pixel, including the destination pixels outside the updated box. */
static int pitched_upload(IDirect3DDevice9 *device)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *src = NULL, *dst = NULL, *readback = NULL;
    const RECT region = {13, 7, 36, 24};
    const POINT offset = {4, 9};
    const DWORD background = 0xff654321;
    DWORD pixels[97 * 59];
    HANDLE memory = pixels;
    D3DLOCKED_RECT lock;
    int failures = 0;
    memset(pixels, 0, sizeof(pixels));
    CHECK(IDirect3DDevice9_CreateTexture(device, 97, 59, 1, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_SYSTEMMEM, &texture, &memory));
    CHECK(IDirect3DTexture9_GetSurfaceLevel(texture, 0, &src));
    CHECK(IDirect3DDevice9_CreateRenderTarget(device, 32, 32, D3DFMT_A8R8G8B8,
                                              D3DMULTISAMPLE_NONE, 0, FALSE, &dst, NULL));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32, D3DFMT_A8R8G8B8,
                                                      D3DPOOL_SYSTEMMEM, &readback, NULL));
    CHECK(IDirect3DDevice9_ColorFill(device, dst, NULL, background));
    for (int pass = 1; pass <= 2; ++pass) {
        for (int y = 0; y < 59; ++y)
            for (int x = 0; x < 97; ++x)
                pixels[y * 97 + x] = 0xff000000u | (pass << 16) | (y << 8) | x;
        CHECK(IDirect3DDevice9_UpdateSurface(device, src, &region, dst, &offset));
        CHECK(IDirect3DDevice9_GetRenderTargetData(device, dst, readback));
        CHECK(IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY));
        for (int y = 0; y < 32; ++y) {
            const DWORD *row = (const DWORD *)((const BYTE *)lock.pBits + y * lock.Pitch);
            for (int x = 0; x < 32; ++x) {
                int sx = x - offset.x + region.left, sy = y - offset.y + region.top;
                DWORD expected = background;
                if (sx >= region.left && sx < region.right && sy >= region.top && sy < region.bottom)
                    expected = pixels[sy * 97 + sx];
                if (row[x] != expected) {
                    if (!failures)
                        printf("pixel (%d,%d) got %08lx expected %08lx\n", x, y, row[x], expected);
                    ++failures;
                }
            }
        }
        CHECK(IDirect3DSurface9_UnlockRect(readback));
    }
    IDirect3DSurface9_Release(readback);
    IDirect3DSurface9_Release(dst);
    IDirect3DSurface9_Release(src);
    IDirect3DTexture9_Release(texture);
    printf("Pitched subrectangle uploads: %d mismatches across 2048 pixels\n", failures);
    return failures != 0;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc = {0};
    HWND window;
    IDirect3D9 *d3d;
    IDirect3D9Ex *d3d_ex = NULL;
    IDirect3DDevice9Ex *device_ex = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DTexture9 *source = NULL, *dest = NULL, *managed = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    const unsigned width = 250, height = 269;
    DWORD *pixels, old_protect;
    HANDLE memory;
    unsigned i, x, y, pass;
    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, fault);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "RippleD3D9Upload";
    RegisterClassA(&wc);
    window = CreateWindowA(wc.lpszClassName, "Texture upload check", WS_OVERLAPPEDWINDOW,
                           80, 80, 160, 120, NULL, NULL, wc.hInstance, NULL);
    if (!window) return 1;
    CHECK(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d_ex));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = pp.BackBufferHeight = 32;
    pp.hDeviceWindow = window;
    CHECK(IDirect3D9Ex_CreateDeviceEx(d3d_ex, 0, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, NULL, &device_ex));
    device = (IDirect3DDevice9 *)device_ex;
    if (argc == 2 && (!strcmp(argv[1], "pitch") || !strcmp(argv[1], "bitmap"))) {
        int rc = !strcmp(argv[1], "bitmap") ? bitmap_uploads(device) : pitched_upload(device);
        IDirect3DDevice9_Release(device);
        IDirect3D9Ex_Release(d3d_ex);
        DestroyWindow(window);
        return rc;
    }
    pixels = VirtualAlloc(NULL, width * height * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pixels) return 1;
    for (i = 0; i < width * height; ++i) pixels[i] = 0xff2468ac;
    memory = pixels;
    CHECK(IDirect3DDevice9_CreateTexture(device, width, height, 1, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_SYSTEMMEM, &source, &memory));
    printf("borrowed source created; guarding its pixels during unrelated creation\n");
    if (!VirtualProtect(pixels, width * height * 4, PAGE_NOACCESS, &old_protect)) return 1;
    guarded = 1;
    CHECK(IDirect3DDevice9_CreateTexture(device, width, height, 1, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &dest, NULL));
    guarded = 0;
    if (!VirtualProtect(pixels, width * height * 4, old_protect, &old_protect)) return 1;
    printf("PASS: unrelated creation did not read source pixels\n");
    for (pass = 0; pass < 2; ++pass) {
        DWORD color = pass ? 0xffb65a21 : 0xff2468ac;
        for (i = 0; i < width * height; ++i) pixels[i] = color;
        CHECK(IDirect3DTexture9_AddDirtyRect(source, NULL));
        CHECK(IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)source,
                                            (IDirect3DBaseTexture9 *)dest));
        printf("explicit upload %u: ", pass);
        if (picture(device, dest, color)) return 1;
    }
    CHECK(IDirect3DDevice9_SetTexture(device, 0, NULL));
    IDirect3DTexture9_Release(dest);
    IDirect3DTexture9_Release(source);
    VirtualFree(pixels, 0, MEM_RELEASE);
    IDirect3DDevice9_Release(device);
    IDirect3D9Ex_Release(d3d_ex);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 1;
    device = NULL;
    CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    CHECK(IDirect3DDevice9_CreateTexture(device, 64, 64, 1, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_MANAGED, &managed, NULL));
    for (pass = 0; pass < 2; ++pass) {
        D3DLOCKED_RECT lock;
        DWORD color = pass ? 0xffa54b72 : 0xff31bc65;
        CHECK(IDirect3DTexture9_LockRect(managed, 0, &lock, NULL, 0));
        for (y = 0; y < 64; ++y)
            for (x = 0; x < 64; ++x)
                ((DWORD *)((BYTE *)lock.pBits + y * lock.Pitch))[x] = color;
        CHECK(IDirect3DTexture9_UnlockRect(managed, 0));
        printf("managed upload %u: ", pass);
        if (picture(device, managed, color)) return 1;
    }
    CHECK(IDirect3DDevice9_SetTexture(device, 0, NULL));
    IDirect3DTexture9_Release(managed);
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    DestroyWindow(window);
    puts("PASS: texture upload regression checks");
    return 0;
}
