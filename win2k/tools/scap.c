/* A small picture of the screen every second, for test runs on a display that no
 * host tool can see (a guest drawing on a real graphics card).
 *
 *   scap <folder> <seconds> [stop file]
 *
 * Each second the whole screen is read through GDI, scaled to 256x192 and
 * written as <folder>\NNNN.bmp (24 bits a pixel, about 148 KB). The run ends
 * after <seconds>, or when the stop file exists. One line a picture goes to
 * the standard output: its number, the tick count, and how long the read took.
 * Whether a full-screen Direct3D picture is seen this way depends on the
 * display driver; the pictures themselves show it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Width = 256, Height = 192 };

int main(int argc, char **argv)
{
    BITMAPINFO info;
    BITMAPFILEHEADER file;
    HDC screen, memory;
    HBITMAP bitmap;
    void *bits = NULL;
    DWORD bytes = Width * Height * 3, start, written;
    int seconds, n;
    if (argc < 3) { printf("SCAP usage: scap folder seconds [stopfile]\n"); return 2; }
    seconds = atoi(argv[2]);
    CreateDirectoryA(argv[1], NULL);
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = Width;
    info.bmiHeader.biHeight = Height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = bytes;
    memset(&file, 0, sizeof(file));
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + bytes;
    memory = CreateCompatibleDC(NULL);
    bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!memory || !bitmap || !bits) { printf("SCAP no bitmap\n"); return 1; }
    SelectObject(memory, bitmap);
    SetStretchBltMode(memory, COLORONCOLOR);
    start = GetTickCount();
    printf("SCAP begin %s seconds=%d\n", argv[1], seconds);
    for (n = 0; n < seconds; ++n) {
        char name[MAX_PATH];
        DWORD t0 = GetTickCount(), t1;
        HANDLE h;
        BOOL ok;
        if (argc > 3 && GetFileAttributesA(argv[3]) != INVALID_FILE_ATTRIBUTES) break;
        /* A fresh screen DC each time: a mode change gives the screen a new size. */
        screen = GetDC(NULL);
        ok = StretchBlt(memory, 0, 0, Width, Height, screen, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                        GetSystemMetrics(SM_CYSCREEN), SRCCOPY);
        ReleaseDC(NULL, screen);
        GdiFlush();
        t1 = GetTickCount();
        _snprintf(name, sizeof(name), "%s\\%04d.bmp", argv[1], n);
        name[sizeof(name) - 1] = 0;
        h = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, &file, sizeof(file), &written, NULL);
            WriteFile(h, &info.bmiHeader, sizeof(info.bmiHeader), &written, NULL);
            WriteFile(h, bits, bytes, &written, NULL);
            CloseHandle(h);
        }
        printf("SCAP %04d tick=%lu read_ms=%lu%s%s\n", n, (unsigned long)(t0 - start), (unsigned long)(t1 - t0),
               ok ? "" : " read_failed", h == INVALID_HANDLE_VALUE ? " write_failed" : "");
        fflush(stdout);
        t1 = GetTickCount() - t0;
        if (t1 < 1000) Sleep(1000 - t1);
    }
    printf("SCAP end pictures=%d\n", n);
    return 0;
}
