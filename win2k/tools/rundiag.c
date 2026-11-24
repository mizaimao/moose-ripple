/* Run a program under the debugger and report what it does before it ends:
 * the modules it loads, its exceptions (first and second chance, with the
 * module and offset), its debug output, the text of any message box it
 * shows (which is then dismissed), and its exit code.
 *
 *   rundiag <seconds> <program> [arguments]
 *
 * After the given time the program's windows are asked to close; it is
 * ended ten seconds later if it is still running. Every line starts with
 * RUNDIAG so a test script can pick them out. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD child;
static int exceptions, strings, dialogs;

static void out(const char *s)
{
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, lstrlenA(s), &n, NULL);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &n, NULL);
}

/* The module holding an address, and the offset into it. */
static void where(DWORD pid, DWORD address, char *text, int size)
{
    MODULEENTRY32 m;
    HANDLE list = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    _snprintf(text, size, "%08lx", address);
    text[size - 1] = 0;
    if (list == INVALID_HANDLE_VALUE) return;
    m.dwSize = sizeof(m);
    if (Module32First(list, &m)) do {
        DWORD offset = address - (DWORD)m.modBaseAddr;
        if (offset < m.modBaseSize) {
            _snprintf(text, size, "%s+%lx", m.szModule, offset);
            text[size - 1] = 0;
            break;
        }
    } while (Module32Next(list, &m));
    CloseHandle(list);
}

static void read_string(HANDLE process, const void *at, BOOL unicode, DWORD length, char *text, int size)
{
    SIZE_T got = 0;
    text[0] = 0;
    if (!at) return;
    if (unicode) {
        WCHAR wide[260];
        DWORD n = length && length < 259 ? length : 259;
        if (ReadProcessMemory(process, at, wide, n * sizeof(WCHAR), &got)) {
            wide[got / sizeof(WCHAR)] = 0;
            WideCharToMultiByte(CP_ACP, 0, wide, -1, text, size, NULL, NULL);
        }
    } else {
        DWORD n = length && length < (DWORD)size - 1 ? length : (DWORD)size - 1;
        if (ReadProcessMemory(process, at, text, n, &got)) text[got] = 0;
    }
    text[size - 1] = 0;
    {   /* one line */
        char *p;
        for (p = text; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
    }
}

static BOOL CALLBACK dialog_text(HWND w, LPARAM arg)
{
    char name[16], text[256], line[300];
    if (GetClassNameA(w, name, sizeof(name)) && !lstrcmpiA(name, "Static") && GetWindowTextA(w, text, sizeof(text))) {
        char *p;
        for (p = text; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
        _snprintf(line, sizeof(line), "RUNDIAG dialog text: %s", text);
        line[sizeof(line) - 1] = 0;
        out(line);
    }
    (void)arg;
    return TRUE;
}

/* Report and dismiss the program's message boxes; close: ask its windows to close. */
static BOOL CALLBACK visit(HWND w, LPARAM close)
{
    DWORD pid = 0;
    char name[64], title[128], line[256];
    GetWindowThreadProcessId(w, &pid);
    if (pid != child || !IsWindowVisible(w)) return TRUE;
    if (GetClassNameA(w, name, sizeof(name)) && !lstrcmpA(name, "#32770")) {
        GetWindowTextA(w, title, sizeof(title));
        _snprintf(line, sizeof(line), "RUNDIAG dialog \"%s\"", title);
        line[sizeof(line) - 1] = 0;
        out(line);
        EnumChildWindows(w, dialog_text, 0);
        ++dialogs;
        PostMessageA(w, WM_COMMAND, IDOK, 0);
        PostMessageA(w, WM_CLOSE, 0, 0);
    } else if (close) PostMessageA(w, WM_CLOSE, 0, 0);
    return TRUE;
}

int main(void)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION pi;
    char *command = GetCommandLineA(), line[400], place[96], text[260];
    DWORD seconds, start, lastLook = 0;
    BOOL quoted = FALSE, closing = FALSE;
    /* Skip this program's name, then read the time. */
    while (*command && (quoted || (*command != ' ' && *command != '\t'))) { if (*command == '"') quoted = !quoted; ++command; }
    while (*command == ' ' || *command == '\t') ++command;
    seconds = strtoul(command, &command, 10);
    while (*command == ' ' || *command == '\t') ++command;
    if (!seconds || !*command) { out("RUNDIAG usage: rundiag <seconds> <program> [arguments]"); return 2; }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _snprintf(line, sizeof(line), "RUNDIAG start %s", command);
    line[sizeof(line) - 1] = 0;
    out(line);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &startup, &pi)) {
        _snprintf(line, sizeof(line), "RUNDIAG launch failed %lu", GetLastError());
        out(line);
        return 1;
    }
    child = pi.dwProcessId;
    start = GetTickCount();
    for (;;) {
        DEBUG_EVENT e;
        DWORD now = GetTickCount();
        if (now - lastLook > 500) { EnumWindows(visit, closing); lastLook = now; }
        if (!closing && now - start > seconds * 1000) {
            out("RUNDIAG time up, closing");
            closing = TRUE;
            EnumWindows(visit, TRUE);
            PostThreadMessageA(pi.dwThreadId, WM_QUIT, 0, 0);
        }
        if (closing && now - start > seconds * 1000 + 10000) {
            out("RUNDIAG still running, ended");
            TerminateProcess(pi.hProcess, 0xdead);
            closing = FALSE;
            seconds += 3600;
        }
        if (!WaitForDebugEvent(&e, 100)) continue;
        {
            DWORD action = DBG_CONTINUE;
            switch (e.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (e.u.CreateProcessInfo.hFile) CloseHandle(e.u.CreateProcessInfo.hFile);
                break;
            case LOAD_DLL_DEBUG_EVENT: {
                void *name = NULL;
                SIZE_T got;
                text[0] = 0;
                if (e.u.LoadDll.lpImageName &&
                    ReadProcessMemory(pi.hProcess, e.u.LoadDll.lpImageName, &name, sizeof(name), &got) && name)
                    read_string(pi.hProcess, name, e.u.LoadDll.fUnicode != 0, 0, text, sizeof(text));
                _snprintf(line, sizeof(line), "RUNDIAG load %08lx %s", (DWORD)e.u.LoadDll.lpBaseOfDll, text);
                line[sizeof(line) - 1] = 0;
                out(line);
                if (e.u.LoadDll.hFile) CloseHandle(e.u.LoadDll.hFile);
                break;
            }
            case EXCEPTION_DEBUG_EVENT: {
                const EXCEPTION_RECORD *r = &e.u.Exception.ExceptionRecord;
                if (r->ExceptionCode == EXCEPTION_BREAKPOINT && !exceptions) { ++exceptions; break; }
                action = DBG_EXCEPTION_NOT_HANDLED;
                if (++exceptions <= 40 || !e.u.Exception.dwFirstChance) {
                    where(e.dwProcessId, (DWORD)r->ExceptionAddress, place, sizeof(place));
                    _snprintf(line, sizeof(line), "RUNDIAG exception %08lx at %s %s chance info %08lx %08lx",
                              r->ExceptionCode, place, e.u.Exception.dwFirstChance ? "first" : "second",
                              r->NumberParameters > 0 ? (DWORD)r->ExceptionInformation[0] : 0,
                              r->NumberParameters > 1 ? (DWORD)r->ExceptionInformation[1] : 0);
                    line[sizeof(line) - 1] = 0;
                    out(line);
                }
                break;
            }
            case OUTPUT_DEBUG_STRING_EVENT:
                if (++strings <= 200) {
                    read_string(pi.hProcess, e.u.DebugString.lpDebugStringData, e.u.DebugString.fUnicode != 0,
                                e.u.DebugString.nDebugStringLength, text, sizeof(text));
                    _snprintf(line, sizeof(line), "RUNDIAG debug %s", text);
                    line[sizeof(line) - 1] = 0;
                    out(line);
                }
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                _snprintf(line, sizeof(line), "RUNDIAG exit %08lx after %lu ms, %d exceptions, %d debug strings, %d dialogs",
                          e.u.ExitProcess.dwExitCode, GetTickCount() - start, exceptions - 1, strings, dialogs);
                line[sizeof(line) - 1] = 0;
                out(line);
                ContinueDebugEvent(e.dwProcessId, e.dwThreadId, DBG_CONTINUE);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return 0;
            }
            ContinueDebugEvent(e.dwProcessId, e.dwThreadId, action);
        }
    }
}
