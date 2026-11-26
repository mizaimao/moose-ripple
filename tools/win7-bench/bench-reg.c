/* bench-reg.exe FILE: register 3DMark06 with the code in FILE's first line, through the
 * program's own dialogs: "Please Register" has Register (IDOK), Purchase and Continue; a
 * dialog with edit fields takes the code (in one field, or one group of the code per field)
 * and its IDOK confirms; a plain message box afterwards is answered with OK. The code is
 * never printed: only window titles and the number of fields filled.
 * Exit code: 0 the main window says it is not the Basic Edition, 2 it still is, 1 3DMark06
 * is not running or FILE has no code.  MIT licence (LICENSE at the repository root). */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD pid;
static char code[128];
static HWND edits[16];
static int edit_count;
static BOOL acted, basic, main_seen;

static BOOL CALLBACK control(HWND w, LPARAM unused)
{
    char cls[64] = "";
    (void)unused;
    if (!IsWindowVisible(w) || !IsWindowEnabled(w)) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    if (!_stricmp(cls, "Edit") && !(GetWindowLongA(w, GWL_STYLE) & ES_READONLY) && edit_count < 16)
        edits[edit_count++] = w;
    return TRUE;
}

static void press_ok(HWND dialog)
{
    HWND ok = GetDlgItem(dialog, IDOK);
    if (ok) {
        SetForegroundWindow(dialog);
        SendMessageTimeoutA(ok, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 3000, NULL);
    } else {
        PostMessageA(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    }
}

static BOOL CALLBACK window(HWND w, LPARAM unused)
{
    DWORD owner = 0;
    char cls[64] = "", title[120] = "";
    (void)unused;
    GetWindowThreadProcessId(w, &owner);
    if (owner != pid || !IsWindowVisible(w)) return TRUE;
    GetWindowTextA(w, title, sizeof(title));
    GetClassNameA(w, cls, sizeof(cls));
    if (!strncmp(title, "3DMark06 - ", 11)) {
        main_seen = TRUE;
        basic = strstr(title, "Basic") != NULL;
        printf("main window '%s'\n", title);
        return TRUE;
    }
    if (acted || !IsWindowEnabled(w) || !strcmp(title, "Tip Of The Day")) return TRUE;
    edit_count = 0;
    EnumChildWindows(w, control, 0);
    if (edit_count) {
        char copy[128], *groups[16], *p;
        int n = 0, i;
        strcpy(copy, code);
        for (p = strtok(copy, "-"); p && n < 16; p = strtok(NULL, "-")) groups[n++] = p;
        if (edit_count == n && n > 1) {
            for (i = 0; i < n; i++) SendMessageTimeoutA(edits[i], WM_SETTEXT, 0, (LPARAM)groups[i], SMTO_ABORTIFHUNG, 2000, NULL);
        } else {
            SendMessageTimeoutA(edits[0], WM_SETTEXT, 0, (LPARAM)code, SMTO_ABORTIFHUNG, 2000, NULL);
        }
        printf("window '%s': %d field(s), code entered, OK pressed\n", title, edit_count);
        press_ok(w);
        acted = TRUE;
    } else if (!strcmp(title, "Please Register")) {
        printf("window '%s': Register pressed\n", title);
        press_ok(w);
        acted = TRUE;
    } else if (!strcmp(cls, "#32770")) {
        printf("window '%s': message answered with OK\n", title);
        press_ok(w);
        acted = TRUE;
    } else {
        printf("window '%s' class %s: left alone\n", title, cls);
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    HANDLE snap;
    PROCESSENTRY32 pe = { 0 };
    FILE *f;
    int round;
    size_t n;
    if (argc != 2 || !(f = fopen(argv[1], "r"))) return 1;
    if (!fgets(code, sizeof(code), f)) code[0] = 0;
    fclose(f);
    n = strlen(code);
    while (n && (code[n - 1] == '\r' || code[n - 1] == '\n' || code[n - 1] == ' ')) code[--n] = 0;
    if (n < 8) { puts("no code in the file"); return 1; }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) do {
        if (!_stricmp(pe.szExeFile, "3DMark06.exe")) { pid = pe.th32ProcessID; break; }
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    if (!pid) { puts("3DMark06 is closed"); return 1; }
    for (round = 0; round < 8; round++) {
        acted = FALSE; main_seen = FALSE; basic = FALSE;
        EnumWindows(window, 0);
        if (main_seen && !basic) return 0;
        if (!acted && main_seen) break;
        Sleep(2500);
    }
    return 2;
}
