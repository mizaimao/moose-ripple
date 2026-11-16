/*
 * dbgrun: run a program under a minimal debugger and keep what it writes with
 * OutputDebugString, such as the messages of DirectX's debug runtime.
 *
 *   dbgrun LOGFILE PROGRAM [ARGUMENTS...]
 *
 * Exceptions, DLL loads and unloads and threads are logged; exceptions are passed on to the
 * program. Only the program itself is debugged: the processes it starts run on their own,
 * since before Windows XP a debugger can't let go of a process, and one that stays (such as
 * DirectDraw's helper, DDHELP) must keep running after dbgrun ends. Built by
 * tools/win9x/build-tools.sh.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* What follows the first n words of a command line; a space or tab outside double quotes ends
   a word. */
static char *skip_words(char *s, int n)
{
    int quoted = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    while (n > 0 && *s) {
        if (*s == '"') {
            quoted = !quoted;
        } else if (!quoted && (*s == ' ' || *s == '\t')) {
            while (*s == ' ' || *s == '\t')
                s++;
            n--;
            continue;
        }
        s++;
    }
    return s;
}

int main(int argc, char **argv)
{
    char cmd[1024];
    char *rest;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DEBUG_EVENT ev;
    FILE *log;

    if (argc < 3) {
        printf("usage: dbgrun LOGFILE PROGRAM [ARGUMENTS...]\n");
        return 2;
    }
    /* The program's command line is the rest of dbgrun's, after its name and LOGFILE, as it
       was typed: the quotes stay, so a path or an argument with spaces arrives whole. */
    rest = skip_words(GetCommandLine(), 2);
    if (strlen(rest) >= sizeof cmd) {
        printf("dbgrun: the command line is too long\n");
        return 2;
    }
    strcpy(cmd, rest);
    log = fopen(argv[1], "w");
    if (!log) {
        printf("can't write %s\n", argv[1]);
        return 2;
    }
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;     /* the program writes where dbgrun's output goes */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si,
                       &pi)) {
        DWORD err = GetLastError();
        printf("dbgrun: CreateProcess(%s) failed, error %lu\n", cmd, err);
        fprintf(log, "CreateProcess(%s) failed, error %lu\n", cmd, err);
        fclose(log);
        return 2;
    }
    for (;;) {
        DWORD status = DBG_CONTINUE;

        if (!WaitForDebugEvent(&ev, INFINITE))
            break;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            fprintf(log, "[process %lx started]\n", ev.dwProcessId);
            if (ev.u.CreateProcessInfo.hFile)
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT: {
            /* The name, when the system gives one: a pointer (in the debuggee) to a pointer
               to the name, which may be Unicode. */
            char name[MAX_PATH] = "?";
            DWORD p = 0, got = 0;
            HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, ev.dwProcessId);
            if (h && ev.u.LoadDll.lpImageName &&
                ReadProcessMemory(h, ev.u.LoadDll.lpImageName, &p, 4, &got) && p) {
                if (ev.u.LoadDll.fUnicode) {
                    WCHAR w[MAX_PATH];
                    if (ReadProcessMemory(h, (LPCVOID)p, w, sizeof w - 2, &got)) {
                        w[MAX_PATH - 1] = 0;
                        WideCharToMultiByte(CP_ACP, 0, w, -1, name, sizeof name, NULL, NULL);
                    }
                } else if (ReadProcessMemory(h, (LPCVOID)p, name, sizeof name - 1, &got)) {
                    name[got < sizeof name ? got : sizeof name - 1] = 0;
                }
            }
            if (h)
                CloseHandle(h);
            fprintf(log, "[dll %p loaded in process %lx: %s]\n", ev.u.LoadDll.lpBaseOfDll,
                    ev.dwProcessId, name);
            if (ev.u.LoadDll.hFile)
                CloseHandle(ev.u.LoadDll.hFile);
            break;
        }
        case UNLOAD_DLL_DEBUG_EVENT:
            fprintf(log, "[dll %p unloaded in process %lx]\n", ev.u.UnloadDll.lpBaseOfDll,
                    ev.dwProcessId);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            fprintf(log, "[thread %lx started in process %lx]\n", ev.dwThreadId, ev.dwProcessId);
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            fprintf(log, "[thread %lx ended in process %lx]\n", ev.dwThreadId, ev.dwProcessId);
            break;
        case OUTPUT_DEBUG_STRING_EVENT: {
            char buf[2048];
            DWORD n = ev.u.DebugString.nDebugStringLength, got = 0;
            HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, ev.dwProcessId);
            size_t len;
            if (n >= sizeof buf)
                n = sizeof buf - 1;
            buf[0] = 0;
            if (h && ReadProcessMemory(h, ev.u.DebugString.lpDebugStringData, buf, n, &got))
                buf[got] = 0;
            if (h)
                CloseHandle(h);
            fputs(buf, log);
            len = strlen(buf);          /* 0 for OutputDebugString("") */
            if (len && buf[len - 1] != '\n')
                fputc('\n', log);
            fflush(log);
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
            if (ev.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) {
                fprintf(log, "[exception %08lx at %p in process %lx%s]\n",
                        ev.u.Exception.ExceptionRecord.ExceptionCode,
                        ev.u.Exception.ExceptionRecord.ExceptionAddress, ev.dwProcessId,
                        ev.u.Exception.dwFirstChance ? ", first chance" : "");
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            fprintf(log, "[process %lx ended, code %lu]\n", ev.dwProcessId,
                    ev.u.ExitProcess.dwExitCode);
            break;
        }
        fflush(log);
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
        if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT && ev.dwProcessId == pi.dwProcessId)
            break;
    }
    fclose(log);
    return 0;
}
