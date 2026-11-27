/* gammatest: the display's gamma ramp (GDI's SetDeviceGammaRamp), for checking that a ramp a
 * program sets reaches the picture that is shown. The shown picture cannot be read back in the
 * guest (the ramp is applied after everything a program can read), so the host captures the
 * screen while this draws known colours.
 *
 *   gammatest get            the ramp in force: entries 0, 64, 128, 192 and 255 of each channel
 *   gammatest set NAME       identity | double | half | invert | red (red doubled, the rest kept)
 *   gammatest bars SECONDS   a full-screen OpenGL window, redrawn ten times a second for SECONDS:
 *                            16 columns with the values 0, 17, 34 .. 255; the top half grey,
 *                            below it a row of red, one of green and one of blue
 *
 * A capture's pixel in column k must be the ramp's entry for 17 * k in each channel drawn.
 * Lines start with "GAMMATEST"; the exit code is 0 when the call worked. Plain C89 for Visual
 * C++ 6: cl /W3 /O2 /MT gammatest.c /link user32.lib gdi32.lib opengl32.lib. MIT licence. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static WORD ramp[3][256];

static int fill(const char *name)
{
    int i, c;
    for (i = 0; i < 256; ++i) {
        int v = i;
        if (!strcmp(name, "identity")) v = i;
        else if (!strcmp(name, "double") || !strcmp(name, "red")) v = i * 2 > 255 ? 255 : i * 2;
        else if (!strcmp(name, "half")) v = i / 2;
        else if (!strcmp(name, "invert")) v = 255 - i;
        else return 0;
        for (c = 0; c < 3; ++c)
            ramp[c][i] = (WORD)((!strcmp(name, "red") && c ? i : v) * 257);
    }
    return 1;
}

static int get(void)
{
    static const int at[5] = { 0, 64, 128, 192, 255 };
    HDC dc = GetDC(NULL);
    BOOL ok = dc && GetDeviceGammaRamp(dc, ramp);
    int c, k;
    if (dc) ReleaseDC(NULL, dc);
    if (!ok) {
        printf("GAMMATEST get failed error=%lu\n", GetLastError());
        return 1;
    }
    for (c = 0; c < 3; ++c) {
        printf("GAMMATEST get %s", c == 0 ? "red" : c == 1 ? "green" : "blue");
        for (k = 0; k < 5; ++k) printf(" %d=%u", at[k], ramp[c][at[k]] >> 8);
        printf("\n");
    }
    return 0;
}

static int set(const char *name)
{
    HDC dc;
    BOOL ok;
    if (!fill(name)) {
        printf("GAMMATEST set: no ramp called %s\n", name);
        return 2;
    }
    dc = GetDC(NULL);
    ok = dc && SetDeviceGammaRamp(dc, ramp);
    if (dc) ReleaseDC(NULL, dc);
    printf("GAMMATEST set %s %s", name, ok ? "done" : "failed");
    if (!ok) printf(" error=%lu", GetLastError());
    printf("\n");
    return ok ? 0 : 1;
}

static void cell(int x0, int y0, int x1, int y1, int r, int g, int b)
{
    glScissor(x0, y0, x1 - x0, y1 - y0);
    glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static int bars(int seconds)
{
    static PIXELFORMATDESCRIPTOR pfd;
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN), frames, k, format;
    HWND window = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "gammatest", WS_POPUP | WS_VISIBLE,
                                  0, 0, w, h, NULL, NULL, GetModuleHandleA(NULL), NULL);
    HDC dc = window ? GetDC(window) : NULL;
    HGLRC rc;
    MSG msg;

    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    format = dc ? ChoosePixelFormat(dc, &pfd) : 0;
    if (!format || !SetPixelFormat(dc, format, &pfd) || !(rc = wglCreateContext(dc)) ||
        !wglMakeCurrent(dc, rc)) {
        printf("GAMMATEST bars: no OpenGL window error=%lu\n", GetLastError());
        return 1;
    }
    printf("GAMMATEST bars %dx%d renderer=%s\n", w, h, (const char *)glGetString(GL_RENDERER));
    glEnable(GL_SCISSOR_TEST);
    for (frames = 0; frames < seconds * 10; ++frames) {
        for (k = 0; k < 16; ++k) {
            int x0 = w * k / 16, x1 = w * (k + 1) / 16, v = 17 * k;
            cell(x0, h / 2, x1, h, v, v, v);                    /* OpenGL's rows count upward */
            cell(x0, h / 3, x1, h / 2, v, 0, 0);
            cell(x0, h / 6, x1, h / 3, 0, v, 0);
            cell(x0, 0, x1, h / 6, 0, 0, v);
        }
        SwapBuffers(dc);
        if (frames == 5) printf("GAMMATEST bars shown\n");
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        Sleep(100);
    }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    printf("GAMMATEST bars end\n");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "get")) return get();
    if (argc >= 3 && !strcmp(argv[1], "set")) return set(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "bars")) return bars(atoi(argv[2]));
    printf("GAMMATEST usage: gammatest get | set identity|double|half|invert|red | bars SECONDS\n");
    return 2;
}
