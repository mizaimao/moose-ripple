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

static int picture(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, DWORD expected)
{
    struct vertex { float x, y, z, rhw, u, v; } vertices[] = {
        {-0.5f, -0.5f, 0, 1, 0, 0}, {31.5f, -0.5f, 0, 1, 1, 0},
        {-0.5f, 31.5f, 0, 1, 0, 1}, {31.5f, 31.5f, 0, 1, 1, 1}
    };
    IDirect3DSurface9 *target = NULL, *readback = NULL;
    D3DLOCKED_RECT lock;
    DWORD pixel;
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
    IDirect3DSurface9_UnlockRect(readback);
    IDirect3DSurface9_Release(readback);
    IDirect3DSurface9_Release(target);
    printf("pixel=%06lx expected=%06lx: %s\n", (unsigned long)pixel,
           (unsigned long)(expected & 0xffffff), pixel == (expected & 0xffffff) ? "PASS" : "FAIL");
    return pixel != (expected & 0xffffff);
}

int main(void)
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
