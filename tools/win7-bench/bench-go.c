/* One pass over 3DMark06's windows: print them and their controls; with "run", answer the free
 * edition's registration dialog (Continue), close the tip of the day and press Run 3DMark.
 *   bench-go.exe [run [alt]]      ("alt" presses with WM_COMMAND to the dialog instead of BM_CLICK)
 * Exit code: 0 Run pressed (or nothing asked), 3 a dialog was answered (call again), 2 no Run
 * button yet, 1 3DMark06 is not running. MIT licence (LICENSE at the repository root). */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD pid;
static HWND run_button, continue_button, tip_close;
static BOOL in_tip, alt_press;

static BOOL CALLBACK control(HWND w, LPARAM unused)
{
    char cls[64] = "", text[120] = "";
    DWORD_PTR copied = 0;
    RECT r;
    (void)unused;
    if (!IsWindowVisible(w)) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    if (!_stricmp(cls, "Edit")) return TRUE;
    SendMessageTimeoutA(w, WM_GETTEXT, sizeof(text), (LPARAM)text, SMTO_ABORTIFHUNG, 200, &copied);
    GetWindowRect(w, &r);
    if (strstr(text, "Run 3DMark") || strstr(text, "Run &3DMark")) run_button = w;
    if (!strcmp(text, "Continue") || !strcmp(text, "&Continue")) continue_button = w;
    if (in_tip && (!strcmp(text, "Close") || !strcmp(text, "&Close"))) tip_close = w;
    if (!_stricmp(cls, "Button") || w == run_button || w == continue_button) {
        for (char *p = text; *p; ++p)
            if ((unsigned char)*p < 32) *p = ' ';
        printf("  %s id=%d at %ld,%ld enabled=%d %s\n", cls, GetDlgCtrlID(w), r.left, r.top,
               IsWindowEnabled(w), text);
    }
    return TRUE;
}

static BOOL CALLBACK window(HWND w, LPARAM unused)
{
    DWORD owner = 0;
    char cls[64] = "", title[120] = "";
    RECT r;
    (void)unused;
    GetWindowThreadProcessId(w, &owner);
    if (owner != pid || !IsWindowVisible(w)) return TRUE;
    GetWindowTextA(w, title, sizeof(title));
    GetClassNameA(w, cls, sizeof(cls));
    GetWindowRect(w, &r);
    printf("window '%s' class %s at %ld,%ld %ldx%ld enabled=%d\n", title, cls, r.left, r.top,
           r.right - r.left, r.bottom - r.top, IsWindowEnabled(w));
    in_tip = !strcmp(title, "Tip Of The Day");
    EnumChildWindows(w, control, 0);
    /* The buttons are drawn by the program and have no text: the free edition's "Please
     * Register" dialog has Register (IDOK), Purchase (21) and Continue (IDCANCEL). */
    if (!strcmp(title, "Please Register")) continue_button = GetDlgItem(w, IDCANCEL);
    if (in_tip && !tip_close) tip_close = GetDlgItem(w, alt_press ? IDCANCEL : IDOK);
    if (!strncmp(title, "3DMark06 - ", 11) && !run_button) run_button = GetDlgItem(w, IDOK);
    in_tip = FALSE;
    return TRUE;
}

static void press(HWND button, BOOL alt)
{
    HWND dialog = GetParent(button);
    if (alt) {
        PostMessageA(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), (LPARAM)button);
    } else {
        SetForegroundWindow(dialog);
        SendMessageTimeoutA(button, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
    }
}

int main(int argc, char **argv)
{
    BOOL run = argc >= 2 && !strcmp(argv[1], "run");
    BOOL alt = alt_press = argc >= 3 && !strcmp(argv[2], "alt");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {0};
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) do {
        if (!_stricmp(pe.szExeFile, "3DMark06.exe")) { pid = pe.th32ProcessID; break; }
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    if (!pid) { puts("3DMark06 is closed"); return 1; }
    EnumWindows(window, 0);
    if (!run) return 0;
    if (continue_button && IsWindowEnabled(continue_button)) {
        press(continue_button, alt);
        puts("Continue pressed");
        return 3;
    }
    if (tip_close && IsWindowEnabled(tip_close)) {
        press(tip_close, alt);
        puts("Tip closed");
        return 3;
    }
    if (!run_button || !IsWindowEnabled(run_button) || !IsWindowEnabled(GetParent(run_button))) {
        puts("Run button unavailable");
        return 2;
    }
    PostMessageA(run_button, BM_CLICK, 0, 0);
    puts("Requested benchmark");
    return 0;
}
