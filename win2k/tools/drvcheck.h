/* Two automatic checks for the pixel tests, so that a run fails on its own (development only):
 *
 * 1. What the driver skipped. driver_skips() reads the driver's counts of draws, clears, states,
 *    shaders and formats it left out or could not do: from the Windows 2000/XP display driver by
 *    its escape (rbskips.h), from the Windows 9x driver by RippleSkips(), which rippledd.dll
 *    exports. skips_case() is called after each case; a case during which a count rose fails.
 *
 * 2. The reference picture. A run on the reference rasterizer (the test's "ref" argument; it needs
 *    Microsoft's d3dref9.dll or d3dref8.dll from the DirectX SDK beside the test) keeps every case's
 *    picture in a folder. Later runs on the hardware device compare their pictures with those:
 *    ref_case() counts the pixels that differ by more than RefTolerance in a colour channel.
 *    A case whose own samples fail on the reference rasterizer has no reference there: the
 *    "ref" run keeps no picture for it, later runs do not compare it, and both say how many
 *    such cases there are in their end line (ref_not_kept).
 *
 * Plain C89; included once by each test, which is why everything here is static. */
#ifndef DRVCHECK_H
#define DRVCHECK_H
#include "../rbskips.h"

enum { SkipKindsMax = 16, RefTolerance = 16, RefNames = 512 };

static unsigned long skipsLast[SkipKindsMax], skipsTotal;
static int skipKinds, skipsNamed, skipsOff;

/* The counts by kind; returns how many kinds, 0 when the running driver gives none. */
static int driver_skips(unsigned long *counts)
{
    RB2K_SKIPS s;
    HDC dc = GetDC(NULL);
    int n = 0;
    memset(&s, 0, sizeof(s));
    if (dc && ExtEscape(dc, RB2K_D3D7_SKIPS_ESCAPE, 0, NULL, sizeof(s), (LPSTR)&s) > 0 &&
        s.version == RB2K_SKIPS_VERSION) {
        for (n = 0; n < RB2K_SKIP_KINDS; ++n) counts[n] = s.count[n];
        skipsNamed = 1;
    }
    if (dc) ReleaseDC(NULL, dc);
    if (!n) {
        HMODULE lib = LoadLibraryA("rippledd.dll");
        DWORD (WINAPI *get)(DWORD *, DWORD) =
            lib ? (DWORD (WINAPI *)(DWORD *, DWORD))GetProcAddress(lib, "RippleSkips") : NULL;
        if (get) {
            DWORD filled[SkipKindsMax];
            DWORD got = get(filled, SkipKindsMax), k;
            if (got > SkipKindsMax) got = SkipKindsMax;
            for (k = 0; k < got; ++k) counts[k] = filled[k];
            n = (int)got;
        }
    }
    return n;
}

static void skips_start(void)
{
    skipKinds = skipsOff ? 0 : driver_skips(skipsLast);
}

/* After a case: says what the driver skipped while it ran, and returns how much (0 for nothing,
 * and when the driver gives no counts). tag: the test's line prefix. */
static unsigned long skips_case(const char *tag, const char *name)
{
    static const char *const names[RB2K_SKIP_KINDS] = RB2K_SKIP_KIND_NAMES;
    unsigned long now[SkipKindsMax], rose = 0;
    int k;
    if (!skipKinds || driver_skips(now) != skipKinds) return 0;
    for (k = 0; k < skipKinds; ++k) rose += now[k] - skipsLast[k];
    if (rose) {
        printf("%s case=%s skips=%lu", tag, name, rose);
        for (k = 0; k < skipKinds; ++k) {
            if (now[k] == skipsLast[k]) continue;
            if (skipsNamed && k < RB2K_SKIP_KINDS) printf(" %s=%lu", names[k], now[k] - skipsLast[k]);
            else printf(" kind%d=%lu", k, now[k] - skipsLast[k]);
        }
        printf("\n");
    }
    memcpy(skipsLast, now, sizeof(now));
    skipsTotal += rose;
    return rose;
}

/* For the end line: " skips=N", or " skips=unavailable". */
static const char *skips_summary(void)
{
    static char text[40];
    if (skipKinds) sprintf(text, " skips=%lu", skipsTotal);
    else strcpy(text, skipsOff ? " skips=off" : " skips=unavailable");
    return text;
}

/* ---- reference pictures ---- */
enum { RefOff, RefWrite, RefCompare };
static int refMode, refCompared, refFailed, refMissing, refNotKept;
static int refAtX, refAtY;                 /* the first pixel of the last compared case that differed */
static unsigned long refMine, refTheirs;     /* and its two colours */
static const char *refFolder;
static char refSeen[RefNames][40];
static int refSeenCount;

/* The file of a case's picture: its name, with a number when the name comes again in a run. */
static void ref_path(char *path, const char *name)
{
    int i, times = 1;
    for (i = 0; i < refSeenCount; ++i) if (!strcmp(refSeen[i], name)) ++times;
    if (refSeenCount < RefNames) {
        strncpy(refSeen[refSeenCount], name, sizeof(refSeen[0]) - 1);
        refSeen[refSeenCount][sizeof(refSeen[0]) - 1] = 0;
        ++refSeenCount;
    }
    if (times > 1) sprintf(path, "%s\\%.39s~%d.pix", refFolder, name, times);
    else sprintf(path, "%s\\%.39s.pix", refFolder, name);
}

/* pixels: side rows of side 32-bit pixels, pitch bytes apart. Writing: keeps the picture, returns
 * 0. Comparing: returns how many pixels differ from the reference picture by more than
 * RefTolerance in a channel (alpha is not compared), or -1 when the reference run kept none for
 * this case. worst gets the largest channel difference found.
 * failed: the case's own samples did not pass in this run. A reference run then keeps no picture
 * (-1) and leaves a note in its place; a comparing run that finds the note does not compare (-1).
 * Both count the case in refNotKept and say so on a line of its own. tag: the test's line prefix. */
static long ref_case(const char *tag, const char *name, const void *pixels, int pitch, int side, int *worst, int failed)
{
    char path[MAX_PATH], note[MAX_PATH];
    HANDLE h;
    DWORD done = 0;
    long differ = 0;
    int x, y, k;
    *worst = 0;
    if (refMode == RefOff) return -1;
    ref_path(path, name);
    strcpy(note, path);
    strcpy(note + strlen(note) - 3, "not");     /* name.pix: the picture; name.not: none was kept */
    if (refMode == RefWrite) {
        CreateDirectoryA(refFolder, NULL);
        if (failed) {
            DeleteFileA(path);
            h = CreateFileA(note, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            ++refNotKept;
            printf("%s case=%s reference not kept: the case's samples fail on this device\n", tag, name);
            return -1;
        }
        DeleteFileA(note);
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return -1;
        for (y = 0; y < side; ++y) WriteFile(h, (const BYTE *)pixels + y * pitch, side * 4, &done, NULL);
        CloseHandle(h);
        return 0;
    }
    if (GetFileAttributesA(note) != 0xFFFFFFFF) {
        ++refNotKept;
        printf("%s case=%s not compared: the reference run kept no picture (its samples failed there)\n", tag, name);
        return -1;
    }
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { ++refMissing; return -1; }
    for (y = 0; y < side; ++y) {
        static DWORD row[1024];
        const DWORD *mine = (const DWORD *)((const BYTE *)pixels + y * pitch);
        if (side > 1024 || !ReadFile(h, row, side * 4, &done, NULL) || done != (DWORD)side * 4) { differ = -1; break; }
        for (x = 0; x < side; ++x) {
            int off = 0;
            for (k = 0; k < 24; k += 8) {
                int d = (int)((mine[x] >> k) & 255) - (int)((row[x] >> k) & 255);
                if (d < 0) d = -d;
                if (d > *worst) *worst = d;
                if (d > RefTolerance) off = 1;
            }
            if (off && !differ) { refAtX = x; refAtY = y; refMine = mine[x] & 0xffffff; refTheirs = row[x] & 0xffffff; }
            differ += off;
        }
    }
    CloseHandle(h);
    if (differ >= 0) ++refCompared;
    return differ;
}

/* The reference part of a test's end line; empty when this check is off. */
static const char *ref_summary(void)
{
    static char text[96];
    text[0] = 0;
    if (refMode == RefWrite) sprintf(text, " ref_not_kept=%d", refNotKept);
    else if (refMode == RefCompare) {
        if (refCompared || refNotKept)
            sprintf(text, " ref_compared=%d ref_failed=%d ref_not_kept=%d", refCompared, refFailed, refNotKept);
        else strcpy(text, " ref=unavailable");
    }
    return text;
}

#endif
