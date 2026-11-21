/* Inspect, check or set the saved desktop mode. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

static HANDLE out;

static void line(const char *t)
{
    DWORD n;
    WriteFile(out, t, lstrlenA(t), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
}

static BOOL show(const char *what, DWORD which, DEVMODEA *m)
{
    char msg[160];
    BOOL found;
    ZeroMemory(m, sizeof(*m));
    m->dmSize = sizeof(*m);
    found = EnumDisplaySettingsA(NULL, which, m);
    if (!found) {
        wsprintfA(msg, "REGMODE %s unavailable error=%lu", what, GetLastError());
    } else {
        wsprintfA(msg, "REGMODE %s %lux%lux%lu Hz=%lu fields=%08lx", what, m->dmPelsWidth,
                  m->dmPelsHeight, m->dmBitsPerPel, m->dmDisplayFrequency, m->dmFields);
    }
    line(msg);
    return found;
}

static BOOL dimensions(char *p, DWORD *width, DWORD *height)
{
    while (*p == ' ') ++p;
    if (*p < '0' || *p > '9') return FALSE;
    *width = strtoul(p, &p, 10);
    if (*p != 'x' && *p != 'X') return FALSE;
    ++p;
    if (*p < '0' || *p > '9') return FALSE;
    *height = strtoul(p, &p, 10);
    while (*p == ' ') ++p;
    return !*p && *width && *height;
}

static BOOL matches(const DEVMODEA *m, DWORD width, DWORD height)
{
    return m->dmPelsWidth == width && m->dmPelsHeight == height &&
           m->dmBitsPerPel == 32 && m->dmDisplayFrequency == 60;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show_cmd)
{
    char msg[160];
    DEVMODEA saved, current;
    DWORD width, height;
    BOOL have_saved, have_current;
    int status = 0;
    (void)inst; (void)prev; (void)show_cmd;
    /* A program started from Explorer may hold a standard handle that leads nowhere. */
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN)
        out = CreateFileA("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) return 1;
    have_saved = show("saved", ENUM_REGISTRY_SETTINGS, &saved);
    have_current = show("current", ENUM_CURRENT_SETTINGS, &current);
    if (!_strnicmp(args, "/check ", 7) && dimensions(args + 7, &width, &height)) {
        BOOL pass = have_saved && have_current && matches(&saved, width, height) &&
                    matches(&current, width, height);
        wsprintfA(msg, "REGMODE check %lux%lux32 Hz=60 %s", width, height, pass ? "PASS" : "FAIL");
        line(msg);
        status = pass ? 0 : 1;
    } else if (!_strnicmp(args, "/set ", 5) && dimensions(args + 5, &width, &height)) {
        DEVMODEA m;
        LONG r;
        ZeroMemory(&m, sizeof(m));
        m.dmSize = sizeof(m);
        m.dmPelsWidth = width;
        m.dmPelsHeight = height;
        m.dmBitsPerPel = 32;
        m.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        r = ChangeDisplaySettingsA(&m, CDS_UPDATEREGISTRY);
        wsprintfA(msg, "REGMODE set %lux%lux32 result=%ld", m.dmPelsWidth, m.dmPelsHeight, r);
        line(msg);
        show("saved", ENUM_REGISTRY_SETTINGS, &saved);
        show("current", ENUM_CURRENT_SETTINGS, &current);
        status = r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
    } else if (!lstrcmpiA(args, "/reset") || !lstrcmpiA(args, "/r")) {
        LONG r = ChangeDisplaySettingsA(NULL, 0);
        wsprintfA(msg, "REGMODE reset to saved result=%ld", r);
        line(msg);
        show("current", ENUM_CURRENT_SETTINGS, &current);
        status = r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
    } else if (*args) {
        line("REGMODE usage: regmode [/check WxH | /set WxH | /reset]");
        status = 2;
    }
    line("REGMODE end");
    return status;
}
