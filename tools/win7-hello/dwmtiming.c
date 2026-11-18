/* Measure Windows 7's compositor during repeatable window operations. No capture.
 * Build with llvm-mingw: clang -O2 -Wall -Wextra dwmtiming.c -ldwmapi -lgdi32
 * Run outside the driver package directory: dwmtiming.exe MODE SECONDS > run.csv
 * MODE: idle, move, repaint, resize, animate, transitions. Only this program's window is changed.
 * Counters are raw DWM_TIMING_INFO values, not inferred screen refreshes. See
 * https://learn.microsoft.com/windows/win32/api/dwmapi/ns-dwmapi-dwm_timing_info
 * Duration uses QPC: the tick clock can lose time under CPU emulation. The CSV
 * also records tick time and timer resolution. Check hr/window_hr before using
 * DWM counters; some composited desktops refuse the timing query. Actions count
 * operations requested by this tool, not rendered or displayed frames.
 * MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#include <windows.h>
#include <dwmapi.h>
#include <evntprov.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG actions;
static HANDLE sample_process;
static LARGE_INTEGER frequency, start;
static const char *mode;
static ULONGLONG tick_start;
static HWND probe_window;
/* Enable this provider alongside Microsoft-Windows-Dwm-Core to correlate each
 * ShowWindow request with compositor frames in the same clock domain. */
static const GUID transition_provider = {0x6c38bba1,0x1768,0x48c6,{0x9c,0x07,0x40,0xda,0xdb,0xa4,0xb3,0x8f}};
static REGHANDLE transition_handle;
typedef LONG (WINAPI *query_timer_fn)(PULONG, PULONG, PULONG);
static query_timer_fn query_timer;

static double elapsed_ms(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}

static unsigned long long cpu_time(HANDLE process)
{
    FILETIME created, exited, kernel, user;
    if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (((unsigned long long)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
         + (((unsigned long long)user.dwHighDateTime << 32) | user.dwLowDateTime);
}

static HANDLE compositor(DWORD *pid)
{
    HANDLE process = NULL;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry;
    DWORD session, own_session;
    *pid = 0;
    if (snapshot == INVALID_HANDLE_VALUE)
        return NULL;
    ProcessIdToSessionId(GetCurrentProcessId(), &own_session);
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, "dwm.exe") == 0 &&
                ProcessIdToSessionId(entry.th32ProcessID, &session) && session == own_session) {
                *pid = entry.th32ProcessID;
                process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                      FALSE, *pid);
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return process;
}

static void sample(void)
{
    static DWORD pid;
    static unsigned n;
    HANDLE process = sample_process;
    if (!n) printf("ms,mode,actions,query_ms,hr,dwm_pid,dwm_cpu_100ns,probe_cpu_100ns,"
           "frame,submitted,confirmed,late,outstanding,refresh,compose_qpc,"
           "displayed,dropped,missed,pixels,refresh_num,refresh_den,tick_ms,timer_100ns,window_hr\n");
    {
        DWM_TIMING_INFO t;
        double before = elapsed_ms(), after;
        HRESULT hr;
        HRESULT window_hr;
        DWM_TIMING_INFO window_timing;
        ULONG minimum = 0, maximum = 0, current = 0;
        memset(&t, 0, sizeof(t));
        t.cbSize = sizeof(t);
        hr = DwmGetCompositionTimingInfo(NULL, &t);
        memset(&window_timing, 0, sizeof(window_timing));
        window_timing.cbSize = sizeof(window_timing);
        window_hr = DwmGetCompositionTimingInfo(probe_window, &window_timing);
        if (FAILED(hr) && SUCCEEDED(window_hr)) t = window_timing;
        if (query_timer) query_timer(&minimum, &maximum, &current);
        after = elapsed_ms();
        if ((!process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0) && n % 10 == 0) {
            if (process) CloseHandle(process);
            process = compositor(&pid);
        }
        printf("%.3f,%s,%ld,%.3f,0x%08lx,%lu,%llu,%llu,"
               "%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%lu,0x%08lx\n",
               after, mode, InterlockedCompareExchange(&actions, 0, 0), after - before,
               (unsigned long)hr, (unsigned long)pid, cpu_time(process),
               cpu_time(GetCurrentProcess()), t.cFrame, t.cFrameSubmitted,
               t.cFrameConfirmed, t.cFramesLate, t.cFramesOutstanding, t.cRefresh,
               t.qpcCompose, t.cFramesDisplayed, t.cFramesDropped, t.cFramesMissed,
               t.cPixelsDrawn, t.rateRefresh.uiNumerator, t.rateRefresh.uiDenominator,
               GetTickCount64() - tick_start, current, (unsigned long)window_hr);
        if (++n % 10 == 0) fflush(stdout);
    }
    sample_process = process;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT rect;
        HDC dc = BeginPaint(window, &paint);
        LONG frame = InterlockedCompareExchange(&actions, 0, 0);
        int y;
        GetClientRect(window, &rect);
        FillRect(dc, &rect, (HBRUSH)GetStockObject(WHITE_BRUSH));
        for (y = 32; y < rect.bottom; y += 32) {
            RECT stripe = { (frame * 7) % 200, y, rect.right, y + 8 };
            FillRect(dc, &stripe, (HBRUSH)GetStockObject(GRAY_BRUSH));
        }
        const char title[] = "Compositor timing: this window closes automatically";
        TextOutA(dc, 12, 8, title, sizeof(title) - 1);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(window, message, wp, lp);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND window;
    MSG msg;
    BOOL enabled = FALSE, quit = FALSE;
    ANIMATIONINFO animation = { sizeof(animation), 0 };
    int seconds = argc > 2 ? atoi(argv[2]) : 15;
    double next_animation = 0, next_operation = 0, next_sample = 0;
    unsigned step = 0;
    mode = argc > 1 ? argv[1] : "move";
    if (seconds < 1 || seconds > 300 ||
        (strcmp(mode, "idle") && strcmp(mode, "move") && strcmp(mode, "repaint") &&
         strcmp(mode, "resize") && strcmp(mode, "animate") && strcmp(mode, "transitions"))) {
        fprintf(stderr, "Usage: dwmtiming.exe idle|move|repaint|resize|animate|transitions [1-300 seconds]\n");
        return 2;
    }
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    tick_start = GetTickCount64();
    query_timer = (query_timer_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                       "NtQueryTimerResolution");
    DwmIsCompositionEnabled(&enabled);
    SystemParametersInfo(SPI_GETANIMATION, sizeof(animation), &animation, 0);
    fprintf(stderr, "mode=%s seconds=%d composition=%d animate=%d display=%dx%d qpc=%lld\n",
            mode, seconds, enabled, animation.iMinAnimate,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), frequency.QuadPart);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "RippleDwmTiming";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassA(&wc)) return 1;
    window = CreateWindowA(wc.lpszClassName, "Ripple compositor timing", WS_OVERLAPPEDWINDOW,
                           80, 80, 673, 430, NULL, NULL, wc.hInstance, NULL);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNORMAL);
    fprintf(stderr, "initial window: visible=%d iconic=%d\n",
            IsWindowVisible(window), IsIconic(window));
    /* A parent's STARTUPINFO can override the first ShowWindow call. */
    ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    UpdateWindow(window);
    {
        DWM_TIMING_INFO timing = {0};
        HRESULT global_hr, window_hr;
        timing.cbSize = sizeof(timing);
        global_hr = DwmGetCompositionTimingInfo(NULL, &timing);
        memset(&timing, 0, sizeof(timing));
        timing.cbSize = sizeof(timing);
        window_hr = DwmGetCompositionTimingInfo(window, &timing);
        fprintf(stderr, "UI-thread timing: global=0x%08lx window=0x%08lx\n",
                (unsigned long)global_hr, (unsigned long)window_hr);
    }
    probe_window = window;
    const BOOL transitions = strcmp(mode, "transitions") == 0;
    if (transitions) {
        ULONG err = EventRegister(&transition_provider, NULL, NULL, &transition_handle);
        if (err || !EventProviderEnabled(transition_handle, 4, 0)) {
            fprintf(stderr, "Enable the transition ETW provider before running this mode.\n");
            return 1;
        }
        next_animation = elapsed_ms() + 1500;
    }
    while (!quit && elapsed_ms() < seconds * 1000.0) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = TRUE; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        /* Moving/resizing generates messages itself. A message-aware wait alone
         * can return immediately and flood the compositor with requests. */
        double now = elapsed_ms();
        if (now >= next_sample) {
            sample();
            now = elapsed_ms();
            next_sample = now + 100;
        }
        if (now < next_operation) {
            DWORD wait_ms = (DWORD)(next_operation - now) + 1;
            MsgWaitForMultipleObjects(0, NULL, FALSE, wait_ms, QS_ALLINPUT);
            continue;
        }
        next_operation = now + 16;
        if (!strcmp(mode, "move")) {
            SetWindowPos(window, NULL, 80 + (step % 240), 80 + (step % 120), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            InterlockedIncrement(&actions);
        } else if (!strcmp(mode, "resize")) {
            SetWindowPos(window, NULL, 0, 0, 641 + (step % 80), 401 + (step % 60),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            InterlockedIncrement(&actions);
        } else if (!strcmp(mode, "repaint")) {
            InvalidateRect(window, NULL, FALSE);
            UpdateWindow(window);
            InterlockedIncrement(&actions);
        } else if ((!strcmp(mode, "animate") || transitions) && elapsed_ms() >= next_animation) {
            const BOOL restore = IsIconic(window);
            if (transitions && next_animation + 1400 >= seconds * 1000.0) break;
            if (transitions)
                EventWriteString(transition_handle, 4, 0, restore ? L"restore-start" : L"minimize-start");
            ShowWindow(window, restore ? SW_RESTORE : SW_MINIMIZE);
            if (transitions)
                EventWriteString(transition_handle, 4, 0, restore ? L"restore-return" : L"minimize-return");
            next_animation = elapsed_ms() + (transitions ? 1500 : 750);
            InterlockedIncrement(&actions);
        }
        ++step;
    }
    if (sample_process) CloseHandle(sample_process);
    if (transitions) EventUnregister(transition_handle);
    fflush(stdout);
    if (IsWindow(window)) DestroyWindow(window);
    return 0;
}
