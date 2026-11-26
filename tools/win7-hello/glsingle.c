/*
 * The single-buffered case of gltest.c: a window whose pixel format has no back buffer, a
 * triangle drawn straight to the front buffer and flushed, and a report (glsingle.txt, next
 * to where it is started) of the renderer, the pixel format's flags, and the triangle's
 * pixel as read back and as it is on the screen. A window like this does not get the
 * OpenGL driver's Direct3D framebuffer; it shows what the fallback does.
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = { CS_OWNDC, DefWindowProcA, 0, 0, inst, 0, 0, 0, 0, "rbglsingle" };
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
                                  PFD_TYPE_RGBA, 32 }, got;
    unsigned char px[4] = { 0 };
    HWND w;
    HDC dc, screen;
    HGLRC rc;
    FILE *f;
    RECT r;
    POINT p;
    COLORREF c;
    int format, i;
    (void)prev; (void)cmd; (void)show;

    RegisterClassA(&wc);
    w = CreateWindowA("rbglsingle", "moose-ripple GL single", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                      100, 100, 256, 256, 0, 0, inst, 0);
    dc = GetDC(w);
    format = ChoosePixelFormat(dc, &pfd);
    SetPixelFormat(dc, format, &pfd);
    DescribePixelFormat(dc, format, sizeof(got), &got);
    rc = wglCreateContext(dc);
    f = fopen("glsingle.txt", "wb");
    if (!f) return 2;
    if (!rc || !wglMakeCurrent(dc, rc)) { fputs("no OpenGL context\r\n", f); fclose(f); return 1; }
    for (i = 0; i < 30; i++) {
        MSG m;
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        glViewport(0, 0, 64, 64);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex2f(-1.0f, -1.0f); glVertex2f(1.0f, -1.0f); glVertex2f(0.0f, 1.0f);
        glEnd();
        glFlush();
        Sleep(20);
    }
    glFinish();
    glReadPixels(32, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    Sleep(500);
    GetClientRect(w, &r);
    p.x = 32; p.y = r.bottom - 1 - 20;
    ClientToScreen(w, &p);
    screen = GetDC(0);
    c = GetPixel(screen, p.x, p.y);
    ReleaseDC(0, screen);
    fprintf(f, "GL_RENDERER: %s\r\npixel format %d: %s, %s\r\n"
            "pixel in the triangle: %u %u %u, on the screen: %u %u %u (red expected: 255 0 0)\r\n",
            (const char *)glGetString(GL_RENDERER), format,
            (got.dwFlags & PFD_DOUBLEBUFFER) ? "double-buffered" : "single-buffered",
            (got.dwFlags & PFD_SUPPORT_GDI) ? "GDI-compatible" : "not GDI-compatible",
            px[0], px[1], px[2], (unsigned)GetRValue(c), (unsigned)GetGValue(c), (unsigned)GetBValue(c));
    fclose(f);
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    return 0;
}
