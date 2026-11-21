/* Present to a window through a real HWND clipper. All pixel samples come
 * from windows created by this test, including the overlapping sibling. */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include "rb2kprobe.h"

enum { FrameW=676, FrameH=514, CanvasX=16, CanvasY=12, CanvasW=640, CanvasH=480,
       CoverX=221, CoverY=161, CoverW=133, CoverH=91, SourceW=701, SourceH=509 };
static const RECT destination = {-9, -5, CanvasW-8, CanvasH-7};
static const COLORREF frameColor=RGB(29,54,71), canvasColor=RGB(79,36,42),
                      coverColor=RGB(202,153,53), solidColor=RGB(67,120,201);
static const char className[] = "RippleWindowBlt";

typedef struct {
    HWND frame, canvas, cover;
    LPDIRECTDRAWSURFACE primary, texture, upload;
    LPDIRECTDRAWCLIPPER clipper;
    HDC display;
    const char *path;
    BOOL hardware;
} WINDOW_TEST;

static BOOL call(const char *name, HRESULT hr)
{
    printf("Window %s hr=%08lX\n", name, (DWORD)hr);
    fflush(stdout);
    return SUCCEEDED(hr);
}

static LRESULT CALLBACK paint(HWND w, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_CREATE) {
        SetWindowLongA(w, GWL_USERDATA, (LONG)((CREATESTRUCTA *)lp)->lpCreateParams);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HBRUSH brush = CreateSolidBrush((COLORREF)GetWindowLongA(w, GWL_USERDATA));
        HDC dc = BeginPaint(w, &ps);
        FillRect(dc, &ps.rcPaint, brush);
        EndPaint(w, &ps);
        DeleteObject(brush);
        return 0;
    }
    return DefWindowProcA(w, message, wp, lp);
}

static BOOL windows(WINDOW_TEST *t, HINSTANCE instance)
{
    DWORD child = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    RECT work;
    int x, y;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0) ||
        work.right-work.left < FrameW || work.bottom-work.top < FrameH) {
        puts("Window test needs a desktop large enough for a 640x480 client and its margins");
        return FALSE;
    }
    x = work.left + (work.right-work.left-FrameW)/2;
    y = work.top + (work.bottom-work.top-FrameH)/2;
    t->frame = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, className, "DirectDraw window test",
        WS_POPUP | WS_CLIPCHILDREN, x, y, FrameW, FrameH, NULL, NULL, instance, (PVOID)frameColor);
    if (!t->frame) return FALSE;
    t->canvas = CreateWindowExA(0, className, "", child, CanvasX, CanvasY, CanvasW, CanvasH,
                                t->frame, NULL, instance, (PVOID)canvasColor);
    t->cover = CreateWindowExA(0, className, "", child, CoverX, CoverY, CoverW, CoverH,
                               t->frame, NULL, instance, (PVOID)coverColor);
    if (!t->canvas || !t->cover) return FALSE;
    ShowWindow(t->frame, SW_SHOWNOACTIVATE);
    SetWindowPos(t->cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RedrawWindow(t->frame, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    GdiFlush();
    return TRUE;
}

static DWORD pattern(int x, int y)
{
    return (((17*x + 3*y) & 255UL) << 16) | (((x + 13*y) & 255UL) << 8) | ((x ^ y) & 255UL);
}

static BOOL texture(LPDIRECTDRAW dd, DWORD memory, LPDIRECTDRAWSURFACE *out)
{
    DDSURFACEDESC d;
    ZeroMemory(&d, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = SourceW; d.dwHeight = SourceH;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | memory;
    if (!call("create offscreen", IDirectDraw_CreateSurface(dd, &d, out, NULL))) return FALSE;
    d.dwSize = sizeof(d);
    return call("describe offscreen", IDirectDrawSurface_GetSurfaceDesc(*out, &d)) &&
           (d.ddsCaps.dwCaps & memory) && d.dwWidth==SourceW && d.dwHeight==SourceH &&
           d.ddpfPixelFormat.dwRGBBitCount==32 && d.ddpfPixelFormat.dwRBitMask==0xff0000 &&
           d.ddpfPixelFormat.dwGBitMask==0xff00 && d.ddpfPixelFormat.dwBBitMask==0xff;
}

static BOOL upload_pattern(WINDOW_TEST *t)
{
    DDSURFACEDESC d;
    BOOL valid;
    int y, x;
    ZeroMemory(&d, sizeof(d)); d.dwSize = sizeof(d);
    if (!call("write-only lock", IDirectDrawSurface_Lock(t->upload, NULL, &d,
              DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL))) return FALSE;
    valid = d.lpSurface && d.lPitch >= SourceW * 4;
    if (valid) {
        for (y=0; y<SourceH; ++y) {
            DWORD *row = (DWORD *)((BYTE *)d.lpSurface + y*d.lPitch);
            for (x=0; x<SourceW; ++x) row[x] = pattern(x, y);
        }
    }
    if (!call("write unlock", IDirectDrawSurface_Unlock(t->upload, d.lpSurface)) || !valid)
        return FALSE;
    return call("copy pattern to source", IDirectDrawSurface_Blt(t->texture, NULL, t->upload,
                                                                 NULL, DDBLT_WAIT, NULL));
}

static BOOL sample(WINDOW_TEST *t, BOOL patterned, BOOL covered)
{
    HDC frame=GetDC(t->frame), canvas=GetDC(t->canvas), cover=GetDC(t->cover);
    ULONG counts[4]={0}, wrong=0;
    RECT visible={CanvasX,CanvasY,CanvasX+destination.right,CanvasY+destination.bottom};
    POINT at;
    int x,y;
    if (!frame || !canvas || !cover) wrong++;
    for (y=0; y<FrameH; ++y) {
        for (x=0; x<FrameW; ++x) {
            HWND owner=t->frame;
            HDC dc=frame;
            COLORREF expected=frameColor;
            int group=0;
            /* Include both sides of the blit, clip and occluder edges. */
            if (x%17 && x!=CanvasX-1 && x!=CanvasX && x!=visible.right-1 && x!=visible.right &&
                x!=CoverX-1 && x!=CoverX && x!=CoverX+CoverW-1 && x!=CoverX+CoverW) continue;
            if (y%13 && y!=CanvasY-1 && y!=CanvasY && y!=visible.bottom-1 && y!=visible.bottom &&
                y!=CoverY-1 && y!=CoverY && y!=CoverY+CoverH-1 && y!=CoverY+CoverH) continue;
            at.x=x; at.y=y;
            if (x>=CanvasX && y>=CanvasY && x<CanvasX+CanvasW && y<CanvasY+CanvasH) {
                owner=t->canvas; dc=canvas; expected=canvasColor; group=1;
                if (PtInRect(&visible,at)) {
                    DWORD rgb=pattern(19+x-CanvasX-destination.left,11+y-CanvasY-destination.top);
                    expected=patterned ? RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255) : solidColor;
                    group=2;
                }
            }
            if (covered && x>=CoverX && x<CoverX+CoverW && y>=CoverY && y<CoverY+CoverH) {
                owner=t->cover; dc=cover; expected=coverColor; group=3;
            }
            ClientToScreen(t->frame,&at);
            if (WindowFromPoint(at)!=owner) { ++wrong; continue; }
            ScreenToClient(owner,&at);
            if (!dc || GetPixel(dc,at.x,at.y)!=expected) ++wrong;
            ++counts[group];
        }
    }
    if (frame) ReleaseDC(t->frame,frame);
    if (canvas) ReleaseDC(t->canvas,canvas);
    if (cover) ReleaseDC(t->cover,cover);
    printf("Window %s %s covered=%d samples guard=%lu margin=%lu image=%lu occluder=%lu mismatches=%lu\n",
           t->path,patterned?"pattern":"solid",covered,counts[0],counts[1],counts[2],counts[3],wrong);
    return !wrong && counts[0] && counts[1] && counts[2] && (!covered || counts[3]);
}

static BOOL counters(WINDOW_TEST *t, RB2K_DD_PRESENT_STATS *out)
{
    return t->display && ExtEscape(t->display, RB2K_DD_STATS_ESCAPE, 0, NULL,
                                  sizeof(*out), (LPSTR)out)>0;
}

static BOOL present(WINDOW_TEST *t, BOOL patterned, BOOL covered)
{
    RECT dst=destination, src;
    POINT org={0,0};
    RB2K_DD_PRESENT_STATS before,after;
    BOOL measured=counters(t,&before);
    ULONG visible=destination.right*destination.bottom - (covered ? CoverW*CoverH : 0);
    ClientToScreen(t->canvas,&org);
    SetRect(&src,19,11,19+dst.right-dst.left,11+dst.bottom-dst.top);
    OffsetRect(&dst,org.x,org.y);
    if (!call("primary Blt",IDirectDrawSurface_Blt(t->primary,&dst,t->texture,&src,DDBLT_WAIT,NULL))) return FALSE;
    GdiFlush();
    if (!sample(t,patterned,covered)) return FALSE;
    if (t->hardware) {
        ULONG rects, reads, bytes, uploads, lockReads;
        if (!measured || !counters(t,&after)) return FALSE;
        rects=after.presents-before.presents;
        reads=after.presentReadbacks-before.presentReadbacks;
        bytes=after.presentBytes-before.presentBytes;
        uploads=after.offscreen.transfers.uploads-before.offscreen.transfers.uploads;
        lockReads=after.offscreen.transfers.readbacks-before.offscreen.transfers.readbacks;
        printf("Window present rectangles=%lu readbacks=%lu bytes=%lu visible_pixels=%lu uploads=%lu lock_readbacks=%lu\n",
               rects,reads,bytes,visible,uploads,lockReads);
        if (!rects || uploads || lockReads || bytes!=(patterned ? visible*4 : 0) ||
            reads!=(patterned ? rects : 0)) return FALSE;
    }
    return TRUE;
}

BOOL probe_ddraw_window(LPDIRECTDRAW dd, const char *path, DWORD memory)
{
    WINDOW_TEST t;
    WNDCLASSA wc;
    DDSURFACEDESC d;
    DDBLTFX fx;
    BOOL ok=FALSE;
    ZeroMemory(&t,sizeof(t)); t.path=path; t.hardware=(memory==DDSCAPS_VIDEOMEMORY);
    ZeroMemory(&wc,sizeof(wc)); wc.lpfnWndProc=paint; wc.hInstance=GetModuleHandleA(NULL);
    wc.lpszClassName=className;
    if (!RegisterClassA(&wc)) return FALSE;
    if (!windows(&t,wc.hInstance)) goto done;
    if (!call("normal cooperative level",IDirectDraw_SetCooperativeLevel(dd,t.frame,DDSCL_NORMAL))) goto done;
    ZeroMemory(&d,sizeof(d)); d.dwSize=sizeof(d); d.dwFlags=DDSD_CAPS;
    d.ddsCaps.dwCaps=DDSCAPS_PRIMARYSURFACE;
    if (!call("create primary",IDirectDraw_CreateSurface(dd,&d,&t.primary,NULL)) ||
        !call("create clipper",IDirectDraw_CreateClipper(dd,0,&t.clipper,NULL)) ||
        !call("clip to child window",IDirectDrawClipper_SetHWnd(t.clipper,0,t.canvas)) ||
        !call("attach clipper",IDirectDrawSurface_SetClipper(t.primary,t.clipper)) ||
        !texture(dd,memory,&t.texture) || !texture(dd,memory,&t.upload)) goto done;
    if (t.hardware) t.display=CreateDCA("DISPLAY",NULL,NULL,NULL);
    ZeroMemory(&fx,sizeof(fx)); fx.dwSize=sizeof(fx); fx.dwFillColor=0x004378c9;
    if (!call("fill source",IDirectDrawSurface_Blt(t.texture,NULL,NULL,NULL,DDBLT_COLORFILL|DDBLT_WAIT,&fx)) ||
        !present(&t,FALSE,TRUE) || !upload_pattern(&t) || !present(&t,TRUE,TRUE)) goto done;
    ShowWindow(t.cover,SW_HIDE);
    RedrawWindow(t.canvas,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
    ok=present(&t,TRUE,FALSE);
done:
    if (t.display) DeleteDC(t.display);
    if (t.upload) IDirectDrawSurface_Release(t.upload);
    if (t.texture) IDirectDrawSurface_Release(t.texture);
    if (t.primary) IDirectDrawSurface_Release(t.primary);
    if (t.clipper) IDirectDrawClipper_Release(t.clipper);
    if (t.frame) DestroyWindow(t.frame);
    UnregisterClassA(className,wc.hInstance);
    return ok;
}
