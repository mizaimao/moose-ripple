/* Measure held-button movement and Explorer's search-band repaint. MIT (LICENSE). */
#define _WIN32_WINNT 0x0601
#include "explorermotion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const GUID motion_provider = {0x6c38bba1,0x1768,0x48c6,{0x9c,0x07,0x40,0xda,0xdb,0xa4,0xb3,0x8f}};

struct band_observer {
    HWND window;
    LONG margin, y, height;
    HANDLE stop;
    int round;
};

static BOOL CALLBACK find_search(HWND w, LPARAM param)
{
    char cls[64];
    if (IsWindowVisible(w) && GetClassNameA(w, cls, sizeof(cls)) && !strcmp(cls, "UniversalSearchBand")) {
        *(HWND *)param = w;
        return FALSE;
    }
    return TRUE;
}

static BOOL near_white(DWORD pixel)
{
    return GetRValue(pixel) >= 249 && GetGValue(pixel) >= 249 && GetBValue(pixel) >= 249;
}

/* Read both the glass above the edit and the edit itself in the same BitBlt.
 * Readback affects frame timing, so regular motion runs without this observer. */
static DWORD WINAPI observe_band(void *arg)
{
    const struct band_observer *obs = arg;
    enum { width = 216 };
    LONG height = obs->height;
    BITMAPINFO info = {0};
    DWORD *pixels = NULL, rc = 1;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    HGDIOBJ old = dc && bitmap ? SelectObject(dc, bitmap) : NULL;
    LARGE_INTEGER freq, start, now;
    if (!screen || !old || old == HGDI_ERROR || !QueryPerformanceFrequency(&freq)) goto cleanup;
    QueryPerformanceCounter(&start);
    do {
        RECT r;
        if (!GetWindowRect(obs->window, &r) ||
            !BitBlt(dc, 0, 0, width, height, screen, r.right - obs->margin - width,
                    obs->y, SRCCOPY) || !GdiFlush()) goto cleanup;
        int glass = 0, glass_run = 0, edit = 0, edit_run = 0;
        for (int x = 0; x < width; ++x) {
            glass_run = near_white(pixels[x]) && near_white(pixels[width + x]) ? glass_run + 1 : 0;
            /* Include the top and bottom edges to exclude the edit's light interior. */
            edit_run = near_white(pixels[2 * width + x]) &&
                       near_white(pixels[(2 + (height - 2) / 2) * width + x]) &&
                       near_white(pixels[(height - 1) * width + x]) ? edit_run + 1 : 0;
            glass = max(glass, glass_run);
            edit = max(edit, edit_run);
        }
        QueryPerformanceCounter(&now);
        printf("search,%d,%.1f,%ld,%d,%d\n", obs->round,
               (now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart, r.right, glass, edit);
    } while (WaitForSingleObject(obs->stop, 12) == WAIT_TIMEOUT);
    rc = 0;
cleanup:
    if (old && old != HGDI_ERROR) SelectObject(dc, old);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    if (screen) ReleaseDC(NULL, screen);
    return rc;
}

static BOOL mouse_button(DWORD flags)
{
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    return SendInput(1, &input, sizeof(input)) == 1;
}

static BOOL grip(HWND window, const RECT *r, BOOL resize, POINT *p)
{
    for (int i = 2; i <= 30; i += 2) {
        p->x = resize ? r->right - i : (r->left + r->right) / 2;
        p->y = resize ? r->bottom - i : r->top + i;
        DWORD_PTR hit;
        if (GetAncestor(WindowFromPoint(*p), GA_ROOT) == window &&
            SendMessageTimeout(window, WM_NCHITTEST, 0, MAKELPARAM(p->x, p->y),
                               SMTO_ABORTIFHUNG, 500, &hit) &&
            hit == (DWORD_PTR)(resize ? HTBOTTOMRIGHT : HTCAPTION)) return TRUE;
    }
    return FALSE;
}

BOOL measure_window_motion(HWND window, REGHANDLE trace, enum window_gesture gesture, int round)
{
    enum { hz = 60, steps = 3 * hz };
    BOOL resize = gesture != MOVE_WINDOW, search = gesture == CHECK_SEARCH_BAND;
    struct band_observer obs = {0};
    HANDLE observer = NULL;
    struct { int step; LONGLONG elapsed; RECT r; } samples[steps];
    LARGE_INTEGER freq, start, now;
    RECT work, r, end;
    POINT p;
    BOOL full = FALSE, ok = FALSE;
    int count = 0, changes = 0;
    const char *mode = resize ? "resize" : "drag";
    if (!SystemParametersInfo(SPI_GETDRAGFULLWINDOWS, 0, &full, 0) || !full) {
        fprintf(stderr, "Enable Show window contents while dragging.\n");
        return FALSE;
    }
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0) ||
        !QueryPerformanceFrequency(&freq)) return FALSE;
    int width = min(680, work.right - work.left - 260);
    int height = min(440, work.bottom - work.top - 160);
    if (width < 420 || height < 280 ||
        !SetWindowPos(window, HWND_TOP, work.left + 24, work.top + 24,
                      width, height, SWP_SHOWWINDOW)) return FALSE;
    Sleep(800);
    if (!GetWindowRect(window, &r) || !grip(window, &r, resize, &p) ||
        !SetCursorPos(p.x, p.y) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return FALSE;
    Sleep(100);
    if (search) {
        HWND band = NULL;
        RECT edit;
        EnumChildWindows(window, find_search, (LPARAM)&band);
        if (!band || !GetWindowRect(band, &edit) || edit.bottom <= edit.top) return FALSE;
        obs.window = window;
        obs.margin = r.right - edit.right;
        obs.y = edit.top - 2;
        obs.height = edit.bottom - obs.y;
        obs.round = round;
        obs.stop = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!obs.stop) return FALSE;
        observer = CreateThread(NULL, 0, observe_band, &obs, 0, NULL);
        if (!observer) { CloseHandle(obs.stop); return FALSE; }
        Sleep(300);
    }
    if (!mouse_button(MOUSEEVENTF_LEFTDOWN)) goto stop_observer;
    Sleep(100);
    if (EventWriteString(trace, 4, 0, resize ? L"resize-start" : L"drag-start")) goto release;
    QueryPerformanceCounter(&start);
    for (int previous = 0; previous < steps;) {
        QueryPerformanceCounter(&now);
        int step = (int)min((now.QuadPart - start.QuadPart) * (search ? 6 * hz : hz) / freq.QuadPart, steps);
        if (step <= previous) {
            Sleep(1);
            continue;
        }
        if (!SetCursorPos(p.x + 216 * step / steps, p.y + 108 * step / steps) ||
            !GetWindowRect(window, &samples[count].r)) goto release;
        samples[count].elapsed = now.QuadPart - start.QuadPart;
        samples[count].step = step;
        previous = step;
        if (count && !EqualRect(&samples[count].r, &samples[count - 1].r)) ++changes;
        ++count;
    }
    ok = EventWriteString(trace, 4, 0, resize ? L"resize-stop" : L"drag-stop") == 0;
    Sleep(250);
release:
    if (!mouse_button(MOUSEEVENTF_LEFTUP)) ok = FALSE;
    Sleep(350);
stop_observer:
    if (observer) {
        DWORD result;
        Sleep(700);
        SetEvent(obs.stop);
        WaitForSingleObject(observer, INFINITE);
        if (!GetExitCodeThread(observer, &result) || result) ok = FALSE;
        CloseHandle(observer);
        CloseHandle(obs.stop);
    }
    if (!ok || !GetWindowRect(window, &end)) return FALSE;
    for (int i = 0; i < count; ++i) {
        const RECT *s = &samples[i].r;
        printf("sample,%s,%d,%d,%.3f,%ld,%ld,%ld,%ld\n", mode, round,
               samples[i].step, samples[i].elapsed * 1000.0 / freq.QuadPart,
               s->left, s->top, s->right, s->bottom);
    }
    printf("motion=%s round=%d distinct_changes=%d delta=%ld,%ld,%ld,%ld\n",
           mode, round, changes, end.left - r.left, end.top - r.top,
           end.right - r.right, end.bottom - r.bottom);
    return changes >= (search ? 1 : 3) && labs(end.left - r.left - (resize ? 0 : 216)) <= 2 &&
           labs(end.top - r.top - (resize ? 0 : 108)) <= 2 &&
           labs(end.right - r.right - 216) <= 2 && labs(end.bottom - r.bottom - 108) <= 2;
}
