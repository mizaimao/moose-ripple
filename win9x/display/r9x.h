/*
 * moose-ripple display driver for Windows 9x (16-bit minidriver on Microsoft's DIB engine).
 *
 * The structures and constants below are the documented driver interface of Windows 95/98
 * (the Windows 98 DDK's "Display Drivers" chapters), written out here in our own words so the
 * driver needs none of the DDK's headers. Only the DDK's import libraries are linked.
 *
 * Built as a small-model 16-bit DLL with no C run-time: DS is our data segment only inside
 * functions marked DDI (or after the assembly thunks have loaded it), and SS is the caller's.
 */
#ifndef R9X_H
#define R9X_H

typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef unsigned long  DWORD;
typedef short          SHORT;
typedef long           LONG;
typedef void __far    *LPVOID;

#define DDI   __far __pascal __loadds      /* entry point called by GDI or USER */
#define NULL  0

/* ---- GDIINFO: what Enable returns when asked for the device's description -------------- */
typedef struct { SHORT x, y; } PTPAIR;
typedef struct {
    SHORT dpVersion, dpTechnology, dpHorzSize, dpVertSize, dpHorzRes, dpVertRes;
    SHORT dpBitsPixel, dpPlanes, dpNumBrushes, dpNumPens, dpCapsFE, dpNumFonts;
    SHORT dpNumColors, dpDEVICEsize;                /* from here to dpStyleLen: the DIB engine */
    WORD  dpCurves, dpLines, dpPolygonals, dpText, dpClip, dpRaster;
    SHORT dpAspectX, dpAspectY, dpAspectXY, dpStyleLen;
    PTPAIR dpMLoWin, dpMLoVpt, dpMHiWin, dpMHiVpt, dpELoWin, dpELoVpt;
    PTPAIR dpEHiWin, dpEHiVpt, dpTwpWin, dpTwpVpt;
    SHORT dpLogPixelsX, dpLogPixelsY, dpDCManage;
    WORD  dpCaps1;
    SHORT dpSpotSizeX[2], dpSpotSizeY[2];
    WORD  dpNumPalReg, dpPalReserved, dpColorRes;
} GDIINFO;                                          /* 110 bytes */

#define DT_RASDISPLAY   1
#define DC_IgnoreDFNP   0x0004
#define RC_SAVEBITMAP   0x0040
#define RC_PALETTE      0x0100
#define RC_DIBTODEV     0x0200
#define C1_REINIT_ABLE  0x0080
#define C1_BYTE_PACKED  0x0400
#define C1_COLORCURSOR  0x0800
#define C1_SLOW_CARD    0x2000

/* ---- DIBENGINE: the start of our PDEVICE, in the DIB engine's format ---------------------- */
typedef struct {
    WORD  deType, deWidth, deHeight, deWidthBytes;
    BYTE  dePlanes, deBitsPixel;
    DWORD deReserved1;
    LONG  deDeltaScan;
    LPVOID delpPDevice;
    DWORD deBitsOffset;
    WORD  deBitsSelector;
    WORD  deFlags;
    WORD  deVersion;
    LPVOID deBitmapInfo;
    LPVOID deBeginAccess, deEndAccess;
    DWORD deDriverReserved;
} DIBENGINE;                                        /* 48 bytes */

#define MINIDRIVER   0x0001
#define PALETTIZED   0x0002
#define BUSY         0x0010
#define FIVE6FIVE    0x0040
#define VRAM         0x8000

typedef struct {
    DWORD biSize;
    LONG  biWidth, biHeight;
    WORD  biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG  biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BMIHEADER;                                        /* 40 bytes */
#define BI_BITFIELDS 3

/* one colour of the device's colour table: blue, green, red, flags */
#define NONSTATIC   0x80
#define MAPTOWHITE  0x01

/* ---- The VDD (virtual display device) as seen from the driver ----------------------------- */
typedef struct {
    WORD  diHdrSize, diInfoFlags;
    DWORD diDevNodeHandle;
    char  diDriverName[16];
    WORD  diXRes, diYRes, diDPI;
    BYTE  diPlanes, diBpp;
    WORD  diRefreshRateMax, diRefreshRateMin, diLowHorz, diHighHorz, diLowVert, diHighVert;
    DWORD diMonitorDevNodeHandle;
    BYTE  diHorzSyncPolarity, diVertSyncPolarity;
    DWORD diUnitNumber, diDisplayFlags, diXDesktopPos, diYDesktopPos;
    DWORD diXDesktopSize, diYDesktopSize;
} DISPLAYINFO;

#define VDD_DRIVER_REGISTER     0x80
#define VDD_DRIVER_UNREGISTER   0x81
#define VDD_SAVE_DRIVER_STATE   0x82
#define VDD_REGISTER_DISPLAY_DRIVER_INFO 0x83   /* passed on to the mini-VDD */
#define VDD_GET_DISPLAY_CONFIG  0x85
#define VDD_PRE_MODE_CHANGE     0x86
#define VDD_POST_MODE_CHANGE    0x87

#define STOP_IO_TRAP   0x4000       /* int 2Fh: the VDD stops trapping the VGA ports for us */
#define START_IO_TRAP  0x4007

/* Our mini-VDD (win9x/minivdd): told through VDD_REGISTER_DISPLAY_DRIVER_INFO with ECX 1 when
   the card is in the driver's mode; it answers "RVD1" in EAX. */
#define RVD_HIRES_ON   1UL
#define RVD_SIGNATURE  0x31445652UL
#define RVD_SCANOUT_QUERY  0x53434F55UL
#define RVD_SCANOUT_REPLY  0x32445652UL
#define RVD_DISPLAY_MODE   0x52564400UL
#define RVD_DISPLAY_COLOR  0x52565000UL

/* ---- Escapes -------------------------------------------------------------------------------- */
#define QUERYESCSUPPORT 8
#define R9X_NOT_OURS    ((int)0x8000) /* control_escape: let the DIB engine answer (not a DCI code) */

/* ---- ValidateMode --------------------------------------------------------------------------- */
typedef struct { WORD dvmSize, dvmBpp; SHORT dvmXRes, dvmYRes; } DISPVALMODE;
#define VALMODE_YES         0
#define VALMODE_NO_WRONGDRV 1
#define VALMODE_NO_NOMEM    2
#define VALMODE_NO_NODAC    3

/* ---- Imports: the DIB engine (DIBENG.DLL) and KERNEL -------------------------------------- */
extern WORD  __far __pascal DIB_Enable(LPVOID, WORD, LPVOID, LPVOID, LPVOID);
extern short __far __pascal DIB_Control(LPVOID pdev, WORD function, LPVOID in, LPVOID out);
extern WORD  __far __pascal DIB_Disable(LPVOID);
extern void  __far __pascal DIB_BeginAccess(LPVOID, SHORT, SHORT, SHORT, SHORT, WORD);
extern void  __far __pascal DIB_EndAccess(LPVOID, WORD);
extern WORD  __far __pascal DIB_SetPaletteExt(WORD start, WORD count, LPVOID colors, LPVOID pdev);
extern WORD  __far __pascal DIB_SetPaletteTranslateExt(LPVOID xlat, LPVOID pdev);
extern WORD  __far __pascal AllocSelector(WORD);
extern WORD  __far __pascal FreeSelector(WORD);
extern WORD  __far __pascal SetSelectorBase(WORD sel, DWORD base);
extern WORD  __far __pascal SetSelectorLimit(WORD sel, DWORD limit);
extern WORD  __far __pascal GetModuleHandle(const char __far *name);
extern LPVOID __far __pascal GetProcAddress(WORD module, LPVOID name);

/* ---- Our assembly helpers (asm.asm): near, C calling convention --------------------------- */
extern WORD  __cdecl inpw_(WORD port);
extern void  __cdecl outpw_(WORD port, WORD value);
extern BYTE  __cdecl inp_(WORD port);
extern void  __cdecl outp_(WORD port, BYTE value);
extern DWORD __cdecl pci_read(WORD busdevfn, WORD reg);          /* configuration mechanism 1 */
extern DWORD __cdecl dpmi_map(DWORD phys, DWORD size);           /* linear address, 0 on failure */
extern void  __cdecl vdd_init(void);                              /* find the VDD's entry point */
extern DWORD __cdecl vdd_call(DWORD func, DWORD ecx, DWORD edx, WORD es, WORD di);
extern void  __cdecl int2f(WORD ax);
extern void  __cdecl bios_textmode(void);                         /* int 10h, mode 3 */
extern void  __cdecl hook_int2f(void);                            /* screen switch notifications */
extern void  __cdecl unhook_int2f(void);
extern WORD  __cdecl muldiv_(WORD a, WORD b, WORD c);             /* a * b / c, rounded */
extern DWORD __cdecl mul32(WORD a, WORD b);
extern DWORD __cdecl div32(DWORD a, WORD b);                     /* a / b */
extern WORD  __cdecl dib_create(LPVOID lpInfo, LPVOID lpDevice, LPVOID lpBits, WORD flags);
extern DWORD __cdecl sel_limit(WORD sel);
extern void  __cdecl fb_fill(WORD sel, DWORD offset, DWORD count, BYTE value);
extern void  __cdecl dbg_putc(BYTE c);                            /* QEMU's debug port 0xE9 */
extern void  __far __pascal ResetHiResMode(void);                 /* the VDD calls this */

/* ---- The driver's own state (enable.c) ------------------------------------------------------ */
extern LPVOID lpDriverPDevice;      /* the PDEVICE GDI gave us; the thunks pass it to the engine */
extern WORD   wPalettized;
extern WORD   wEnabled;

void dbg(const char __near *s);
void dbg_hex(DWORD v);

/* DirectDraw (ddraw16.c) */
int  dd_escape(WORD function, LPVOID in, LPVOID out);
void dd_mode_set(DWORD lfb, DWORD vram, WORD width, WORD height, WORD bpp, WORD pitch);
void dd_mode_lost(void);

#endif
