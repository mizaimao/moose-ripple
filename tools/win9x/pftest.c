/*
 * pftest: which pixel format Windows' ChoosePixelFormat gives the requests programs of the time
 * make, and whether it is the OpenGL driver's (accelerated) or one of Windows' own software
 * formats (PFD_GENERIC_FORMAT without PFD_GENERIC_ACCELERATED). A program that gets a software
 * format runs on Microsoft's OpenGL 1.1 ("GDI Generic") instead of the driver, without telling.
 *
 *   pftest            prints the formats and the choices; exit status 0 when the driver offers
 *                     formats and every request in the screen's colour depth got one of them
 *                     (a request in another depth may get a software format)
 *
 * Built by tools/win9x/build-tools.sh (Visual C++ 6).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

struct request {
    const char *who;
    BYTE color, depth, stencil;
    DWORD flags;
};

/* What the programs ask for (from their sources or logs). Colour 0: the screen's depth. */
static const struct request requests[] = {
    { "Unreal Tournament, 32-bit", 24, 32, 8, PFD_DOUBLEBUFFER },
    { "Unreal Tournament, 16-bit", 16, 16, 0, PFD_DOUBLEBUFFER },
    { "Quake III Arena",           32, 24, 8, PFD_DOUBLEBUFFER },
    { "screen depth, 16-bit z",     0, 16, 0, PFD_DOUBLEBUFFER },
    { "screen depth, 24-bit z",     0, 24, 0, PFD_DOUBLEBUFFER },
    { "screen depth, 32-bit z",     0, 32, 0, PFD_DOUBLEBUFFER },
    { "screen depth, stencil",      0, 24, 8, PFD_DOUBLEBUFFER },
    { "single-buffered",            0, 16, 0, 0 },
};

static int is_software(const PIXELFORMATDESCRIPTOR *p)
{
    return (p->dwFlags & PFD_GENERIC_FORMAT) && !(p->dwFlags & PFD_GENERIC_ACCELERATED);
}

static void describe(HDC dc, int i, PIXELFORMATDESCRIPTOR *p)
{
    memset(p, 0, sizeof *p);
    DescribePixelFormat(dc, i, sizeof *p, p);
}

int main(void)
{
    HDC dc;
    PIXELFORMATDESCRIPTOR p;
    int screen, n, i, ours = 0, failures = 0;

    /* GDI hands pixel format calls to OpenGL only once opengl32.dll is in the process (a
       game has it linked in). */
    if (!LoadLibrary("opengl32.dll")) {
        printf("opengl32.dll does not load\n");
        return 1;
    }
    dc = GetDC(NULL);
    screen = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
    n = DescribePixelFormat(dc, 1, sizeof p, &p);
    if (n <= 0) {
        printf("no pixel formats\n");
        return 1;
    }

    for (i = 1; i <= n; i++) {
        describe(dc, i, &p);
        if (!is_software(&p))
            ours++;
        printf("  %2d: %2d colour (%d alpha), %2d depth, %d stencil, %2d accum, %s, %s\n", i,
               p.cColorBits, p.cAlphaBits, p.cDepthBits, p.cStencilBits, p.cAccumBits,
               (p.dwFlags & PFD_DOUBLEBUFFER) ? "double" : "single",
               is_software(&p) ? "software" : "driver");
    }
    printf("screen %d bits; %d pixel formats, %d of them the driver's\n", screen, n, ours);
    if (!ours) {
        printf("pftest FAILED: the OpenGL driver offers no formats (is it installed?)\n");
        return 1;
    }
    for (i = 0; i < (int)(sizeof requests / sizeof requests[0]); i++) {
        const struct request *r = &requests[i];
        PIXELFORMATDESCRIPTOR want;
        int got;

        memset(&want, 0, sizeof want);
        want.nSize = sizeof want;
        want.nVersion = 1;
        want.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | r->flags;
        want.iPixelType = PFD_TYPE_RGBA;
        want.cColorBits = r->color ? r->color : (BYTE)screen;
        want.cDepthBits = r->depth;
        want.cStencilBits = r->stencil;
        want.iLayerType = PFD_MAIN_PLANE;
        got = ChoosePixelFormat(dc, &want);
        describe(dc, got, &p);
        printf("%-28s asks %2d/%2d/%d: format %2d, %2d colour, %2d depth, %d stencil, %2d accum, %s\n",
               r->who, want.cColorBits, r->depth, r->stencil, got, p.cColorBits, p.cDepthBits,
               p.cStencilBits, p.cAccumBits, is_software(&p) ? "SOFTWARE" : "driver");
        /* Only a request in the screen's own depth must get the driver: the driver offers
           formats for that depth (a 16-bit request on a 32-bit screen may get software). */
        if (!got || (is_software(&p) && (want.cColorBits == screen ||
                                         (screen == 32 && want.cColorBits == 24))))
            failures++;
    }
    ReleaseDC(NULL, dc);
    printf(failures ? "pftest FAILED (%d)\n" : "pftest passed\n", failures);
    return failures ? 1 : 0;
}
