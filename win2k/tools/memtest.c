/* MEMTEST: where a guest's time goes, independent of any graphics API.
 * Times a compute loop, writes to ordinary memory, GDI fills into a DIB in
 * memory and on the screen, and copies that read or write the framebuffer.
 * Writes to standard output, or to COM1 when started without one. */
#include <windows.h>
#include <string.h>

static HANDLE serial;
static DWORD start;

static void line(const char *t)
{
    DWORD n;
    WriteFile(serial, t, lstrlenA(t), &n, NULL);
    WriteFile(serial, "\r\n", 2, &n, NULL);
}

static void report(const char *what)
{
    char msg[160];
    wsprintfA(msg, "MEMTEST %s ms=%lu", what, GetTickCount() - start);
    line(msg);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    DWORD i;
    volatile DWORD acc = 0;
    BYTE *ram;
    BITMAPINFO bi;
    void *bits;
    HBITMAP bmp;
    HDC dc, mem;
    int w, h;
    (void)inst; (void)prev; (void)args; (void)show;
    /* A program started from Explorer may hold a standard handle that leads nowhere. */
    serial = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!serial || serial == INVALID_HANDLE_VALUE || GetFileType(serial) == FILE_TYPE_UNKNOWN)
        serial = CreateFileA("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (serial == INVALID_HANDLE_VALUE) return 1;

    start = GetTickCount();
    for (i = 0; i < 200000000UL; ++i) acc += i ^ (acc >> 3);
    report("compute 200M iterations");

    ram = VirtualAlloc(NULL, 3145728, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    start = GetTickCount();
    for (i = 0; ram && i < 200; ++i) { memset(ram, (int)i, 3145728); acc += ram[i * 4099]; }
    report(ram ? "ram 200 x 3 MB writes" : "ram allocation failed");

    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    dc = GetDC(NULL);
    mem = CreateCompatibleDC(dc);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { line("MEMTEST dib allocation failed"); line("MEMTEST end"); return 1; }
    SelectObject(mem, bmp);

    start = GetTickCount();
    for (i = 0; i < 200; ++i) { PatBlt(mem, 0, 0, w, h, (i & 1) ? BLACKNESS : WHITENESS); GdiFlush(); }
    report("dib 200 full-size fills");

    start = GetTickCount();
    for (i = 0; i < 200; ++i) { PatBlt(dc, 0, 0, w, h, (i & 1) ? BLACKNESS : WHITENESS); GdiFlush(); }
    report("screen 200 full-screen fills");

    start = GetTickCount();
    for (i = 0; i < 50; ++i) { BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY); GdiFlush(); }
    report("dib-to-screen 50 copies");

    start = GetTickCount();
    for (i = 0; i < 50; ++i) { BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY); GdiFlush(); }
    report("screen-to-dib 50 copies");

    start = GetTickCount();
    for (i = 0; i < 50; ++i) { BitBlt(dc, 0, 1, w, h - 1, dc, 0, 0, SRCCOPY); GdiFlush(); }
    report("screen-scroll 50 copies");

    DeleteDC(mem);
    DeleteObject(bmp);
    ReleaseDC(NULL, dc);
    InvalidateRect(NULL, NULL, TRUE);
    line("MEMTEST end");
    CloseHandle(serial);
    return 0;
}
