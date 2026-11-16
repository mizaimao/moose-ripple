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

/* rdd.h's description of DDHALINFO must match the DDK's. */
#define CHECK(name, cond) typedef char name[(cond) ? 1 : -1]
CHECK(c_halinfo, sizeof(RDD_HALINFO) == sizeof(DDHALINFO));
CHECK(c_vmidata, FIELD_OFFSET(RDD_HALINFO, fpPrimary) == FIELD_OFFSET(DDHALINFO, vmiData));
CHECK(c_caps, FIELD_OFFSET(RDD_HALINFO, ddCaps) == FIELD_OFFSET(DDHALINFO, ddCaps));
CHECK(c_heaps, FIELD_OFFSET(RDD_HALINFO, dwNumHeaps) == FIELD_OFFSET(DDHALINFO, vmiData) + FIELD_OFFSET(VIDMEMINFO, dwNumHeaps));
CHECK(c_modeidx, FIELD_OFFSET(RDD_HALINFO, dwModeIndex) == FIELD_OFFSET(DDHALINFO, dwModeIndex));
CHECK(c_hinst, FIELD_OFFSET(RDD_HALINFO, hInstance) == FIELD_OFFSET(DDHALINFO, hInstance));
CHECK(c_ddcb, sizeof(((RDD_SHARED *)0)->ddcb) == sizeof(DDHAL_DDCALLBACKS));
CHECK(c_surfcb, sizeof(((RDD_SHARED *)0)->surfcb) == sizeof(DDHAL_DDSURFACECALLBACKS));
CHECK(c_palcb, sizeof(((RDD_SHARED *)0)->palcb) == sizeof(DDHAL_DDPALETTECALLBACKS));
CHECK(c_vidmem, sizeof(RDD_VIDMEM) == sizeof(VIDMEM));
CHECK(c_modeinfo, sizeof(RDD_MODEINFO) == sizeof(DDHALMODEINFO));
CHECK(c_destroy, FIELD_OFFSET(DDHAL_DDCALLBACKS, DestroyDriver) == RDD_CB_DESTROYDRIVER * 4);

static HINSTANCE dll_instance;
static RDD_SHARED *shared;             /* the 16-bit driver's block, flat */

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

/* Where in the current 1/60 s the "beam" is, from 0 to period - 1. */
static DWORD phase(DWORD *period)
{
    LARGE_INTEGER t;
    if (!QueryPerformanceCounter(&t)) {
        t.LowPart = GetTickCount();
        t.HighPart = 0;
    }
    *period = frame_ticks;
    return shifted(t) % frame_ticks;
}

static DWORD WINAPI WaitForVerticalBlank(LPDDHAL_WAITFORVERTICALBLANKDATA d)
{
    DWORD period, p = phase(&period), blank = period / 20;

    d->ddRVal = DD_OK;
    switch (d->dwFlags) {
    case DDWAITVB_I_TESTVB:
        d->bIsInVB = p < blank;
        break;
    case DDWAITVB_BLOCKBEGIN:
    case DDWAITVB_BLOCKBEGINEVENT:
        while (phase(&period) >= blank)
            Sleep(0);
        break;
    case DDWAITVB_BLOCKEND:
        while (phase(&period) < blank)
            Sleep(0);
        break;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI GetScanLine(LPDDHAL_GETSCANLINEDATA d)
{
    DWORD period, p = phase(&period), blank = period / 20;

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
    /* Video memory holds surfaces in the screen's own format only (for now). */
    d->ddRVal = d->bIsDifferentPixelFormat ? DDERR_INVALIDPIXELFORMAT : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Show another surface: point the start of the display at it. DirectDraw places back buffers
   in the heap below the screen at the start of a line, so a line number is all the card needs. */
static DWORD WINAPI Flip(LPDDHAL_FLIPDATA d)
{
    DWORD off = d->lpSurfTarg->lpGbl->fpVidMem - shared->lfb;

    if (!shared->pitch || off % shared->pitch || off / shared->pitch + shared->height > shared->lines) {
        dbg("rdd: can't show a surface at offset "); dbg_hex(off); dbg("\r\n");
        d->ddRVal = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    vbe_write(VBE_Y_OFFSET, (WORD)(off / shared->pitch));
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Flips and blits are done as soon as they are asked for. */
static DWORD WINAPI GetFlipStatus(LPDDHAL_GETFLIPSTATUSDATA d)
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
static DWORD WINAPI FlipToGDISurface(LPDDHAL_FLIPTOGDISURFACEDATA d)
{
    vbe_write(VBE_Y_OFFSET, 0);
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- What DirectDraw asks for by GUID (DirectX 5 and later) --------------------------------- */
static DWORD WINAPI GetDriverInfo(LPDDHAL_GETDRIVERINFODATA d)
{
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
    clock_init();

    cb = (LPDDHAL_DDCALLBACKS)s->ddcb;
    zero(cb, sizeof *cb);
    cb->dwSize = sizeof *cb;
    cb->dwFlags = DDHAL_CB32_CANCREATESURFACE | DDHAL_CB32_WAITFORVERTICALBLANK |
                  DDHAL_CB32_GETSCANLINE | DDHAL_CB32_FLIPTOGDISURFACE;
    cb->CanCreateSurface = CanCreateSurface;
    cb->WaitForVerticalBlank = WaitForVerticalBlank;
    cb->GetScanLine = GetScanLine;
    cb->FlipToGDISurface = FlipToGDISurface;
    /* DestroyDriver is the 16-bit driver's (ddraw16.c), without its 32-bit flag. */

    scb = (LPDDHAL_DDSURFACECALLBACKS)s->surfcb;
    zero(scb, sizeof *scb);
    scb->dwSize = sizeof *scb;
    scb->dwFlags = DDHAL_SURFCB32_FLIP | DDHAL_SURFCB32_GETBLTSTATUS | DDHAL_SURFCB32_GETFLIPSTATUS;
    scb->Flip = Flip;
    scb->GetBltStatus = GetBltStatus;
    scb->GetFlipStatus = GetFlipStatus;

    pcb = (LPDDHAL_DDPALETTECALLBACKS)s->palcb;
    zero(pcb, sizeof *pcb);
    pcb->dwSize = sizeof *pcb;

    s->hal.dwSize = sizeof(DDHALINFO);
    fill_caps((DDCORECAPS *)s->hal.ddCaps);
    s->hal.GetDriverInfo = (DWORD)GetDriverInfo;
    s->hal.dwFlags |= RDD_HALINFO_GETDRIVERINFOSET;
    s->hal.lpD3DGlobalDriverData = 0;
    s->hal.lpD3DHALCallbacks = 0;
    s->hal.lpDDExeBufCallbacks = 0;
    s->dll_ready = 1;
    dbg("rdd: DirectDraw HAL DLL ready\r\n");
    return (DWORD)s;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        dll_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
