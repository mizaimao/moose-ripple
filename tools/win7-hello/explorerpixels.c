/* Time a small patch of Explorer's backing bitmap and composed desktop.
 * Each read has a beginning and end on the same clock. MIT (LICENSE). */
#define _WIN32_WINNT 0x0601
#include "explorerpixels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { capacity = 512 };
struct observation { LONGLONG begin, end; };
struct series {
    struct observation times[capacity];
    DWORD *images;
    int count;
};
struct pixel_run {
    HWND window, child;
    LONG goal;
    RECT area;
    HANDLE stop, reader, geometry;
    struct series screen, source;
    int pixels;
    LARGE_INTEGER origin, frequency;
    LONGLONG last_old, first_new;
};

static LONGLONG tick(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static double ms(const struct pixel_run *run, LONGLONG t)
{
    return (t - run->origin.QuadPart) * 1000.0 / run->frequency.QuadPart;
}

static DWORD WINAPI track_geometry(void *arg)
{
    struct pixel_run *run = arg;
    do {
        RECT r;
        LONGLONG before = tick();
        if (!GetWindowRect(run->child, &r)) return 1;
        if (r.right == run->goal) {
            run->first_new = tick();
            return 0;
        }
        run->last_old = before;
    } while (WaitForSingleObject(run->stop, 2) == WAIT_TIMEOUT);
    return 1;
}

static BOOL read_patch(struct pixel_run *run, struct series *s, HDC input, HDC dc,
                       const DWORD *pixels, LONG x, LONG y)
{
    const RECT *a = &run->area;
    struct observation *obs = &s->times[s->count];
    obs->begin = tick();
    if (!BitBlt(dc, 0, 0, a->right - a->left, a->bottom - a->top, input,
                x, y, SRCCOPY) || !GdiFlush()) return FALSE;
    obs->end = tick();
    memcpy(s->images + (size_t)s->count * run->pixels, pixels, run->pixels * sizeof(DWORD));
    ++s->count;
    return TRUE;
}

static DWORD WINAPI read_pixels(void *arg)
{
    struct pixel_run *run = arg;
    RECT a = run->area;
    HDC screen = GetDC(NULL), dc = screen ? CreateCompatibleDC(screen) : NULL;
    BITMAPINFO bi = {0};
    DWORD *pixels = NULL, result = 1;
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = a.right - a.left;
    bi.bmiHeader.biHeight = a.top - a.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0) : NULL;
    HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : NULL;
    if (!old || old == HGDI_ERROR) goto done;
    do {
        if (run->screen.count == capacity || run->source.count == capacity) goto done;
        if (run->screen.images && !read_patch(run, &run->screen, screen, dc, pixels, a.left, a.top)) goto done;
        if (run->child) {
            RECT r;
            if (!GetWindowRect(run->window, &r)) goto done;
            if (r.right >= a.right && r.bottom >= a.bottom) {
                HDC src = GetWindowDC(run->window);
                if (!src) goto done;
                for (int i = 0; i < run->pixels; ++i) pixels[i] = 0x00ff00ff;
                BOOL ok = read_patch(run, &run->source, src, dc, pixels, a.left - r.left, a.top - r.top);
                ReleaseDC(run->window, src);
                if (!ok) goto done;
                for (int i = 0; i < run->pixels; ++i) {
                    if ((pixels[i] & 0xffffff) == 0xff00ff) {
                        --run->source.count;
                        break;
                    }
                }
            }
        }
    } while (WaitForSingleObject(run->stop, 4) == WAIT_TIMEOUT);
    result = 0;
done:
    if (old && old != HGDI_ERROR) SelectObject(dc, old);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    if (screen) ReleaseDC(NULL, screen);
    return result;
}

static double changed(const DWORD *a, const DWORD *b, int pixels)
{
    int n = 0;
    for (int i = 0; i < pixels; ++i) {
        if (abs((int)GetRValue(a[i]) - GetRValue(b[i])) > 8 ||
            abs((int)GetGValue(a[i]) - GetGValue(b[i])) > 8 ||
            abs((int)GetBValue(a[i]) - GetBValue(b[i])) > 8) ++n;
    }
    return 100.0 * n / pixels;
}

static BOOL report(const struct pixel_run *run, const struct series *s, const char *kind,
                   const char *gesture, int round)
{
    if (s->count < 3) return FALSE;
    const DWORD *first = s->images, *last = first + (size_t)(s->count - 1) * run->pixels;
    if (run->child) {
        /* A uniform final patch cannot identify the search button. */
        int different = 0;
        for (int i = 1; i < run->pixels; ++i) {
            if (changed(last, last + i, 1) > 0) ++different;
        }
        if (different < run->pixels / 20) return FALSE;
    }
    for (int i = 0; i < s->count; ++i) {
        const DWORD *cur = first + (size_t)i * run->pixels;
        printf("pixels,%s,%d,%s,%.3f,%.3f,%.2f,%.2f,%.2f\n", gesture, round, kind,
               ms(run, s->times[i].begin), ms(run, s->times[i].end),
               changed(cur, first, run->pixels), changed(cur, last, run->pixels),
               i ? changed(cur, cur - run->pixels, run->pixels) : 0.0);
    }
    return TRUE;
}

BOOL finish_pixels(struct pixel_run *run, const char *gesture, int round)
{
    DWORD rc;
    BOOL ok = run && run->reader;
    if (!run) return FALSE;
    if (run->stop) SetEvent(run->stop);
    if (run->reader) {
        WaitForSingleObject(run->reader, INFINITE);
        ok = GetExitCodeThread(run->reader, &rc) && rc == 0;
        CloseHandle(run->reader);
    }
    if (run->geometry) {
        WaitForSingleObject(run->geometry, INFINITE);
        ok = ok && run->last_old && run->first_new;
        CloseHandle(run->geometry);
    }
    if (gesture && ok) {
        if (run->screen.images) ok = report(run, &run->screen, "screen", gesture, round);
        if (run->child) {
            printf("geometry,%s,%d,%.3f,%.3f\n", gesture, round,
                   ms(run, run->last_old), ms(run, run->first_new));
            ok = report(run, &run->source, "window", gesture, round) && ok;
        }
    }
    if (run->stop) CloseHandle(run->stop);
    free(run->screen.images);
    free(run->source.images);
    free(run);
    return ok;
}

struct pixel_run *start_pixels(HWND window, HWND child, LONG goal, RECT area, BOOL sample_screen)
{
    LONG width = area.right - area.left, height = area.bottom - area.top;
    if ((!child && !sample_screen) || width <= 0 || height <= 0 ||
        width > 64 || height > 4096 || width * height > 8192) return NULL;
    struct pixel_run *run = calloc(1, sizeof(*run));
    if (!run) return NULL;
    run->window = window;
    run->child = child;
    run->goal = goal;
    run->area = area;
    run->pixels = width * height;
    if (sample_screen) run->screen.images = calloc(capacity, run->pixels * sizeof(DWORD));
    if (child) run->source.images = calloc(capacity, run->pixels * sizeof(DWORD));
    run->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if ((sample_screen && !run->screen.images) || (child && !run->source.images) || !run->stop ||
        !QueryPerformanceFrequency(&run->frequency) || !QueryPerformanceCounter(&run->origin)) goto failed;
    if (child && !(run->geometry = CreateThread(NULL, 0, track_geometry, run, 0, NULL))) goto failed;
    run->reader = CreateThread(NULL, 0, read_pixels, run, 0, NULL);
    if (run->reader) return run;
failed:
    finish_pixels(run, NULL, 0);
    return NULL;
}
