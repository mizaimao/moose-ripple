/* Windows 2000 display diagnostic. Pattern checks temporarily select a mode. */
#define WINVER 0x0500
#define _WIN32_WINNT 0x0500
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <string.h>
#include "rb2kprobe.h"

int probe_ddraw(enum RB2K_DDRAW_TEST test);

static HANDLE output;
static BOOL output_failed;

static void line(const char *s)
{
    DWORD length = lstrlenA(s), done;
    if (!WriteFile(output, s, length, &done, NULL) || done != length ||
        !WriteFile(output, "\r\n", 2, &done, NULL) || done != 2)
        output_failed = TRUE;
}

static int digits(const char *s, int n)
{
    int value=0, i;
    for (i=0; i<n; i++) {
        int c=s[i], digit;
        if (c>='0' && c<='9') digit=c-'0';
        else if (c>='A' && c<='F') digit=c-'A'+10;
        else if (c>='a' && c<='f') digit=c-'a'+10;
        else return -1;
        value=(value<<4)|digit;
    }
    return value;
}

static BYTE ids[8194];
static BOOL property(HDEVINFO set, PSP_DEVINFO_DATA dev, DWORD key)
{
    DWORD type, length;
    if (!SetupDiGetDeviceRegistryPropertyA(set,dev,key,&type,ids,sizeof(ids)-2,&length) ||
        type!=REG_MULTI_SZ) return FALSE;
    ids[length]=ids[length+1]=0;
    return TRUE;
}

static BOOL pci(void)
{
    static const GUID display={0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
    HDEVINFO set=SetupDiGetClassDevsA(NULL,"PCI",NULL,DIGCF_PRESENT|DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA dev;
    DWORD index=0, found=0;
    BOOL complete;
    char buf[200];
    if (set==INVALID_HANDLE_VALUE) { line("PCI enumeration failed"); return FALSE; }
    dev.cbSize=sizeof(dev);
    while (SetupDiEnumDeviceInfo(set,index++,&dev)) {
        char *s;
        int vendor=-1, device=-1, class_code=-1;
        ULONG status=0, problem=0;
        CONFIGRET cm;
        if(property(set,&dev,SPDRP_HARDWAREID)) {
            for(s=(char*)ids; *s; s+=lstrlenA(s)+1) {
                if(lstrlenA(s)>=21 && !strncmp(s,"PCI\\VEN_",8) && !strncmp(s+12,"&DEV_",5)) {
                    vendor=digits(s+8,4); device=digits(s+17,4); break;
                }
            }
        }
        if(property(set,&dev,SPDRP_COMPATIBLEIDS)) {
            for(s=(char*)ids; *s; s+=lstrlenA(s)+1) {
                if(lstrlenA(s)==13 && !strncmp(s,"PCI\\CC_",7)) {
                    class_code=digits(s+7,6); break;
                }
            }
        }
        if (memcmp(&dev.ClassGuid,&display,sizeof display) &&
            (class_code<0 || (class_code>>16)!=3)) continue;
        ++found;
        cm=CM_Get_DevNode_Status(&status,&problem,dev.DevInst,0);
        wsprintfA(buf,"PCI display vendor=%04X device=%04X class=%06X cm_result=%lu status=%08lX problem=%lu",
                  vendor,device,class_code,cm,status,problem);
        line(buf);
    }
    complete=GetLastError()==ERROR_NO_MORE_ITEMS;
    SetupDiDestroyDeviceInfoList(set);
    wsprintfA(buf,"PCI display devices=%lu",found); line(buf);
    return complete && found!=0;
}

static BOOL mode(DWORD index, DEVMODEA *m)
{
    ZeroMemory(m,sizeof(*m)); m->dmSize=sizeof(*m);
    return EnumDisplaySettingsA(NULL,index,m);
}
static void show_mode(const char *name, const DEVMODEA *m)
{
    char buf[112];
    wsprintfA(buf,"%s %lux%lux%lu Hz=%lu flags=%08lX",name,
              m->dmPelsWidth,m->dmPelsHeight,m->dmBitsPerPel,m->dmDisplayFrequency,m->dmDisplayFlags);
    line(buf);
}

static const COLORREF colours[]={RGB(255,0,0),RGB(0,255,0),RGB(0,0,255),RGB(255,255,255)};
static RECT quadrant(const RECT *bounds, int i)
{
    RECT r;
    r.left=(i&1)?bounds->right/2:0; r.right=(i&1)?bounds->right:bounds->right/2;
    r.top=(i&2)?bounds->bottom/2:0; r.bottom=(i&2)?bounds->bottom:bounds->bottom/2;
    return r;
}
static LRESULT CALLBACK window_proc(HWND window,UINT msg,WPARAM wp,LPARAM lp)
{
    if(msg==WM_PAINT) {
        PAINTSTRUCT p;
        RECT bounds;
        HDC dc=BeginPaint(window,&p);
        int i;
        GetClientRect(window,&bounds);
        for(i=0;i<4;i++) {
            HBRUSH brush=CreateSolidBrush(colours[i]);
            if(brush) {
                RECT r=quadrant(&bounds,i);
                FillRect(dc,&r,brush);
                DeleteObject(brush);
            }
        }
        EndPaint(window,&p);
        return 0;
    }
    return DefWindowProcA(window,msg,wp,lp);
}

static BOOL paint_test(DWORD width, DWORD height)
{
    WNDCLASSA wc;
    HWND w;
    HDC dc;
    RECT bounds;
    int i;
    BOOL ok=TRUE;
    char buf[96];
    ZeroMemory(&wc,sizeof wc);
    wc.hInstance=GetModuleHandleA(NULL); wc.lpszClassName="W2KProbeColours";
    wc.lpfnWndProc=window_proc;
    if(!RegisterClassA(&wc)) return FALSE;
    w=CreateWindowExA(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,"Display test",
        WS_POPUP,0,0,(int)width,(int)height,NULL,NULL,wc.hInstance,NULL);
    if(!w){ UnregisterClassA(wc.lpszClassName,wc.hInstance); return FALSE; }
    ShowWindow(w,SW_SHOWNOACTIVATE);
    RedrawWindow(w,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
    GdiFlush();
    GetClientRect(w,&bounds);
    dc=GetDC(w);
    for(i=0;i<4;i++){
        RECT r=quadrant(&bounds,i);
        POINT at;
        COLORREF sample=CLR_INVALID;
        at.x=(r.left+r.right)/2; at.y=(r.top+r.bottom)/2;
        if(dc) {
            POINT screen=at;
            ClientToScreen(w,&screen);
            if(WindowFromPoint(screen)==w) sample=GetPixel(dc,at.x,at.y);
        }
        if(sample!=CLR_INVALID)
            wsprintfA(buf,"Sample %d RGB=%u,%u,%u %s",i,GetRValue(sample),
                GetGValue(sample),GetBValue(sample),sample==colours[i]?"PASS":"FAIL");
        else
            wsprintfA(buf,"Sample %d unavailable FAIL",i);
        line(buf); if(sample!=colours[i]) ok=FALSE;
    }
    if(dc) ReleaseDC(w,dc);
    DestroyWindow(w);
    UnregisterClassA(wc.lpszClassName,wc.hInstance);
    return ok;
}

static BOOL same_mode(const DEVMODEA *a,const DEVMODEA *b)
{
    return a->dmPelsWidth==b->dmPelsWidth && a->dmPelsHeight==b->dmPelsHeight &&
           a->dmBitsPerPel==b->dmBitsPerPel && a->dmDisplayFrequency==b->dmDisplayFrequency &&
           a->dmDisplayFlags==b->dmDisplayFlags;
}

static int run(BOOL pattern, DWORD width, DWORD height, DWORD bpp)
{
    OSVERSIONINFOEXA os;
    DEVMODEA original,m,desired,restore;
    DWORD i;
    LONG result;
    BOOL ok, matched, had_target=FALSE;
    char buf[150];
    ZeroMemory(&os,sizeof os); os.dwOSVersionInfoSize=sizeof os;
    if(!GetVersionExA((OSVERSIONINFOA*)&os)) return 1;
    wsprintfA(buf,"Windows NT=%u version=%lu.%lu build=%lu SP=%u.%u",
              os.dwPlatformId==VER_PLATFORM_WIN32_NT,os.dwMajorVersion,os.dwMinorVersion,
              os.dwBuildNumber,os.wServicePackMajor,os.wServicePackMinor);
    line(buf);
    ok=pci();
    if(!mode(ENUM_CURRENT_SETTINGS,&original)) { line("Current mode unavailable"); return 1; }
    show_mode("Current",&original);
    for(i=0;mode(i,&m);i++) {
        show_mode("Mode",&m);
        if(m.dmPelsWidth==width && m.dmPelsHeight==height && m.dmBitsPerPel==bpp) had_target=TRUE;
    }
    wsprintfA(buf,"Modes=%lu target_%lux%lux%lu=%s",i,width,height,bpp,had_target?"yes":"no"); line(buf);
    if (!i) ok=FALSE;
    if(!pattern) return ok?0:1;
    if(!had_target) { line("Pattern SKIP requested mode unavailable"); return 3; }
    ZeroMemory(&desired,sizeof desired); desired.dmSize=sizeof desired;
    desired.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL;
    desired.dmPelsWidth=width; desired.dmPelsHeight=height; desired.dmBitsPerPel=bpp;
    result=ChangeDisplaySettingsA(&desired,CDS_TEST);
    wsprintfA(buf,"Test mode result=%ld",result); line(buf);
    if(result!=DISP_CHANGE_SUCCESSFUL) return 3;
    result=ChangeDisplaySettingsA(&desired,CDS_FULLSCREEN);
    wsprintfA(buf,"Set mode result=%ld",result); line(buf);
    if(result!=DISP_CHANGE_SUCCESSFUL) return 1;
    matched=mode(ENUM_CURRENT_SETTINGS,&m) && m.dmPelsWidth==width && m.dmPelsHeight==height && m.dmBitsPerPel==bpp;
    if(matched) show_mode("Pattern mode",&m);
    if(!matched || !paint_test(width,height)) ok=FALSE;
    restore=original;
    restore.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL|DM_DISPLAYFLAGS;
    if(original.dmDisplayFrequency>1) restore.dmFields|=DM_DISPLAYFREQUENCY;
    result=ChangeDisplaySettingsA(&restore,0);
    matched=mode(ENUM_CURRENT_SETTINGS,&m) && same_mode(&m,&original);
    wsprintfA(buf,"Restore result=%ld exact_match=%s",result,matched?"yes":"no"); line(buf);
    return result==DISP_CHANGE_SUCCESSFUL && matched && ok?0:1;
}

static int gpu_test(void)
{
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    RB2K_GPU_RESULT result;
    int found, received = 0;
    int escape = RB2K_GPU_ESCAPE;
    char buf[180];
    static const char * const stages[] = {
        "unknown", "transport", "context", "resource", "backing", "attach",
        "clear", "transfer", "compare", "cleanup", "done"
    };
    if (!dc) { line("Display DC unavailable"); return 1; }
    ZeroMemory(&result, sizeof(result));
    found = ExtEscape(dc, QUERYESCSUPPORT, sizeof(escape), (LPCSTR)&escape, 0, NULL);
    if (found > 0)
        received = ExtEscape(dc, escape, 0, NULL, sizeof(result), (LPSTR)&result);
    DeleteDC(dc);
    if (found <= 0 || received <= 0 || result.version != RB2K_GPU_VERSION) {
        wsprintfA(buf, "GPU probe unavailable support=%d escape=%d version=%lu",
                  found, received, result.version);
        line(buf);
        return 1;
    }
    wsprintfA(buf, "GPU stage=%s (%lu) status=%lu response=%08lX features=%08lX",
        result.stage < sizeof(stages)/sizeof(stages[0]) ? stages[result.stage] : "unknown",
        result.stage, result.status, result.response, result.features);
    line(buf);
    wsprintfA(buf, "GPU pixel expected=%08lX observed=%08lX", result.expected, result.observed);
    line(buf);
    if (result.stage != RB2K_STAGE_DONE || result.status || result.expected != 0xffff8000UL ||
        result.observed != result.expected) {
        line("GPU pixel FAIL");
        return 1;
    }
    line("GPU pixel PASS");
    return 0;
}

extern DWORD rb_flip_width, rb_flip_height;

/* WIDTHxHEIGHTxBITS, for example 1280x1024x32. */
static BOOL mode_argument(const char *text, DWORD *width, DWORD *height, DWORD *bpp)
{
    DWORD *field[3], i;
    field[0] = width; field[1] = height; field[2] = bpp;
    for (i = 0; i < 3; ++i) {
        DWORD value = 0;
        if (*text < '0' || *text > '9') return FALSE;
        while (*text >= '0' && *text <= '9') value = value * 10 + (DWORD)(*text++ - '0');
        *field[i] = value;
        if (i < 2 && *text++ != 'x') return FALSE;
    }
    return !*text && *width && *height && (*bpp == 8 || *bpp == 16 || *bpp == 32);
}

int main(int argc,char **argv)
{
    int result;
    DWORD width, height, bpp;
    output=GetStdHandle(STD_OUTPUT_HANDLE);
    if (argc == 2 && !lstrcmpiA(argv[1], "/gpu")) {
        result = gpu_test();
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/ddraw-pair")) {
        result = probe_ddraw(RB2K_DDRAW_PAIR);
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/flip")) {
        result = probe_ddraw(RB2K_DDRAW_FLIP);
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/flip16")) {
        result = probe_ddraw(RB2K_DDRAW_FLIP16);
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/flip8")) {
        result = probe_ddraw(RB2K_DDRAW_FLIP8);
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/window")) {
        result = probe_ddraw(RB2K_DDRAW_WINDOW);
    } else if (argc == 2 && (!lstrcmpiA(argv[1], "/ddraw") || !lstrcmpiA(argv[1], "/ddraw-full"))) {
        result = probe_ddraw(!lstrcmpiA(argv[1], "/ddraw-full") ? RB2K_DDRAW_COPY : RB2K_DDRAW_FILL);
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/pattern800")) {
        result = run(TRUE, 800, 600, 32);
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/pattern16")) {
        result = run(TRUE, 800, 600, 16);
    } else if (argc == 2 && !lstrcmpiA(argv[1], "/pattern8")) {
        result = run(TRUE, 800, 600, 8);
    } else if (argc == 3 && !lstrcmpiA(argv[1], "/pattern") &&
               mode_argument(argv[2], &width, &height, &bpp)) {
        result = run(TRUE, width, height, bpp);
    } else if (argc == 3 && !lstrcmpiA(argv[1], "/flipat") &&
               mode_argument(argv[2], &width, &height, &bpp)) {
        rb_flip_width = width;
        rb_flip_height = height;
        result = probe_ddraw(bpp == 32 ? RB2K_DDRAW_FLIP : bpp == 16 ? RB2K_DDRAW_FLIP16 : RB2K_DDRAW_FLIP8);
    } else if (argc == 1 || (argc == 2 && !lstrcmpiA(argv[1], "/pattern"))) {
        result = run(argc == 2, RB2K_DEFAULT_WIDTH, RB2K_DEFAULT_HEIGHT, 32);
    } else {
        line("Usage: W2KPROBE [/pattern | /pattern800 | /pattern16 | /pattern8 | /gpu | /ddraw | /ddraw-full | /ddraw-pair | /window | /flip | /flip16 | /flip8 | /pattern WxHxB | /flipat WxHxB]"); return 2;
    }
    if(output_failed) return 1;
    return result;
}
