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
 *
 * LOGFILE E9 writes to QEMU's debug port (-debugcon) instead of a file: for a program that
 * hangs Windows, whose log can't be read inside it afterwards.
 *
 * With the environment variable DBGRUN_DUMP set to a number of seconds, every thread of the
 * program is also logged that often: where it is (EIP), and the addresses on its stack that
 * point into a loaded module, as module+offset. For a program that hangs: the last dump says
 * what each thread waits in, and the program's own addresses below it say who called.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <conio.h>
#include <tlhelp32.h>

/* The log: a file, or QEMU's debug port. */
static FILE *logfile;
static int to_e9;

static void say(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    int i;

    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    if (to_e9) {                        /* Windows 9x lets a program use the port */
        for (i = 0; line[i]; i++)
            _outp(0xE9, line[i]);
    } else {
        fputs(line, logfile);
        fflush(logfile);
    }
}

/* The program's threads and modules, for the dumps. */
#define MAX_THREADS 64
#define MAX_MODULES 128
static struct { DWORD id; HANDLE h; DWORD tib; } threads[MAX_THREADS];
static struct { DWORD base, size; char name[48]; } modules[MAX_MODULES];

static void thread_add(DWORD id, HANDLE h)
{
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        if (!threads[i].h) {
            threads[i].id = id;
            threads[i].h = h;
            threads[i].tib = 0;
            return;
        }
    }
}

static void thread_remove(DWORD id)
{
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].h && threads[i].id == id)
            threads[i].h = NULL;
    }
}

/* A module, with its size from its PE header (SizeOfImage). */
static void module_add(HANDLE proc, DWORD base, const char *name)
{
    DWORD lfanew = 0, size = 0, got;
    const char *s = strrchr(name, '\\');
    int i;

    if (!ReadProcessMemory(proc, (LPCVOID)(base + 0x3c), &lfanew, 4, &got) ||
        !ReadProcessMemory(proc, (LPCVOID)(base + lfanew + 0x50), &size, 4, &got))
        size = 0;
    for (i = 0; i < MAX_MODULES; i++) {
        if (!modules[i].base) {
            modules[i].base = base;
            modules[i].size = size;
            strncpy(modules[i].name, s ? s + 1 : name, sizeof modules[i].name - 1);
            return;
        }
    }
}

static void module_remove(DWORD base)
{
    int i;
    for (i = 0; i < MAX_MODULES; i++) {
        if (modules[i].base == base)
            modules[i].base = 0;
    }
}

static int module_of(DWORD a)
{
    int i;
    for (i = 0; i < MAX_MODULES; i++) {
        if (modules[i].base && a >= modules[i].base && a - modules[i].base < modules[i].size)
            return i;
    }
    return -1;
}

/* Windows 9x's 16-bit lock (the Win16Mutex): KERNEL32 hands out a pointer to it by ordinal 93
   only (GetpWin16Lock), and GetProcAddress won't look up KERNEL32's ordinals, so the export
   table is read here. Its owner is in it, as an internal thread address. */
static void *win16_lock(void)
{
    BYTE *k = (BYTE *)GetModuleHandle("KERNEL32.DLL");
    IMAGE_EXPORT_DIRECTORY *e;
    void (WINAPI *get)(void **);
    void *p = NULL;

    if (!k)
        return NULL;
    e = (IMAGE_EXPORT_DIRECTORY *)(k + ((IMAGE_NT_HEADERS *)(k + ((IMAGE_DOS_HEADER *)k)->e_lfanew))
        ->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    if (93 < e->Base || 93 - e->Base >= e->NumberOfFunctions)
        return NULL;
    get = (void (WINAPI *)(void **))(k + ((DWORD *)(k + e->AddressOfFunctions))[93 - e->Base]);
    get(&p);
    return p;
}

/* Every thread of the system, with its process: who else may hold the lock. */
static void dump_system_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
    PROCESSENTRY32 pe;
    THREADENTRY32 te;
    BOOL ok;

    if (snap == INVALID_HANDLE_VALUE)
        return;
    pe.dwSize = sizeof pe;
    for (ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        say("  process %lx: %s\n", pe.th32ProcessID, pe.szExeFile);
    te.dwSize = sizeof te;
    for (ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        say("  thread %lx in process %lx\n", te.th32ThreadID, te.th32OwnerProcessID);
    CloseHandle(snap);
}

/* Every thread: where it is, and the stack's addresses that point into a module (return
   addresses among them; the stack isn't walked, so some are only data). */
static void dump_threads(HANDLE proc)
{
    int i;

    say("[dump at %lu ms]\n", GetTickCount());
    {
        DWORD *w = (DWORD *)win16_lock(), tib, j;
        __asm { mov eax, fs:[0x18] }
        __asm { mov tib, eax }
        say("  dbgrun thread %lx, tib %08lx; win16 lock at %p:", GetCurrentThreadId(), tib, w);
        for (j = 0; w && j < 12; j++)
            say(" %08lx", w[j]);
        say("\n");
    }
    for (i = 0; i < MAX_THREADS; i++) {
        CONTEXT c;
        DWORD stack[256], got = 0, k, shown = 0;
        int m;

        if (!threads[i].h)
            continue;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
        SuspendThread(threads[i].h);
        if (!GetThreadContext(threads[i].h, &c)) {
            ResumeThread(threads[i].h);
            say("  thread %lx: no context, error %lu\n", threads[i].id, GetLastError());
            continue;
        }
        ReadProcessMemory(proc, (LPCVOID)c.Esp, stack, sizeof stack, &got);
        ResumeThread(threads[i].h);
        m = module_of(c.Eip);
        say("  thread %lx: eip %08lx %s+%lx, esp %08lx, ebp %08lx, eax %08lx\n", threads[i].id,
            c.Eip, m >= 0 ? modules[m].name : "?", m >= 0 ? c.Eip - modules[m].base : 0, c.Esp,
            c.Ebp, c.Eax);
        if (!threads[i].tib) {
            /* Once per thread: later, with the 16-bit lock held elsewhere, this call waits. */
            LDT_ENTRY fs;
            if (GetThreadSelectorEntry(threads[i].h, c.SegFs, &fs))
                threads[i].tib = fs.BaseLow | (DWORD)fs.HighWord.Bytes.BaseMid << 16 |
                                 (DWORD)fs.HighWord.Bytes.BaseHi << 24;
        }
        if (threads[i].tib) {
            /* The thread's system records around its TIB: the values that look like ring 0
               thread handles, to match the 16-bit lock's owner and waiters. */
            DWORD rec[128], n = 0, j, hits = 0;
            ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(threads[i].tib - 0x100), rec,
                              sizeof rec, &n);
            say("    tib %08lx, ring 0 values:", threads[i].tib);
            for (j = 0; j < n / 4 && hits < 12; j++) {
                if (rec[j] >= 0xC1000000 && rec[j] < 0xC2000000) {
                    say(" %+ld:%08lx", (long)(j * 4) - 0x100, rec[j]);
                    hits++;
                }
            }
            say("\n");
        }
        for (k = 0; k < got / 4 && shown < 24; k++) {
            m = module_of(stack[k]);
            if (m < 0)
                continue;
            say("    esp+%03lx: %08lx %s+%lx\n", k * 4, stack[k], modules[m].name,
                stack[k] - modules[m].base);
            shown++;
        }
    }
    dump_system_threads();
}

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
    char env[16];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DEBUG_EVENT ev;
    DWORD dump_ms = 0, last_dump;
    HANDLE proc = NULL;

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
    to_e9 = _stricmp(argv[1], "E9") == 0;
    if (!to_e9) {
        logfile = fopen(argv[1], "w");
        if (!logfile) {
            printf("can't write %s\n", argv[1]);
            return 2;
        }
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
        say("CreateProcess(%s) failed, error %lu\n", cmd, err);
        if (logfile)
            fclose(logfile);
        return 2;
    }
    if (GetEnvironmentVariable("DBGRUN_DUMP", env, sizeof env) > 0 && atoi(env) > 0)
        dump_ms = (DWORD)atoi(env) * 1000;
    last_dump = GetTickCount();
    for (;;) {
        DWORD status = DBG_CONTINUE;

        if (dump_ms && proc && GetTickCount() - last_dump >= dump_ms) {
            dump_threads(proc);
            last_dump = GetTickCount();
        }
        if (!WaitForDebugEvent(&ev, dump_ms ? 500 : INFINITE)) {
            if (dump_ms && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT)
                continue;               /* no event for half a second: time for a dump? */
            break;
        }
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            say("[process %lx started]\n", ev.dwProcessId);
            if (ev.dwProcessId == pi.dwProcessId) {
                proc = OpenProcess(PROCESS_VM_READ, FALSE, ev.dwProcessId);
                thread_add(ev.dwThreadId, ev.u.CreateProcessInfo.hThread);
                if (proc)
                    module_add(proc, (DWORD)ev.u.CreateProcessInfo.lpBaseOfImage, "program");
            }
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
            say("[dll %p loaded in process %lx: %s]\n", ev.u.LoadDll.lpBaseOfDll, ev.dwProcessId,
                name);
            if (proc && ev.dwProcessId == pi.dwProcessId)
                module_add(proc, (DWORD)ev.u.LoadDll.lpBaseOfDll, name);
            if (ev.u.LoadDll.hFile)
                CloseHandle(ev.u.LoadDll.hFile);
            break;
        }
        case UNLOAD_DLL_DEBUG_EVENT:
            say("[dll %p unloaded in process %lx]\n", ev.u.UnloadDll.lpBaseOfDll,
                ev.dwProcessId);
            module_remove((DWORD)ev.u.UnloadDll.lpBaseOfDll);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            say("[thread %lx started in process %lx]\n", ev.dwThreadId, ev.dwProcessId);
            if (ev.dwProcessId == pi.dwProcessId)
                thread_add(ev.dwThreadId, ev.u.CreateThread.hThread);
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            say("[thread %lx ended in process %lx]\n", ev.dwThreadId, ev.dwProcessId);
            thread_remove(ev.dwThreadId);
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
            len = strlen(buf);          /* 0 for OutputDebugString("") */
            say("%s%s", buf, len && buf[len - 1] != '\n' ? "\n" : "");
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
            if (ev.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) {
                say("[exception %08lx at %p in process %lx%s]\n",
                    ev.u.Exception.ExceptionRecord.ExceptionCode,
                    ev.u.Exception.ExceptionRecord.ExceptionAddress, ev.dwProcessId,
                    ev.u.Exception.dwFirstChance ? ", first chance" : "");
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            say("[process %lx ended, code %lu]\n", ev.dwProcessId, ev.u.ExitProcess.dwExitCode);
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
        if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT && ev.dwProcessId == pi.dwProcessId)
            break;
    }
    if (logfile)
        fclose(logfile);
    return 0;
}
