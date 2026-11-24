/* Compare the tick clock with QPC while requesting several timer resolutions.
 * Each request is released before the next measurement. The console is hidden
 * to avoid cursor drawing keeping the display driver busy. Run on an otherwise
 * idle guest; other processes can request a finer timer than this test.
 * Run: timerbench.exe > timerbench.txt
 * "timerbench.exe counter" checks that QPC never steps backwards; "timerbench.exe cost"
 * measures the processor time the ticks take at each resolution; "timerbench.exe hold 10000 60"
 * keeps a 1 ms timer requested for a minute while another test runs.
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef LONG (WINAPI *query_fn)(PULONG, PULONG, PULONG);
typedef LONG (WINAPI *set_fn)(ULONG, BOOLEAN, PULONG);

/* Use the tick clock to end each run even if QPC goes backwards. */
static int check_counter(void)
{
    const char *modes[] = {"pinned", "unbound", "rotating"};
    DWORD_PTR allowed, system, cpus[sizeof(DWORD_PTR) * 8];
    HANDLE thread = GetCurrentThread();
    LARGE_INTEGER frequency;
    unsigned count = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) ||
        !allowed || !QueryPerformanceFrequency(&frequency)) return 2;
    for (DWORD_PTR bit = 1; bit; bit <<= 1)
        if (allowed & bit) cpus[count++] = bit;
    DWORD_PTR original = SetThreadAffinityMask(thread, cpus[0]);
    if (!original) return 2;
    int rc = 0;
    printf("frequency=%lld processors=%u\n", frequency.QuadPart, count);
    for (unsigned mode = 0; mode < 3; ++mode) {
        LARGE_INTEGER previous, now;
        LONGLONG worst = 0;
        unsigned samples = 0, backwards = 0;
        if (!SetThreadAffinityMask(thread, mode == 0 ? cpus[0] : allowed) ||
            !QueryPerformanceCounter(&previous)) { rc = 2; break; }
        ULONGLONG start = GetTickCount64();
        do {
            if (mode == 2 && !SetThreadAffinityMask(thread, cpus[samples % count])) {
                rc = 2;
                break;
            }
            Sleep(1);
            if (!QueryPerformanceCounter(&now)) { rc = 2; break; }
            LONGLONG delta = now.QuadPart - previous.QuadPart;
            if (delta < 0) {
                if (!backwards)
                    printf("first regression: previous=%lld current=%lld\n", previous.QuadPart, now.QuadPart);
                ++backwards;
                if (delta < worst) worst = delta;
            }
            previous = now;
            ++samples;
        } while (GetTickCount64() - start < 12000);
        printf("mode=%s samples=%u backwards=%u worst_ms=%.3f\n",
               modes[mode], samples, backwards, 1000.0 * worst / frequency.QuadPart);
        if (rc == 2) break;
        if (backwards) rc = 1;
    }
    return SetThreadAffinityMask(thread, original) ? rc : 2;
}

/* What the timer ticks cost: the same arithmetic on every processor for three counter seconds
 * at each timer resolution. Where each tick is expensive (an emulated processor), a fine timer
 * takes a visible share of the machine, and a host that delivers late ticks afterwards takes
 * more than one that drops them. */
static volatile LONG cost_stop;

static DWORD WINAPI cost_thread(void *arg)
{
    unsigned long long *count = arg, n = 0;
    unsigned x = 1;
    while (!cost_stop) {
        for (int i = 0; i < 4096; ++i) x = x * 1664525u + 1013904223u;
        n += 1 + (x & 0);
    }
    *count = n;
    return 0;
}

struct counter_reads { unsigned long long calls, backwards; };

static DWORD WINAPI counter_thread(void *arg)
{
    struct counter_reads *reads = arg;
    LARGE_INTEGER previous, now;
    unsigned long long calls = 0, backwards = 0;
    QueryPerformanceCounter(&previous);
    while (!cost_stop) {
        for (int i = 0; i < 64; ++i) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart < previous.QuadPart) ++backwards;
            previous = now;
        }
        calls += 64;
    }
    reads->calls = calls;
    reads->backwards = backwards;
    return 0;
}

static int check_cost(void)
{
    ULONG requests[] = {0, 10000, 5000, 0};
    query_fn query = (query_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    set_fn set = (set_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution");
    SYSTEM_INFO info;
    LARGE_INTEGER frequency, start, now;
    if (!query || !set || !QueryPerformanceFrequency(&frequency)) return 2;
    GetSystemInfo(&info);
    unsigned threads = info.dwNumberOfProcessors > 64 ? 64 : info.dwNumberOfProcessors;
    printf("processors=%u frequency=%lld\n", threads, frequency.QuadPart);
    for (unsigned i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        HANDLE handles[64];
        unsigned long long counts[64] = {0}, total = 0;
        ULONG minimum, maximum, current;
        Sleep(500);
        if (requests[i] && set(requests[i], TRUE, &current) < 0) return 1;
        query(&minimum, &maximum, &current);
        cost_stop = 0;
        ULONGLONG tick = GetTickCount64();
        QueryPerformanceCounter(&start);
        for (unsigned t = 0; t < threads; ++t)
            handles[t] = CreateThread(NULL, 0, cost_thread, &counts[t], 0, NULL);
        do {
            Sleep(20);
            QueryPerformanceCounter(&now);
        } while ((now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart < 3000);
        cost_stop = 1;
        WaitForMultipleObjects(threads, handles, TRUE, INFINITE);
        QueryPerformanceCounter(&now);
        tick = GetTickCount64() - tick;
        for (unsigned t = 0; t < threads; ++t) {
            total += counts[t];
            CloseHandle(handles[t]);
        }
        double seconds = (double)(now.QuadPart - start.QuadPart) / frequency.QuadPart;
        printf("request=%lu actual=%lu work_per_s=%.0f qpc_ms=%.3f tick_ms=%llu\n", requests[i], current,
               total / seconds, seconds * 1000, tick);
        fflush(stdout);
        if (requests[i]) set(requests[i], FALSE, &current);
    }
    /* What reading the counter costs, from one thread and then from every processor at once.
     * The tick clock ends each run, so a counter that steps backwards cannot hold it up. */
    const unsigned readers[] = {1, threads};
    for (unsigned i = 0; i < (threads > 1 ? 2u : 1u); ++i) {
        HANDLE handles[64];
        struct counter_reads reads[64] = {{0, 0}};
        unsigned long long total = 0, backwards = 0;
        Sleep(500);
        cost_stop = 0;
        ULONGLONG tick = GetTickCount64();
        QueryPerformanceCounter(&start);
        for (unsigned t = 0; t < readers[i]; ++t)
            handles[t] = CreateThread(NULL, 0, counter_thread, &reads[t], 0, NULL);
        while (GetTickCount64() - tick < 3000)
            Sleep(20);
        cost_stop = 1;
        WaitForMultipleObjects(readers[i], handles, TRUE, INFINITE);
        QueryPerformanceCounter(&now);
        tick = GetTickCount64() - tick;
        for (unsigned t = 0; t < readers[i]; ++t) {
            total += reads[t].calls;
            backwards += reads[t].backwards;
            CloseHandle(handles[t]);
        }
        double seconds = (double)(now.QuadPart - start.QuadPart) / frequency.QuadPart;
        printf("counter_readers=%u reads_per_s=%.0f backwards=%llu qpc_ms=%.3f tick_ms=%llu\n", readers[i],
               total / seconds, backwards, seconds * 1000, tick);
        fflush(stdout);
    }
    return 0;
}

/* Keep a timer resolution requested (in 100 ns units; 0 requests nothing) for some seconds, so
 * that another test can run with and without a fine timer in force. */
static int hold(ULONG request, unsigned seconds)
{
    query_fn query = (query_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    set_fn set = (set_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution");
    ULONG minimum, maximum, current;
    if (!query || !set || (request && set(request, TRUE, &current) < 0)) return 2;
    query(&minimum, &maximum, &current);
    printf("request=%lu actual=%lu for %u s\n", request, current, seconds);
    fflush(stdout);
    Sleep(seconds * 1000);
    query(&minimum, &maximum, &current);
    printf("actual at the end=%lu\n", current);
    if (request) set(request, FALSE, &current);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "counter")) return check_counter();
    if (argc == 2 && !strcmp(argv[1], "cost")) return check_cost();
    if (argc == 4 && !strcmp(argv[1], "hold"))
        return hold(strtoul(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 10));
    if (argc != 1) {
        fprintf(stderr, "Usage: timerbench.exe [counter|cost|hold UNITS_OF_100NS SECONDS]\n");
        return 2;
    }
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
        now = start;
        BOOL backwards = FALSE;
        do {
            LARGE_INTEGER previous = now;
            Sleep(20);
            ++samples;
            QueryPerformanceCounter(&now);
            backwards = now.QuadPart < previous.QuadPart;
        } while (!backwards && (now.QuadPart-start.QuadPart)*1000/frequency.QuadPart < 3000);
        printf("request=%lu actual=%lu qpc_ms=%.3f tick_ms=%llu sleeps=%u\n", requests[i], current,
            (double)(now.QuadPart-start.QuadPart)*1000/frequency.QuadPart, GetTickCount64()-tick, samples);
        if (requests[i]) set(requests[i], FALSE, &current);
        if (backwards) {
            fprintf(stderr, "QPC moved backwards during the timer-resolution test.\n");
            if (console_visible) ShowWindow(console, SW_SHOW);
            return 1;
        }
        fflush(stdout);
    }
    if (console_visible) ShowWindow(console, SW_SHOW);
    return 0;
}
