/* Does a program's own work for the next frame overlap the GPU's work on the last one?
 * Each frame draws QUADS blended screen-sized rectangles in one call, so the GPU needs many
 * milliseconds a frame, and before drawing the program keeps a processor busy for a set time,
 * as a game does preparing its frame. With a driver and Windows that let the program run
 * ahead, the frame time is the longer of the two times. Where each frame waits for the GPU
 * to finish the last one, it is their sum: the second line of a pair is then slower than the
 * first by the processor time added.
 *   frame-overlap [OUT.txt] [QUADS] [BUSY_MS] [SECONDS]
 * Runs fullscreen with immediate presentation, first with no processor time and then with
 * BUSY_MS (default 10), for each of the discard and flip swap effects.
 * Build with llvm-mingw clang frame-overlap.c -O2 -ld3d9 -luser32.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

struct vertex { float x, y, z, rhw; D3DCOLOR colour; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static FILE *out;
static LARGE_INTEGER frequency;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);
}

static HRESULT run(IDirect3DDevice9 *device, IDirect3DVertexBuffer9 *quads, unsigned count,
                   const char *name, double busy_ms, double seconds)
{
    HRESULT hr = S_OK;
    unsigned frames = 0;
    double in_present = 0, in_draw = 0, start = 0, elapsed = 0;
    for (int warm = 12; SUCCEEDED(hr) && (warm > 0 || elapsed < seconds * 1000.0); --warm) {
        if (warm == 0)
            start = now_ms();
        /* the program's own work for this frame */
        const double until = now_ms() + busy_ms;
        while (busy_ms > 0 && now_ms() < until)
            ;
        const double a = now_ms();
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff101820, 1.0f, 0);
        IDirect3DDevice9_BeginScene(device);
        IDirect3DDevice9_SetStreamSource(device, 0, quads, 0, sizeof(struct vertex));
        IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, count * 2);
        IDirect3DDevice9_EndScene(device);
        const double b = now_ms();
        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        const double c = now_ms();
        if (warm <= 0) {
            ++frames;
            in_draw += b - a;
            in_present += c - b;
            elapsed = c - start;
        }
        if ((frames & 15) == 0)
            pump();
    }
    if (FAILED(hr) || !frames)
        fprintf(out, "%s, %.0f ms of processor time a frame: failed %08lx\n", name, busy_ms,
                (unsigned long)hr);
    else
        fprintf(out, "%s, %.0f ms of processor time a frame: %.2f ms a frame (%.1f fps), "
                     "%.2f ms of it in Present, %.2f ms in the draw calls\n",
                name, busy_ms, elapsed / frames, frames * 1000.0 / elapsed, in_present / frames,
                in_draw / frames);
    fflush(out);
    return hr;
}

int main(int argc, char **argv)
{
    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out)
        return 2;
    const unsigned count = argc > 2 ? (unsigned)atoi(argv[2]) : 200;
    const double busy = argc > 3 ? atof(argv[3]) : 10.0;
    const double seconds = argc > 4 ? atof(argv[4]) : 4.0;
    QueryPerformanceFrequency(&frequency);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND window = CreateWindowA("STATIC", "frame overlap", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480,
                                NULL, NULL, GetModuleHandleA(NULL), NULL);
    D3DDISPLAYMODE mode;
    if (!d3d || !window || !count ||
        FAILED(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode))) {
        fprintf(out, "no Direct3D 9 adapter or window\n");
        return 1;
    }
    SetForegroundWindow(window);
    fprintf(out, "process %u-bit, mode %ux%u, %u blended quads a frame\n",
            (unsigned)(sizeof(void *) * 8), mode.Width, mode.Height, count);
    static const struct { const char *name; D3DSWAPEFFECT effect; } kinds[] = {
        {"discard", D3DSWAPEFFECT_DISCARD}, {"flip", D3DSWAPEFFECT_FLIP},
    };
    for (unsigned k = 0; k < 2; ++k) {
        D3DPRESENT_PARAMETERS pp = {0};
        pp.BackBufferWidth = mode.Width;
        pp.BackBufferHeight = mode.Height;
        pp.BackBufferFormat = mode.Format;
        pp.BackBufferCount = 1;
        pp.SwapEffect = kinds[k].effect;
        pp.hDeviceWindow = window;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9 *device = NULL;
        HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                             D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
        if (FAILED(hr)) {
            fprintf(out, "%s: CreateDevice failed %08lx\n", kinds[k].name, (unsigned long)hr);
            continue;
        }
        IDirect3DVertexBuffer9 *quads = NULL;
        struct vertex *v = NULL;
        hr = IDirect3DDevice9_CreateVertexBuffer(device, count * 6 * sizeof(struct vertex),
                                                 D3DUSAGE_WRITEONLY, FVF, D3DPOOL_DEFAULT, &quads, NULL);
        if (SUCCEEDED(hr))
            hr = IDirect3DVertexBuffer9_Lock(quads, 0, 0, (void **)&v, 0);
        if (SUCCEEDED(hr)) {
            const float w = (float)mode.Width, h = (float)mode.Height;
            for (unsigned i = 0; i < count; ++i) {
                const D3DCOLOR colour = 0x06000000 | ((i * 53) & 0xff) << 16 | ((i * 29) & 0xff) << 8 |
                                        ((i * 11) & 0xff);
                const struct vertex quad[6] = {
                    {0, 0, 0.5f, 1, colour}, {w, 0, 0.5f, 1, colour}, {0, h, 0.5f, 1, colour},
                    {w, 0, 0.5f, 1, colour}, {w, h, 0.5f, 1, colour}, {0, h, 0.5f, 1, colour},
                };
                memcpy(v + i * 6, quad, sizeof(quad));
            }
            IDirect3DVertexBuffer9_Unlock(quads);
            IDirect3DDevice9_SetFVF(device, FVF);
            IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            if (SUCCEEDED(run(device, quads, count, kinds[k].name, 0.0, seconds)))
                run(device, quads, count, kinds[k].name, busy, seconds);
        } else {
            fprintf(out, "%s: vertex buffer failed %08lx\n", kinds[k].name, (unsigned long)hr);
        }
        if (quads)
            IDirect3DVertexBuffer9_Release(quads);
        IDirect3DDevice9_Release(device);
        pump();
        Sleep(300);
    }
    fprintf(out, "end\n");
    DestroyWindow(window);
    IDirect3D9_Release(d3d);
    if (out != stdout)
        fclose(out);
    return 0;
}
