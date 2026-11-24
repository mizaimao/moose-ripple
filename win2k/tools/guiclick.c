/* Press through a program's dialogs without a person, for installing test
 * programs in a test machine and for starting benchmarks from a script.
 *
 *   guiclick <seconds> [-press text]... [-check text]... [-until text] <program> [arguments]
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
 * The run ends when the program has exited and no window has changed for
 * ten seconds, or when the time is up. With -until, a window showing that
 * text ends it too: ten seconds later every window of the run is asked to
 * close. Every line starts with GUICLICK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MaxNames 8
#define MaxWindows 64
static const char *press[MaxNames + 16], *checks[MaxNames], *until;
static int pressCount, checkCount, userPress, pressed[MaxNames];
static DWORD untilSeen;
static DWORD shellPid, selfPid;
static struct { HWND w; DWORD hash, acted; } seen[MaxWindows];
static DWORD lastChange;
static BOOL closeAll;

static void out(const char *s)
{
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, lstrlenA(s), &n, NULL);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &n, NULL);
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

typedef struct { HWND button[48]; char text[48][96]; LONG style[48]; int count; DWORD hash; int report; } CONTROLS;

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
    if (c->report) {
        if (!lstrcmpiA(name, "Edit"))
            _snprintf(line, sizeof(line), "GUICLICK   Edit (%d characters)", GetWindowTextLengthA(w));
        else {
            char *q;
            for (q = text; *q; ++q) if (*q == '\r' || *q == '\n') *q = ' ';
            _snprintf(line, sizeof(line), "GUICLICK   %s \"%s\"%s%s", name, text, IsWindowEnabled(w) ? "" : " disabled",
                      !lstrcmpiA(name, "Button") && SendMessageA(w, BM_GETCHECK, 0, 0) == BST_CHECKED ? " checked" : "");
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
    for (k = 0; k < pressCount; ++k)
        for (i = 0; i < c->count; ++i) {
            LONG kind = c->style[i] & 15;
            if ((kind != BS_PUSHBUTTON && kind != BS_DEFPUSHBUTTON) || !IsWindowEnabled(c->button[i])) continue;
            if (!starts(c->text[i], press[k]) || (k < userPress && pressed[k])) continue;
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
        lastChange = now;
        _snprintf(line, sizeof(line), "GUICLICK window \"%s\" class %s", title, name);
        line[sizeof(line) - 1] = 0;
        out(line);
        memset(&c, 0, sizeof(c));
        c.report = 1;
        EnumChildWindows(w, child, (LPARAM)&c);
        c.hash = seen[slot].hash;
    }
    /* Act once the window has stood still for a second, and at most every
     * three seconds while it does not change. */
    if (now - lastChange > 1000 && now - seen[slot].acted > 3000) {
        seen[slot].acted = now;
        act(&c);
    }
    (void)arg;
    return TRUE;
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
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    selfPid = GetCurrentProcessId();
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    _snprintf(line, sizeof(line), "GUICLICK start %s", command);
    line[sizeof(line) - 1] = 0;
    out(line);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
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
