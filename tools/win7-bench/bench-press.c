/* bench-press.exe TEXT: press the first visible button of 3DMark06 whose text contains TEXT.
 * Exit code: 0 pressed, 2 no such button, 1 3DMark06 is not running.
 * MIT licence (LICENSE at the repository root). */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD pid;
static const char *wanted;
static HWND found;

static BOOL CALLBACK control(HWND w, LPARAM unused)
{
    char cls[64] = "", text[120] = "";
    DWORD_PTR copied = 0;
    (void)unused;
    if (found || !IsWindowVisible(w)) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    if (_stricmp(cls, "Button")) return TRUE;
    SendMessageTimeoutA(w, WM_GETTEXT, sizeof(text), (LPARAM)text, SMTO_ABORTIFHUNG, 200, &copied);
    if (strstr(text, wanted)) found = w;
    return TRUE;
}

static BOOL CALLBACK window(HWND w, LPARAM unused)
{
    DWORD wp = 0;
    (void)unused;
    GetWindowThreadProcessId(w, &wp);
    if (wp == pid && IsWindowVisible(w)) EnumChildWindows(w, control, 0);
    return TRUE;
}

int main(int argc, char **argv)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { 0 };
    if (argc != 2) return 2;
    wanted = argv[1];
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) do {
        if (!_stricmp(pe.szExeFile, "3DMark06.exe")) pid = pe.th32ProcessID;
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    if (!pid) { puts("3DMark06 is closed"); return 1; }
    EnumWindows(window, 0);
    if (!found) { printf("no button with \"%s\"\n", wanted); return 2; }
    PostMessageA(found, BM_CLICK, 0, 0);
    printf("pressed \"%s\"\n", wanted);
    return 0;
}
