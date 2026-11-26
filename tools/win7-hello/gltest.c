/*
 * A minimal OpenGL check for Windows (docs/windows/mesa-win7.md): a window, a WGL context, one
 * triangle, and a report of the renderer and the colour read back at the triangle's centre,
 * written to gltest.txt next to the program and shown in a message box. Put Mesa's
 * opengl32.dll (with libgallium_wgl.dll and libglapi.dll) next to it to test Mesa.
 * With an argument (a frame count, default 120) it asks nothing: it draws that many frames,
 * then also reads the triangle's pixel off the screen, to tell whether the frames arrived there.
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = { CS_OWNDC, DefWindowProcA, 0, 0, inst, 0, 0, 0, 0, "rbgltest" };
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                  PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    unsigned char px[4] = { 0 };
    char msg[1024];
    HWND w;
    HDC dc;
    HGLRC rc;
    FILE *f;
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
        f = fopen("gltest.txt", "wb");
        if (f) { fputs("no OpenGL context\r\n", f); fclose(f); }
        return 1;
    }
    t0 = GetTickCount();
    for (i = 0; i < frames; i++) {
        MSG m;
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        glViewport(0, 0, 64, 64);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex2f(-1.0f, -1.0f); glVertex2f(1.0f, -1.0f); glVertex2f(0.0f, 1.0f);
        glEnd();
        if (i == 0) {
            glFinish();
            glReadPixels(32, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
        SwapBuffers(dc);
    }
    t0 = GetTickCount() - t0;
    snprintf(msg, sizeof(msg), "GL_VENDOR: %s\r\nGL_RENDERER: %s\r\nGL_VERSION: %s\r\n"
             "pixel in the triangle: %u %u %u (red expected: 255 0 0)\r\n",
             (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
             (const char *)glGetString(GL_VERSION), px[0], px[1], px[2]);
    if (quiet) {
        RECT rc2;
        POINT p;
        COLORREF c;
        HDC screen = GetDC(0);
        Sleep(500);
        GetClientRect(w, &rc2);
        p.x = 32; p.y = rc2.bottom - 1 - 20;
        ClientToScreen(w, &p);
        c = GetPixel(screen, p.x, p.y);
        ReleaseDC(0, screen);
        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg),
                 "the same pixel on the screen: %u %u %u\r\n%d frames in %lu ms\r\n",
                 (unsigned)GetRValue(c), (unsigned)GetGValue(c), (unsigned)GetBValue(c), frames, (unsigned long)t0);
    }
    f = fopen("gltest.txt", "wb");
    if (f) { fputs(msg, f); fclose(f); }
    if (!quiet) MessageBoxA(w, msg, "moose-ripple GL test", MB_OK);
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    return 0;
}
