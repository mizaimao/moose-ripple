/* Exercise SwapBuffers and GDI access to known pixels in the test's own
 * window. Set RIPPLE_GL_PRESENT to 0 or 1 before starting the process. */
#include <windows.h>
#include <winioctl.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include "rb2krvd.h"

static int counters(HDC dc, RB2K_GL_STATS *gl, RB2K_RVD_STATS *rvd)
{
    return ExtEscape(dc, RB2K_GL_STATS_ESCAPE, 0, NULL, sizeof(*gl), (LPSTR)gl) > 0 &&
        ExtEscape(dc, RB2K_RVD_STATS_ESCAPE, 0, NULL, sizeof(*rvd), (LPSTR)rvd) > 0;
}

static int profile(HDC dc, RB2K_GL_PROFILE *out)
{
    return ExtEscape(dc, RB2K_GL_PROFILE_ESCAPE, 0, NULL, sizeof(*out), (LPSTR)out) > 0;
}

static void scene(int width, int height, int parity)
{
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, width / 2, height / 2);
    glClearColor(parity ? 1.0f : 0.0f, parity ? 0.0f : 1.0f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static int check(HDC dc, int x, int y, COLORREF expected)
{
    COLORREF actual = GetPixel(dc, x, y);
    if (actual == expected) return 1;
    printf("GLP pixel (%d,%d) expected=%06lx got=%08lx\n", x, y, expected, actual);
    return 0;
}

static int check_uploads(HDC dc, BOOL inlineWrites)
{
    static const RECT boxes[] = {{16,20,29,37}, {40,20,104,84}, {112,20,177,21}};
    static const COLORREF colors[] = {RGB(231,17,83), RGB(14,181,92), RGB(226,106,7)};
    RB2K_GL_PROFILE before, after;
    int pass, n, ok = 1;
    if (!profile(dc, &before)) return 0;
    for (pass = 0; pass < 2; ++pass) {
        for (n = 0; n < 3; ++n) {
            HBRUSH brush = CreateSolidBrush(colors[(n + pass) % 3]);
            if (!brush) return 0;
            ok &= FillRect(dc, &boxes[n], brush) != 0;
            DeleteObject(brush);
        }
        GdiFlush();
        for (n = 0; n < 3; ++n) {
            ok &= check(dc, boxes[n].left, boxes[n].top, colors[(n + pass) % 3]);
            ok &= check(dc, boxes[n].right - 1, boxes[n].bottom - 1, colors[(n + pass) % 3]);
            ok &= check(dc, boxes[n].right, boxes[n].top, RGB(0,0,255));
        }
    }
    if (!profile(dc, &after)) return 0;
    printf("GLP GDI uploads inline_rects=%lu batches=%lu pixels=%s\n",
           after.inlineRects - before.inlineRects, after.inlineBatches - before.inlineBatches,
           ok ? "pass" : "fail");
    if (inlineWrites) ok &= after.inlineRects - before.inlineRects >= 4;
    else ok &= after.inlineRects == before.inlineRects;
    return ok;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pf;
    HWND window, cover = NULL;
    HDC dc, coverDC = NULL;
    POINT savedCursor;
    HGLRC ctx;
    LARGE_INTEGER frequency, start, stop;
    RB2K_GL_STATS before, after, read;
    RB2K_RVD_STATS rb0, rb1;
    RB2K_GL_PROFILE p0, p1;
    char mode[8] = "";
    const char *renderer;
    int format, width, height, x, y, n, good, full, direct, hidden = 0;
    BOOL cursor, covered, stripes, hold, uploads;
    const int frames = 240;
    if (argc < 2 || argc > 3 || (strcmp(argv[1], "window") && strcmp(argv[1], "full"))) return 2;
    cursor = argc == 3 && !strcmp(argv[2], "cursor");
    covered = argc == 3 && !strcmp(argv[2], "covered");
    stripes = argc == 3 && !strcmp(argv[2], "stripes");
    hold = argc == 3 && !strcmp(argv[2], "hold");
    uploads = argc == 3 && !strcmp(argv[2], "uploads");
    if (argc == 3 && !cursor && !covered && !stripes && !hold && !uploads) return 2;
    full = !strcmp(argv[1], "full");
    GetEnvironmentVariableA("RIPPLE_GL_PRESENT", mode, sizeof(mode));
    direct = !strcmp(mode, "1") || !strcmp(mode, "rect") || !strcmp(mode, "inline");
    width = full ? GetSystemMetrics(SM_CXSCREEN) : 640;
    height = full ? GetSystemMetrics(SM_CYSCREEN) : 480;
    x = full ? 0 : 64; y = full ? 0 : 80;
    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "RippleGLPresentTest";
    if (!RegisterClassA(&wc)) return 1;
    window = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "OpenGL presentation test",
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, x, y, width, height,
        NULL, NULL, wc.hInstance, NULL);
    if (!window) return 1;
    SetForegroundWindow(window);
    UpdateWindow(window);
    dc = GetDC(window);
    GetCursorPos(&savedCursor);
    if (cursor) SetCursorPos(x + width / 2, y + height / 2);
    else {
        do { ++hidden; } while (ShowCursor(FALSE) >= 0 && hidden < 16);
        SetCursor(NULL);
    }
    ZeroMemory(&pf, sizeof(pf));
    pf.nSize = sizeof(pf); pf.nVersion = 1;
    pf.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pf.iPixelType = PFD_TYPE_RGBA; pf.cColorBits = 32;
    format = ChoosePixelFormat(dc, &pf);
    if (!format || !SetPixelFormat(dc, format, &pf) || !(ctx = wglCreateContext(dc)) ||
        !wglMakeCurrent(dc, ctx)) return 1;
    renderer = (const char *)glGetString(GL_RENDERER);
    printf("GLP begin mode=%s shape=%s view=%s %dx%d renderer=%s\n", mode, argv[1],
        argc == 3 ? argv[2] : "hidden", width, height, renderer ? renderer : "(none)");
    fflush(stdout);
    if (!renderer || !strstr(renderer, "virgl")) return 1;
    glViewport(0, 0, width, height);
    if (covered || stripes) {
        RECT box = {0, 0, 80, 64};
        HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
        int left = stripes ? x : x + width / 2 - 40, top = stripes ? y : y + height / 2 - 32;
        if (stripes) { box.right = 120; box.bottom = height; }
        cover = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, "Clip check",
            WS_POPUP, left, top, box.right, box.bottom, window, NULL, wc.hInstance, NULL);
        if (!cover || !green) return 1;
        if (stripes) {
            HRGN combined = CreateRectRgn(0, 0, 0, 0);
            int j;
            if (!combined) return 1;
            for (j = 0; j < 20; ++j) {
                HRGN band = CreateRectRgn(6*j + 1, 0, 6*j + 3, height);
                if (!band || CombineRgn(combined, combined, band, RGN_OR) == ERROR) return 1;
                DeleteObject(band);
            }
            if (!SetWindowRgn(cover, combined, FALSE)) { DeleteObject(combined); return 1; }
        }
        ShowWindow(cover, SW_SHOWNOACTIVATE);
        coverDC = GetDC(cover);
        FillRect(coverDC, &box, green);
        DeleteObject(green);
        GdiFlush();
    }
    for (n = 0; n < 12; ++n) {
        scene(width, height, n & 1);
        if (!SwapBuffers(dc)) return 1;
    }
    Sleep(700);
    if (!counters(dc, &before, &rb0) || !profile(dc, &p0) || !QueryPerformanceFrequency(&frequency)) return 1;
    QueryPerformanceCounter(&start);
    for (n = 0; n < frames; ++n) {
        scene(width, height, n & 1);
        if (!SwapBuffers(dc)) return 1;
    }
    glFinish();
    QueryPerformanceCounter(&stop);
    if (!counters(dc, &after, &rb1) || !profile(dc, &p1)) return 1;
    printf("GLP timed frames=%d ms=%.2f gpu=%lu frame_readbacks=%lu desktop_readbacks=%lu uploads=%lu pixels_read=%lu full_reads=%lu last_read=%lux%lu\n",
        frames, 1000.0 * (stop.QuadPart - start.QuadPart) / frequency.QuadPart,
        after.frames - before.frames, rb1.calls[14] - rb0.calls[14],
        after.readbacks - before.readbacks, after.uploads - before.uploads,
        after.readbackPixels - before.readbackPixels, after.fullFrameReads - before.fullFrameReads,
        after.lastReadWidth, after.lastReadHeight);
    printf("GLP copy rects=%lu fences=%lu\n", p1.rectangles - p0.rectangles, p1.fences - p0.fences);
    for (n = 0; n < RB2K_GL_WRITERS; ++n) {
        const RB2K_GL_WRITER *a = &p0.writers[n], *b = &p1.writers[n];
        if (b->uploads != a->uploads)
            printf("GLP paint tag=%lu calls=%lu small=%lu excluded=%lu last=%lux%lu\n", b->tag,
                   b->uploads - a->uploads, b->small - a->small, b->excluded - a->excluded,
                   b->width, b->height);
    }
    {
        BOOL foreground = GetForegroundWindow() == window;
        GLenum error = glGetError();
        good = foreground && error == GL_NO_ERROR;
        if (!good) printf("GLP foreground=%d gl_error=%04x\n", (int)foreground, (unsigned)error);
    }
    good &= check(dc, width / 4, height / 4, RGB(0, 0, 255));
    good &= check(dc, width / 4, height * 3 / 4, RGB(255, 0, 0));
    good &= check(dc, width * 3 / 4, height * 3 / 4, RGB(0, 0, 255));
    if (covered) good &= check(coverDC, 40, 32, RGB(0, 255, 0));
    if (stripes) {
        good &= check(coverDC, 1, height / 2, RGB(0, 255, 0));
        good &= check(dc, 4, height * 3 / 4, RGB(255, 0, 0));
        if (direct) good &= p1.rectangles - p0.rectangles >= 20u * frames;
    }
    if (!counters(dc, &read, &rb1)) return 1;
    printf("GLP test-pixel reads triggered %lu desktop readbacks\n", read.readbacks - after.readbacks);
    if (direct) good &= after.frames - before.frames == (ULONG)frames &&
        (!strcmp(mode, "rect") ? p1.fences - p0.fences == p1.rectangles - p0.rectangles :
                                 p1.fences - p0.fences == (ULONG)frames) &&
        (cursor || after.readbackPixels - before.readbackPixels < (ULONG)width * height) &&
        after.fullFrameReads == before.fullFrameReads &&
        rb1.calls[14] == rb0.calls[14];
    else good &= after.frames == before.frames && rb1.calls[14] - rb0.calls[14] >= (ULONG)frames;
    if (uploads) good &= check_uploads(dc, !strcmp(mode, "inline"));
    if (hold && good && direct) {
        scene(width, height, 1);
        if (!SwapBuffers(dc) || !counters(dc, &after, &rb1) || after.frames != read.frames + 1) return 1;
        puts("GLP HOLD holding fresh GPU quarter-red blue frame for 150 seconds");
        fflush(stdout);
        Sleep(150000);
    }
    if (cover) { ReleaseDC(cover, coverDC); DestroyWindow(cover); }
    while (hidden > 0) { ShowCursor(TRUE); --hidden; }
    if (cursor) SetCursorPos(savedCursor.x, savedCursor.y);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(ctx);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    printf("GLP %s\n", good ? "PASS" : "FAIL");
    return good ? 0 : 1;
}
