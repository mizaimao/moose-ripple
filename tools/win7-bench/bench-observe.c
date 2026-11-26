/* bench-observe.exe: 3DMark06's visible windows with their place and size, their buttons, and
 * (once a result window is up) their static texts; "run" also closes the tip of the day and
 * presses Run 3DMark, "watch" repeats every 20 seconds and reports to the first serial port.
 * MIT licence (LICENSE at the repository root). */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tlhelp32.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
static DWORD pid;
static HWND button, tip;
static BOOL results, dialog_error, fullscreen;
static BOOL CALLBACK control(HWND w, LPARAM unused)
{
    char cls[64] = "", text[200] = "";
    DWORD_PTR copied = 0;
    (void)unused;
    if (!IsWindowVisible(w)) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    if (!_stricmp(cls, "Edit")) return TRUE;
    SendMessageTimeoutA(w, WM_GETTEXT, sizeof(text), (LPARAM)text,
                        SMTO_ABORTIFHUNG, 200, &copied);
    RECT r; GetWindowRect(w, &r);
    if (strstr(text, "Run 3DMark") || strstr(text, "Run &3DMark") || strstr(text, "RUN 3DMARK"))
        button = w;
    if (strstr(text,"E_FAIL") || strstr(text,"error occurred")) dialog_error=TRUE;
    if (!_stricmp(cls,"Button") || button == w || dialog_error ||
        (results && !_stricmp(cls,"Static") && *text)) {
        for (char *p = text; *p; ++p)
            if (*p < 32) *p = ' ';
        printf("  %s id=%d at %ld,%ld %s\n", cls, GetDlgCtrlID(w), r.left,r.top,text);
    }
    return TRUE;
}
static BOOL CALLBACK window(HWND w, LPARAM unused)
{
    DWORD wp = 0;
    char cls[64] = "", title[120] = "";
    RECT r;
    (void)unused;
    GetWindowThreadProcessId(w, &wp);
    if (wp != pid || !IsWindowVisible(w)) return TRUE;
    GetWindowTextA(w,title,sizeof(title));
    GetClassNameA(w,cls,sizeof(cls));
    GetWindowRect(w,&r);
    if (strstr(title,"Result") || strstr(title,"Score")) results=TRUE;
    if (r.left==0 && r.top==0 && r.right==1280 && r.bottom==1024) fullscreen=TRUE;
    if (!strcmp(title,"Tip Of The Day")) tip=GetDlgItem(w,IDOK);
    if (!strcmp(title,"3DMark06 - Advanced Edition")) button=GetDlgItem(w,IDOK);
    printf("window '%s' class %s at %ld,%ld %ldx%ld\n", title,cls,r.left,r.top,r.right-r.left,r.bottom-r.top);
    EnumChildWindows(w,control,0);
    return TRUE;
}
int main(int argc,char **argv)
{
    BOOL watching=argc==2 && !strcmp(argv[1],"watch");
    if(watching) {
        if(!freopen("C:\\RippleVista\\appwork\\installed-watch.txt","w",stdout))return 2;
        setvbuf(stdout,NULL,_IONBF,0);
    }
    unsigned rounds=0;
    do {
    BOOL reporting=FALSE;
    pid=0; results=dialog_error=fullscreen=FALSE; button=tip=NULL;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    PROCESSENTRY32 pe={0}; pe.dwSize=sizeof(pe);
    if (Process32First(snap,&pe)) do {
        if (!_stricmp(pe.szExeFile, "3DMark06.exe")) pid=pe.th32ProcessID;
        if (!_stricmp(pe.szExeFile, "WerFault.exe")) reporting=TRUE;
    } while (Process32Next(snap,&pe));
    CloseHandle(snap);
    if (!pid) { puts("3DMark06 is closed"); return 1; }
    SYSTEMTIME t; GetLocalTime(&t);
    printf("%02u:%02u:%02u ",t.wHour,t.wMinute,t.wSecond);
    EnumWindows(window,0);
    if(watching) {
        FILE *port=fopen("COM1","w");
        if(port) {
            fprintf(port,"VISTA_BENCH_STATUS %02u:%02u:%02u full=%d results=%d error=%d wer=%d\n",
                    t.wHour,t.wMinute,t.wSecond,fullscreen,results,dialog_error,reporting);
            fclose(port);
        }
        if(reporting||results||dialog_error||++rounds>=180)break;
        Sleep(20000);
    }
    } while(watching);
    if (argc==2 && !strcmp(argv[1],"run")) {
        if (tip) {
            SendMessageTimeoutA(tip,BM_CLICK,0,0,SMTO_ABORTIFHUNG,1000,NULL);
            Sleep(300);
        }
        if (!button || !IsWindowEnabled(button)) { puts("Run button unavailable"); return 2; }
        PostMessageA(button,BM_CLICK,0,0);
        puts("Requested benchmark");
    }
    return 0;
}
