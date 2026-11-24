/* How fast Direct3D 9 presents frames that cost almost nothing to draw: a clear and a
 * Present, as fast as they return. A driver that makes each frame wait for a vertical blank
 * shows here as a rate at the refresh rate and frame times that never fall below one
 * refresh period, even with immediate presentation.
 *   present-rate [OUT.txt] [SECONDS]
 * Runs fullscreen with the discard and flip swap effects, immediate and one-interval
 * presentation, then in a window. Rates are by the performance counter and by the tick clock.
 * Build with llvm-mingw clang present-rate.c -O2 -ld3d9 -luser32 -lwinmm.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *out;
static double seconds = 4.0;

static int compare_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);
}

static void run(IDirect3D9 *d3d, HWND window, const char *name, BOOL windowed,
                D3DSWAPEFFECT effect, UINT interval, const D3DDISPLAYMODE *mode)
{
    enum { MAX_FRAMES = 400000 };
    static double times[MAX_FRAMES];
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = windowed;
    pp.BackBufferWidth = windowed ? 640 : mode->Width;
    pp.BackBufferHeight = windowed ? 480 : mode->Height;
    pp.BackBufferFormat = windowed ? D3DFMT_UNKNOWN : mode->Format;
    pp.BackBufferCount = 1;
    pp.SwapEffect = effect;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = interval;
    IDirect3DDevice9 *device = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr)) {
        fprintf(out, "%s: CreateDevice failed %08lx\n", name, (unsigned long)hr);
        return;
    }
    LARGE_INTEGER frequency, start, last, now;
    QueryPerformanceFrequency(&frequency);
    /* a few frames first, so mode changes and first-use costs stay out of the count */
    for (unsigned i = 0; i < 8 && SUCCEEDED(hr); ++i) {
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff203040, 1.0f, 0);
        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    }
    unsigned frames = 0;
    const DWORD tick_start = timeGetTime();
    QueryPerformanceCounter(&start);
    last = start;
    double elapsed = 0;
    while (SUCCEEDED(hr) && elapsed < seconds && frames < MAX_FRAMES) {
        const D3DCOLOR colour = 0xff000000 | ((frames * 7) & 0xff) << 16 | ((frames * 3) & 0xff) << 8 | 0x40;
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, colour, 1.0f, 0);
        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        QueryPerformanceCounter(&now);
        times[frames++] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)frequency.QuadPart;
        last = now;
        elapsed = (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
        if ((frames & 63) == 0)
            pump();
    }
    const DWORD ticks = timeGetTime() - tick_start;
    if (FAILED(hr) || !frames) {
        fprintf(out, "%s: Present failed %08lx after %u frames\n", name, (unsigned long)hr, frames);
    } else {
        qsort(times, frames, sizeof(times[0]), compare_double);
        fprintf(out, "%s: %u frames, %.1f fps by the counter, %.1f fps by the tick clock; "
                     "frame ms min %.3f median %.3f p95 %.3f max %.3f\n",
                name, frames, frames / elapsed, ticks ? frames * 1000.0 / ticks : 0.0,
                times[0], times[frames / 2], times[(size_t)(frames * 0.95)], times[frames - 1]);
    }
    fflush(out);
    IDirect3DDevice9_Release(device);
    pump();
    Sleep(300);
}

int main(int argc, char **argv)
{
    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out)
        return 2;
    if (argc > 2)
        seconds = atof(argv[2]);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND window = CreateWindowA("STATIC", "present rate", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480,
                                NULL, NULL, GetModuleHandleA(NULL), NULL);
    D3DDISPLAYMODE mode;
    if (!d3d || !window || FAILED(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode))) {
        fprintf(out, "no Direct3D 9 adapter or window\n");
        return 1;
    }
    SetForegroundWindow(window);
    fprintf(out, "process %u-bit, mode %ux%u at %u Hz\n", (unsigned)(sizeof(void *) * 8),
            mode.Width, mode.Height, mode.RefreshRate);
    run(d3d, window, "fullscreen discard immediate", FALSE, D3DSWAPEFFECT_DISCARD,
        D3DPRESENT_INTERVAL_IMMEDIATE, &mode);
    run(d3d, window, "fullscreen flip immediate", FALSE, D3DSWAPEFFECT_FLIP,
        D3DPRESENT_INTERVAL_IMMEDIATE, &mode);
    run(d3d, window, "fullscreen discard one interval", FALSE, D3DSWAPEFFECT_DISCARD,
        D3DPRESENT_INTERVAL_ONE, &mode);
    run(d3d, window, "windowed discard immediate", TRUE, D3DSWAPEFFECT_DISCARD,
        D3DPRESENT_INTERVAL_IMMEDIATE, &mode);
    fprintf(out, "end\n");
    DestroyWindow(window);
    IDirect3D9_Release(d3d);
    if (out != stdout)
        fclose(out);
    return 0;
}
