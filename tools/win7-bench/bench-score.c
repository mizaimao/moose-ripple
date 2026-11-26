/* bench-score.exe: what 3DMark06's windows say about scores and frame rates, read through
 * their accessibility objects (the score window draws its numbers itself). Nothing that looks
 * like a registration code is printed. MIT licence (LICENSE at the repository root). */
#define _WIN32_WINNT 0x0600
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <oleacc.h>
#include <wchar.h>
#include <wctype.h>
#include <stdio.h>

static DWORD app;
static void show(BSTR s)
{
    if(!s || !*s || wcslen(s)>120)return;
    wchar_t low[128];unsigned digits=0,other=0;
    size_t n=wcslen(s);
    for(size_t i=0;i<n;i++){
        low[i]=towlower(s[i]);
        if(iswdigit(s[i]))digits++;
        else if(!iswspace(s[i]) && s[i]!=L'.' && s[i]!=L',')other++;
    }
    low[n]=0;
    if(wcsstr(low,L"license")||wcsstr(low,L"serial")||wcsstr(low,L"registration"))return;
    if((digits && !other) || wcsstr(low,L"3dmark") || wcsstr(low,L"score") ||
       wcsstr(low,L"fps") || wcsstr(low,L"shader model") || wcsstr(low,L"cpu test"))
        printf("  %ls\n",s);
}
static void read_item(IAccessible *a,VARIANT id)
{
    BSTR s=NULL;
    if(SUCCEEDED(IAccessible_get_accName(a,id,&s))){show(s);SysFreeString(s);}
    s=NULL;
    if(SUCCEEDED(IAccessible_get_accValue(a,id,&s))){show(s);SysFreeString(s);}
}
static void visit(IAccessible *a,unsigned level)
{
    VARIANT self;VariantInit(&self);V_VT(&self)=VT_I4;V_I4(&self)=CHILDID_SELF;
    read_item(a,self);
    if(level>=5)return;
    LONG n=0,got=0;
    if(FAILED(IAccessible_get_accChildCount(a,&n)) || n<=0)return;
    VARIANT v[64];if(n>64)n=64;
    if(FAILED(AccessibleChildren(a,0,n,v,&got)))return;
    for(LONG i=0;i<got;i++){
        if(V_VT(&v[i])==VT_DISPATCH && V_DISPATCH(&v[i])){
            IAccessible *c=NULL;
            if(SUCCEEDED(IDispatch_QueryInterface(V_DISPATCH(&v[i]),&IID_IAccessible,(void**)&c))){
                visit(c,level+1);IAccessible_Release(c);
            }
        }else if(V_VT(&v[i])==VT_I4)read_item(a,v[i]);
        VariantClear(&v[i]);
    }
}
static BOOL CALLBACK win(HWND w,LPARAM ignored)
{
    DWORD p=0;GetWindowThreadProcessId(w,&p);
    if(p!=app || !IsWindowVisible(w))return TRUE;
    wchar_t title[120];GetWindowTextW(w,title,120);
    printf("Window %ls\n",title);
    IAccessible *a=NULL;
    if(SUCCEEDED(AccessibleObjectFromWindow(w,OBJID_CLIENT,&IID_IAccessible,(void**)&a))){
        visit(a,0);IAccessible_Release(a);
    }
    return TRUE;
}
int main(void)
{
    if(FAILED(CoInitialize(NULL)))return 1;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    PROCESSENTRY32 e={0};e.dwSize=sizeof(e);
    if(Process32First(snap,&e))do{
        if(!_stricmp(e.szExeFile,"3DMark06.exe")){app=e.th32ProcessID;break;}
    }while(Process32Next(snap,&e));
    CloseHandle(snap);
    if(app)EnumWindows(win,0);
    else puts("3DMark06 is closed");
    CoUninitialize();
    return !app;
}
