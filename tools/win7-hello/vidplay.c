/* Does a video show a picture? Plays a file in a window of its own for a few seconds through
 * one of Windows' two playback pipelines, both ending in the Enhanced Video Renderer, and
 * reads the screen where the window is:
 *   vidplay ds FILE [SECONDS [WIDTH HEIGHT]]   DirectShow: a filter graph with the renderer added first
 *   vidplay mf FILE [SECONDS]                  Media Foundation, through MFPlay
 * WIDTH and HEIGHT: the picture's size in the window, by default 640 by 360.
 * Prints how many of 64 points spread over the picture are not black, twice, a second apart,
 * and whether the picture changed in between; then how much rows next to each other differ
 * against rows two apart, which shows the combing of interlaced video that is not
 * deinterlaced. Exit code 0 if at least a quarter of the points are lit.
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

enum { GRID = 8 };
/* The picture's size in the window. Given as the video's own size, the picture is shown
 * row for row (and stretched to the window, not letterboxed), which the combing figure needs. */
static int WIDTH = 640, HEIGHT = 360;
static int fit;

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

/* Combing: an interlaced frame shown whole has its two fields, taken a sixtieth of a second
 * apart, on alternate rows, so where something moves, rows next to each other differ more
 * than rows two apart. A block in the middle of the picture is read and both differences
 * summed; the result is the first as a percentage of the second. A picture without combing
 * stays well below 100. */
static int combing(HWND window)
{
    enum { SIDE = 160 };
    static DWORD pixels[SIDE * SIDE];
    BITMAPINFO info = { { sizeof(BITMAPINFOHEADER), SIDE, -SIDE, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    POINT corner = { (WIDTH - SIDE) / 2, (HEIGHT - SIDE) / 2 };
    ClientToScreen(window, &corner);
    HDC screen = GetDC(NULL);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, SIDE, SIDE);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, SIDE, SIDE, screen, corner.x, corner.y, SRCCOPY);
    SelectObject(memory, old);
    GetDIBits(memory, bitmap, 0, SIDE, pixels, &info, DIB_RGB_COLORS);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, screen);
    long long next = 0, second = 0;
    for (int y = 0; y + 2 < SIDE; ++y) {
        for (int x = 0; x < SIDE; ++x) {
            const int a = (pixels[y * SIDE + x] >> 8) & 0xff;
            const int b = (pixels[(y + 1) * SIDE + x] >> 8) & 0xff;
            const int c = (pixels[(y + 2) * SIDE + x] >> 8) & 0xff;
            next += a > b ? a - b : b - a;
            second += a > c ? a - c : c - a;
        }
    }
    return second ? (int)(100 * next / second) : 0;
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
    if (SUCCEEDED(hr) && fit) hr = IMFVideoDisplayControl_SetAspectRatioMode(display, MFVideoARMode_None);
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
        printf("usage: vidplay ds|mf FILE [SECONDS [WIDTH HEIGHT]]\n");
        return 2;
    }
    const int seconds = argc > 3 ? atoi(argv[3]) : 4;
    if (argc > 5 && atoi(argv[4]) >= 160 && atoi(argv[5]) >= 160) {
        WIDTH = atoi(argv[4]);
        HEIGHT = atoi(argv[5]);
        fit = 1;
    }
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
    int worst = 0, sum = 0;
    for (int i = 0; i < 8; ++i) {
        const int now = combing(window);
        if (now > worst) worst = now;
        sum += now;
        pump(150);
    }
    printf("rows next to each other differ by %d%% of rows two apart on average, %d%% at most (8 looks)\n",
           sum / 8, worst);
    fflush(stdout);
    return lit2 >= GRID * GRID / 4 ? 0 : 1;
}
