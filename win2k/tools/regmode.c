/* REGMODE: the display mode Windows has saved for the desktop and the one in
 * use; "/set WxH" makes WxHx32 the saved mode. Writes to standard output, or
 * to COM1 when started without one. */
#include <windows.h>
#include <stdlib.h>

static HANDLE out;

static void line(const char *t)
{
    DWORD n;
    WriteFile(out, t, lstrlenA(t), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
}

static void show(const char *what, DWORD which)
{
    DEVMODEA m;
    char msg[160];
    ZeroMemory(&m, sizeof(m));
    m.dmSize = sizeof(m);
    if (!EnumDisplaySettingsA(NULL, which, &m)) {
        wsprintfA(msg, "REGMODE %s unavailable error=%lu", what, GetLastError());
    } else {
        wsprintfA(msg, "REGMODE %s %lux%lux%lu Hz=%lu fields=%08lx", what, m.dmPelsWidth,
                  m.dmPelsHeight, m.dmBitsPerPel, m.dmDisplayFrequency, m.dmFields);
    }
    line(msg);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show_cmd)
{
    char msg[160];
    (void)inst; (void)prev; (void)show_cmd;
    /* A program started from Explorer may hold a standard handle that leads nowhere. */
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN)
        out = CreateFileA("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) return 1;
    show("saved", ENUM_REGISTRY_SETTINGS);
    show("current", ENUM_CURRENT_SETTINGS);
    if (args && args[0] == '/' && (args[1] == 's' || args[1] == 'S')) {
        DEVMODEA m;
        char *p = args + 4;
        LONG r;
        while (*p == ' ') ++p;
        ZeroMemory(&m, sizeof(m));
        m.dmSize = sizeof(m);
        m.dmPelsWidth = strtoul(p, &p, 10);
        if (*p == 'x' || *p == 'X') ++p;
        m.dmPelsHeight = strtoul(p, &p, 10);
        m.dmBitsPerPel = 32;
        m.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        r = ChangeDisplaySettingsA(&m, CDS_UPDATEREGISTRY);
        wsprintfA(msg, "REGMODE set %lux%lux32 result=%ld", m.dmPelsWidth, m.dmPelsHeight, r);
        line(msg);
        show("saved", ENUM_REGISTRY_SETTINGS);
        show("current", ENUM_CURRENT_SETTINGS);
    } else if (args && args[0] == '/' && (args[1] == 'r' || args[1] == 'R')) {
        LONG r = ChangeDisplaySettingsA(NULL, 0);
        wsprintfA(msg, "REGMODE reset to saved result=%ld", r);
        line(msg);
        show("current", ENUM_CURRENT_SETTINGS);
    }
    line("REGMODE end");
    return 0;
}
