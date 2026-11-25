/* Where modules are loaded, for turning sampled instruction addresses into module names.
 *   modlist.exe          the kernel and its drivers (build 64-bit for a 64-bit Windows)
 *   modlist.exe PID      the modules of one process, by number or by name such as dwm.exe
 *                        (build with the bitness of that process)
 * One line each: "kernel BASE NAME" or "module BASE SIZE NAME", numbers in hexadecimal.
 * Build with llvm-mingw clang modlist.c -O2 -lpsapi.
 * MIT licence (LICENSE at the repository root).
 */
#define PSAPI_VERSION 1 /* psapi.dll: Windows Vista has no K32 copies in kernel32 */
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        static void *bases[2048];
        DWORD needed = 0;
        if (!EnumDeviceDrivers(bases, sizeof(bases), &needed))
            return 1;
        for (DWORD i = 0; i < needed / sizeof(bases[0]) && i < 2048; ++i) {
            char name[260] = "?";
            GetDeviceDriverBaseNameA(bases[i], name, sizeof(name));
            printf("kernel %p %s\n", bases[i], name);
        }
        return 0;
    }
    DWORD pid = strtoul(argv[1], NULL, 10);
    if (!pid) {     /* a process name: the newest process of that name */
        PROCESSENTRY32 process = {.dwSize = sizeof(process)};
        HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (all != INVALID_HANDLE_VALUE && Process32First(all, &process)) {
            do
                if (!_stricmp(process.szExeFile, argv[1]))
                    pid = process.th32ProcessID;
            while (Process32Next(all, &process));
        }
        if (!pid)
            return 1;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32 entry = {.dwSize = sizeof(entry)};
    if (snap == INVALID_HANDLE_VALUE || !Module32First(snap, &entry))
        return 1;
    do
        printf("module %p %lx %s\n", (void *)entry.modBaseAddr, entry.modBaseSize, entry.szModule);
    while (Module32Next(snap, &entry));
    CloseHandle(snap);
    return 0;
}
