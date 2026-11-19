/* Time Windows' fading desktop switch (the one logon uses) on the installed display driver:
 * switch to an empty desktop of our own and back with SwitchDesktopWithFade, and watch the
 * display's gamma ramp meanwhile from a second thread.
 *   fadeprobe.exe [FADE_MS]      default 1000
 * Prints whether the gamma ramp can be read and set, how long each switch took, and the lowest
 * top-of-ramp value seen during each. MIT licence (LICENSE at the repository root).
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef BOOL (WINAPI *fade_fn)(HDESK, DWORD);

static volatile LONG watching;
static volatile LONG lowest = 65535, reads, failed;
static DWORD last_error;

static DWORD WINAPI watch(void *arg)
{
    (void)arg;
    HDC screen = GetDC(NULL);
    WORD ramp[3][256];
    while (watching) {
        if (GetDeviceGammaRamp(screen, ramp)) {
            if (ramp[0][255] < lowest) lowest = ramp[0][255];
            InterlockedIncrement(&reads);
        } else {
            InterlockedIncrement(&failed);
        }
        Sleep(5);
    }
    ReleaseDC(NULL, screen);
    return 0;
}

static double timed(fade_fn fade, HDESK target, DWORD ms, BOOL *ok)
{
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    lowest = 65535; reads = 0; failed = 0;
    QueryPerformanceCounter(&a);
    SetLastError(0);
    *ok = fade ? fade(target, ms) : SwitchDesktop(target);
    last_error = GetLastError();
    QueryPerformanceCounter(&b);
    return (b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;
}

int main(int argc, char **argv)
{
    const DWORD ms = argc > 1 ? (DWORD)atoi(argv[1]) : 1000;
    HDESK home = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!home) { fprintf(stderr, "no input desktop (%lu)\n", GetLastError()); return 1; }
    fade_fn fade = (fade_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SwitchDesktopWithFade");
    printf("SwitchDesktopWithFade %s, fade asked %lu ms\n", fade ? "found" : "missing", ms);

    HDC screen = GetDC(NULL);
    WORD ramp[3][256];
    BOOL readable = GetDeviceGammaRamp(screen, ramp);
    printf("gamma ramp readable %d", readable);
    if (readable) {
        SetLastError(0);
        BOOL set = SetDeviceGammaRamp(screen, ramp);
        printf(", red 0..255 = %u..%u, setting the same ramp %d (error %lu)", ramp[0][0], ramp[0][255],
               set, GetLastError());
    }
    printf("\n");
    if (readable) {
        /* half brightness for a moment: does the driver take it, and does a capture show it? */
        WORD dim[3][256];
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) dim[c][i] = (WORD)(ramp[c][i] / 2);
        const int x = GetSystemMetrics(SM_CXSCREEN) / 2, y = 8;
        COLORREF before = GetPixel(screen, x, y);
        SetLastError(0);
        BOOL set = SetDeviceGammaRamp(screen, dim);
        DWORD error = GetLastError();
        Sleep(600);
        WORD now[3][256];
        BOOL reread = GetDeviceGammaRamp(screen, now);
        COLORREF during = GetPixel(screen, x, y);
        BOOL restored = SetDeviceGammaRamp(screen, ramp);
        printf("half ramp: set %d (error %lu), read back top %u, captured pixel %06lx then %06lx, restored %d\n",
               set, error, reread ? now[0][255] : 0, (unsigned long)before, (unsigned long)during, restored);
    }
    ReleaseDC(NULL, screen);

    HDESK other = CreateDesktopW(L"RippleFadeProbe", NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!other) { fprintf(stderr, "CreateDesktop failed (%lu)\n", GetLastError()); return 1; }
    watching = 1;
    HANDLE thread = CreateThread(NULL, 0, watch, NULL, 0, NULL);

    BOOL ok;
    double t = timed(fade, other, ms, &ok);
    printf("to the empty desktop: ok %d (error %lu) in %.0f ms, ramp top lowest %ld in %ld reads (%ld failed)\n",
           ok, last_error, t, lowest, reads, failed);
    if (!ok) {
        t = timed(NULL, other, 0, &ok);
        printf("plain SwitchDesktop instead: ok %d (error %lu) in %.0f ms\n", ok, last_error, t);
    }
    Sleep(700);
    t = timed(fade, home, ms, &ok);
    printf("back: ok %d (error %lu) in %.0f ms, ramp top lowest %ld in %ld reads (%ld failed)\n",
           ok, last_error, t, lowest, reads, failed);
    if (!ok && !SwitchDesktop(home)) fprintf(stderr, "could not switch back (%lu)\n", GetLastError());

    watching = 0;
    if (thread) WaitForSingleObject(thread, 2000);
    CloseDesktop(other);
    CloseDesktop(home);
    return 0;
}
