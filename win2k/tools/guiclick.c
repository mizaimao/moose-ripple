/* Press through a program's dialogs without a person, for installing test
 * programs in a test machine and for starting benchmarks from a script.
 *
 *   guiclick <seconds> [-press text]... [-check text]... [-dismiss text]... [-close text]... [-until text]
 *            [-debug] <program> [arguments]
 *
 * Every visible top-level window other than the shell's is reported when
 * its contents change: its title and class, then each control's class and
 * text. Edit controls are reported by length only, so typed keys or serial
 * numbers never reach the log. Then, in a dialog:
 *   - radio buttons that accept a licence, or that put off a restart, are
 *     selected, and so are the controls named with -check;
 *   - check boxes that would launch a program or show a readme are cleared;
 *   - the first enabled button among the -press names, then Next, Install,
 *     Yes, Accept, Agree, Continue, Finish, OK and Close, is pressed. Cancel,
 *     Back, No and Browse never are. Each -press name is pressed once.
 *   - a window showing a -dismiss text has its first enabled push button
 *     pressed, whatever its label (owner-drawn buttons often have none).
 *   - a window showing a -close text is asked to close.
 *   - "-press #N" presses the enabled button whose control ID is N, owner-drawn
 *     or not; buttons are reported with their IDs and places for this.
 * With -debug the program runs under guiclick as its debugger: its
 * exceptions are reported with the module and offset, and for one that
 * ends it, the registers and the code addresses found on its stack.
 * The screen saver is off while guiclick runs.
 * The run ends when the program has exited and no window has changed for
 * ten seconds, or when the time is up. With -until, a window showing that
 * text ends it too: ten seconds later every window of the run is asked to
 * close. Every line starts with GUICLICK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MaxNames 8
#define MaxWindows 64
static const char *press[MaxNames + 16], *checks[MaxNames], *dismiss[MaxNames], *closes[MaxNames], *until;
static int pressCount, checkCount, dismissCount, closeCount, userPress, pressed[MaxNames];
static DWORD untilSeen;
static DWORD shellPid, selfPid;
static struct { HWND w; DWORD hash, changed, acted; } seen[MaxWindows];
static DWORD lastChange;
static BOOL closeAll, debugging;

static void out(const char *s)
{
    char line[520];
    DWORD n;
    int length = lstrlenA(s);
    if (length > (int)sizeof(line) - 3) length = sizeof(line) - 3;
    memcpy(line, s, length);
    line[length] = '\r';
    line[length + 1] = '\n';
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, length + 2, &n, NULL);   /* one write: two threads print */
}

/* Lower-case copy without the ampersands of keyboard accelerators. */
static void plain(const char *in, char *outText, int size)
{
    int n = 0;
    for (; *in && n < size - 1; ++in) {
        if (*in == '&') continue;
        outText[n++] = (char)(*in >= 'A' && *in <= 'Z' ? *in + 32 : *in);
    }
    outText[n] = 0;
}

static BOOL contains(const char *text, const char *part)
{
    char a[256], b[64];
    plain(text, a, sizeof(a));
    plain(part, b, sizeof(b));
    return strstr(a, b) != NULL;
}

static BOOL starts(const char *text, const char *part)
{
    char a[256], b[64];
    plain(text, a, sizeof(a));
    plain(part, b, sizeof(b));
    return !strncmp(a, b, strlen(b));
}

typedef struct { HWND top, button[48]; char text[48][96]; LONG style[48]; int count; DWORD hash; int report, dismiss, close; } CONTROLS;

static BOOL CALLBACK child(HWND w, LPARAM arg)
{
    CONTROLS *c = (CONTROLS *)arg;
    char name[32], text[96], line[200];
    const char *p;
    LONG style = GetWindowLongA(w, GWL_STYLE);
    if (!IsWindowVisible(w)) return TRUE;
    GetClassNameA(w, name, sizeof(name));
    text[0] = 0;
    if (lstrcmpiA(name, "Edit")) GetWindowTextA(w, text, sizeof(text));
    for (p = name; *p; ++p) c->hash = c->hash * 31 + (unsigned char)*p;
    for (p = text; *p; ++p) c->hash = c->hash * 31 + (unsigned char)*p;
    c->hash = c->hash * 31 + (IsWindowEnabled(w) ? 1 : 0);
    if (until && !untilSeen && text[0] && contains(text, until)) untilSeen = GetTickCount() | 1;
    {
        int d;
        for (d = 0; d < dismissCount; ++d) if (text[0] && contains(text, dismiss[d])) c->dismiss = 1;
        for (d = 0; d < closeCount; ++d) if (text[0] && contains(text, closes[d])) c->close = 1;
    }
    if (c->report) {
        if (!lstrcmpiA(name, "Edit"))
            _snprintf(line, sizeof(line), "GUICLICK   Edit (%d characters)", GetWindowTextLengthA(w));
        else {
            char *q;
            for (q = text; *q; ++q) if (*q == '\r' || *q == '\n') *q = ' ';
            _snprintf(line, sizeof(line), "GUICLICK   %s \"%s\"%s%s", name, text, IsWindowEnabled(w) ? "" : " disabled",
                      !lstrcmpiA(name, "Button") && SendMessageA(w, BM_GETCHECK, 0, 0) == BST_CHECKED ? " checked" : "");
            if (!lstrcmpiA(name, "Button")) {
                /* Its control ID and its place in the window, for -press #N. */
                RECT r, t;
                size_t n = strlen(line);
                GetWindowRect(w, &r);
                GetWindowRect(c->top, &t);
                _snprintf(line + n, sizeof(line) - n, " id=%d at %ld,%ld %ldx%ld", GetDlgCtrlID(w), r.left - t.left,
                          r.top - t.top, r.right - r.left, r.bottom - r.top);
            }
        }
        line[sizeof(line) - 1] = 0;
        out(line);
    }
    if (!lstrcmpiA(name, "Button") && c->count < 48) {
        c->button[c->count] = w;
        lstrcpynA(c->text[c->count], text, sizeof(c->text[0]));
        c->style[c->count] = style;
        ++c->count;
    }
    return TRUE;
}

static void act(CONTROLS *c)
{
    char line[160];
    int i, k;
    for (i = 0; i < c->count; ++i) {
        LONG kind = c->style[i] & 15;
        BOOL radio = kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON;
        BOOL box = kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX;
        BOOL want = FALSE, clear = FALSE;
        if (!IsWindowEnabled(c->button[i])) continue;
        if (radio && contains(c->text[i], "accept") && !contains(c->text[i], "not accept") && !contains(c->text[i], "do not"))
            want = TRUE;
        if (radio && (contains(c->text[i], "restart my computer later") || contains(c->text[i], "no, i will restart")))
            want = TRUE;
        for (k = 0; k < checkCount; ++k) if (contains(c->text[i], checks[k])) want = TRUE;
        if (box && !want && (contains(c->text[i], "launch") || contains(c->text[i], "readme") ||
                             contains(c->text[i], "view the") || contains(c->text[i], "run ")))
            clear = TRUE;
        if (want && SendMessageA(c->button[i], BM_GETCHECK, 0, 0) != BST_CHECKED) {
            SendMessageA(c->button[i], BM_CLICK, 0, 0);
            _snprintf(line, sizeof(line), "GUICLICK select \"%s\"", c->text[i]);
            line[sizeof(line) - 1] = 0;
            out(line);
            return;     /* the dialog changes; act again on the next look */
        }
        if (clear && SendMessageA(c->button[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            SendMessageA(c->button[i], BM_CLICK, 0, 0);
            _snprintf(line, sizeof(line), "GUICLICK clear \"%s\"", c->text[i]);
            line[sizeof(line) - 1] = 0;
            out(line);
            return;
        }
    }
    if (c->close) {
        out("GUICLICK close");
        PostMessageA(c->top, WM_CLOSE, 0, 0);
        return;
    }
    for (k = 0; k < userPress; ++k)
        if (press[k][0] == '#' && !pressed[k])
            for (i = 0; i < c->count; ++i) {
                int id = atoi(press[k] + 1);
                if (GetDlgCtrlID(c->button[i]) != id || !IsWindowEnabled(c->button[i])) continue;
                pressed[k] = 1;
                _snprintf(line, sizeof(line), "GUICLICK press id=%d", id);
                line[sizeof(line) - 1] = 0;
                out(line);
                PostMessageA(GetParent(c->button[i]), WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)c->button[i]);
                return;
            }
    if (c->dismiss)
        for (i = 0; i < c->count; ++i) {
            LONG kind = c->style[i] & 15;
            if ((kind != BS_PUSHBUTTON && kind != BS_DEFPUSHBUTTON && kind != BS_OWNERDRAW) || !IsWindowEnabled(c->button[i]))
                continue;
            _snprintf(line, sizeof(line), "GUICLICK dismiss with \"%s\"", c->text[i]);
            line[sizeof(line) - 1] = 0;
            out(line);
            PostMessageA(GetParent(c->button[i]), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(c->button[i]), BN_CLICKED),
                         (LPARAM)c->button[i]);
            return;
        }
    for (k = 0; k < pressCount; ++k)
        for (i = 0; i < c->count; ++i) {
            LONG kind = c->style[i] & 15;
            if ((kind != BS_PUSHBUTTON && kind != BS_DEFPUSHBUTTON) || !IsWindowEnabled(c->button[i])) continue;
            if (press[k][0] == '#' || !starts(c->text[i], press[k]) || (k < userPress && pressed[k])) continue;
            if (k < userPress) pressed[k] = 1;
            if (starts(c->text[i], "cancel") || starts(c->text[i], "< back") || starts(c->text[i], "back") ||
                starts(c->text[i], "no") || starts(c->text[i], "browse")) continue;
            _snprintf(line, sizeof(line), "GUICLICK press \"%s\"", c->text[i]);
            line[sizeof(line) - 1] = 0;
            out(line);
            PostMessageA(GetParent(c->button[i]), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(c->button[i]), BN_CLICKED),
                         (LPARAM)c->button[i]);
            return;
        }
}

static BOOL CALLBACK top(HWND w, LPARAM arg)
{
    DWORD pid = 0, now = GetTickCount();
    CONTROLS c;
    char title[128], name[64], line[260];
    int slot, free = -1;
    GetWindowThreadProcessId(w, &pid);
    if (!IsWindowVisible(w) || pid == shellPid || pid == selfPid) return TRUE;
    GetWindowTextA(w, title, sizeof(title));
    GetClassNameA(w, name, sizeof(name));
    if (!lstrcmpA(name, "Shell_TrayWnd") || !lstrcmpA(name, "Progman") || !lstrcmpA(name, "ConsoleWindowClass")) return TRUE;
    if (closeAll) { PostMessageA(w, WM_CLOSE, 0, 0); return TRUE; }
    if (until && !untilSeen && title[0] && contains(title, until)) untilSeen = GetTickCount() | 1;
    memset(&c, 0, sizeof(c));
    c.top = w;
    c.hash = 5381;
    {
        const char *p;
        for (p = title; *p; ++p) c.hash = c.hash * 31 + (unsigned char)*p;
    }
    EnumChildWindows(w, child, (LPARAM)&c);
    for (slot = 0; slot < MaxWindows && seen[slot].w != w; ++slot) if (!seen[slot].w && free < 0) free = slot;
    if (slot == MaxWindows) {
        if (free < 0) free = 0;
        slot = free;
        seen[slot].w = w;
        seen[slot].hash = 0;
    }
    if (seen[slot].hash != c.hash) {
        seen[slot].hash = c.hash;
        seen[slot].acted = 0;
        seen[slot].changed = lastChange = now;
        _snprintf(line, sizeof(line), "GUICLICK window \"%s\" class %s", title, name);
        line[sizeof(line) - 1] = 0;
        out(line);
        memset(&c, 0, sizeof(c));
        c.top = w;
        c.report = 1;
        EnumChildWindows(w, child, (LPARAM)&c);
        c.hash = seen[slot].hash;
    }
    /* Act once the window has stood still for a second, and at most every
     * three seconds while it does not change. */
    if (now - seen[slot].changed > 1000 && now - seen[slot].acted > 3000) {
        seen[slot].acted = now;
        act(&c);
    }
    (void)arg;
    return TRUE;
}

/* The module holding an address, and the offset into it. */
static BOOL where(DWORD pid, DWORD address, char *text, int size)
{
    MODULEENTRY32 m;
    BOOL found = FALSE;
    HANDLE list = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    _snprintf(text, size, "%08lx", address);
    text[size - 1] = 0;
    if (list == INVALID_HANDLE_VALUE) return FALSE;
    m.dwSize = sizeof(m);
    if (Module32First(list, &m)) do {
        DWORD offset = address - (DWORD)m.modBaseAddr;
        if (offset < m.modBaseSize) {
            _snprintf(text, size, "%s+%lx", m.szModule, offset);
            text[size - 1] = 0;
            found = TRUE;
            break;
        }
    } while (Module32Next(list, &m));
    CloseHandle(list);
    return found;
}

/* Handle the debugged program's events for up to the given time. */
static void debug_events(HANDLE process, DWORD ms, BOOL *exited)
{
    static int exceptions, strings, loads;
    DWORD until = GetTickCount() + ms;
    char line[300], place[96];
    for (;;) {
        DEBUG_EVENT e;
        DWORD now = GetTickCount(), action = DBG_CONTINUE;
        if ((LONG)(until - now) <= 0 || !WaitForDebugEvent(&e, until - now)) return;
        switch (e.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (e.u.CreateProcessInfo.hFile) CloseHandle(e.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (e.u.LoadDll.hFile) CloseHandle(e.u.LoadDll.hFile);
            if (++loads <= 80) {
                where(e.dwProcessId, (DWORD)e.u.LoadDll.lpBaseOfDll, place, sizeof(place));
                _snprintf(line, sizeof(line), "GUICLICK load %08lx %s", (DWORD)e.u.LoadDll.lpBaseOfDll, place);
                line[sizeof(line) - 1] = 0;
                out(line);
            }
            break;
        case OUTPUT_DEBUG_STRING_EVENT:
            if (++strings <= 100 && !e.u.DebugString.fUnicode) {
                char text[200];
                SIZE_T got = 0;
                DWORD n = e.u.DebugString.nDebugStringLength < sizeof(text) ? e.u.DebugString.nDebugStringLength : sizeof(text) - 1;
                char *p;
                ReadProcessMemory(process, e.u.DebugString.lpDebugStringData, text, n, &got);
                text[got < sizeof(text) ? got : sizeof(text) - 1] = 0;
                for (p = text; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
                _snprintf(line, sizeof(line), "GUICLICK debug %s", text);
                line[sizeof(line) - 1] = 0;
                out(line);
            }
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_RECORD *r = &e.u.Exception.ExceptionRecord;
            if (r->ExceptionCode == EXCEPTION_BREAKPOINT && !exceptions) { ++exceptions; break; }
            action = DBG_EXCEPTION_NOT_HANDLED;
            if (++exceptions <= 30 || !e.u.Exception.dwFirstChance) {
                where(e.dwProcessId, (DWORD)r->ExceptionAddress, place, sizeof(place));
                _snprintf(line, sizeof(line), "GUICLICK exception %08lx at %s %s chance info %08lx %08lx",
                          r->ExceptionCode, place, e.u.Exception.dwFirstChance ? "first" : "second",
                          r->NumberParameters > 0 ? (DWORD)r->ExceptionInformation[0] : 0,
                          r->NumberParameters > 1 ? (DWORD)r->ExceptionInformation[1] : 0);
                line[sizeof(line) - 1] = 0;
                out(line);
            }
            if (!e.u.Exception.dwFirstChance) {
                /* It ends the program: its registers and the code addresses on its stack. */
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, e.dwThreadId);
                CONTEXT c;
                c.ContextFlags = CONTEXT_FULL;
                if (thread && GetThreadContext(thread, &c)) {
                    DWORD stack[256];
                    SIZE_T got = 0, n = 0;
                    int i, shown = 0;
                    _snprintf(line, sizeof(line), "GUICLICK registers eip=%08lx esp=%08lx ebp=%08lx eax=%08lx ebx=%08lx "
                              "ecx=%08lx edx=%08lx esi=%08lx edi=%08lx", c.Eip, c.Esp, c.Ebp, c.Eax, c.Ebx, c.Ecx, c.Edx,
                              c.Esi, c.Edi);
                    line[sizeof(line) - 1] = 0;
                    out(line);
                    {   /* the memory the fault touched, and the code at the fault */
                        MEMORY_BASIC_INFORMATION m;
                        BYTE code[16];
                        SIZE_T n = 0;
                        DWORD at = r->NumberParameters > 1 ? (DWORD)r->ExceptionInformation[1] : 0;
                        if (VirtualQueryEx(process, (PVOID)at, &m, sizeof(m))) {
                            _snprintf(line, sizeof(line), "GUICLICK fault memory %08lx base %08lx allocation %08lx size %08lx "
                                      "state %lx protect %lx type %lx", at, (DWORD)m.BaseAddress, (DWORD)m.AllocationBase,
                                      (DWORD)m.RegionSize, m.State, m.Protect, m.Type);
                            line[sizeof(line) - 1] = 0;
                            out(line);
                        }
                        if (VirtualQueryEx(process, (PVOID)(at - 1), &m, sizeof(m))) {
                            _snprintf(line, sizeof(line), "GUICLICK memory below base %08lx allocation %08lx size %08lx "
                                      "state %lx protect %lx type %lx", (DWORD)m.BaseAddress, (DWORD)m.AllocationBase,
                                      (DWORD)m.RegionSize, m.State, m.Protect, m.Type);
                            line[sizeof(line) - 1] = 0;
                            out(line);
                        }
                        if (ReadProcessMemory(process, (PVOID)(c.Eip - 8), code, sizeof(code), &n) && n == sizeof(code)) {
                            _snprintf(line, sizeof(line), "GUICLICK code at eip-8 %02x %02x %02x %02x %02x %02x %02x %02x | "
                                      "%02x %02x %02x %02x %02x %02x %02x %02x", code[0], code[1], code[2], code[3],
                                      code[4], code[5], code[6], code[7], code[8], code[9], code[10], code[11], code[12],
                                      code[13], code[14], code[15]);
                            line[sizeof(line) - 1] = 0;
                            out(line);
                        }
                    }
                    {   /* the frame chain: each frame's return address */
                        DWORD frame = c.Ebp, link[2];
                        int depth;
                        for (depth = 0; depth < 16 && frame; ++depth) {
                            if (!ReadProcessMemory(process, (PVOID)frame, link, sizeof(link), &n) || n != sizeof(link)) break;
                            where(e.dwProcessId, link[1], place, sizeof(place));
                            _snprintf(line, sizeof(line), "GUICLICK frame %d %s", depth, place);
                            line[sizeof(line) - 1] = 0;
                            out(line);
                            if (link[0] <= frame) break;
                            frame = link[0];
                        }
                    }
                    ReadProcessMemory(process, (PVOID)c.Esp, stack, sizeof(stack), &got);
                    for (i = 0; i < (int)(got / sizeof(DWORD)) && shown < 24; ++i)
                        if (where(e.dwProcessId, stack[i], place, sizeof(place))) {
                            _snprintf(line, sizeof(line), "GUICLICK stack +%03x %s", i * 4, place);
                            line[sizeof(line) - 1] = 0;
                            out(line);
                            ++shown;
                        }
                }
                if (thread) CloseHandle(thread);
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            _snprintf(line, sizeof(line), "GUICLICK debugged program exit %08lx", e.u.ExitProcess.dwExitCode);
            line[sizeof(line) - 1] = 0;
            out(line);
            *exited = TRUE;
            break;
        }
        ContinueDebugEvent(e.dwProcessId, e.dwThreadId, action);
    }
}

/* With -debug a thread of its own starts the program and handles its debug
 * events, so a window query by the main thread never waits on a program that
 * is stopped at a debug event. */
static struct { char *command; STARTUPINFOA *si; PROCESS_INFORMATION *pi; HANDLE started; BOOL ok; } debuggee;

static DWORD WINAPI debugger(LPVOID unused)
{
    BOOL ended = FALSE;
    debuggee.ok = CreateProcessA(NULL, debuggee.command, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL,
                                 debuggee.si, debuggee.pi);
    SetEvent(debuggee.started);
    if (!debuggee.ok) return 1;
    while (!ended) debug_events(debuggee.pi->hProcess, 1000, &ended);
    (void)unused;
    return 0;
}

static char *word(char **p)
{
    char *s = *p, *start;
    BOOL quoted = FALSE;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) { *p = s; return NULL; }
    start = s;
    if (*s == '"') { quoted = TRUE; start = ++s; while (*s && *s != '"') ++s; }
    else while (*s && *s != ' ' && *s != '\t') ++s;
    if (*s) { *s = 0; ++s; }
    if (quoted && *s == ' ') ++s;
    *p = s;
    return start;
}

int main(void)
{
    static const char *const defaults[] = { "Next", "Install", "Yes", "Accept", "Agree", "I Agree", "Continue",
                                             "Finish", "OK", "Close" };
    char *command = GetCommandLineA(), *w, line[400];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD seconds, start, i;
    BOOL quoted = FALSE, exited = FALSE;
    while (*command && (quoted || (*command != ' ' && *command != '\t'))) { if (*command == '"') quoted = !quoted; ++command; }
    seconds = strtoul(command, &command, 10);
    for (;;) {
        char *save = command;
        w = word(&command);
        if (!w) break;
        if (!lstrcmpA(w, "-press") && pressCount < MaxNames) press[pressCount++] = word(&command);
        else if (!lstrcmpA(w, "-check") && checkCount < MaxNames) checks[checkCount++] = word(&command);
        else if (!lstrcmpA(w, "-until")) until = word(&command);
        else if (!lstrcmpA(w, "-dismiss") && dismissCount < MaxNames) dismiss[dismissCount++] = word(&command);
        else if (!lstrcmpA(w, "-close") && closeCount < MaxNames) closes[closeCount++] = word(&command);
        else if (!lstrcmpA(w, "-debug")) debugging = TRUE;
        else {
            /* The program and its arguments, as given. */
            command = save;
            while (*command == ' ' || *command == '\t') ++command;
            break;
        }
    }
    userPress = pressCount;
    for (i = 0; i < sizeof(defaults) / sizeof(*defaults); ++i) press[pressCount++] = defaults[i];
    if (!seconds || !*command) { out("GUICLICK usage: guiclick <seconds> [-press text] [-check text] <program> [arguments]"); return 2; }
    SystemParametersInfoA(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, 0);
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    selfPid = GetCurrentProcessId();
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    _snprintf(line, sizeof(line), "GUICLICK start %s", command);
    line[sizeof(line) - 1] = 0;
    out(line);
    if (debugging) {
        DWORD id;
        HANDLE thread;
        debuggee.command = command;
        debuggee.si = &si;
        debuggee.pi = &pi;
        debuggee.started = CreateEventA(NULL, TRUE, FALSE, NULL);
        thread = CreateThread(NULL, 0, debugger, NULL, 0, &id);
        if (thread) { WaitForSingleObject(debuggee.started, INFINITE); CloseHandle(thread); }
    }
    if (debugging ? !debuggee.ok : !CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        _snprintf(line, sizeof(line), "GUICLICK launch failed %lu", GetLastError());
        out(line);
        return 1;
    }
    start = lastChange = GetTickCount();
    for (;;) {
        DWORD code;
        Sleep(400);
        EnumWindows(top, 0);
        if (!exited && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &code);
            _snprintf(line, sizeof(line), "GUICLICK program exit %lu after %lu s", code, (GetTickCount() - start) / 1000);
            out(line);
            exited = TRUE;
        }
        if (exited && GetTickCount() - lastChange > 10000) { out("GUICLICK done"); break; }
        if (untilSeen && GetTickCount() - untilSeen > 10000) {
            out("GUICLICK until text seen, closing");
            closeAll = TRUE;
            EnumWindows(top, 0);
            closeAll = FALSE;
            untilSeen = 0;
            until = NULL;
            start = GetTickCount() - seconds * 1000 + 30000;   /* thirty seconds more at most */
        }
        if (GetTickCount() - start > seconds * 1000) { out("GUICLICK time up"); break; }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
