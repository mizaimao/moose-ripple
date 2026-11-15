/*
 * gltest: a small OpenGL 1.1 check for Windows 98 (and anything later).
 *
 *   gltest [seconds] [width height]
 *
 * Opens a window, makes an OpenGL context the way a period game does (ChoosePixelFormat,
 * wglCreateContext), prints who draws (GL_VENDOR, GL_RENDERER, GL_VERSION), then draws a
 * frame with known colours: a clear, a smooth-shaded triangle, a depth-tested pair of quads
 * and a textured quad. It reads pixels back and compares them with what must be there, then
 * spins the scene for the given number of seconds (default 3) and prints the frame rate.
 *
 * Exit status 0 when every pixel check passed. Output goes to standard output; run it as
 *   gltest > out.txt
 * Built by tools/win9x/build-tools.sh.
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* Windows 9x keeps its windows and drawing objects in small fixed heaps ("system resources");
   a driver that loses a little of them every frame takes the whole system down. Windows 98's
   Resource Meter library tells how much is free (0 system, 1 GDI, 2 USER). */
static void resources(const char *when)
{
    typedef int (WINAPI *free_fn)(int);
    HMODULE lib = LoadLibrary("rsrc32.dll");
    free_fn free_resources = lib ? (free_fn)GetProcAddress(lib, "_MyGetFreeSystemResources32@4") : NULL;
    MEMORYSTATUS ms;

    ms.dwLength = sizeof ms;
    GlobalMemoryStatus(&ms);
    printf("%s: ", when);
    if (free_resources)
        printf("free resources: system %d%%, GDI %d%%, USER %d%%; ", free_resources(0),
               free_resources(1), free_resources(2));
    printf("free memory %lu of %lu MB\n", (unsigned long)(ms.dwAvailPhys >> 20),
           (unsigned long)(ms.dwTotalPhys >> 20));
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

/* The pixel at x,y (from the bottom left) must be r,g,b, give or take a little (16-bit screens
   and dithering round colours). */
static void expect(const char *what, int x, int y, int r, int g, int b)
{
    unsigned char p[4] = { 0, 0, 0, 0 };
    int ok;

    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    ok = abs(p[0] - r) <= 12 && abs(p[1] - g) <= 12 && abs(p[2] - b) <= 12;
    printf("  %-28s at %3d,%3d: %3d %3d %3d  %s\n", what, x, y, p[0], p[1], p[2],
           ok ? "ok" : "WRONG");
    if (!ok) {
        printf("    expected %d %d %d\n", r, g, b);
        failures++;
    }
}

static void scene(float angle, GLuint tex)
{
    glClearColor(0.0f, 0.0f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Top left quarter: a smooth-shaded triangle, spinning. */
    glPushMatrix();
    glTranslatef(-0.5f, 0.5f, 0);
    glRotatef(angle, 0, 0, 1);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-0.4f, -0.4f);
    glColor3f(0, 1, 0); glVertex2f(0.4f, -0.4f);
    glColor3f(0, 0, 1); glVertex2f(0.0f, 0.4f);
    glEnd();
    glPopMatrix();

    /* Top right: depth test. The green quad is nearer (with glOrtho, larger z is
       nearer) and drawn first; red must not cover it. */
    glEnable(GL_DEPTH_TEST);
    glBegin(GL_QUADS);
    glColor3f(0, 1, 0);
    glVertex3f(0.3f, 0.3f, 0.5f); glVertex3f(0.7f, 0.3f, 0.5f);
    glVertex3f(0.7f, 0.7f, 0.5f); glVertex3f(0.3f, 0.7f, 0.5f);
    glColor3f(1, 0, 0);
    glVertex3f(0.2f, 0.2f, -0.5f); glVertex3f(0.8f, 0.2f, -0.5f);
    glVertex3f(0.8f, 0.8f, -0.5f); glVertex3f(0.2f, 0.8f, -0.5f);
    glEnd();
    glDisable(GL_DEPTH_TEST);

    /* Bottom left: a 2x2 texture, magnified without filtering: four flat squares. */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-0.9f, -0.9f);
    glTexCoord2f(1, 0); glVertex2f(-0.1f, -0.9f);
    glTexCoord2f(1, 1); glVertex2f(-0.1f, -0.1f);
    glTexCoord2f(0, 1); glVertex2f(-0.9f, -0.1f);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    /* Bottom right: blending. Half-transparent white over the blue clear colour. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 0.5f);
    glBegin(GL_QUADS);
    glVertex2f(0.1f, -0.9f); glVertex2f(0.9f, -0.9f);
    glVertex2f(0.9f, -0.1f); glVertex2f(0.1f, -0.1f);
    glEnd();
    glDisable(GL_BLEND);
}

int main(int argc, char **argv)
{
    static const unsigned char texels[2 * 2 * 3] = {
        255, 0, 0,     0, 255, 0,           /* bottom row: red, green */
        0, 0, 255,     255, 255, 0,         /* top row: blue, yellow */
    };
    int seconds = argc > 1 ? atoi(argv[1]) : 3;
    int w = argc > 3 ? atoi(argv[2]) : 320, h = argc > 3 ? atoi(argv[3]) : 240;
    WNDCLASS wc;
    PIXELFORMATDESCRIPTOR pfd;
    RECT r;
    HWND win;
    HDC dc;
    HGLRC rc;
    GLuint tex;
    DWORD start, now;
    int format, frames = 0;
    MSG msg;

    resources("at the start");
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "gltest";
    RegisterClass(&wc);
    SetRect(&r, 0, 0, w, h);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    win = CreateWindow("gltest", "gltest", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                       40, 40, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!win) {
        printf("CreateWindow failed, error %lu\n", GetLastError());
        return 2;
    }
    ShowWindow(win, SW_SHOWNORMAL);
    UpdateWindow(win);
    dc = GetDC(win);

    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = (BYTE)GetDeviceCaps(dc, BITSPIXEL);
    pfd.cDepthBits = 16;
    format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd)) {
        printf("no pixel format (ChoosePixelFormat gave %d), error %lu\n", format, GetLastError());
        return 2;
    }
    DescribePixelFormat(dc, format, sizeof pfd, &pfd);
    rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        printf("no OpenGL context, error %lu\n", GetLastError());
        return 2;
    }
    printf("pixel format %d: %d colour bits, %d depth bits, %d stencil bits%s%s\n", format,
           pfd.cColorBits, pfd.cDepthBits, pfd.cStencilBits,
           (pfd.dwFlags & PFD_GENERIC_FORMAT) ? ", generic (Microsoft's software OpenGL)" : "",
           (pfd.dwFlags & PFD_GENERIC_ACCELERATED) ? ", MCD" : "");
    printf("GL_VENDOR:   %s\nGL_RENDERER: %s\nGL_VERSION:  %s\n",
           (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));

    /* Frames as fast as they come, as games of the time ask for (r_swapInterval 0). */
    {
        typedef BOOL (WINAPI *swap_interval_fn)(int);
        swap_interval_fn swap_interval = (swap_interval_fn)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap_interval)
            swap_interval(0);
        printf("wglSwapIntervalEXT: %s\n", swap_interval ? "set to 0" : "not there");
    }

    glViewport(0, 0, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);

    scene(0, tex);
    glFinish();
    printf("pixel checks (window %dx%d):\n", w, h);
    expect("clear colour", w / 2, h / 2, 0, 0, 102);
    expect("triangle, centre", w / 4, h * 41 / 60, 85, 85, 85);
    expect("depth test: near quad", w * 3 / 4, h * 3 / 4, 0, 255, 0);
    expect("depth test: far quad", w * 5 / 8 - w / 80, h * 3 / 4, 255, 0, 0);
    expect("texture: red texel", w / 8, h / 8, 255, 0, 0);
    expect("texture: green texel", w * 3 / 8, h / 8, 0, 255, 0);
    expect("texture: blue texel", w / 8, h * 3 / 8, 0, 0, 255);
    expect("texture: yellow texel", w * 3 / 8, h * 3 / 8, 255, 255, 0);
    expect("blend: white on blue", w * 3 / 4, h / 4, 128, 128, 179);
    SwapBuffers(dc);

    start = GetTickCount();
    for (;;) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto done;
            DispatchMessage(&msg);
        }
        now = GetTickCount();
        if (now - start >= (DWORD)seconds * 1000)
            break;
        scene((now - start) * 0.09f, tex);
        SwapBuffers(dc);
        frames++;
    }
done:
    now = GetTickCount();
    if (now > start && frames)
        printf("%d frames in %.1f s: %.1f frames a second\n", frames, (now - start) / 1000.0,
               frames * 1000.0 / (now - start));
    printf(failures ? "gltest FAILED: %d pixel checks wrong\n" : "gltest passed\n", failures);
    fflush(stdout);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(win, dc);
    DestroyWindow(win);
    resources("at the end");
    return failures ? 1 : 0;
}
