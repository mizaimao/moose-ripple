/*
 * A minimal OpenGL check for Windows (docs/windows/mesa-win7.md): a window, a WGL context, one
 * triangle, and a report of the renderer and the colour read back at the triangle's centre,
 * written to gltest.txt next to the program and shown in a message box. Put Mesa's
 * opengl32.dll (with libgallium_wgl.dll and libglapi.dll) next to it to test Mesa.
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

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
    (void)prev; (void)cmd; (void)show;

    RegisterClassA(&wc);
    w = CreateWindowA("rbgltest", "moose-ripple GL test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                      100, 100, 256, 256, 0, 0, inst, 0);
    dc = GetDC(w);
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        MessageBoxA(w, "no OpenGL context", "moose-ripple GL test", MB_OK);
        return 1;
    }
    glViewport(0, 0, 64, 64);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f); glVertex2f(1.0f, -1.0f); glVertex2f(0.0f, 1.0f);
    glEnd();
    glFinish();
    glReadPixels(32, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    SwapBuffers(dc);
    snprintf(msg, sizeof(msg), "GL_VENDOR: %s\r\nGL_RENDERER: %s\r\nGL_VERSION: %s\r\n"
             "pixel in the triangle: %u %u %u (red expected: 255 0 0)\r\n",
             (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
             (const char *)glGetString(GL_VERSION), px[0], px[1], px[2]);
    f = fopen("gltest.txt", "wb");
    if (f) { fputs(msg, f); fclose(f); }
    MessageBoxA(w, msg, "moose-ripple GL test", MB_OK);
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    return 0;
}
