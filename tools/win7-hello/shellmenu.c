/* Exercise Windows 7's desktop context menu -> New, without creating any files.
 * Reports whether a second menu window really opened and whether Explorer stayed alive.
 * English keyboard/menu mnemonics. Run: shellmenu.exe [rounds] > shellmenu.txt
 * Build: llvm-mingw clang -O2 -Wall -Wextra shellmenu.c -o shellmenu.exe
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void key(WORD code, BOOL up)
{
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = code;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(input));
}
static void tap(WORD code)
{
    key(code, FALSE);
    Sleep(60);
    key(code, TRUE);
}
static BOOL CALLBACK count_menu(HWND window, LPARAM arg)
{
    char name[64];
    if (IsWindowVisible(window) && GetClassNameA(window, name, sizeof(name)) &&
        !strcmp(name, "#32768"))
        ++*(unsigned *)arg;
    return TRUE;
}
static unsigned menus(void)
{
    unsigned count = 0;
    EnumWindows(count_menu, (LPARAM)&count);
    return count;
}
static DWORD explorer(void)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &pid);
    return pid;
}
int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 5, i, failures = 0;
    DWORD original = explorer();
    if (rounds < 1 || rounds > 50 || !original) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("explorer_pid=%lu rounds=%d\n", (unsigned long)original, rounds);
    key(VK_LWIN, FALSE); tap('D'); key(VK_LWIN, TRUE);
    Sleep(1500);
    for (i = 0; i < rounds; ++i) {
        INPUT mouse[2] = {{0}, {0}};
        unsigned parent, submenu;
        DWORD pid;
        SetCursorPos(GetSystemMetrics(SM_CXSCREEN) - 100, GetSystemMetrics(SM_CYSCREEN) / 2);
        mouse[0].type = mouse[1].type = INPUT_MOUSE;
        mouse[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        mouse[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(2, mouse, sizeof(mouse[0]));
        Sleep(1200);
        parent = menus();
        tap('W'); /* New on an English Windows 7 desktop context menu. */
        Sleep(1500);
        if (menus() < 2) { tap(VK_RIGHT); Sleep(1000); }
        submenu = menus();
        pid = explorer();
        printf("round=%d parent_menus=%u submenu_menus=%u explorer_pid=%lu %s\n",
               i + 1, parent, submenu, (unsigned long)pid,
               parent >= 1 && submenu >= 2 && pid == original ? "PASS" : "FAIL");
        if (parent < 1 || submenu < 2 || pid != original) ++failures;
        tap(VK_ESCAPE); tap(VK_ESCAPE);
        Sleep(500);
        if (pid != original) break;
    }
    printf("failures=%d\n", failures);
    return failures != 0;
}
