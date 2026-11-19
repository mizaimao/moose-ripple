/* What the display shows from the moment this logon session's Default desktop becomes the
 * input desktop: 48 reduced captures of the screen, one every 150 ms or as fast as they come,
 * on one sheet (logonshots.bmp, rows of six, in the user's temporary folder) with their times
 * and the shell's state (logonshots.txt). Start it from a task that runs at logon.
 * Reading the screen costs time and makes the compositor work; take the times of an
 * undisturbed logon from a trace. MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <string.h>

enum { COLS = 6, ROWS = 8, FRAMES = COLS * ROWS, FW = 320, FH = 256, STEP_MS = 150 };

static LARGE_INTEGER freq, origin;

static double elapsed_ms(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - origin.QuadPart) * 1000.0 / freq.QuadPart;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR args, int show)
{
    (void)instance; (void)prev; (void)args; (void)show;
    HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    DWORD needed;
    BOOL active = FALSE;
    ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < 60000) {
        if (GetUserObjectInformationW(desktop, UOI_IO, &active, sizeof(active), &needed) && active)
            break;
        Sleep(5);
    }
    if (!active) return 1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&origin);
    const double waited = (double)(GetTickCount64() - begin);

    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int width = COLS * FW, height = ROWS * FH, stride = width * 3;
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    BYTE *bits = NULL;
    HBITMAP sheet = CreateDIBSection(screen, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!screen || !dc || !sheet || !bits) return 1;
    SelectObject(dc, sheet);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);

    static double when[FRAMES], took[FRAMES];
    static int changed[FRAMES], shell[FRAMES], tray[FRAMES], blt[FRAMES];
    for (int n = 0; n < FRAMES; ++n) {
        while (elapsed_ms() < n * STEP_MS) Sleep(1);
        when[n] = elapsed_ms();
        HWND bar = FindWindowW(L"Shell_TrayWnd", NULL);
        shell[n] = GetShellWindow() != NULL;
        tray[n] = bar ? (IsWindowVisible(bar) ? 2 : 1) : 0;
        blt[n] = StretchBlt(dc, (n % COLS) * FW, (n / COLS) * FH, FW, FH, screen, 0, 0, sw, sh, SRCCOPY);
        GdiFlush();
        took[n] = elapsed_ms() - when[n];
        /* the sheet is stored bottom row first */
        changed[n] = n == 0;
        for (int y = 0; n && y < FH && !changed[n]; ++y) {
            const BYTE *row = bits + (height - 1 - ((n / COLS) * FH + y)) * stride + (n % COLS) * FW * 3;
            const int m = n - 1;
            const BYTE *old = bits + (height - 1 - ((m / COLS) * FH + y)) * stride + (m % COLS) * FW * 3;
            if (memcmp(row, old, FW * 3)) changed[n] = 1;
        }
    }

    char folder[MAX_PATH], path[MAX_PATH];
    DWORD len = GetTempPathA(sizeof(folder), folder);
    if (!len || len >= sizeof(folder) - 20) return 1;
    snprintf(path, sizeof(path), "%slogonshots.bmp", folder);
    FILE *out = fopen(path, "wb");
    if (!out) return 1;
    BITMAPFILEHEADER file;
    memset(&file, 0, sizeof(file));
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + (DWORD)stride * height;
    fwrite(&file, sizeof(file), 1, out);
    fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, out);
    fwrite(bits, stride, height, out);
    fclose(out);

    snprintf(path, sizeof(path), "%slogonshots.txt", folder);
    out = fopen(path, "w");
    if (!out) return 1;
    fprintf(out, "screen %dx%d, waited %.0f ms for the desktop; tray: 0 none, 1 hidden, 2 visible\n",
            sw, sh, waited);
    for (int n = 0; n < FRAMES; ++n)
        fprintf(out, "frame %2d row %d col %d at %7.0f ms, read in %4.0f ms, %s, shell %d, tray %d%s\n",
                n, n / COLS, n % COLS, when[n], took[n], changed[n] ? "changed" : "same   ",
                shell[n], tray[n], blt[n] ? "" : ", read failed");
    fclose(out);
    return 0;
}
