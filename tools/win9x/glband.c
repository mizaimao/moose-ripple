/*
 * glband: where a window's OpenGL frames land on the screen, and which way up.
 *
 *   glband [seconds [x y]]
 *
 * Opens a 640x480 OpenGL window (client area at x, y; default 100, 100) and presents the same
 * picture as fast as it can for the given seconds (default 10): four horizontal bands, red at
 * the top, then green, blue and yellow at the bottom, with a white 64x64 square in the top-left
 * corner. A capture of the screen shows at a glance whether the frames are in place and upright
 * (docs/win9x/mesa.md). Prints the frame count to standard output and to QEMU's debug port
 * (0xE9), where nothing listens on a real PC.
 *
 * Built by tools/win9x/build-tools.sh.
 */
#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <GL/gl.h>

static void say(const char *p)
{
    fputs(p, stdout);
    while (*p)
        _outp(0xE9, *p++);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    return DefWindowProc(w, m, a, b);
}

static void band(int y, int h, float r, float g, float b)
{
    glScissor(0, y, 640, h);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

int main(int argc, char **argv)
{
    WNDCLASS wc;
    PIXELFORMATDESCRIPTOR pfd;
    RECT r;
    HWND win;
    HDC dc;
    HGLRC rc;
    MSG msg;
    DWORD t0, frames = 0, seconds = argc > 1 ? (DWORD)atoi(argv[1]) : 10;
    int x = argc > 3 ? atoi(argv[2]) : 100, y = argc > 3 ? atoi(argv[3]) : 100;
    char line[96];

    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "glband";
    RegisterClass(&wc);
    SetRect(&r, 0, 0, 640, 480);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    win = CreateWindow("glband", "glband", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                       x + r.left, y + r.top, r.right - r.left, r.bottom - r.top,
                       NULL, NULL, wc.hInstance, NULL);
    dc = GetDC(win);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        say("glband: no OpenGL context\r\n");
        return 2;
    }
    glViewport(0, 0, 640, 480);
    glEnable(GL_SCISSOR_TEST);
    say("glband: begin\r\n");
    t0 = GetTickCount();
    while (GetTickCount() - t0 < seconds * 1000) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        band(360, 120, 1, 0, 0);            /* OpenGL counts rows from the bottom */
        band(240, 120, 0, 1, 0);
        band(120, 120, 0, 0, 1);
        band(0, 120, 1, 1, 0);
        glScissor(0, 480 - 64, 64, 64);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(dc);
        frames++;
    }
    sprintf(line, "glband: end, %lu frames in %lu s\r\n", frames, seconds);
    say(line);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(win, dc);
    DestroyWindow(win);
    return 0;
}
