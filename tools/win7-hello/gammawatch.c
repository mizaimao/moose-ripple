/* Does Windows fade with the display's gamma ramp around logon? Poll the ramp every few
 * milliseconds for 20 seconds and write every change of its top red value, with the moment
 * this session's Default desktop becomes the input desktop, to gammawatch.txt in the user's
 * temporary folder. Start it from a task that runs at logon.
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>

enum { MAX_EVENTS = 4000 };

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR args, int show)
{
    (void)instance; (void)prev; (void)args; (void)show;
    static struct { double ms; int kind; long value; } events[MAX_EVENTS];
    int count = 0;
    HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    HDC screen = GetDC(NULL);
    LARGE_INTEGER freq, origin, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&origin);
    long last = -2, polls = 0, failures = 0;
    BOOL was_active = FALSE;
    double ms = 0;
    while (ms < 20000 && count < MAX_EVENTS - 2) {
        QueryPerformanceCounter(&now);
        ms = (now.QuadPart - origin.QuadPart) * 1000.0 / freq.QuadPart;
        WORD ramp[3][256];
        long top = GetDeviceGammaRamp(screen, ramp) ? ramp[0][255] : -1;
        ++polls;
        if (top < 0) ++failures;
        if (top != last) {
            events[count].ms = ms; events[count].kind = 0; events[count++].value = top;
            last = top;
        }
        BOOL active = FALSE;
        DWORD needed;
        if (GetUserObjectInformationW(desktop, UOI_IO, &active, sizeof(active), &needed) &&
            active != was_active) {
            events[count].ms = ms; events[count].kind = 1; events[count++].value = active;
            was_active = active;
        }
        Sleep(3);
    }

    char folder[MAX_PATH], path[MAX_PATH];
    DWORD len = GetTempPathA(sizeof(folder), folder);
    if (!len || len >= sizeof(folder) - 20) return 1;
    snprintf(path, sizeof(path), "%sgammawatch.txt", folder);
    FILE *out = fopen(path, "w");
    if (!out) return 1;
    fprintf(out, "%ld polls in %.0f ms, %ld could not read the ramp, %d events\n", polls, ms, failures, count);
    for (int i = 0; i < count; ++i) {
        if (events[i].kind)
            fprintf(out, "%8.0f ms  Default desktop %s\n", events[i].ms,
                    events[i].value ? "active" : "inactive");
        else
            fprintf(out, "%8.0f ms  ramp top %ld\n", events[i].ms, events[i].value);
    }
    fclose(out);
    return 0;
}
