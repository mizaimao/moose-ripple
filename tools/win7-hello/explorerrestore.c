/* Time restores of a dedicated Explorer window on Windows 7.
 * Run with the transition ETW provider enabled (tools/win7-debug/README.md).
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include <windows.h>
#include <oleacc.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <evntprov.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID provider = {0x6c38bba1,0x1768,0x48c6,{0x9c,0x07,0x40,0xda,0xdb,0xa4,0xb3,0x8f}};
static REGHANDLE trace;
static char folder[MAX_PATH], name[80];
static HWND target;
static HWND taskbar;
static POINT button, parked;

/* Resolve the dedicated folder by its accessible name. Pinned icons and
 * grouped buttons can otherwise open a different window. */
static BOOL find_button(void)
{
    RECT bar;
    wchar_t text[80];
    taskbar = FindWindowA("Shell_TrayWnd", NULL);
    if (!GetWindowRect(taskbar, &bar) ||
        !MultiByteToWideChar(CP_ACP, 0, name, -1, text, 80)) return FALSE;
    BOOL horizontal = bar.right - bar.left > bar.bottom - bar.top;
    LONG end = horizontal ? bar.right : bar.bottom;
    for (LONG i = horizontal ? bar.left : bar.top; i < end; i += 8) {
        POINT point = {horizontal ? i : (bar.left + bar.right) / 2,
                       horizontal ? (bar.top + bar.bottom) / 2 : i};
        IAccessible *object = NULL;
        VARIANT child;
        BSTR title = NULL;
        VariantInit(&child);
        if (FAILED(AccessibleObjectFromPoint(point, &object, &child))) continue;
        BOOL match = SUCCEEDED(IAccessible_get_accName(object, child, &title)) &&
                     title && wcsstr(title, text);
        if (match) {
            LONG x, y, width, height;
            button = point;
            if (SUCCEEDED(IAccessible_accLocation(object, &x, &y, &width, &height, child)) &&
                width > 0 && height > 0) {
                button.x = x + width / 2;
                button.y = y + height / 2;
            }
        }
        SysFreeString(title);
        VariantClear(&child);
        IAccessible_Release(object);
        if (match) {
            wchar_t marker[160];
            RECT window;
            if (!GetWindowRect(target, &window) || !PtInRect(&bar, button)) return FALSE;
            swprintf(marker, 160, L"taskbar %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
                     bar.left, bar.top, bar.right, bar.bottom, button.x, button.y,
                     window.left, window.top, window.right, window.bottom);
            EventWriteString(trace, 4, 0, marker);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL click_restore(void)
{
    INPUT click[2] = {0};
    if (!IsIconic(target) || GetAncestor(WindowFromPoint(button), GA_ROOT) != taskbar ||
        !SetCursorPos(button.x, button.y)) return FALSE;
    click[0].type = click[1].type = INPUT_MOUSE;
    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (EventWriteString(trace, 4, 0, L"restore-start") ||
        SendInput(2, click, sizeof(INPUT)) != 2) return FALSE;
    ULONGLONG start = GetTickCount64();
    while (IsIconic(target) && GetTickCount64() - start < 1200) Sleep(2);
    if (!IsWindow(target) || IsIconic(target)) return FALSE;
    EventWriteString(trace, 4, 0, L"restore-unminimized");
    while (GetTickCount64() - start < 1500) Sleep(20);
    SetCursorPos(parked.x, parked.y);
    return GetForegroundWindow() == target;
}

static BOOL CALLBACK find_folder(HWND w, LPARAM unused)
{
    char cls[64], title[MAX_PATH], exe[MAX_PATH];
    DWORD pid, size = sizeof(exe);
    HANDLE process;
    (void)unused;
    if (!IsWindowVisible(w) || !GetClassNameA(w, cls, sizeof(cls)) ||
        (strcmp(cls, "CabinetWClass") && strcmp(cls, "ExploreWClass")) ||
        !GetWindowTextA(w, title, sizeof(title)) ||
        (_stricmp(title, name) && _stricmp(title, folder))) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    if (QueryFullProcessImageNameA(process, 0, exe, &size)) {
        const char *leaf = strrchr(exe, '\\');
        if (leaf && !_stricmp(leaf + 1, "explorer.exe")) target = w;
    }
    CloseHandle(process);
    return !target;
}

static BOOL transition(BOOL restore)
{
    if (!IsWindow(target) || !!IsIconic(target) != restore) return FALSE;
    if (EventWriteString(trace, 4, 0, restore ? L"restore-start" : L"minimize-start") ||
        !PostMessage(target, WM_SYSCOMMAND, restore ? SC_RESTORE : SC_MINIMIZE, 0))
        return FALSE;
    Sleep(1500);
    return IsWindow(target) && !!IsIconic(target) != restore;
}

int main(int argc, char **argv)
{
    BOOL composition = FALSE;
    ANIMATIONINFO anim = {sizeof(anim), 0};
    DWORD pid, temp_len;
    char temp[MAX_PATH];
    BOOL created = FALSE;
    int rc = 1;
    char *end;
    long rounds = 30;
    BOOL clicks = argc == 3 && !strcmp(argv[2], "taskbar");
    if (argc >= 2) {
        rounds = strtol(argv[1], &end, 10);
        if (*end) rounds = 0;
    }
    if ((argc > 2 && !clicks) || rounds < 1 || rounds > 100) {
        fprintf(stderr, "Usage: explorerrestore.exe [rounds from 1 to 100 [taskbar]]\n");
        return 1;
    }
    HWND console = NULL;
    POINT original_cursor;
    BOOL saved_cursor = FALSE;
    if (clicks && FAILED(CoInitialize(NULL))) return 1;
    if (EventRegister(&provider, NULL, NULL, &trace)) goto done;
    if (!EventProviderEnabled(trace, 4, 0) ||
        FAILED(DwmIsCompositionEnabled(&composition)) || !composition ||
        !SystemParametersInfo(SPI_GETANIMATION, sizeof(anim), &anim, 0) || !anim.iMinAnimate) {
        fprintf(stderr, "Enable Aero, window animation and the transition ETW provider.\n");
        goto done;
    }
    if (clicks) {
        RECT work;
        if (!GetCursorPos(&original_cursor) ||
            !SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0)) goto done;
        saved_cursor = TRUE;
        parked.x = work.right - 8;
        parked.y = work.top + 8;
        if (!SetCursorPos(parked.x, parked.y)) goto done;
        if (IsWindowVisible(GetConsoleWindow())) {
            console = GetConsoleWindow();
            ShowWindow(console, SW_HIDE);
        }
    }
    temp_len = GetTempPathA(sizeof(temp), temp);
    snprintf(name, sizeof(name), "RippleRestore-%lu-%lu", GetCurrentProcessId(), GetTickCount());
    if (!temp_len || temp_len >= sizeof(temp) || temp_len + strlen(name) >= sizeof(folder))
        goto done;
    snprintf(folder, sizeof(folder), "%s%s", temp, name);
    if (!CreateDirectoryA(folder, NULL)) goto done;
    created = TRUE;
    if ((INT_PTR)ShellExecuteA(NULL, "open", folder, NULL, NULL, SW_SHOWNORMAL) <= 32) goto done;
    for (int i = 0; i < 150 && !target; ++i) {
        EnumWindows(find_folder, 0);
        if (!target) Sleep(100);
    }
    if (!target) { fprintf(stderr, "Dedicated Explorer window missing.\n"); goto done; }
    ShowWindow(target, SW_RESTORE);
    if (!SetWindowPos(target, HWND_TOP, 80, 80, 800, 540, SWP_SHOWWINDOW)) goto done;
    GetWindowThreadProcessId(target, &pid);
    printf("explorer_pid=%lu window=%p size=800x540 desktop=%dx%d\n", pid, (void *)target,
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    Sleep(2000);
    if (clicks) {
        if (!find_button()) {
            fprintf(stderr, "Dedicated folder taskbar button not found.\n");
            goto done;
        }
        printf("taskbar_button=%ld,%ld\n", button.x, button.y);
    }
    for (int i = 0; i < rounds; ++i) {
        if (!transition(FALSE) || !(clicks ? click_restore() : transition(TRUE)) ||
            FAILED(DwmIsCompositionEnabled(&composition)) || !composition) {
            fprintf(stderr, "Transition failed at round %d.\n", i + 1);
            goto done;
        }
        printf("round=%d restored=1 composition=1\n", i + 1);
    }
    rc = 0;
done:
    if (target && IsWindow(target)) {
        PostMessage(target, WM_CLOSE, 0, 0);
        for (int i = 0; i < 50 && IsWindow(target); ++i) Sleep(100);
    }
    if (created && !RemoveDirectoryA(folder))
        fprintf(stderr, "Temporary folder retained (error %lu).\n", GetLastError());
    if (saved_cursor) SetCursorPos(original_cursor.x, original_cursor.y);
    if (console) ShowWindow(console, SW_SHOWNA);
    if (trace) EventUnregister(trace);
    if (clicks) CoUninitialize();
    return rc;
}
