/* Compare Explorer with a resizable, solid-background Win32 window. MIT (LICENSE). */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <dwmapi.h>
#include "explorermotion.h"
#include <stdio.h>
#include <stdlib.h>

static REGHANDLE trace;
static HWND window;

static DWORD WINAPI exercise(void *arg)
{
    long rounds = *(const long *)arg;
    BOOL ok = EventWriteString(trace, 4, 0, L"motion-run-start") == 0;
    for (int i = 1; i <= rounds && ok; ++i) {
        BOOL aero = FALSE;
        ok = measure_window_motion(window, trace, MOVE_WINDOW, i) && measure_window_motion(window, trace, RESIZE_WINDOW, i) &&
             SUCCEEDED(DwmIsCompositionEnabled(&aero)) && aero;
    }
    if (ok) ok = EventWriteString(trace, 4, 0, L"motion-run-stop") == 0;
    PostMessage(window, WM_CLOSE, 0, 0);
    return ok ? 0 : 1;
}

static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProc(w, msg, wp, lp);
}

int main(int argc, char **argv)
{
    char *end = NULL;
    long rounds = argc == 2 ? strtol(argv[1], &end, 10) : 6;
    if (argc > 2 || (end && *end) || rounds < 1 || rounds > 100) {
        fprintf(stderr, "Usage: motioncontrol.exe [rounds from 1 to 100]\n");
        return 1;
    }
    DWORD rc = 1;
    POINT cursor;
    BOOL aero = FALSE;
    if (!GetCursorPos(&cursor) || FAILED(DwmIsCompositionEnabled(&aero)) || !aero ||
        EventRegister(&motion_provider, NULL, NULL, &trace)) return 1;
    HWND console = IsWindowVisible(GetConsoleWindow()) ? GetConsoleWindow() : NULL;
    if (console) ShowWindow(console, SW_HIDE);
    WNDCLASS wc = {0};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "RippleMotionControl";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!EventProviderEnabled(trace, 4, 0) || !RegisterClass(&wc)) goto done;
    window = CreateWindow(wc.lpszClassName, "Movement control", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          24, 24, 680, 440, NULL, NULL, wc.hInstance, NULL);
    if (!window) goto done;
    /* The window's thread must keep dispatching messages while the other thread moves it. */
    HANDLE worker = CreateThread(NULL, 0, exercise, &rounds, 0, NULL);
    if (worker) {
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        WaitForSingleObject(worker, INFINITE);
        GetExitCodeThread(worker, &rc);
        CloseHandle(worker);
    }
    if (IsWindow(window)) DestroyWindow(window);
done:
    SetCursorPos(cursor.x, cursor.y);
    if (console) ShowWindow(console, SW_SHOWNA);
    EventUnregister(trace);
    return rc;
}
