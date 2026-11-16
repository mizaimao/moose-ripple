/*
 * moose-ripple display driver for Windows 9x: enabling, modes, palette.
 *
 * Stage one of the driver: 2D only. QEMU's virtio-vga starts out as a VGA card with the
 * Bochs/QEMU "VBE" mode-setting registers (I/O ports 01CEh/01CFh) and a linear frame buffer in
 * its first PCI memory range. This driver sets a mode there, hands the frame buffer to
 * Microsoft's DIB engine, and lets the engine do all drawing in software. The virtio side of
 * the card is not touched yet.
 */
#include "r9x.h"

LPVOID lpDriverPDevice;             /* also read by the thunks in asm.asm */
WORD   wPalettized;
WORD   wEnabled;

static WORD  wXRes = 640, wYRes = 480, wBpp = 8, wDpi = 96;
static WORD  wEngineSize;           /* bytes of the PDEVICE that belong to the DIB engine */
static WORD  wScreenSel;            /* selector covering the frame buffer */
static DWORD dwVramSize;
static DWORD dwLinear;              /* flat address of the frame buffer */
static BYTE  bReEnable;
static BYTE  bRepaintOff, bRepaintPending;
static void (__far __pascal *lpRepaint)(void);  /* USER's "repaint the whole screen" */
static BYTE __far *lpColorTable;    /* the screen's colour table inside the PDEVICE (8 bpp) */
static DISPLAYINFO dispInfo;
static GDIINFO     engineInfo;      /* scratch: asking the engine for its PDEVICE size */

typedef union { LPVOID p; DWORD d; struct { WORD lo, hi; } w; } FARP;

/* With TRACE defined, a letter per driver call goes to the debug port (see asm.asm). */
#ifdef TRACE
#define TRACEC(c) dbg_putc(c)
#else
#define TRACEC(c)
#endif

/* ---- Debug output: text to QEMU's debug port ------------------------------------------- */
void dbg(const char __near *s)
{
    while (*s)
        dbg_putc((BYTE)*s++);
}

void dbg_hex(DWORD v)
{
    static const char __near digits[] = "0123456789abcdef";
    BYTE __far *b = (BYTE __far *)&v;
    SHORT i;
    for (i = 3; i >= 0; i--) {
        dbg_putc((BYTE)digits[b[i] >> 4]);
        dbg_putc((BYTE)digits[b[i] & 15]);
    }
}

/* ---- The card: Bochs/QEMU VBE registers ---------------------------------------------- */
#define VBE_INDEX       0x01CE
#define VBE_DATA        0x01CF
#define VBE_ID          0
#define VBE_XRES        1
#define VBE_YRES        2
#define VBE_BPP         3
#define VBE_ENABLE      4
#define VBE_VIRT_WIDTH  6
#define VBE_VIRT_HEIGHT 7
#define VBE_X_OFFSET    8
#define VBE_Y_OFFSET    9
#define VBE_VRAM_64K    10
#define VBE_ENABLED     0x01
#define VBE_LFB         0x40

static void vbe_write(WORD index, WORD value)
{
    outpw_(VBE_INDEX, index);
    outpw_(VBE_DATA, value);
}

static WORD vbe_read(WORD index)
{
    outpw_(VBE_INDEX, index);
    return inpw_(VBE_DATA);
}

static WORD stride(void)
{
    return wXRes * ((wBpp + 7) / 8);
}

/* Find the card on PCI bus 0 and make a selector for its frame buffer. */
static WORD map_card(void)
{
    WORD  devfn, sel;
    DWORD id, base = 0, linear;
    FARP  size, cls, ours;

    if ((vbe_read(VBE_ID) & 0xFFF0) != 0xB0C0) {
        dbg("r9x: no VBE registers\r\n");
        return 0;
    }
    for (devfn = 0; devfn < 0x100; devfn++) {
        id = pci_read(devfn, 0x00);
        /* virtio-vga (1AF4:1050) or QEMU's standard VGA (1234:1111), display class */
        cls.d = pci_read(devfn, 0x08);
        if ((id == 0x10501AF4UL || id == 0x11111234UL) && cls.w.hi == 0x0300) {
            base = pci_read(devfn, 0x10) & 0xFFFFFFF0UL;
            break;
        }
    }
    if (!base) {
        dbg("r9x: card not found on PCI bus 0\r\n");
        return 0;
    }
    size.w.lo = 0;
    size.w.hi = vbe_read(VBE_VRAM_64K);             /* video memory in 64 KB units */
    dwVramSize = size.d;
    linear = dpmi_map(base, dwVramSize);
    if (!linear) {
        dbg("r9x: can't map the frame buffer\r\n");
        return 0;
    }
    dwLinear = linear;
    /* A copy of our data segment's descriptor (a plain read-write data segment), then moved
       onto the frame buffer. AllocSelector(0) gives an empty descriptor, which is not usable. */
    ours.p = (LPVOID)&dispInfo;
    sel = AllocSelector(ours.w.hi);
    if (!sel)
        return 0;
    SetSelectorBase(sel, linear);
    SetSelectorLimit(sel, dwVramSize - 1);
    if (sel_limit(sel) != dwVramSize - 1) {
        dbg("r9x: frame buffer selector has limit "); dbg_hex(sel_limit(sel)); dbg("\r\n");
        FreeSelector(sel);
        return 0;
    }
    wScreenSel = sel;
    dbg("r9x: frame buffer at "); dbg_hex(base);
    dbg(", bytes "); dbg_hex(dwVramSize); dbg("\r\n");
    return 1;
}

static void set_mode(void)
{
    vbe_write(VBE_ENABLE, 0);
    vbe_write(VBE_XRES, wXRes);
    vbe_write(VBE_YRES, wYRes);
    vbe_write(VBE_BPP, wBpp);
    vbe_write(VBE_VIRT_WIDTH, wXRes);
    /* All of video memory as one tall virtual screen: DirectDraw's back buffers lie below the
       visible part, and a flip moves the start of the display down to one (VBE_Y_OFFSET). */
    {
        DWORD lines = div32(dwVramSize, stride());
        vbe_write(VBE_VIRT_HEIGHT, (WORD)(lines > 0xFFFF ? 0xFFFF : lines));
    }
    vbe_write(VBE_X_OFFSET, 0);
    vbe_write(VBE_Y_OFFSET, 0);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB);   /* this also clears the screen */
}

/* The VGA palette registers take 6 bits a colour; the table holds blue, green, red, flags. */
static void set_dac(WORD start, WORD count, BYTE __far *colors)
{
    outp_(0x3C8, (BYTE)start);
    while (count--) {
        outp_(0x3C9, (BYTE)(colors[2] >> 2));
        outp_(0x3C9, (BYTE)(colors[1] >> 2));
        outp_(0x3C9, (BYTE)(colors[0] >> 2));
        colors += 4;
    }
}

static WORD mode_fits(WORD x, WORD y, WORD bpp)
{
    WORD bytes = (bpp + 7) / 8;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return 0;
    if (x < 320 || y < 200 || x > 4096 || y > 4096)
        return 0;
    if ((x * bytes) & 3)                    /* the DIB engine's lines are 4-byte aligned */
        return 0;
    return dwVramSize == 0 || mul32(x * bytes, y) <= dwVramSize;
}

/* ---- The VDD -------------------------------------------------------------------------------- */
static void vdd_register(void)
{
    FARP  reset;
    DWORD r;

    reset.p = (LPVOID)ResetHiResMode;
    /* ECX: bytes of the visible screen; EDX 0: the VDD may virtualise the card for DOS boxes.
       The answer is the memory the screen and the VDD use; the function code back means no VDD
       took the call. */
    r = vdd_call(VDD_DRIVER_REGISTER, mul32(stride(), wYRes), 0, reset.w.hi, reset.w.lo);
    dbg("r9x: VDD register "); dbg_hex(r); dbg("\r\n");
}

/* Tell the mini-VDD that the card is in our mode now: from here on it keeps other VMs and the
   video BIOS away from the mode-setting ports. Without it (not installed) the driver works, but
   a DOS session in a window can switch the card to text mode. */
static void minivdd_hires(void)
{
    static BYTE said;
    DWORD r = vdd_call(VDD_REGISTER_DISPLAY_DRIVER_INFO, RVD_HIRES_ON, 0, 0, 0);

    if (!said) {
        said = 1;
        dbg(r == RVD_SIGNATURE ? "r9x: mini-VDD present\r\n" : "r9x: no mini-VDD\r\n");
    }
}

/* What the user chose in Display Properties, as the VDD read it from the registry. */
static void read_settings(void)
{
    FARP  buf;
    DWORD r;

    buf.p = (LPVOID)&dispInfo;
    r = vdd_call(VDD_GET_DISPLAY_CONFIG, sizeof(DISPLAYINFO), 0, buf.w.hi, buf.w.lo);
    if (r == VDD_GET_DISPLAY_CONFIG || r == 0xFFFFFFFFUL) {
        dbg("r9x: no display settings from the VDD, keeping "); /* 640x480, 8 bpp at first */
    } else {
        if (dispInfo.diXRes && dispInfo.diYRes) {
            wXRes = dispInfo.diXRes;
            wYRes = dispInfo.diYRes;
        }
        if (dispInfo.diBpp)
            wBpp = dispInfo.diBpp;
        if (dispInfo.diDPI)
            wDpi = dispInfo.diDPI;
        dbg("r9x: settings ");
    }
    dbg_hex(wXRes); dbg(" x "); dbg_hex(wYRes); dbg(", bpp "); dbg_hex(wBpp);
    dbg(", VDD flags "); dbg_hex(dispInfo.diInfoFlags); dbg("\r\n");
}

/* ---- Initialisation ------------------------------------------------------------------------ */
void __cdecl r9x_init(void)                 /* from LibEntry */
{
    dbg("r9x: moose-ripple display driver loaded\r\n");
    vdd_init();
    read_settings();
}

static WORD hw_enable(void)
{
    if (!wScreenSel && !map_card())
        return 0;
    if (!mode_fits(wXRes, wYRes, wBpp)) {
        dbg("r9x: mode doesn't fit, using 640x480 8 bpp\r\n");
        wXRes = 640; wYRes = 480; wBpp = 8;
    }
    vdd_call(VDD_PRE_MODE_CHANGE, 0, 0, 0, 0);
    set_mode();
    vdd_register();
    minivdd_hires();
    vdd_call(VDD_POST_MODE_CHANGE, 0, 0, 0, 0);
    vdd_call(VDD_SAVE_DRIVER_STATE, 0, 0, 0, 0);
    return 1;
}

/* Back from a full-screen DOS session (via ResetHiResMode in asm.asm, DS already ours). */
void __cdecl r9x_reset(void)
{
    DIBENGINE __far *pd = (DIBENGINE __far *)lpDriverPDevice;

    dbg(wEnabled ? "r9x: the VDD asks for our mode again\r\n" :
                   "r9x: the VDD asks for our mode again, but the driver is disabled\r\n");
    if (!wEnabled)
        return;
    set_mode();
    if (wBpp == 8 && lpColorTable)
        set_dac(0, 256, lpColorTable);
    pd->deFlags &= ~BUSY;
    vdd_register();
    minivdd_hires();
    vdd_call(VDD_SAVE_DRIVER_STATE, 0, 0, 0, 0);
}

static void repaint(void)
{
    if (!lpRepaint)
        return;
    if (bRepaintOff)
        bRepaintPending = 1;
    else
        lpRepaint();
}

/* The VDD announces a screen switch (int 2Fh, via Int2FHook in asm.asm, DS already ours):
   4001h, something else takes the screen: stop drawing; 4002h, the screen is ours again: the
   card may be in any mode by now, so set ours and have USER draw everything again. */
void __cdecl r9x_switch(WORD code)
{
    DIBENGINE __far *pd = (DIBENGINE __far *)lpDriverPDevice;

    dbg(code == 0x4001 ? "r9x: switch out\r\n" : "r9x: switch in\r\n");
    if (!wEnabled)
        return;
    if (code == 0x4001) {
        pd->deFlags |= BUSY;
        return;
    }
    if (pd->deFlags & BUSY)
        r9x_reset();
    repaint();
}

/* ---- The screen's colour table at 8 bits a pixel ------------------------------------------ */
/* The 20 colours Windows keeps fixed in a 256-colour palette: 10 at the bottom, 10 at the top.
   Blue, green, red, flags. */
static const BYTE __near staticLow[10][4] = {
    {0x00,0x00,0x00,0}, {0x00,0x00,0x80,0}, {0x00,0x80,0x00,0}, {0x00,0x80,0x80,0},
    {0x80,0x00,0x00,0}, {0x80,0x00,0x80,0}, {0x80,0x80,0x00,0}, {0xC0,0xC0,0xC0,MAPTOWHITE},
    {0xC0,0xDC,0xC0,NONSTATIC|MAPTOWHITE}, {0xF0,0xCA,0xA6,NONSTATIC|MAPTOWHITE}
};
static const BYTE __near staticHigh[10][4] = {
    {0xF0,0xFB,0xFF,NONSTATIC|MAPTOWHITE}, {0xA4,0xA0,0xA0,NONSTATIC|MAPTOWHITE},
    {0x80,0x80,0x80,MAPTOWHITE}, {0x00,0x00,0xFF,0}, {0x00,0xFF,0x00,MAPTOWHITE},
    {0x00,0xFF,0xFF,MAPTOWHITE}, {0xFF,0x00,0x00,0}, {0xFF,0x00,0xFF,0},
    {0xFF,0xFF,0x00,MAPTOWHITE}, {0xFF,0xFF,0xFF,MAPTOWHITE}
};

static void default_colors(BYTE __far *t)
{
    WORD i, j;
    for (i = 0; i < 10; i++)
        for (j = 0; j < 4; j++)
            t[i * 4 + j] = staticLow[i][j];
    for (i = 10; i < 246; i++) {
        t[i * 4] = t[i * 4 + 1] = t[i * 4 + 2] = 0;
        t[i * 4 + 3] = NONSTATIC;
    }
    for (i = 0; i < 10; i++)
        for (j = 0; j < 4; j++)
            t[(246 + i) * 4 + j] = staticHigh[i][j];
}

/* Bytes that follow the engine's part of the PDEVICE: the screen's bitmap description. */
static WORD info_size(void)
{
    if (wBpp == 8)
        return sizeof(BMIHEADER) + 256 * 4;
    if (wBpp == 16)
        return sizeof(BMIHEADER) + 3 * 4;       /* three colour masks */
    return sizeof(BMIHEADER);
}

/* ---- Enable: GDI asks for the device's description, or for the device itself ----------- */
WORD DDI Enable(LPVOID lpDevice, WORD style, LPVOID lpDeviceType, LPVOID lpOutputFile, LPVOID lpStuff)
{
    TRACEC((BYTE)(style & 1 ? 'N' : 'n'));
    if (style & 1) {
        GDIINFO __far *g = (GDIINFO __far *)lpDevice;

        DIB_Enable(lpDevice, style, lpDeviceType, lpOutputFile, lpStuff);
        wEngineSize = g->dpDEVICEsize;

        g->dpVersion    = 0x0400;
        g->dpTechnology = DT_RASDISPLAY;
        g->dpHorzSize   = 208;                  /* millimetres: a 4:3 screen, as VGA drivers say */
        g->dpVertSize   = 156;
        g->dpHorzRes    = wXRes;
        g->dpVertRes    = wYRes;
        g->dpBitsPixel  = wBpp;
        g->dpPlanes     = 1;
        g->dpNumBrushes = -1;
        g->dpCapsFE     = 0;
        g->dpNumFonts   = 0;
        g->dpDEVICEsize = wEngineSize + info_size();

        /* Mapping modes: window extents in the mode's unit, viewport extents in pixels. */
        g->dpMLoWin.x = 2080;   g->dpMLoWin.y = 1560;           /* 0.1 mm */
        g->dpMLoVpt.x = wXRes;  g->dpMLoVpt.y = -(SHORT)wYRes;
        g->dpMHiWin.x = 20800;  g->dpMHiWin.y = 15600;          /* 0.01 mm */
        g->dpMHiVpt.x = wXRes;  g->dpMHiVpt.y = -(SHORT)wYRes;
        g->dpELoWin.x = 325;    g->dpELoWin.y = 325;            /* 0.01 inch */
        g->dpELoVpt.x = muldiv_(wXRes, 254, 640);
        g->dpELoVpt.y = -(SHORT)muldiv_(wYRes, 254, 480);
        g->dpEHiWin.x = 1625;   g->dpEHiWin.y = 1625;           /* 0.001 inch */
        g->dpEHiVpt.x = muldiv_(wXRes, 127, 640);
        g->dpEHiVpt.y = -(SHORT)muldiv_(wYRes, 127, 480);
        g->dpTwpWin.x = 2340;   g->dpTwpWin.y = 2340;           /* twips */
        g->dpTwpVpt.x = muldiv_(wXRes, 127, 640);
        g->dpTwpVpt.y = -(SHORT)muldiv_(wYRes, 127, 480);

        g->dpLogPixelsX = wDpi;
        g->dpLogPixelsY = wDpi;
        g->dpDCManage   = DC_IgnoreDFNP;
        /* no hardware drawing yet: the engine does it all (C1_SLOW_CARD) */
        g->dpCaps1 |= C1_REINIT_ABLE | C1_BYTE_PACKED | C1_COLORCURSOR | C1_SLOW_CARD;

        if (wBpp == 8) {
            g->dpNumPens     = 16;
            g->dpNumColors   = 20;
            g->dpNumPalReg   = 256;
            g->dpPalReserved = 20;
            g->dpColorRes    = 18;
            g->dpRaster |= RC_PALETTE | RC_DIBTODEV | RC_SAVEBITMAP;
        } else {
            g->dpNumPens     = -1;
            g->dpNumColors   = -1;
            g->dpNumPalReg   = 0;
            g->dpPalReserved = 0;
            g->dpColorRes    = 0;
            g->dpRaster |= RC_DIBTODEV;
        }
        return sizeof(GDIINFO);
    }

    /* Enable the device: set the mode and build the PDEVICE in the buffer GDI allocated. */
    {
        DIBENGINE __far *pd = (DIBENGINE __far *)lpDevice;
        BMIHEADER __far *bi;
        FARP  bits;
        WORD  flags = MINIDRIVER | VRAM;

        lpDriverPDevice = lpDevice;
        if (!wEngineSize) {                     /* GDI asked for the device first */
            DIB_Enable((LPVOID)&engineInfo, 1, lpDeviceType, lpOutputFile, lpStuff);
            wEngineSize = engineInfo.dpDEVICEsize;
        }
        if (!hw_enable())
            return 0;
        if (!bReEnable)
            int2f(STOP_IO_TRAP);

        wPalettized = (wBpp == 8);
        DIB_Enable(lpDevice, style, lpDeviceType, lpOutputFile, lpStuff);
        if (wPalettized)
            DIB_SetPaletteTranslateExt(NULL, lpDevice);

        bi = (BMIHEADER __far *)((BYTE __far *)lpDevice + wEngineSize);
        bi->biSize        = sizeof(BMIHEADER);
        bi->biWidth       = wXRes;
        bi->biHeight      = wYRes;
        bi->biPlanes      = 1;
        bi->biBitCount    = wBpp;
        bi->biCompression = 0;
        bi->biSizeImage   = 0;
        bi->biXPelsPerMeter = 0;
        bi->biYPelsPerMeter = 0;
        bi->biClrUsed     = 0;
        bi->biClrImportant = 0;
        lpColorTable = NULL;
        if (wBpp == 8) {
            flags |= PALETTIZED;
            lpColorTable = (BYTE __far *)(bi + 1);
            if (!bReEnable)
                default_colors(lpColorTable);
        } else if (wBpp == 16) {                /* the card's 16-bit format is 5-6-5 */
            DWORD __far *masks = (DWORD __far *)(bi + 1);
            bi->biCompression = BI_BITFIELDS;
            masks[0] = 0xF800; masks[1] = 0x07E0; masks[2] = 0x001F;
            flags |= FIVE6FIVE;
        }

        bits.w.lo = 0;
        bits.w.hi = wScreenSel;
        if (!dib_create((LPVOID)bi, lpDevice, bits.p, flags)) {
            dbg("r9x: CreateDIBPDevice failed\r\n");
            return 0;
        }
        pd->deBeginAccess = (LPVOID)DIB_BeginAccess;
        pd->deEndAccess   = (LPVOID)DIB_EndAccess;

        if (wBpp == 8)
            set_dac(0, 256, lpColorTable);
        if (!bReEnable) {
            static const char __near user[] = "USER";
            FARP ordinal;
            ordinal.d = 275;                    /* USER's repaint entry, by number */
            lpRepaint = (void (__far __pascal *)(void))
                GetProcAddress(GetModuleHandle((const char __far *)user), ordinal.p);
            hook_int2f();
        }
        wEnabled = 1;
        dd_mode_set(dwLinear, dwVramSize, wXRes, wYRes, wBpp, stride());
        dbg("r9x: enabled\r\n");
        return 1;
    }
}

/* A new resolution or colour depth without restarting Windows. */
WORD DDI ReEnable(LPVOID lpPDevice, LPVOID lpGDIInfo)
{
    WORD ox = wXRes, oy = wYRes, ob = wBpp, ok;

    read_settings();
    bReEnable = 1;
    DIB_BeginAccess(lpPDevice, 0, 0, ox - 1, oy - 1, 8);    /* 8: keep the pointer away */
    ok = Enable(lpPDevice, 0, NULL, NULL, NULL);
    DIB_EndAccess(lpPDevice, 8);
    if (!ok) {
        wXRes = ox; wYRes = oy; wBpp = ob;
        Enable(lpPDevice, 0, NULL, NULL, NULL);
    } else {
        Enable(lpGDIInfo, 1, NULL, NULL, NULL);
    }
    bReEnable = 0;
    return ok;
}

WORD DDI Disable(LPVOID lpDevice)
{
    DIBENGINE __far *pd = (DIBENGINE __far *)lpDevice;

    /* In the order of the DDK's sample drivers: the VDD traps the VGA ports again before it is
       told, and our int 2Fh handler stays in place until the end. */
    dbg("r9x: Disable\r\n");
    wEnabled = 0;
    pd->deFlags |= BUSY;
    int2f(START_IO_TRAP);
    dbg("r9x: Disable: I/O trapping on\r\n");
    vdd_call(VDD_DRIVER_UNREGISTER, 0, 0, 0, 0);
    dbg("r9x: Disable: unregistered from the VDD\r\n");
    vbe_write(VBE_ENABLE, 0);
    bios_textmode();
    unhook_int2f();
    dbg("r9x: disabled\r\n");
    return 0xFFFF;
}

/* GDI's palette manager changes colours: the engine updates the table, we tell the card. */
DWORD DDI SetPalette(WORD start, WORD count, LPVOID lpPalette)
{
    DIBENGINE __far *pd = (DIBENGINE __far *)lpDriverPDevice;

    TRACEC('p');
    DIB_SetPaletteExt(start, count, lpPalette, lpDriverPDevice);
    if (lpColorTable && !(pd->deFlags & BUSY))
        set_dac(start, count, lpColorTable + start * 4);
    return 0;
}

/* Display Properties asks whether a mode would work, possibly before this driver is in use. */
static WORD validate(DISPVALMODE __far *m);

/* The export ValidateMode is in asm.asm: it returns this in all of EAX. */
/* ---- Escapes (GDI's Control) -----------------------------------------------------------------
   OpenGL: Windows' opengl32.dll asks the display driver for the name of its installable client
   driver (ICD) with the escape OPENGL_GETINFO, after asking QUERYESCSUPPORT whether the driver
   knows that escape. The name is looked up under
   HKLM\Software\Microsoft\Windows\CurrentVersion\OpenGLDrivers, where the value of that name is
   the DLL to load (the INF writes it). The numbers in front of the name are the ICD interface
   version (2: OpenGL 1.1) and the driver's own version, which goes to its DrvValidateVersion.
   Returns R9X_NOT_OURS for escapes that are not ours: asm.asm passes those to the DIB engine. */
#define OPENGL_GETINFO  0x1101

int __cdecl control_escape(WORD function, LPVOID in, LPVOID out)
{
    static const char name[] = "RIPPLEGL";
    int r = dd_escape(function, in, out);  /* DirectDraw's, ddraw16.c */

    if (r != R9X_NOT_OURS)
        return r;

    if (function == QUERYESCSUPPORT && in && *(WORD __far *)in == OPENGL_GETINFO)
        return 1;
    if (function == OPENGL_GETINFO && out) {
        DWORD __far *o = (DWORD __far *)out;
        char __far *n = (char __far *)(o + 2);
        WORD i;
        o[0] = 2;
        o[1] = 1;
        for (i = 0; i < sizeof name; i++)
            n[i] = name[i];
        dbg("r9x: OpenGL driver name asked for\r\n");
        return 1;
    }
    return R9X_NOT_OURS;
}

WORD __cdecl validate_mode(DISPVALMODE __far *m)
{
    WORD r = validate(m);
#ifdef TRACE
    dbg("\r\nr9x: validate "); dbg_hex(m->dvmXRes); dbg(" "); dbg_hex(m->dvmYRes); dbg(" ");
    dbg_hex(m->dvmBpp); dbg(" -> "); dbg_hex(r); dbg("\r\n");
#endif
    return r;
}

static WORD validate(DISPVALMODE __far *m)
{
    if (!wScreenSel) {
        /* not enabled yet: answer from the card's memory size alone */
        if ((vbe_read(VBE_ID) & 0xFFF0) != 0xB0C0)
            return VALMODE_NO_WRONGDRV;
        if (!dwVramSize) {
            FARP size;
            size.w.lo = 0;
            size.w.hi = vbe_read(VBE_VRAM_64K);
            dwVramSize = size.d;
        }
    }
    if (m->dvmBpp != 8 && m->dvmBpp != 16 && m->dvmBpp != 24 && m->dvmBpp != 32)
        return VALMODE_NO_NODAC;
    if (!mode_fits(m->dvmXRes, m->dvmYRes, m->dvmBpp))
        return VALMODE_NO_NOMEM;
    return VALMODE_YES;
}

/* USER loads the driver's resources by number; the fonts differ at 120 dpi. */
DWORD DDI GetDriverResourceID(WORD id, LPVOID lpType)
{
    (void)lpType;
    TRACEC('G'); TRACEC((BYTE)('0' + (id & 7)));
    if (id == 3 && wDpi != 96)
        id = 2003;
    return id;
}

void DDI UserRepaintDisable(WORD off)
{
    TRACEC('u');
    bRepaintOff = (BYTE)off;
    if (!off && bRepaintPending) {
        bRepaintPending = 0;
        repaint();
    }
}
