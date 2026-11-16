/*
 * ddtest: check a display driver's DirectDraw HAL from a program, the way a game uses it.
 *
 *   ddtest [width height bpp [seconds]]        default: 640 480 32, 2 seconds
 *
 * Prints what the HAL offers (capabilities, video memory), then takes the screen
 * (exclusive, full-screen), sets the mode, makes a primary surface with one back buffer,
 * and flips between them, filling the back buffer with a new colour each frame. It checks
 * that the surfaces are in video memory, that a flip shows the other buffer (the card's
 * display start moves), that the picture on the screen is the one drawn last, and how long
 * DirectDraw's emulated vertical blank takes. Output on standard output: ddtest > out.txt.
 * Exit status 0 when every check passed. Built by tools/win9x/build-tools.sh.
 */
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Everything printed also goes to QEMU's debug port (0xE9): it survives a crash or a hung
   screen, and a log file the guest never flushed. */
#include <stdarg.h>
static int say(const char *fmt, ...)
{
    char buf[512];
    char *p;
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p; p++) {                /* the port first: console output can block */
        char c = *p;
        __asm {
            mov al, c
            out 0E9h, al
        }
    }
    fputs(buf, stdout);
    return n;
}
#define printf say

#ifndef DDFLIP_NOVSYNC                  /* DirectX 6; Visual C++ 6 has DirectX 5's header */
#define DDFLIP_NOVSYNC 0x00000008L
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "WRONG");
    if (!ok)
        failures++;
}

/* QEMU's VBE display start (line), read through the card's index/data ports. */
static WORD vbe_y_offset(void)
{
    WORD v;
    __asm {
        mov dx, 01CEh
        mov ax, 9
        out dx, ax
        inc dx
        in  ax, dx
        mov v, ax
    }
    return v;
}

static DWORD colour(int bpp, int i)
{
    static const DWORD rgb[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00 };
    DWORD c = rgb[i & 3];
    if (bpp == 16)
        return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
    if (bpp == 8)
        return 9 + (i & 3);             /* entries of the system palette's static colours */
    return c;
}

static void fill(LPDIRECTDRAWSURFACE s, int bpp, DWORD c, int w, int h)
{
    DDSURFACEDESC d;
    int x, y;

    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (IDirectDrawSurface_Lock(s, NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL) != DD_OK)
        return;
    for (y = 0; y < h; y++) {
        BYTE *row = (BYTE *)d.lpSurface + y * d.lPitch;
        for (x = 0; x < w; x++) {
            if (bpp == 32) ((DWORD *)row)[x] = c;
            else if (bpp == 16) ((WORD *)row)[x] = (WORD)c;
            else if (bpp == 8) row[x] = (BYTE)c;
            else { row[x * 3] = (BYTE)c; row[x * 3 + 1] = (BYTE)(c >> 8); row[x * 3 + 2] = (BYTE)(c >> 16); }
        }
    }
    IDirectDrawSurface_Unlock(s, d.lpSurface);
}

static DWORD read_pixel(LPDIRECTDRAWSURFACE s, int bpp, int x, int y)
{
    DDSURFACEDESC d;
    DWORD v = 0xFFFFFFFF;
    BYTE *row;

    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (IDirectDrawSurface_Lock(s, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL) != DD_OK)
        return v;
    row = (BYTE *)d.lpSurface + y * d.lPitch;
    if (bpp == 32) v = ((DWORD *)row)[x] & 0xFFFFFF;
    else if (bpp == 16) v = ((WORD *)row)[x];
    else if (bpp == 8) v = row[x];
    else v = row[x * 3] | (row[x * 3 + 1] << 8) | (row[x * 3 + 2] << 16);
    IDirectDrawSurface_Unlock(s, d.lpSurface);
    return v;
}

static void print_caps(const char *who, DDCAPS *c)
{
    printf("%s: caps %08lx caps2 %08lx surface caps %08lx, video memory %lu KB (%lu KB free)\n",
           who, c->dwCaps, c->dwCaps2, c->ddsCaps.dwCaps, c->dwVidMemTotal / 1024,
           c->dwVidMemFree / 1024);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProc(w, m, wp, lp);
}

int main(int argc, char **argv)
{
    int w = argc > 3 ? atoi(argv[1]) : 640, h = argc > 3 ? atoi(argv[2]) : 480;
    int bpp = argc > 3 ? atoi(argv[3]) : 32, seconds = argc > 4 ? atoi(argv[4]) : 2;
    LPDIRECTDRAW dd;
    LPDIRECTDRAWSURFACE front = NULL, back = NULL;
    DDSURFACEDESC sd;
    DDSCAPS caps;
    DDCAPS hal, hel;
    WNDCLASS wc;
    HWND win;
    HRESULT r;
    DWORD start, frames = 0, y0, y1, y2, px;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);       /* everything printed survives a crash */
    if (DirectDrawCreate(NULL, &dd, NULL) != DD_OK) {
        printf("DirectDrawCreate failed\n");
        return 2;
    }
    memset(&hal, 0, sizeof hal); hal.dwSize = sizeof hal;
    memset(&hel, 0, sizeof hel); hel.dwSize = sizeof hel;
    IDirectDraw_GetCaps(dd, &hal, &hel);
    print_caps("driver (HAL)", &hal);
    print_caps("emulation (HEL)", &hel);
    check(hal.dwVidMemTotal > 0, "the driver offers video memory");
    check((hal.ddsCaps.dwCaps & DDSCAPS_FLIP) != 0, "the driver can flip");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "ddtest";
    RegisterClass(&wc);
    win = CreateWindowEx(WS_EX_TOPMOST, "ddtest", "ddtest", WS_POPUP | WS_VISIBLE, 0, 0,
                         GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                         NULL, NULL, wc.hInstance, NULL);
    r = IDirectDraw_SetCooperativeLevel(dd, win, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (r != DD_OK) { printf("SetCooperativeLevel: %08lx\n", r); return 2; }
    r = IDirectDraw_SetDisplayMode(dd, w, h, bpp);
    printf("SetDisplayMode %dx%d, %d bits: %08lx\n", w, h, bpp, r);
    if (r != DD_OK) { failures++; goto out; }

    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    sd.dwBackBufferCount = 1;
    r = IDirectDraw_CreateSurface(dd, &sd, &front, NULL);
    printf("primary surface with a back buffer: %08lx\n", r);
    if (r != DD_OK) { failures++; goto out; }
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface_GetAttachedSurface(front, &caps, &back);
    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    IDirectDrawSurface_GetSurfaceDesc(back, &sd);
    printf("back buffer: caps %08lx, pitch %ld\n", sd.ddsCaps.dwCaps, sd.lPitch);
    check((sd.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) != 0, "the back buffer is in video memory");

    /* Flips move the display start between the two buffers. */
    y0 = vbe_y_offset();
    fill(back, bpp, colour(bpp, 0), w, h);
    r = IDirectDrawSurface_Flip(front, NULL, DDFLIP_WAIT);
    y1 = vbe_y_offset();
    fill(back, bpp, colour(bpp, 1), w, h);
    IDirectDrawSurface_Flip(front, NULL, DDFLIP_WAIT);
    y2 = vbe_y_offset();
    printf("display start (line): %lu, after a flip %lu, after another %lu\n", y0, y1, y2);
    check(r == DD_OK, "Flip succeeds");
    check(y1 != y0 && y2 == y0, "a flip shows the other buffer");
    px = read_pixel(front, bpp, w / 2, h / 2);
    printf("pixel on the screen %08lx, drawn last %08lx\n", px, colour(bpp, 1));
    check(px == colour(bpp, 1), "the screen shows what was drawn last");

    /* As fast as it goes, then locked to the (emulated) vertical blank. */
    start = GetTickCount();
    for (i = 0; GetTickCount() - start < (DWORD)seconds * 1000; i++) {
        fill(back, bpp, colour(bpp, i), w, h);
        IDirectDrawSurface_Flip(front, NULL, DDFLIP_WAIT | DDFLIP_NOVSYNC);
        frames++;
    }
    printf("filled and flipped %lu frames in %d s: %.1f a second\n", frames, seconds,
           frames / (double)seconds);
    start = GetTickCount();
    for (i = 0; i < 60; i++)
        IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    printf("60 waits for the vertical blank: %lu ms\n", GetTickCount() - start);
    check(GetTickCount() - start > 800 && GetTickCount() - start < 1300,
          "the vertical blank comes 60 times a second");

out:
    if (front)
        IDirectDrawSurface_Release(front);
    IDirectDraw_RestoreDisplayMode(dd);
    IDirectDraw_SetCooperativeLevel(dd, win, DDSCL_NORMAL);
    IDirectDraw_Release(dd);
    DestroyWindow(win);
    printf("display start after leaving: line %u\n", vbe_y_offset());
    printf(failures ? "ddtest FAILED: %d checks wrong\n" : "ddtest passed\n", failures);
    return failures ? 1 : 0;
}
