/*
 * rippledd.dll: the 32-bit half of the moose-ripple display driver's DirectDraw HAL
 * (hardware abstraction layer) for Windows 95/98/ME.
 *
 * DirectDraw loads this DLL when the 16-bit driver names it (ddraw16.c) and calls DriverInit
 * with the 16:16 address of the block the two share (win9x/display/rdd.h). The DLL fills in
 * its callbacks and capabilities there; the 16-bit driver adds the screen mode and passes the
 * whole to DirectDraw. From then on DirectDraw calls the functions below directly, in the
 * process of whichever program uses DirectDraw.
 *
 * What the card offers DirectDraw: video memory (the frame buffer, with the rest below the
 * visible screen as one heap that DirectDraw manages), and page flipping by moving the start
 * of the display (QEMU's VBE Y offset register). Drawing into surfaces is DirectDraw's own
 * software (its HEL) on that memory. No vertical blank exists on a virtual card; the DLL keeps
 * a 60 Hz clock for programs that wait for one.
 *
 * Built with Visual C++ 6 against the Windows Me DDK's DirectDraw headers (ddraw.h, ddrawi.h:
 * not in this repository), without the C run-time. The DLL is based above 2 GB, in the shared
 * part of the address space, as DirectDraw expects: its data is the same in every process.
 */
#include <windows.h>
#include "ddraw.h"
#include "ddrawi.h"

#include "rdd.h"
#include "rvd.h"
#include "rippled3d.h"

/* rdd.h's description of DDHALINFO must match the DDK's. */
#define CHECK(name, cond) typedef char name[(cond) ? 1 : -1]
CHECK(c_halinfo, sizeof(RDD_HALINFO) == sizeof(DDHALINFO));
CHECK(c_vmidata, FIELD_OFFSET(RDD_HALINFO, fpPrimary) == FIELD_OFFSET(DDHALINFO, vmiData));
CHECK(c_caps, FIELD_OFFSET(RDD_HALINFO, ddCaps) == FIELD_OFFSET(DDHALINFO, ddCaps));
CHECK(c_heaps, FIELD_OFFSET(RDD_HALINFO, dwNumHeaps) == FIELD_OFFSET(DDHALINFO, vmiData) + FIELD_OFFSET(VIDMEMINFO, dwNumHeaps));
CHECK(c_modeidx, FIELD_OFFSET(RDD_HALINFO, dwModeIndex) == FIELD_OFFSET(DDHALINFO, dwModeIndex));
CHECK(c_hinst, FIELD_OFFSET(RDD_HALINFO, hInstance) == FIELD_OFFSET(DDHALINFO, hInstance));
CHECK(c_exebuf, FIELD_OFFSET(RDD_HALINFO, lpDDExeBufCallbacks) == FIELD_OFFSET(DDHALINFO, lpDDExeBufCallbacks));
CHECK(c_exebufcb, sizeof(((RDD_SHARED *)0)->exebufcb) == sizeof(DDHAL_DDEXEBUFCALLBACKS));
CHECK(c_ddcb, sizeof(((RDD_SHARED *)0)->ddcb) == sizeof(DDHAL_DDCALLBACKS));
CHECK(c_surfcb, sizeof(((RDD_SHARED *)0)->surfcb) == sizeof(DDHAL_DDSURFACECALLBACKS));
CHECK(c_palcb, sizeof(((RDD_SHARED *)0)->palcb) == sizeof(DDHAL_DDPALETTECALLBACKS));
CHECK(c_vidmem, sizeof(RDD_VIDMEM) == sizeof(VIDMEM));
CHECK(c_modeinfo, sizeof(RDD_MODEINFO) == sizeof(DDHALMODEINFO));
CHECK(c_destroy, FIELD_OFFSET(DDHAL_DDCALLBACKS, DestroyDriver) == RDD_CB_DESTROYDRIVER * 4);

static HINSTANCE dll_instance;
static RDD_SHARED *shared;             /* the 16-bit driver's block, flat */
static int scanout_used;
static int heap_pages;                 /* the mini-VDD shows pages of the system heap (RVD_DESKTOP_HEAP) */
static FLATPTR gpu_page;               /* address of the page currently presented from the GPU */
static DWORD gpu_mode;

/* ---- Small things ---------------------------------------------------------------------------- */
static void dbg(const char *s)
{
    while (*s) {
        char c = *s++;
        __asm {
            mov al, c
            out 0E9h, al
        }
    }
}

static void dbg_hex(DWORD v)
{
    int i;
    char s[9];
    for (i = 7; i >= 0; i--) {
        s[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    s[8] = 0;
    dbg(s);
}

static void zero(void *p, DWORD n)
{
    BYTE *b = (BYTE *)p;
    while (n--)
        *b++ = 0;
}

/* QEMU's VBE registers: index at 01CEh, data at 01CFh (the mini-VDD lets the Windows VM's
   protected-mode code through to the card while the display driver has its mode). */
static void vbe_write(WORD index, WORD value)
{
    __asm {
        mov dx, 01CEh
        mov ax, index
        out dx, ax
        inc dx
        mov ax, value
        out dx, ax
    }
}
#define VBE_Y_OFFSET 9

/* ---- A 60 Hz clock for the vertical blank a virtual card doesn't have ------------------------ */
/* 32-bit arithmetic only (no C run-time for 64-bit division): the counter is shifted right
   until its frequency fits in 31 bits; its low 32 bits then wrap now and then, which only
   costs one odd frame. */
static DWORD qpc_shift, frame_ticks;

static DWORD shifted(LARGE_INTEGER v)
{
    DWORD i, lo = v.LowPart, hi = (DWORD)v.HighPart;
    for (i = 0; i < qpc_shift; i++) {
        lo = (lo >> 1) | (hi << 31);
        hi >>= 1;
    }
    return lo;
}

static void clock_init(void)
{
    LARGE_INTEGER f;
    DWORD i;

    if (!QueryPerformanceFrequency(&f) || (!f.LowPart && !f.HighPart)) {
        f.LowPart = 1000;               /* fall back to GetTickCount's milliseconds */
        f.HighPart = 0;
    }
    for (i = 0; f.HighPart || (f.LowPart & 0x80000000UL); i++) {
        f.LowPart = (f.LowPart >> 1) | ((DWORD)f.HighPart << 31);
        f.HighPart = (DWORD)f.HighPart >> 1;
    }
    qpc_shift = i;
    frame_ticks = f.LowPart / 60;
    if (!frame_ticks)
        frame_ticks = 1;
}

/* The counter's ticks since some start (shifted, see above); frame_ticks of them make a
   frame. */
static DWORD now(void)
{
    LARGE_INTEGER t;
    if (!QueryPerformanceCounter(&t)) {
        t.LowPart = GetTickCount();
        t.HighPart = 0;
    }
    return shifted(t);
}

/* The blank: the first twentieth of a frame, and at least one tick of the clock. */
static DWORD blank_ticks(void)
{
    DWORD b = frame_ticks / 20;
    return b ? b : 1;
}

/* Where in the current 1/60 s the "beam" is, from 0 to period - 1. */
static DWORD phase(DWORD *period)
{
    *period = frame_ticks;
    return now() % frame_ticks;
}

/* The blank is the first twentieth of each frame, so the next blank starts when the frame
   number changes. The waits watch for that change rather than for a reading inside the blank:
   a coarse clock (GetTickCount's steps can be longer than the blank) might never give one. */
static DWORD WINAPI WaitForVerticalBlank(LPDDHAL_WAITFORVERTICALBLANKDATA d)
{
    DWORD t = now(), frame = t / frame_ticks, blank = blank_ticks();

    d->ddRVal = DD_OK;
    switch (d->dwFlags) {
    case DDWAITVB_I_TESTVB:
        d->bIsInVB = t % frame_ticks < blank;
        break;
    case DDWAITVB_BLOCKBEGIN:           /* the start of the next blank: if one is on now,
                                           it is not the next */
    case DDWAITVB_BLOCKBEGINEVENT:
        while (now() / frame_ticks == frame)
            Sleep(0);
        break;
    case DDWAITVB_BLOCKEND:             /* the end of the current blank, or of the next one */
        if (t % frame_ticks >= blank) {
            while (now() / frame_ticks == frame)
                Sleep(0);
            frame = now() / frame_ticks;
        }
        for (;;) {
            t = now();
            if (t / frame_ticks != frame || t % frame_ticks >= blank)
                break;
            Sleep(0);
        }
        break;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI GetScanLine(LPDDHAL_GETSCANLINEDATA d)
{
    DWORD period, p = phase(&period), blank = blank_ticks();

    if (p < blank) {
        d->ddRVal = DDERR_VERTICALBLANKINPROGRESS;
    } else {
        d->dwScanLine = (DWORD)MulDiv((int)(p - blank), (int)shared->height, (int)(period - blank));
        d->ddRVal = DD_OK;
    }
    return DDHAL_DRIVER_HANDLED;
}

/* ---- Surfaces -------------------------------------------------------------------------------- */
static DWORD WINAPI CanCreateSurface(LPDDHAL_CANCREATESURFACEDATA d)
{
    /* Video memory holds surfaces in the screen's own format, and textures and z-buffers in
       the formats Direct3D was offered. */
    d->ddRVal = !d->bIsDifferentPixelFormat || d3d_surface_format_ok(d->lpDDSurfaceDesc) ?
                DD_OK : DDERR_INVALIDPIXELFORMAT;
    d3d_trace("can create: caps, result", d->lpDDSurfaceDesc->ddsCaps.dwCaps, d->ddRVal);
    if (d->ddRVal != DD_OK) {           /* which format was refused */
        const DDPIXELFORMAT *pf = &d->lpDDSurfaceDesc->ddpfPixelFormat;
        d3d_trace("refused format: flags, fourcc", pf->dwFlags, pf->dwFourCC);
        d3d_trace("refused format: bits, red mask", pf->dwRGBBitCount, pf->dwRBitMask);
        d3d_trace("refused format: green mask, alpha mask", pf->dwGBitMask, pf->dwRGBAlphaBitMask);
    }
    return DDHAL_DRIVER_HANDLED;
}

/* DirectDraw 7 calls this while restoring a lost flip chain. Without a 32-bit entry
   the runtime goes through its Win16 thunk, even though the runtime owns the heaps.
   Let it assign the primary and allocate the attached pages on create and restore. */
static DWORD WINAPI CreateSurface(LPDDHAL_CREATESURFACEDATA d)
{
    DWORD i;

    if (d->dwSCnt)
        d3d_trace("create: surfaces, caps", d->dwSCnt, d->lplpSList[0]->ddsCaps.dwCaps);
    /* DXT surfaces in video memory have no pitch DirectDraw could size them by: ask it for a
       block of the bytes their 4x4 blocks take (each level is a surface of its own). */
    for (i = 0; i < d->dwSCnt; i++) {
        LPDDRAWI_DDRAWSURFACE_LCL s = d->lplpSList[i];
        LPDDRAWI_DDRAWSURFACE_GBL g = s->lpGbl;
        DWORD block = (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) ? d3d_dxt_block(&g->ddpfSurface) : 0;
        DWORD texel = (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) ? d3d_fourcc_bytes(&g->ddpfSurface) : 0;
        if (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)
            continue;
        if (s->lpSurfMore && (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_VOLUME) &&
            (s->dwFlags & DDRAWISURF_HASPIXELFORMAT)) {
            /* A volume level (Direct3D 8 and 9): its slices one after another, rows tight (DXT:
               rows of 4x4 blocks); the list holds the levels in order, each with half the slices
               of the one above. */
            LPDDRAWI_DDRAWSURFACE_LCL top = d->lplpSList[0];
            DWORD depth = top->lpSurfMore ? top->lpSurfMore->ddsCapsEx.dwCaps4 >> i : 1;
            DWORD pitch = block ? ((g->wWidth + 3) / 4) * block :
                          g->wWidth * (texel ? texel : g->ddpfSurface.dwRGBBitCount / 8);
            DWORD rows = block ? (g->wHeight + 3) / 4 : g->wHeight;
            if (!depth)
                depth = 1;
            g->lPitch = (LONG)pitch;
            g->dwBlockSizeX = pitch * rows * depth;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
            continue;
        }
        if (texel) {                    /* an uncompressed format named by FOURCC (Q8W8V8U8): no bit
                                           count for DirectDraw to size it by; rows tight */
            g->lPitch = (LONG)(g->wWidth * texel);
            g->dwBlockSizeX = g->wWidth * texel * g->wHeight;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
            continue;
        }
        if (!block)
            continue;
        g->dwBlockSizeX = ((g->wWidth + 3) / 4) * ((g->wHeight + 3) / 4) * block;
        g->dwBlockSizeY = 1;
        g->dwLinearSize = g->dwBlockSizeX;
        g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* Select the CPU's page when GDI or DirectDraw will draw directly into its memory. The VxD
   follows writes from then on, including ones which bypass the DirectDraw HAL. */
static int select_cpu_page(DWORD offset, int flip, int heap)
{
    HANDLE h;
    DWORD got;
    struct rvd_desktop page;
    int ok;
    h = CreateFile(RVD_DEVICE_NAME, 0, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    page.offset = offset;
    page.flags = (flip ? RVD_DESKTOP_FLIP : 0) | (heap ? RVD_DESKTOP_HEAP : 0);
    ok = DeviceIoControl(h, RVD_DESKTOP, &page, page.flags ? sizeof page : sizeof page.offset,
                         NULL, 0, &got, NULL);
    CloseHandle(h);
    if (ok)
        gpu_page = 0;
    return ok;
}

/* Where the memory of a surface that may be shown lies. In the card's memory it starts a line
   the display can start at (QEMU takes none past RDD_MAX_Y_OFFSET). In the system heap it is a
   back buffer the card's 8 MB had no room for (ddraw16.c), which the card can't show: the GPU
   or the mini-VDD does. *off: bytes from the start of the one or the other. */
#define PAGE_CARD 1
#define PAGE_HEAP 2
static int page_place(FLATPTR mem, DWORD *off)
{
    DWORD o = mem - shared->lfb;

    if (shared->pitch && mem >= shared->lfb && o % shared->pitch == 0 &&
        o / shared->pitch <= RDD_MAX_Y_OFFSET && o / shared->pitch + shared->height <= shared->lines) {
        *off = o;
        return PAGE_CARD;
    }
    o = mem - shared->sysheap;
    if (heap_pages && shared->sysheap && mem >= shared->sysheap && o < shared->sysheap_size &&
        shared->pitch * shared->height <= shared->sysheap_size - o) {
        *off = o;
        return PAGE_HEAP;
    }
    return 0;
}

int rdd_shown_page(FLATPTR mem)
{
    DWORD off;
    return page_place(mem, &off) != 0;
}

/* DDHELP can unload this DLL while the mini-VDD is still displaying the GDI mirror.
   Query that state at DriverInit without changing the page an application is showing. */
static void query_scanout(void)
{
    DWORD got, active[2];               /* taken over; the request flags the VxD knows */
    HANDLE h = CreateFile(RVD_DEVICE_NAME, 0, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        active[1] = 0;
        if (DeviceIoControl(h, RVD_DESKTOP, NULL, 0, active, sizeof active, &got, NULL) &&
            got >= sizeof active[0]) {
            scanout_used = active[0] != 0;
            if (scanout_used)
                dbg("rdd: existing desktop scanout found\r\n");
            heap_pages = got == sizeof active && (active[1] & RVD_DESKTOP_HEAP);
        }
        CloseHandle(h);
    }
}

int rdd_prepare_scanout(void)
{
    /* A previous VxD may lack both requests. Confirm the CPU path before the first GPU flip.
       The new VxD leaves VGA alone until that flip. */
    if (!scanout_used)
        scanout_used = select_cpu_page(0, 0, 0);
    return scanout_used;
}

int rdd_desktop(DWORD offset)
{
    return !scanout_used || select_cpu_page(offset, 0, 0);
}

/* The display shows the CPU's side of the page at mem: in the card's memory, or in the system
   heap, which only the mini-VDD can show (it takes the display from VGA for it). */
static int show_cpu_page(FLATPTR mem, int flip)
{
    DWORD off;

    switch (page_place(mem, &off)) {
    case PAGE_CARD:
        return !scanout_used || select_cpu_page(off, flip, 0);
    case PAGE_HEAP:
        if (!select_cpu_page(off, flip, 1))
            return 0;
        scanout_used = 1;
        return 1;
    }
    return 0;
}

/* Show another surface. In the card's memory DirectDraw places back buffers in the heap below
   the screen at the start of a line, so a line number is all the card needs to start the
   display there. A back buffer in the system heap (the card's memory was full) is shown from
   the GPU when Direct3D drew it, and otherwise by the mini-VDD, from its memory. */
static DWORD WINAPI Flip(LPDDHAL_FLIPDATA d)
{
    FLATPTR mem = d->lpSurfTarg->lpGbl->fpVidMem;
    DWORD off = 0;
    int place = page_place(mem, &off);

    d3d_trace("flip: from, to", d->lpSurfCurr->lpGbl->fpVidMem, mem);
    if (!place) {
        dbg("rdd: can't show a surface at "); dbg_hex(mem); dbg("\r\n");
        d->ddRVal = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    if (d3d_present(d->lpSurfCurr, d->lpSurfTarg)) {
        gpu_page = mem;
        gpu_mode = shared->mode_serial;
        d3d_profile_flip(1);
    } else {
        if (!d3d_surface_access(d->lpSurfTarg) || !show_cpu_page(mem, 1)) {
            d->ddRVal = DDERR_GENERIC;
            return DDHAL_DRIVER_HANDLED;
        }
        if (place == PAGE_CARD)
            vbe_write(VBE_Y_OFFSET, (WORD)(off / shared->pitch));
        d3d_profile_flip(0);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Flips and blits are done as soon as they are asked for. */
static DWORD WINAPI GetFlipStatus(LPDDHAL_GETFLIPSTATUSDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* A surface's memory for the program: the address in video memory (all of it is mapped,
   linear), moved to the corner of the rectangle asked for. */
static DWORD WINAPI Lock(LPDDHAL_LOCKDATA d)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = d->lpDDSurface;
    LPDDRAWI_DDRAWSURFACE_GBL g = s->lpGbl;
    DWORD p = g->fpVidMem, bits = shared->bpp;
    DWORD block = 0;

    d3d_trace("lock: memory, flags", g->fpVidMem, d->dwFlags);
    if (!d3d_surface_access(s) || (p && p == gpu_page && gpu_mode == shared->mode_serial &&
        !(d->dwFlags & DDLOCK_READONLY) &&
        !show_cpu_page(p, 0))) {
        d->ddRVal = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    if (!(d->dwFlags & DDLOCK_READONLY))
        d3d_surface_written(s);         /* textures made from it are out of date */
    if (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) {
        block = d3d_dxt_block(&g->ddpfSurface);
        bits = g->ddpfSurface.dwRGBBitCount;
        if (d3d_fourcc_bytes(&g->ddpfSurface))
            bits = 8 * d3d_fourcc_bytes(&g->ddpfSurface);
    }
    if (block && d->bHasRect)           /* DXT: rows of 4x4 blocks */
        p += (d->rArea.top / 4) * (((DWORD)g->wWidth + 3) / 4) * block + (d->rArea.left / 4) * block;
    else if (d->bHasRect)
        p += d->rArea.top * g->lPitch + d->rArea.left * (bits / 8);
    d->lpSurfData = (LPVOID)p;
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- Direct3D 8 vertex and index buffers the driver allocates (d3d_buffers_on) ---------------
   DirectDraw calls these for execute buffers in video memory, which the Direct3D 8 runtime
   makes its vertex and index buffers when the device reports D3DDEVCAPS_HWVERTEXBUFFER and
   D3DDEVCAPS_HWINDEXBUFFER. Each Lock that may write marks the buffer for the GPU again. */
static DWORD WINAPI CanCreateExecuteBuffer(LPDDHAL_CANCREATESURFACEDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI CreateExecuteBuffer(LPDDHAL_CREATESURFACEDATA d)
{
    DWORD i, j;

    for (i = 0; i < d->dwSCnt; i++) {
        LPDDRAWI_DDRAWSURFACE_GBL g = d->lplpSList[i]->lpGbl;
        DWORD size = g->dwLinearSize ? g->dwLinearSize : d->lpDDSurfaceDesc->dwLinearSize;
        void *p = size ? d3d_buffer_new(size) : NULL;
        d3d_trace("buffer create: bytes, memory", size, (DWORD)p);
        if (!p) {
            for (j = 0; j < i; j++) {
                d3d_buffer_free(d->lplpSList[j]->lpGbl->fpVidMem);
                d->lplpSList[j]->lpGbl->fpVidMem = 0;
                d->lplpSList[j]->lpGbl->dwReserved1 = 0;
            }
            d->ddRVal = DDERR_OUTOFVIDEOMEMORY;
            return DDHAL_DRIVER_HANDLED;
        }
        g->fpVidMem = (FLATPTR)p;
        g->dwLinearSize = size;
        g->dwReserved1 = D3D_BUFFER_MARK;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI DestroyExecuteBuffer(LPDDHAL_DESTROYSURFACEDATA d)
{
    LPDDRAWI_DDRAWSURFACE_GBL g = d->lpDDSurface->lpGbl;
    if (g->dwReserved1 == D3D_BUFFER_MARK) {
        d3d_buffer_free(g->fpVidMem);
        g->fpVidMem = 0;
        g->dwReserved1 = 0;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* The whole buffer, whatever range is asked for: the runtime adds the offset. */
static DWORD WINAPI LockExecuteBuffer(LPDDHAL_LOCKDATA d)
{
    LPDDRAWI_DDRAWSURFACE_GBL g = d->lpDDSurface->lpGbl;
    d3d_trace("buffer lock: memory, flags", g->fpVidMem, d->dwFlags);
    if (d->bHasRect)
        d3d_trace("buffer lock: from, to", d->rArea.left, d->rArea.right);
    if (!(d->dwFlags & DDLOCK_READONLY) && g->dwReserved1 == D3D_BUFFER_MARK)
        d3d_buffer_written(g->fpVidMem);
    d->lpSurfData = (LPVOID)g->fpVidMem;
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI UnlockExecuteBuffer(LPDDHAL_UNLOCKDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI GetBltStatus(LPDDHAL_GETBLTSTATUSDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* GDI's screen is the one at the start of video memory. */
static HRESULT show_gdi(void)
{
    int synced = !scanout_used || d3d_leave_exclusive();
    int shown = rdd_desktop(0);
    vbe_write(VBE_Y_OFFSET, 0);
    return synced && shown ? DD_OK : DDERR_GENERIC;
}

static DWORD WINAPI FlipToGDISurface(LPDDHAL_FLIPTOGDISURFACEDATA d)
{
    d->ddRVal = show_gdi();
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI SetExclusiveMode(LPDDHAL_SETEXCLUSIVEMODEDATA d)
{
    d->ddRVal = d->dwEnterExcl ? DD_OK : show_gdi();
    return DDHAL_DRIVER_HANDLED;
}

/* ---- What DirectDraw asks for by GUID (DirectX 5 and later) --------------------------------- */
static DWORD WINAPI GetDriverInfo(LPDDHAL_GETDRIVERINFODATA d)
{
    if (d3d_driver_info(d))
        return DDHAL_DRIVER_HANDLED;
    dbg("rdd: GetDriverInfo "); dbg_hex(d->guidInfo.Data1); dbg(" (not offered)\r\n");
    d->dwActualSize = 0;
    d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- The capabilities ------------------------------------------------------------------------ */
static void fill_caps(DDCORECAPS *c)
{
    zero(c, sizeof *c);
    c->dwSize = sizeof *c;
    c->dwCaps = DDCAPS_GDI;             /* the screen is GDI's too; blits: DirectDraw's own
                                           software, on video memory */
    c->dwCaps2 = DDCAPS2_FLIPNOVSYNC;
    c->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_OFFSCREENPLAIN |
                        DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY |
                        DDSCAPS_LOCALVIDMEM;
}

/* ---- Entry points ---------------------------------------------------------------------------- */
/* System memory for surfaces that are never shown (the 16-bit side makes it a second heap):
   the card's VGA memory is 8 MB, which leaves 3D programs little room for textures. The
   mini-VDD makes the block once, in memory every process sees. Its handle stays open: closing
   it would end this process's GPU contexts in the VxD. */
static void sys_heap(RDD_SHARED *s)
{
    MEMORYSTATUS ms;
    struct rvd_heap heap;
    rvd_u32 want;
    DWORD got = 0;
    HANDLE h;

    if (s->sysheap)
        return;
    ms.dwLength = sizeof ms;
    GlobalMemoryStatus(&ms);
    want = ms.dwTotalPhys >= 384UL << 20 ? 64UL << 20 :
           ms.dwTotalPhys >= 192UL << 20 ? 32UL << 20 : 16UL << 20;
    h = CreateFile(RVD_DEVICE_NAME, 0, 0, NULL, 0, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (DeviceIoControl(h, RVD_HEAP, &want, sizeof want, &heap, sizeof heap, &got, NULL) &&
        got == sizeof heap && heap.address) {
        s->sysheap_size = heap.size;
        s->sysheap = (rdd_u32)heap.address;
        dbg("rdd: surfaces never shown go to system memory at "); dbg_hex(s->sysheap);
        dbg(", bytes "); dbg_hex(s->sysheap_size); dbg("\r\n");
    }
}

/* Back buffers may go to the system heap too when the mini-VDD can show a page there
   (query_scanout); the 16-bit side opens the heap to them (ddraw16.c). */
static void sys_heap_flip(RDD_SHARED *s)
{
    s->sysheap_flip = s->sysheap && heap_pages;
    if (s->sysheap_flip)
        dbg("rdd: back buffers the card has no room for go to system memory\r\n");
}

typedef LPVOID (WINAPI *MAPSL)(DWORD);

/* DirectDraw calls this once, with the context the 16-bit driver gave it (rdd.h). The answer
   comes back to every callback as the DirectDraw object's dwReserved3. */
DWORD WINAPI DriverInit(DWORD context)
{
    MAPSL map_sl = (MAPSL)GetProcAddress(GetModuleHandle("KERNEL32"), "MapSL");
    RDD_SHARED *s;
    LPDDHAL_DDCALLBACKS cb;
    LPDDHAL_DDSURFACECALLBACKS scb;
    LPDDHAL_DDPALETTECALLBACKS pcb;

    if (!map_sl)
        return 0;
    s = (RDD_SHARED *)map_sl(context);
    if (!s || s->magic != RDD_MAGIC || s->size != sizeof(RDD_SHARED)) {
        dbg("rdd: the 16-bit driver's block doesn't match this DLL\r\n");
        return 0;
    }
    shared = s;
    query_scanout();
    clock_init();
    sys_heap(s);
    sys_heap_flip(s);

    cb = (LPDDHAL_DDCALLBACKS)s->ddcb;
    zero(cb, sizeof *cb);
    cb->dwSize = sizeof *cb;
    cb->dwFlags = DDHAL_CB32_CANCREATESURFACE | DDHAL_CB32_CREATESURFACE |
                  DDHAL_CB32_WAITFORVERTICALBLANK | DDHAL_CB32_GETSCANLINE |
                  DDHAL_CB32_FLIPTOGDISURFACE | DDHAL_CB32_SETEXCLUSIVEMODE;
    cb->CanCreateSurface = CanCreateSurface;
    cb->CreateSurface = CreateSurface;
    cb->WaitForVerticalBlank = WaitForVerticalBlank;
    cb->GetScanLine = GetScanLine;
    cb->FlipToGDISurface = FlipToGDISurface;
    cb->SetExclusiveMode = SetExclusiveMode;
    /* DestroyDriver is the 16-bit driver's (ddraw16.c), without its 32-bit flag. */

    scb = (LPDDHAL_DDSURFACECALLBACKS)s->surfcb;
    zero(scb, sizeof *scb);
    scb->dwSize = sizeof *scb;
    scb->dwFlags = DDHAL_SURFCB32_FLIP | DDHAL_SURFCB32_LOCK | DDHAL_SURFCB32_GETBLTSTATUS |
                   DDHAL_SURFCB32_GETFLIPSTATUS;
    scb->Flip = Flip;
    scb->Lock = Lock;
    scb->GetBltStatus = GetBltStatus;
    scb->GetFlipStatus = GetFlipStatus;

    pcb = (LPDDHAL_DDPALETTECALLBACKS)s->palcb;
    zero(pcb, sizeof *pcb);
    pcb->dwSize = sizeof *pcb;

    s->hal.dwSize = sizeof(DDHALINFO);
    fill_caps((DDCORECAPS *)s->hal.ddCaps);
    s->hal.GetDriverInfo = (DWORD)GetDriverInfo;
    s->hal.dwFlags |= RDD_HALINFO_GETDRIVERINFOSET;
    s->hal.lpDDExeBufCallbacks = 0;
    d3d8_enabled();                     /* first: d3d_init's capabilities depend on it */
    d3d_init(s, (DDCORECAPS *)s->hal.ddCaps);
    if (d3d8_enabled()) {
        s->hal.dwFlags |= RDD_HALINFO_GETDRIVERINFO2;
        ((DDCORECAPS *)s->hal.ddCaps)->dwNumFourCCCodes = RDD_FOURCCS;  /* DXT1..5 (ddraw16.c) */
        if (d3d_buffers_on()) {
            LPDDHAL_DDEXEBUFCALLBACKS x = (LPDDHAL_DDEXEBUFCALLBACKS)s->exebufcb;
            zero(x, sizeof *x);
            x->dwSize = sizeof *x;
            x->dwFlags = DDHAL_EXEBUFCB32_CANCREATEEXEBUF | DDHAL_EXEBUFCB32_CREATEEXEBUF |
                         DDHAL_EXEBUFCB32_DESTROYEXEBUF | DDHAL_EXEBUFCB32_LOCKEXEBUF |
                         DDHAL_EXEBUFCB32_UNLOCKEXEBUF;
            x->CanCreateExecuteBuffer = CanCreateExecuteBuffer;
            x->CreateExecuteBuffer = CreateExecuteBuffer;
            x->DestroyExecuteBuffer = DestroyExecuteBuffer;
            x->LockExecuteBuffer = LockExecuteBuffer;
            x->UnlockExecuteBuffer = UnlockExecuteBuffer;
            s->hal.lpDDExeBufCallbacks = s->self16 + FIELD_OFFSET(RDD_SHARED, exebufcb);
            /* DirectDraw offers execute buffers in video memory only on hardware that has them */
            ((DDCORECAPS *)s->hal.ddCaps)->ddsCaps.dwCaps |= DDSCAPS_EXECUTEBUFFER;
            dbg("rdd: Direct3D 8 buffers in memory the driver allocates\r\n");
        }
    } else {
        s->hal.dwFlags &= ~RDD_HALINFO_GETDRIVERINFO2;
        ((DDCORECAPS *)s->hal.ddCaps)->dwNumFourCCCodes = 0;
    }
    s->dll_ready = 1;
    dbg("rdd: DirectDraw HAL DLL ready, loaded at "); dbg_hex((DWORD)dll_instance);
    dbg(", process "); dbg_hex(GetCurrentProcessId()); dbg("\r\n");
    return (DWORD)s;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        dll_instance = instance;
        DisableThreadLibraryCalls(instance);
        d3d_process(1);
    } else if (reason == DLL_PROCESS_DETACH) {
        d3d_process(0);
    }
    return TRUE;
}
