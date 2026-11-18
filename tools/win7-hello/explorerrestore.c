/* Time 30 restores of a dedicated Explorer window on Windows 7.
 * Run with the transition ETW provider enabled (tools/win7-debug/README.md).
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
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
    if (argc == 2) {
        rounds = strtol(argv[1], &end, 10);
        if (*end) rounds = 0;
    }
    if (argc > 2 || rounds < 1 || rounds > 100) {
        fprintf(stderr, "Usage: explorerrestore.exe [rounds from 1 to 100]\n");
        return 1;
    }
    if (EventRegister(&provider, NULL, NULL, &trace)) return 1;
    if (!EventProviderEnabled(trace, 4, 0) ||
        FAILED(DwmIsCompositionEnabled(&composition)) || !composition ||
        !SystemParametersInfo(SPI_GETANIMATION, sizeof(anim), &anim, 0) || !anim.iMinAnimate) {
        fprintf(stderr, "Enable Aero, window animation and the transition ETW provider.\n");
        goto done;
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
    for (int i = 0; i < rounds; ++i) {
        if (!transition(FALSE) || !transition(TRUE) ||
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
    EventUnregister(trace);
    return rc;
}
