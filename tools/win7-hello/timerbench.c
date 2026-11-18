/* Compare the tick clock with QPC while requesting several timer resolutions.
 * Each request is released before the next measurement. The console is hidden
 * to avoid cursor drawing keeping the display driver busy. Run on an otherwise
 * idle guest; other processes can request a finer timer than this test.
 * Run: timerbench.exe > timerbench.txt
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
typedef LONG (WINAPI *query_fn)(PULONG, PULONG, PULONG);
typedef LONG (WINAPI *set_fn)(ULONG, BOOLEAN, PULONG);
int main(void) {
    ULONG requests[] = {0, 40000, 20000, 10000, 5000, 0};
    unsigned i;
    int cpu[4]; char vendor[13] = {0};
    LARGE_INTEGER frequency, start, now;
    HWND console = GetConsoleWindow();
    BOOL console_visible = IsWindowVisible(console);
    query_fn query = (query_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    set_fn set = (set_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution");
    if (!query || !set) return 2;
    if (console_visible) ShowWindow(console, SW_HIDE);
    __cpuid(cpu, 0x40000000);
    memcpy(vendor, &cpu[1], 12);
    printf("hypervisor=%s maxleaf=%08x\n", vendor, cpu[0]);
    QueryPerformanceFrequency(&frequency);
    for (i = 0; i < sizeof(requests)/sizeof(requests[0]); ++i) {
        ULONG minimum, maximum, current;
        ULONGLONG tick;
        unsigned samples = 0;
        Sleep(1000);
        if (requests[i] && set(requests[i], TRUE, &current) < 0) {
            if (console_visible) ShowWindow(console, SW_SHOW);
            return 1;
        }
        query(&minimum, &maximum, &current);
        tick = GetTickCount64(); QueryPerformanceCounter(&start);
        do { Sleep(20); ++samples; QueryPerformanceCounter(&now); }
        while ((now.QuadPart-start.QuadPart)*1000/frequency.QuadPart < 3000);
        printf("request=%lu actual=%lu qpc_ms=%.3f tick_ms=%llu sleeps=%u\n", requests[i], current,
            (double)(now.QuadPart-start.QuadPart)*1000/frequency.QuadPart, GetTickCount64()-tick, samples);
        if (requests[i]) set(requests[i], FALSE, &current);
        fflush(stdout);
    }
    if (console_visible) ShowWindow(console, SW_SHOW);
    return 0;
}
