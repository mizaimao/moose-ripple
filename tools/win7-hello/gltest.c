/*
 * A minimal OpenGL check for Windows (docs/windows/mesa-win7.md): a window, a WGL context, one
 * triangle, and a report of the renderer and the colour read back at the triangle's centre,
 * written to gltest.txt next to the program and shown in a message box. Put Mesa's
 * opengl32.dll (with libgallium_wgl.dll and libglapi.dll) next to it to test Mesa.
 * With an argument (a frame count, default 120) it asks nothing: it draws that many frames,
 * then also reads the triangle's pixel off the screen, to tell whether the frames arrived there,
 * does both again after resizing its window, and times 30 frames at swap interval 1 (a
 * watchdog ends the program if a swap never returns).
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char msg[2048];

static void save(void)
{
    FILE *f = fopen("gltest.txt", "wb");
    if (f) { fputs(msg, f); fclose(f); }
}

static void add(const char *format, ...)
{
    va_list args;
    size_t n = strlen(msg);
    va_start(args, format);
    vsnprintf(msg + n, sizeof(msg) - n, format, args);
    va_end(args);
    save();
}

static void frame(HDC dc)
{
    MSG m;
    (void)dc;
    while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
    glViewport(0, 0, 64, 64);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f); glVertex2f(1.0f, -1.0f); glVertex2f(0.0f, 1.0f);
    glEnd();
}

/* The colour on the screen where the window shows the triangle's pixel (32, 20). */
static COLORREF on_screen(HWND w)
{
    RECT r;
    POINT p;
    COLORREF c;
    HDC screen = GetDC(0);
    Sleep(500);
    GetClientRect(w, &r);
    p.x = 32; p.y = r.bottom - 1 - 20;
    ClientToScreen(w, &p);
    c = GetPixel(screen, p.x, p.y);
    ReleaseDC(0, screen);
    return c;
}

static DWORD WINAPI watchdog(void *what)
{
    Sleep(20000);
    add("%s: no return in 20 s\r\n", (const char *)what);
    ExitProcess(3);
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = { CS_OWNDC, DefWindowProcA, 0, 0, inst, 0, 0, 0, 0, "rbgltest" };
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                  PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    unsigned char px[4] = { 0 };
    HWND w;
    HDC dc;
    HGLRC rc;
    int quiet = cmd && *cmd, frames = quiet ? atoi(cmd) : 1, i;
    DWORD t0;
    (void)prev; (void)show;
    if (frames < 1) frames = 120;

    RegisterClassA(&wc);
    w = CreateWindowA("rbgltest", "moose-ripple GL test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                      100, 100, 256, 256, 0, 0, inst, 0);
    dc = GetDC(w);
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        if (!quiet) MessageBoxA(w, "no OpenGL context", "moose-ripple GL test", MB_OK);
        add("no OpenGL context\r\n");
        return 1;
    }
    t0 = GetTickCount();
    for (i = 0; i < frames; i++) {
        frame(dc);
        if (i == 0) {
            glFinish();
            glReadPixels(32, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
        SwapBuffers(dc);
    }
    t0 = GetTickCount() - t0;
    add("GL_VENDOR: %s\r\nGL_RENDERER: %s\r\nGL_VERSION: %s\r\n"
             "pixel in the triangle: %u %u %u (red expected: 255 0 0)\r\n",
             (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
             (const char *)glGetString(GL_VERSION), px[0], px[1], px[2]);
    {
        /* The same question for memory that never was a window buffer: a small texture, set
         * and read back. */
        static const unsigned char red[4 * 4 * 4] = {
            255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
            255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
            255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
            255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255 };
        unsigned char back[4 * 4 * 4] = { 0 };
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
        glFinish();
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
        glReadBuffer(GL_FRONT);
        memset(px, 0, sizeof(px));
        glReadPixels(32, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glReadBuffer(GL_BACK);
        add("a texture read back: %u %u %u; the front buffer after the swaps: %u %u %u\r\n",
            back[0], back[1], back[2], px[0], px[1], px[2]);
        glDeleteTextures(1, &tex);
    }
    if (quiet) {
        typedef BOOL (WINAPI *swap_interval_t)(int);
        swap_interval_t swap_interval = (swap_interval_t)(void *)wglGetProcAddress("wglSwapIntervalEXT");
        COLORREF c = on_screen(w);
        add("the same pixel on the screen: %u %u %u\r\n%d frames in %lu ms\r\n",
            (unsigned)GetRValue(c), (unsigned)GetGValue(c), (unsigned)GetBValue(c), frames, (unsigned long)t0);

        SetWindowPos(w, 0, 0, 0, 420, 340, SWP_NOMOVE | SWP_NOZORDER);
        memset(px, 0, sizeof(px));
        for (i = 0; i < 10; i++) {
            frame(dc);
            if (i == 9) { glFinish(); glReadPixels(32, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); }
            SwapBuffers(dc);
        }
        c = on_screen(w);
        add("after a resize to 420x340: %u %u %u, on the screen: %u %u %u\r\n", px[0], px[1], px[2],
            (unsigned)GetRValue(c), (unsigned)GetGValue(c), (unsigned)GetBValue(c));

        if (swap_interval) {
            CreateThread(0, 0, watchdog, (void *)"swap interval", 0, 0);
            swap_interval(0);
            t0 = GetTickCount();
            for (i = 0; i < 300; i++) { frame(dc); SwapBuffers(dc); }
            t0 = GetTickCount() - t0;
            add("300 frames at swap interval 0 in %lu ms\r\n", (unsigned long)t0);
            swap_interval(1);
            t0 = GetTickCount();
            for (i = 0; i < 30; i++) { frame(dc); SwapBuffers(dc); }
            t0 = GetTickCount() - t0;
            swap_interval(0);
            add("30 frames at swap interval 1 in %lu ms\r\n", (unsigned long)t0);
        } else {
            add("no wglSwapIntervalEXT\r\n");
        }
    }
    if (!quiet) MessageBoxA(w, msg, "moose-ripple GL test", MB_OK);
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    return 0;
}
