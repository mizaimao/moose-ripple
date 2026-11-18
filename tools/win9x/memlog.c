/*
 * memlog: where does a long session's memory go? (Windows 95/98)
 *
 *   memlog SECONDS [COUNT [OUT]]
 *
 * Every SECONDS one line: seconds since the start, free physical memory, free page file, what
 * Windows' memory manager counts as free, locked, disk cache, committed and swap file in use,
 * the number of threads, the free system resources
 * (Windows 9x's small fixed heaps for windows and drawing objects), and what our VxD holds
 * (contexts, buffers and the locked memory behind them, zombies, the DirectDraw heap).
 * COUNT lines (0 or none: until the program is ended). OUT is a file, a serial port such as
 * COM1 (if Windows has a driver for it), or E9 for QEMU's debug port (-debugcon), which the
 * host can read while the test runs; default: the console. Each line goes out at once.
 *
 * The memory manager's numbers come from the same place as System Monitor's: the registry key
 * HKEY_DYN_DATA\PerfStats\StatData. The first line lists every counter found there with its
 * starting value, so a counter not printed later can still be found.
 *
 * Free memory alone says little on Windows 98: the disk cache grows into free memory while a
 * game reads its files, and gives it back only when something else asks. A leak shows as free
 * memory falling while the disk cache does not grow, or as VxD memory that does not come back
 * after the program has ended.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include "rvd.h"

static HANDLE out = INVALID_HANDLE_VALUE;
static int to_e9;                               /* OUT is E9: QEMU's debug port */

static void say(const char *fmt, ...)
{
    char line[2048];
    DWORD done;
    va_list ap;
    int n;

    va_start(ap, fmt);
    line[sizeof line - 1] = 0;
    n = _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = (int)strlen(line);
    if (to_e9) {                                /* Windows 9x lets a program use the port */
        int i;
        for (i = 0; i < n; i++)
            _outp(0xE9, line[i]);
    } else if (out != INVALID_HANDLE_VALUE)
        WriteFile(out, line, (DWORD)n, &done, NULL);
    else
        fwrite(line, 1, n, stdout), fflush(stdout);
}

/* System Monitor's counters. Each must be started once (a read of the same name under
   StartStat) before StatData gives it a value. */
#define MAX_STATS 128
static char stat_name[MAX_STATS][64];
static int stats;

static void stats_start(void)
{
    HKEY data, start;
    DWORD i, len, type, v, vlen;

    if (RegOpenKeyEx(HKEY_DYN_DATA, "PerfStats\\StatData", 0, KEY_READ, &data) != ERROR_SUCCESS)
        return;
    if (RegOpenKeyEx(HKEY_DYN_DATA, "PerfStats\\StartStat", 0, KEY_READ, &start) != ERROR_SUCCESS)
        start = NULL;
    for (i = 0; stats < MAX_STATS; i++) {
        len = sizeof stat_name[0];
        if (RegEnumValue(data, i, stat_name[stats], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (start) {
            vlen = sizeof v;
            RegQueryValueEx(start, stat_name[stats], NULL, &type, (BYTE *)&v, &vlen);
        }
        stats++;
    }
    if (start)
        RegCloseKey(start);
    RegCloseKey(data);
}

/* A counter's value, or -1 if Windows doesn't have it. */
static long stat(const char *name)
{
    HKEY data;
    DWORD type, v = 0, vlen = sizeof v;
    long r = -1;

    if (RegOpenKeyEx(HKEY_DYN_DATA, "PerfStats\\StatData", 0, KEY_READ, &data) != ERROR_SUCCESS)
        return -1;
    if (RegQueryValueEx(data, name, NULL, &type, (BYTE *)&v, &vlen) == ERROR_SUCCESS && vlen == 4)
        r = (long)v;
    RegCloseKey(data);
    return r;
}

static void stats_stop(void)
{
    HKEY stop;
    DWORD type, v, vlen;
    int i;

    if (RegOpenKeyEx(HKEY_DYN_DATA, "PerfStats\\StopStat", 0, KEY_READ, &stop) != ERROR_SUCCESS)
        return;
    for (i = 0; i < stats; i++) {
        vlen = sizeof v;
        RegQueryValueEx(stop, stat_name[i], NULL, &type, (BYTE *)&v, &vlen);
    }
    RegCloseKey(stop);
}

/* Free system, GDI and USER resources in percent, from the Resource Meter's library. */
static int resources(int which)
{
    typedef int (WINAPI *free_fn)(int);
    static free_fn fn;
    static int tried;

    if (!tried) {
        HMODULE lib = LoadLibrary("rsrc32.dll");
        fn = lib ? (free_fn)GetProcAddress(lib, "_MyGetFreeSystemResources32@4") : NULL;
        tried = 1;
    }
    return fn ? fn(which) : -1;
}

static BOOL WINAPI on_break(DWORD type)
{
    (void)type;
    stats_stop();
    return FALSE;
}

/* Memory counters in KB. Windows 98 gives System Monitor's in bytes, despite their names. */
static long kb(const char *name)
{
    long v = stat(name);
    return v < 0 ? -1 : (long)((unsigned long)v >> 10);
}

int main(int argc, char **argv)
{
    unsigned long seconds = argc > 1 ? strtoul(argv[1], NULL, 0) : 30;
    unsigned long count = argc > 2 ? strtoul(argv[2], NULL, 0) : 0, line;
    HANDLE vxd;
    DWORD t0 = GetTickCount(), got;
    int i;

    if (argc > 3 && _stricmp(argv[3], "E9") == 0) {
        to_e9 = 1;
    } else if (argc > 3) {
        /* A serial port opens only for one user at a time and only if it exists; a file is
           added to */
        int port = _strnicmp(argv[3], "COM", 3) == 0 && strlen(argv[3]) <= 5;
        out = CreateFile(argv[3], GENERIC_WRITE, port ? 0 : FILE_SHARE_READ, NULL,
                         port ? OPEN_EXISTING : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) {
            printf("memlog: can't open %s (error %lu)\n", argv[3], GetLastError());
            return 1;
        }
        if (port) {                             /* no flow control, and never wait long */
            DCB dcb;
            COMMTIMEOUTS ct;
            memset(&dcb, 0, sizeof dcb);
            dcb.DCBlength = sizeof dcb;
            if (GetCommState(out, &dcb)) {
                dcb.BaudRate = CBR_115200; dcb.ByteSize = 8; dcb.Parity = NOPARITY;
                dcb.StopBits = ONESTOPBIT; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
                dcb.fOutX = FALSE; dcb.fDtrControl = DTR_CONTROL_ENABLE;
                dcb.fRtsControl = RTS_CONTROL_ENABLE;
                SetCommState(out, &dcb);
            }
            memset(&ct, 0, sizeof ct);
            ct.WriteTotalTimeoutConstant = 2000;
            SetCommTimeouts(out, &ct);
        } else {
            SetFilePointer(out, 0, NULL, FILE_END);
        }
    }
    if (seconds == 0)
        seconds = 1;
    vxd = CreateFile(RVD_DEVICE_NAME, 0, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    SetConsoleCtrlHandler(on_break, TRUE);
    stats_start();
    Sleep(1000);                                /* counters started above have a value now */

    say("memlog: every %lu s; counters:", seconds);
    for (i = 0; i < stats; i++)
        say(" %s=%ld", stat_name[i], stat(stat_name[i]));
    say("\r\n");
    say("memlog: s, free KB, page file free KB, VMM free/locked/disk cache/committed/swap in "
        "use KB, threads, resources sys/GDI/USER %%, VxD contexts/buffers/locked KB/zombies/"
        "heap KB\r\n");

    for (line = 0; !count || line < count; line++) {
        MEMORYSTATUS ms;
        struct rvd_stats st;

        if (line)
            Sleep(seconds * 1000);
        ms.dwLength = sizeof ms;
        GlobalMemoryStatus(&ms);
        say("mem %lu %lu %lu %ld %ld %ld %ld %ld %ld %d %d %d", (GetTickCount() - t0) / 1000,
            (unsigned long)(ms.dwAvailPhys >> 10), (unsigned long)(ms.dwAvailPageFile >> 10),
            kb("VMM\\cpgFree"), kb("VMM\\cpgLocked"), kb("VMM\\cpgDiskcache"),
            kb("VMM\\cpgCommit"), kb("VMM\\cpgSwapfileInUse"), stat("KERNEL\\Threads"),
            resources(0), resources(1), resources(2));
        memset(&st, 0, sizeof st);
        if (vxd != INVALID_HANDLE_VALUE &&
            DeviceIoControl(vxd, RVD_GET_STATS, NULL, 0, &st, sizeof st, &got, NULL))
            say(" %lu %lu %lu %lu %lu\r\n", st.contexts, st.buffers, st.buffer_pages * 4,
                st.zombies, st.heap_bytes >> 10);
        else
            say(" - - - - -\r\n");
    }
    stats_stop();
    return 0;
}
