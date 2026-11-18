/* Exercise restore, move and resize of a dedicated Explorer window on Windows 7.
 * Run with the transition ETW provider enabled (tools/win7-debug/README.md).
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include <windows.h>
#include <oleacc.h>
#include <shellapi.h>
#include <dwmapi.h>
#include "explorermotion.h"
#include "explorerpixels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static BOOL sample_transition(BOOL restore, int round, RECT patch)
{
    struct pixel_run *run = start_pixels(target, NULL, 0, patch, TRUE);
    if (!run) return FALSE;
    Sleep(300);
    BOOL ok = restore ? click_restore() : transition(FALSE);
    return finish_pixels(run, restore ? "restore" : "minimize", round) && ok;
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
    BOOL animation_pixels = argc == 3 && !strcmp(argv[2], "animationpixels");
    BOOL clicks = animation_pixels || (argc == 3 && !strcmp(argv[2], "taskbar"));
    enum window_gesture gesture = RESIZE_WINDOW;
    if (argc == 3 && !strcmp(argv[2], "search")) gesture = CHECK_SEARCH_BAND;
    if (argc == 3 && !strcmp(argv[2], "searchphase")) gesture = TIME_SEARCH_REPAINT;
    if (argc == 3 && !strcmp(argv[2], "searchpaint")) gesture = TIME_SEARCH_PAINT;
    BOOL search = gesture != RESIZE_WINDOW;
    BOOL motion = search || (argc == 3 && !strcmp(argv[2], "motion"));
    if (argc >= 2) {
        rounds = strtol(argv[1], &end, 10);
        if (*end) rounds = 0;
    }
    if ((argc > 2 && !clicks && !motion) || rounds < 1 || rounds > 100) {
        fprintf(stderr, "Usage: explorerrestore.exe [1-100 [taskbar|motion|search|searchphase|searchpaint|animationpixels]]\n");
        return 1;
    }
    HWND console = NULL;
    POINT original_cursor;
    BOOL saved_cursor = FALSE;
    if (clicks && FAILED(CoInitialize(NULL))) return 1;
    if (EventRegister(&motion_provider, NULL, NULL, &trace)) goto done;
    if (!EventProviderEnabled(trace, 4, 0) ||
        FAILED(DwmIsCompositionEnabled(&composition)) || !composition ||
        !SystemParametersInfo(SPI_GETANIMATION, sizeof(anim), &anim, 0) || !anim.iMinAnimate) {
        fprintf(stderr, "Enable Aero, window animation and the transition ETW provider.\n");
        goto done;
    }
    if (clicks || motion) {
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
    DWORD thread = GetWindowThreadProcessId(target, &pid);
    printf("explorer_pid=%lu window=%p size=800x540 desktop=%dx%d\n", pid, (void *)target,
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (motion) printf("explorer_ui_thread=%lu\n", thread);
    Sleep(2000);
    RECT patch = {0};
    if (clicks) {
        if (!find_button()) {
            fprintf(stderr, "Dedicated folder taskbar button not found.\n");
            goto done;
        }
        printf("taskbar_button=%ld,%ld\n", button.x, button.y);
        if (animation_pixels) {
            RECT w, bar;
            if (!GetWindowRect(target, &w) || !GetWindowRect(taskbar, &bar) ||
                bar.top <= w.bottom + 32 || button.x <= w.left + 2 || button.x >= w.right - 2) {
                fprintf(stderr, "Place the taskbar below the folder with its button inside the folder's horizontal span.\n");
                goto done;
            }
            patch = (RECT){button.x, w.top + 8, button.x + 2, bar.top - 16};
        }
    }
    if (motion && EventWriteString(trace, 4, 0, L"motion-run-start")) goto done;
    for (int i = 0; i < rounds; ++i) {
        if (motion) {
            if ((!search && !measure_window_motion(target, trace, MOVE_WINDOW, i + 1)) ||
                !measure_window_motion(target, trace, gesture, i + 1) ||
                FAILED(DwmIsCompositionEnabled(&composition)) || !composition) {
                fprintf(stderr, "Movement failed at round %d.\n", i + 1);
                goto done;
            }
            continue;
        }
        BOOL minimized = animation_pixels ? sample_transition(FALSE, i + 1, patch) : transition(FALSE);
        if (!minimized || !(animation_pixels ? sample_transition(TRUE, i + 1, patch) :
                            clicks ? click_restore() : transition(TRUE)) ||
            FAILED(DwmIsCompositionEnabled(&composition)) || !composition) {
            fprintf(stderr, "Transition failed at round %d.\n", i + 1);
            goto done;
        }
        printf("round=%d restored=1 composition=1\n", i + 1);
    }
    if (motion && EventWriteString(trace, 4, 0, L"motion-run-stop")) goto done;
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
