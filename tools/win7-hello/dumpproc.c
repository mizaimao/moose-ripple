/*
 * Write a minidump of a running program, by name: for a program that hangs or waits where our
 * user-mode driver may be the reason (docs/windows/test-vms.md, "Crash dumps"). The dump has
 * every thread's registers and stack, which tools/win7-debug/dumpstack.py reads.
 *   dumpproc.exe NAME.exe OUT.dmp
 * Run it elevated for a program that runs elevated. Console program.
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    PROCESSENTRY32 pe;
    HANDLE snap, proc, file;
    DWORD pid = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: dumpproc NAME.exe OUT.dmp\n");
        return 2;
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    pe.dwSize = sizeof(pe);
    if (snap != INVALID_HANDLE_VALUE && Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, argv[1]) == 0)
                pid = pe.th32ProcessID;     /* the last one of that name: the newest */
        } while (Process32Next(snap, &pe));
    }
    if (!pid) {
        fprintf(stderr, "dumpproc: no process named %s\n", argv[1]);
        return 1;
    }
    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!proc) {
        fprintf(stderr, "dumpproc: cannot open process %lu: error %lu\n", pid, GetLastError());
        return 1;
    }
    file = CreateFileA(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "dumpproc: cannot write %s: error %lu\n", argv[2], GetLastError());
        return 1;
    }
    if (!MiniDumpWriteDump(proc, pid, file, MiniDumpNormal, NULL, NULL, NULL)) {
        fprintf(stderr, "dumpproc: MiniDumpWriteDump failed: error 0x%08lx\n", GetLastError());
        return 1;
    }
    CloseHandle(file);
    printf("dumpproc: process %lu (%s) written to %s\n", pid, argv[1], argv[2]);
    return 0;
}
