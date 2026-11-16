/*
 * DirectDraw for the moose-ripple display driver: the block of memory the 16-bit driver
 * (ripple9x.drv) and the 32-bit HAL DLL (rippledd.dll) share.
 *
 * Windows 98's DirectDraw finds a display driver's hardware abstraction layer (HAL) through
 * the driver's DCICOMMAND escape (docs/win9x/directdraw.md has the sequence). The 16-bit driver
 * names the 32-bit DLL and hands it the 16:16 address of this block; the DLL fills in what is
 * 32-bit (its callbacks, capabilities, Direct3D), the 16-bit driver what changes with the
 * screen mode (frame buffer, pitch, pixel format, the free video memory), and then passes the
 * HAL description in it to DirectDraw.
 *
 * Compiled by both sides: the 16-bit C compiler (r9x.h types) and Visual C++ 6. Every pointer
 * is a DWORD here, 16:16 or flat as noted, so the layout is the same in both. `halinfo` has the
 * layout of DirectDraw's DDHALINFO (460 bytes, DirectX 7); the DLL's build checks the offsets
 * against the DDK's ddrawi.h.
 */
#ifndef RDD_H
#define RDD_H

typedef unsigned long  rdd_u32;
typedef unsigned short rdd_u16;

#define RDD_MAGIC       0x31444452UL    /* "RDD1" */
#define RDD_DLL_NAME    "rippledd.dll"
#define RDD_DLL_ENTRY   "DriverInit"
#define RDD_MAX_MODES   48

/* The escape and its sub-commands (lpInput points to a DCICMD). */
#define RDD_DCICOMMAND          3075
#define RDD_DD_VERSION          0x200   /* DCICMD.dwVersion */
#define RDD_DD_HAL_VERSION      0x100   /* QUERYESCSUPPORT's answer for DCICOMMAND */
#define RDD_CREATEDRIVEROBJECT  10
#define RDD_GET32BITDRIVERNAME  11
#define RDD_NEWCALLBACKFNS      12
#define RDD_VERSIONINFO         13

typedef struct { rdd_u32 dwCommand, dwParam1, dwParam2, dwVersion, dwReserved; } RDD_DCICMD;
typedef struct { rdd_u32 dwHALVersion, dwReserved1, dwReserved2; } RDD_VERSIONDATA;
typedef struct { char szName[260]; char szEntryPoint[64]; rdd_u32 dwContext; } RDD_DRIVERDATA32;
typedef struct { rdd_u32 dwSize, lpSetInfo, lpVidMemAlloc, lpVidMemFree; } RDD_DDRAWFNS;

/* DDHALINFO, field for field; `ddCaps` (DDCORECAPS) is the DLL's business. */
typedef struct {
    rdd_u32 dwSize;
    rdd_u32 lpDDCallbacks, lpDDSurfaceCallbacks, lpDDPaletteCallbacks;    /* 16:16 */
    /* vmiData (VIDMEMINFO) */
    rdd_u32 fpPrimary;                  /* flat address of the visible screen */
    rdd_u32 vmFlags, dwDisplayWidth, dwDisplayHeight, lDisplayPitch;
    rdd_u32 pfSize, pfFlags, pfFourCC, pfRGBBitCount, pfRBitMask, pfGBitMask, pfBBitMask, pfAlphaMask;
    rdd_u32 dwOffscreenAlign, dwOverlayAlign, dwTextureAlign, dwZBufferAlign, dwAlphaAlign;
    rdd_u32 dwNumHeaps, pvmList;        /* pvmList: 16:16 */
    unsigned char ddCaps[316];
    rdd_u32 dwMonitorFrequency;
    rdd_u32 GetDriverInfo;              /* flat */
    rdd_u32 dwModeIndex;
    rdd_u32 lpdwFourCC;                 /* 16:16 */
    rdd_u32 dwNumModes;
    rdd_u32 lpModeInfo;                 /* 16:16 */
    rdd_u32 dwFlags;
    rdd_u32 lpPDevice;                  /* 16:16: the 16-bit driver's PDEVICE */
    rdd_u32 hInstance;
    rdd_u32 lpD3DGlobalDriverData, lpD3DHALCallbacks, lpDDExeBufCallbacks;  /* flat */
} RDD_HALINFO;

#define RDD_PF_PALETTEINDEXED8  0x20
#define RDD_PF_RGB              0x40
#define RDD_HALINFO_ISPRIMARYDISPLAY 0x1
#define RDD_HALINFO_MODEXILLEGAL     0x2
#define RDD_HALINFO_GETDRIVERINFOSET 0x4

/* VIDMEM: one heap of video memory. A rectangular heap here, as wide as the screen's lines:
   DirectDraw places full-width surfaces (back buffers) at the start of a line, which is where
   the card can start showing the screen. */
typedef struct {
    rdd_u32 dwFlags;
    rdd_u32 fpStart;                    /* flat */
    rdd_u32 dwWidth;                    /* bytes (fpEnd for a linear heap) */
    rdd_u32 ddsCaps, ddsCapsAlt;
    rdd_u32 dwHeight;                   /* lines (lpHeap for a linear heap) */
} RDD_VIDMEM;
#define RDD_VIDMEM_ISRECTANGULAR 0x2

/* DDHALMODEINFO */
typedef struct {
    rdd_u32 dwWidth, dwHeight, lPitch, dwBPP;
    rdd_u16 wFlags, wRefreshRate;
    rdd_u32 dwRBitMask, dwGBitMask, dwBBitMask, dwAlphaBitMask;
} RDD_MODEINFO;
#define RDD_MODEINFO_PALETTIZED 0x1

/* The DDHAL_DDCALLBACKS table: dwSize, dwFlags, then the functions in this order. */
#define RDD_CB_DESTROYDRIVER 2          /* index of DestroyDriver: the one 16-bit callback */

typedef struct {
    rdd_u32 magic, size;
    rdd_u32 self16;                     /* this block, 16:16 (16-bit side) */
    rdd_u32 runtime;                    /* DirectDraw's version, from DDVERSIONINFO */
    rdd_u32 setinfo16;                  /* DDRAW16's SetInfo, from DDNEWCALLBACKFNS */
    rdd_u32 dll_ready;                  /* DriverInit has filled in its part */
    rdd_u32 mode_serial;                /* counts mode sets, so the DLL can notice them */
    /* the screen mode (16-bit side, at every mode set) */
    rdd_u32 lfb;                        /* flat address of the frame buffer */
    rdd_u32 vram;                       /* bytes of video memory */
    rdd_u32 width, height, bpp, pitch;
    rdd_u32 lines;                      /* lines of video memory at this pitch */
    /* what DirectDraw reads */
    RDD_HALINFO  hal;
    rdd_u32      ddcb[12];              /* DDHAL_DDCALLBACKS */
    rdd_u32      surfcb[16];            /* DDHAL_DDSURFACECALLBACKS */
    rdd_u32      palcb[4];              /* DDHAL_DDPALETTECALLBACKS */
    RDD_VIDMEM   heap;
    RDD_MODEINFO modes[RDD_MAX_MODES];
} RDD_SHARED;

#endif
