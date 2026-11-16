/*
 * moose-ripple display driver for Windows 9x: the 16-bit half of DirectDraw.
 *
 * DirectDraw asks the display driver for its hardware abstraction layer with the escape
 * DCICOMMAND (through GDI's Control). In the order Windows 98 sends them:
 *   DDVERSIONINFO          which DirectDraw is asking (not answered: see below)
 *   DDGET32BITDRIVERNAME   the 32-bit DLL to load (rippledd.dll), its entry point and a context:
 *                          the 16:16 address of the block we share with it (rdd.h). DirectDraw
 *                          loads the DLL and calls DriverInit, which fills in its part.
 *   DDNEWCALLBACKFNS       DDRAW16's SetInfo, through which the HAL description is handed over
 *   DDCREATEDRIVEROBJECT   fill in the screen mode and pass the description to SetInfo
 * At every later mode set the driver updates its part and calls SetInfo again with "reset".
 * docs/win9x/directdraw.md describes the whole.
 */
#include "r9x.h"
#include "rdd.h"

static RDD_SHARED shared;

typedef union { LPVOID p; DWORD d; struct { WORD lo, hi; } w; } FARP;
typedef WORD (__far __pascal *SETINFO)(LPVOID halinfo, WORD reset);

static DWORD far16(void __near *p)          /* a near pointer into our data as 16:16 */
{
    FARP f;
    f.p = (LPVOID)p;
    return f.d;
}

static void fcopy(void __far *dst, const void __far *src, WORD n)
{
    BYTE __far *d = (BYTE __far *)dst;
    const BYTE __far *s = (const BYTE __far *)src;
    while (n--)
        *d++ = *s++;
}

static void fzero(void __far *dst, WORD n)
{
    BYTE __far *d = (BYTE __far *)dst;
    while (n--)
        *d++ = 0;
}

/* The 16-bit callback DirectDraw calls when it lets go of the driver: no more SetInfo. */
DWORD DDI DestroyDriver16(LPVOID data)
{
    DWORD __far *d = (DWORD __far *)data;   /* lpDD, ddRVal, DestroyDriver */
    shared.setinfo16 = 0;
    d[1] = 0;                               /* DD_OK */
    dbg("r9x: DirectDraw let go of the driver\r\n");
    return 1;                               /* DDHAL_DRIVER_HANDLED */
}

/* ---- The screen mode as DirectDraw sees it ---------------------------------------------- */
static const WORD __near std_sizes[][2] = {
    {320, 200}, {320, 240}, {400, 300}, {512, 384}, {640, 400}, {640, 480}, {800, 600},
    {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960}, {1280, 1024}, {1600, 1200},
};
static const WORD __near std_bpp[] = { 8, 16, 32 };

static void set_masks(RDD_MODEINFO __far *m, WORD bpp)
{
    m->dwRBitMask = m->dwGBitMask = m->dwBBitMask = m->dwAlphaBitMask = 0;
    if (bpp == 16) {
        m->dwRBitMask = 0xF800; m->dwGBitMask = 0x07E0; m->dwBBitMask = 0x001F;
    } else if (bpp >= 24) {
        m->dwRBitMask = 0xFF0000UL; m->dwGBitMask = 0xFF00; m->dwBBitMask = 0xFF;
    }
}

static WORD add_mode(WORD w, WORD h, WORD bpp)
{
    RDD_MODEINFO __near *m;
    WORD n = (WORD)shared.hal.dwNumModes;

    if (n >= RDD_MAX_MODES)
        return 0xFFFF;
    m = &shared.modes[n];
    m->dwWidth = w;
    m->dwHeight = h;
    m->dwBPP = bpp;
    m->lPitch = mul32(w, (bpp + 7) / 8);
    m->wFlags = bpp == 8 ? RDD_MODEINFO_PALETTIZED : 0;
    m->wRefreshRate = 0;
    set_masks(m, bpp);
    shared.hal.dwNumModes = n + 1;
    return n;
}

/* Fill in our part of the HAL description for the current mode. */
static void fill_halinfo(void)
{
    RDD_HALINFO __near *h = &shared.hal;
    DWORD used = mul32((WORD)shared.pitch, (WORD)shared.height);
    WORD i, j, cur = 0xFFFF;

    h->lpDDCallbacks = far16(shared.ddcb);
    h->lpDDSurfaceCallbacks = far16(shared.surfcb);
    h->lpDDPaletteCallbacks = far16(shared.palcb);
    {
        FARP destroy;
        destroy.p = (LPVOID)DestroyDriver16;
        shared.ddcb[RDD_CB_DESTROYDRIVER] = destroy.d;
    }

    h->fpPrimary = shared.lfb;
    h->vmFlags = 0;
    h->dwDisplayWidth = shared.width;
    h->dwDisplayHeight = shared.height;
    h->lDisplayPitch = shared.pitch;
    h->pfSize = 32;
    h->pfFlags = RDD_PF_RGB | (shared.bpp == 8 ? RDD_PF_PALETTEINDEXED8 : 0);
    h->pfFourCC = 0;
    h->pfRGBBitCount = shared.bpp;
    {
        RDD_MODEINFO tmp;
        set_masks(&tmp, (WORD)shared.bpp);
        h->pfRBitMask = tmp.dwRBitMask;
        h->pfGBitMask = tmp.dwGBitMask;
        h->pfBBitMask = tmp.dwBBitMask;
        h->pfAlphaMask = 0;
    }
    h->dwOffscreenAlign = h->dwOverlayAlign = h->dwTextureAlign = 8;
    h->dwZBufferAlign = h->dwAlphaAlign = 8;

    /* The rest of video memory, below the screen: one heap of whole lines. */
    fzero(&shared.heap, sizeof shared.heap);
    if (shared.lines > shared.height + 1) {
        shared.heap.dwFlags = RDD_VIDMEM_ISRECTANGULAR;
        shared.heap.fpStart = shared.lfb + used;
        shared.heap.dwWidth = shared.pitch;
        shared.heap.dwHeight = shared.lines - shared.height;
        h->dwNumHeaps = 1;
    } else {
        h->dwNumHeaps = 0;
    }
    h->pvmList = far16(&shared.heap);

    h->dwMonitorFrequency = 60;
    h->dwNumModes = 0;
    for (i = 0; i < sizeof std_sizes / sizeof std_sizes[0]; i++) {
        for (j = 0; j < sizeof std_bpp / sizeof std_bpp[0]; j++) {
            WORD w = std_sizes[i][0], ht = std_sizes[i][1], b = std_bpp[j];
            if (mul32(w * ((b + 7) / 8), ht) > shared.vram)
                continue;
            if (w == shared.width && ht == shared.height && b == shared.bpp)
                cur = add_mode(w, ht, b);
            else
                add_mode(w, ht, b);
        }
    }
    if (cur == 0xFFFF)                      /* a size not in the list, or 24 bits */
        cur = add_mode((WORD)shared.width, (WORD)shared.height, (WORD)shared.bpp);
    h->dwModeIndex = cur;
    h->lpModeInfo = far16(shared.modes);
    h->lpdwFourCC = 0;
    h->dwFlags |= RDD_HALINFO_ISPRIMARYDISPLAY | RDD_HALINFO_MODEXILLEGAL;
    {
        FARP dgroup;                        /* "the base display driver's instance": DGROUP */
        dgroup.p = (LPVOID)&shared;
        h->hInstance = dgroup.w.hi;
    }
    {
        FARP pd;
        pd.p = lpDriverPDevice;
        h->lpPDevice = pd.d;
    }
}

static WORD call_setinfo(WORD reset)
{
    SETINFO setinfo;
    FARP f;

    f.d = shared.setinfo16;
    setinfo = (SETINFO)f.p;
    return setinfo((LPVOID)&shared.hal, reset);
}

/* Called by Enable after every mode set (enable.c). */
void dd_mode_set(DWORD lfb, DWORD vram, WORD width, WORD height, WORD bpp, WORD pitch)
{
    shared.lfb = lfb;
    shared.vram = vram;
    shared.width = width;
    shared.height = height;
    shared.bpp = bpp;
    shared.pitch = pitch;
    shared.lines = pitch ? div32(vram, pitch) : 0;
    shared.mode_serial++;
    if (shared.setinfo16 && shared.dll_ready) {
        fill_halinfo();
        call_setinfo(1);
        dbg("r9x: DirectDraw told of the new mode\r\n");
    }
}

/* ---- The escape ------------------------------------------------------------------------------ */
static int escape(WORD function, LPVOID in, LPVOID out);

/* DCI 1.0 commands go to the DIB engine, which provides DCI for the screen. With DDTRACE its
   answer is logged, and for the primary surface what it describes. */
static int dci_escape(WORD function, LPVOID in, LPVOID out)
{
    int r = DIB_Control(lpDriverPDevice, function, in, out);
#ifdef DDTRACE
    RDD_DCICMD __far *cmd = (RDD_DCICMD __far *)in;
    dbg("r9x: DCI "); dbg_hex(cmd ? cmd->dwCommand : 0xFFFF); dbg(" v");
    dbg_hex(cmd ? cmd->dwVersion : 0); dbg(": DIB engine says "); dbg_hex((DWORD)(long)r);
    if (out) {
        DWORD __far *o = (DWORD __far *)out;
        dbg(", out "); dbg_hex(o[0]);
        if (r == 0 && o[0]) {
            FARP f;
            DWORD __far *s;
            f.d = o[0];
            s = (DWORD __far *)f.p;         /* a DCISURFACEINFO */
            dbg(": size "); dbg_hex(s[0]); dbg(" caps "); dbg_hex(s[1]);
            dbg(" "); dbg_hex(s[6]); dbg("x"); dbg_hex(s[7]); dbg(" stride "); dbg_hex(s[8]);
            dbg(" bits "); dbg_hex(s[9]); dbg(" at "); dbg_hex(*(WORD __far *)&s[11]);
            dbg(":"); dbg_hex(s[10]);
        }
    }
    dbg("\r\n");
#endif
    return r;
}

/* Returns -1 for escapes that are not DirectDraw's. With DDTRACE defined (DDTRACE=1
   win9x/build.sh) every escape and its answer goes to the debug port. */
int dd_escape(WORD function, LPVOID in, LPVOID out)
{
    int r = escape(function, in, out);
#ifdef DDTRACE
    RDD_DCICMD __far *cmd = (RDD_DCICMD __far *)in;

    if (function == QUERYESCSUPPORT && in) {
        dbg("r9x: QUERYESCSUPPORT "); dbg_hex(*(WORD __far *)in); dbg(" -> "); dbg_hex(r);
        dbg("\r\n");
    } else if (function == RDD_DCICOMMAND && cmd) {
        dbg("r9x: DCICOMMAND "); dbg_hex(cmd->dwCommand); dbg(" "); dbg_hex(cmd->dwParam1);
        dbg(" "); dbg_hex(cmd->dwParam2); dbg(" v"); dbg_hex(cmd->dwVersion);
        dbg(" -> "); dbg_hex(r); dbg("\r\n");
    } else if (function != 0x1101) {
        dbg("r9x: escape "); dbg_hex(function); dbg("\r\n");
    }
#endif
    return r;
}

static int escape(WORD function, LPVOID in, LPVOID out)
{
    RDD_DCICMD __far *cmd = (RDD_DCICMD __far *)in;

    if (function == QUERYESCSUPPORT)
        return (in && *(WORD __far *)in == RDD_DCICOMMAND) ? RDD_DD_HAL_VERSION : R9X_NOT_OURS;
    if (function != RDD_DCICOMMAND)
        return R9X_NOT_OURS;
    /* Other DCI commands (DCI 1.0's, version 0x100: DirectDraw creates the primary surface
       that way) are the DIB engine's. */
    if (!cmd || cmd->dwVersion != RDD_DD_VERSION)
        return dci_escape(function, in, out);
    if (!shared.magic) {
        shared.magic = RDD_MAGIC;
        shared.size = sizeof shared;
        shared.self16 = far16(&shared);
    }
    switch ((WORD)cmd->dwCommand) {
    case RDD_VERSIONINFO:
        /* Not answered, as the DirectX 5 drivers Windows 98's DirectX 6.1 grew up with: an
           answer makes DirectDraw expect a driver of the newer interface. */
        shared.runtime = cmd->dwParam1;
        dbg("r9x: DirectDraw version "); dbg_hex(cmd->dwParam1); dbg("\r\n");
        return 0;
    case RDD_GET32BITDRIVERNAME: {
        static const char __near name[] = RDD_DLL_NAME, entry[] = RDD_DLL_ENTRY;
        RDD_DRIVERDATA32 __far *d = (RDD_DRIVERDATA32 __far *)out;
        if (!d)
            return 0;
        fcopy(d->szName, name, sizeof name);
        fcopy(d->szEntryPoint, entry, sizeof entry);
        d->dwContext = shared.self16;
        return 1;
    }
    case RDD_NEWCALLBACKFNS: {
        FARP f;
        RDD_DDRAWFNS __far *fns;
        f.d = cmd->dwParam1;
        fns = (RDD_DDRAWFNS __far *)f.p;
        if (!fns)
            return 0;
        shared.setinfo16 = fns->lpSetInfo;
        return 1;
    }
    case RDD_CREATEDRIVEROBJECT:
        if (!shared.setinfo16 || !shared.dll_ready || !shared.lfb) {
            dbg("r9x: DirectDraw asked too early (no SetInfo, DLL or mode)\r\n");
            return 0;
        }
        fill_halinfo();
        if (!call_setinfo(0)) {
            dbg("r9x: DirectDraw refused the HAL description\r\n");
            return 0;
        }
        if (out)
            *(DWORD __far *)out = shared.hal.hInstance;     /* our data segment, as hInstance */
        dbg("r9x: DirectDraw HAL registered\r\n");
        return 1;
    }
    return R9X_NOT_OURS;
}
