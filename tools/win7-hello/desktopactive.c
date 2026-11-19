/* Trace when this logon session's Default desktop becomes active. MIT (LICENSE). */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <dwmapi.h>
#include <evntprov.h>
#include <wchar.h>

static const GUID provider = {0x6c38bba1,0x1768,0x48c6,{0x9c,0x07,0x40,0xda,0xdb,0xa4,0xb3,0x8f}};

static int observe(REGHANDLE trace)
{
    HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    WCHAR name[80], mode[64];
    DWORD needed;
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) ||
        _wcsicmp(name, L"Default")) return 1;
    if (swprintf(mode, ARRAYSIZE(mode), L"watch-%dx%d", GetSystemMetrics(SM_CXSCREEN),
                 GetSystemMetrics(SM_CYSCREEN)) < 0) return 1;
    if (EventWriteString(trace, 4, 0, mode)) return 1;

    ULONGLONG start = GetTickCount64(), last_query = 0, first_active = 0;
    BOOL saw_shell = FALSE;
    int last_composition = -1;
    do {
        BOOL active;
        if (!GetUserObjectInformationW(desktop, UOI_IO, &active, sizeof(active), &needed)) return 1;
        ULONGLONG now = GetTickCount64();
        if (!first_active) {
            /* Consecutive samples bound the switch even if the VM misses several ticks. */
            if (EventWriteString(trace, 4, 0, active ? L"watch-default-active" :
                                                      L"watch-default-inactive")) return 1;
            if (active) first_active = now;
        }
        if (!saw_shell && GetShellWindow()) {
            if (EventWriteString(trace, 4, 0, L"watch-shell-window")) return 1;
            saw_shell = TRUE;
        }
        if (!last_query || now - last_query >= 100 || now == first_active) {
            BOOL enabled;
            int composition = SUCCEEDED(DwmIsCompositionEnabled(&enabled)) ? !!enabled : -1;
            if (!last_query || last_composition != composition) {
                const WCHAR *state = composition < 0 ? L"watch-compose-error" :
                                     composition ? L"watch-compose-on" : L"watch-compose-off";
                if (EventWriteString(trace, 4, 0, state)) return 1;
                last_composition = composition;
            }
            last_query = now;
        }
        if (first_active && now - first_active >= 2500) return 0;
        Sleep(10);
    } while (GetTickCount64() - start < 30000);
    return 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR args, int show)
{
    (void)instance; (void)prev; (void)args; (void)show;
    REGHANDLE trace;
    if (EventRegister(&provider, NULL, NULL, &trace)) return 1;
    int rc = 1;
    if (EventProviderEnabled(trace, 4, 0) && !EventWriteString(trace, 4, 0, L"watch-start")) {
        rc = observe(trace);
        if (EventWriteString(trace, 4, 0, rc ? L"watch-incomplete" : L"watch-done")) rc = 1;
    }
    EventUnregister(trace);
    return rc;
}
