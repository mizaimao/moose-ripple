/* Does a video show a picture? Plays a file in a window of its own for a few seconds through
 * one of Windows' two playback pipelines, both ending in the Enhanced Video Renderer, and
 * reads the screen where the window is:
 *   vidplay ds FILE [SECONDS]   DirectShow: a filter graph with the renderer added first
 *   vidplay mf FILE [SECONDS]   Media Foundation, through MFPlay
 * Prints how many of 64 points spread over the picture are not black, twice, a second apart,
 * and whether the picture changed in between. Exit code 0 if at least a quarter are lit.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <initguid.h>
#include <dshow.h>
#include <mfidl.h>
#include <evr.h>
#include <mfplay.h>
#include <stdio.h>
#include <string.h>

/* The Enhanced Video Renderer's DirectShow filter; the headers here do not name it */
DEFINE_GUID(CLSID_EnhancedVideoRenderer, 0xfa10746c, 0x9b63, 0x4b6c, 0xbc, 0x49, 0xfc, 0x30, 0x0e, 0xa5, 0xf2, 0x56);

enum { WIDTH = 640, HEIGHT = 360, GRID = 8 };

static void pump(DWORD ms)
{
    const DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if ((LONG)(end - GetTickCount()) <= 0) break;
        Sleep(10);
    }
}

/* The screen at GRID x GRID places of the window's client area; returns how many are lit */
static int look(HWND window, COLORREF *seen)
{
    int lit = 0;
    HDC screen = GetDC(NULL);
    for (int j = 0; j < GRID; ++j) {
        for (int i = 0; i < GRID; ++i) {
            POINT p = { WIDTH * (2 * i + 1) / (2 * GRID), HEIGHT * (2 * j + 1) / (2 * GRID) };
            ClientToScreen(window, &p);
            const COLORREF c = GetPixel(screen, p.x, p.y);
            seen[j * GRID + i] = c;
            lit += GetRValue(c) > 16 || GetGValue(c) > 16 || GetBValue(c) > 16;
        }
    }
    ReleaseDC(NULL, screen);
    return lit;
}

static HRESULT play_directshow(HWND window, const WCHAR *path)
{
    IGraphBuilder *graph = NULL;
    IBaseFilter *renderer = NULL;
    IMFGetService *services = NULL;
    IMFVideoDisplayControl *display = NULL;
    IMediaControl *control = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder,
                                  (void **)&graph);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_EnhancedVideoRenderer, NULL, CLSCTX_INPROC_SERVER, &IID_IBaseFilter,
                              (void **)&renderer);
    if (SUCCEEDED(hr)) hr = IGraphBuilder_AddFilter(graph, renderer, L"renderer");
    if (SUCCEEDED(hr)) hr = IBaseFilter_QueryInterface(renderer, &IID_IMFGetService, (void **)&services);
    if (SUCCEEDED(hr))
        hr = IMFGetService_GetService(services, &MR_VIDEO_RENDER_SERVICE, &IID_IMFVideoDisplayControl,
                                      (void **)&display);
    if (SUCCEEDED(hr)) hr = IMFVideoDisplayControl_SetVideoWindow(display, window);
    RECT all = { 0, 0, WIDTH, HEIGHT };
    if (SUCCEEDED(hr)) hr = IMFVideoDisplayControl_SetVideoPosition(display, NULL, &all);
    printf("renderer ready hr=0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) hr = IGraphBuilder_RenderFile(graph, path, NULL);
    printf("graph built hr=0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) hr = IGraphBuilder_QueryInterface(graph, &IID_IMediaControl, (void **)&control);
    if (SUCCEEDED(hr)) hr = IMediaControl_Run(control);
    return FAILED(hr) ? hr : S_OK;   /* left running; the process ends with it */
}

static HRESULT play_mediafoundation(HWND window, const WCHAR *path)
{
    IMFPMediaPlayer *player = NULL;
    return MFPCreateMediaPlayer(path, TRUE, 0, NULL, window, &player);
}

int main(int argc, char **argv)
{
    if (argc < 3 || (strcmp(argv[1], "ds") && strcmp(argv[1], "mf"))) {
        printf("usage: vidplay ds|mf FILE [SECONDS]\n");
        return 2;
    }
    const int seconds = argc > 3 ? atoi(argv[3]) : 4;
    WCHAR path[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, path, MAX_PATH);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RippleVidPlay";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    RECT frame = { 0, 0, WIDTH, HEIGHT };
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"video test",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 120,
                                  frame.right - frame.left, frame.bottom - frame.top, NULL, NULL,
                                  wc.hInstance, NULL);
    if (!window) { printf("no window\n"); return 2; }
    pump(200);

    HRESULT hr = argv[1][0] == 'd' ? play_directshow(window, path) : play_mediafoundation(window, path);
    printf("%s playback of %s: hr=0x%08lx\n", argv[1], argv[2], (unsigned long)hr);
    if (FAILED(hr)) return 2;

    static COLORREF first[GRID * GRID], second[GRID * GRID];
    pump((DWORD)seconds * 1000);
    const int lit1 = look(window, first);
    pump(1000);
    const int lit2 = look(window, second);
    int changed = 0;
    for (int i = 0; i < GRID * GRID; ++i) changed += first[i] != second[i];
    printf("points not black: %d then %d of %d; %d changed in a second; the middle is %06lx\n", lit1, lit2,
           GRID * GRID, changed,
           (unsigned long)((GetRValue(second[36]) << 16) | (GetGValue(second[36]) << 8) | GetBValue(second[36])));
    fflush(stdout);
    return lit2 >= GRID * GRID / 4 ? 0 : 1;
}
