/* Exercise the display pointer with two known masks on the test's own window. */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include "rb2kcursor.h"

static HCURSOR active;
static LRESULT CALLBACK procedure(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) { SetCursor(active); return TRUE; }
    return DefWindowProcA(w, msg, wp, lp);
}

static HCURSOR pointer(HINSTANCE inst, BOOL invert, ULONG *hash)
{
    BYTE andMask[128], xorMask[128];
    ULONG h = 2166136261UL;
    int x, y;
    ZeroMemory(andMask, sizeof(andMask));
    ZeroMemory(xorMask, sizeof(xorMask));
    for (y = 0; y < 64; ++y) for (x = 0; x < 64; ++x) {
        ULONG pixel = 0;
        if (x < 32 && y < 32) {
            int n = y*4 + x/8;
            BYTE bit = (BYTE)(0x80 >> (x & 7));
            if (x == y) { xorMask[n] |= bit; pixel = 0xffffffffUL; }
            else if (x == 0 || y == 31) pixel = 0xff000000UL;
            else andMask[n] |= bit;
            if (invert && x == 13 && y == 17) xorMask[n] |= bit;
        }
        h = (h ^ pixel) * 16777619UL;
    }
    *hash = h;
    return CreateCursor(inst, 7, 9, 32, 32, andMask, xorMask);
}

static BOOL snapshot(HDC dc, RB2K_CURSOR_STATS *s)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    GdiFlush();
    return ExtEscape(dc, RB2K_CURSOR_ESCAPE, 0, NULL, sizeof(*s), (LPSTR)s) > 0;
}

static HCURSOR alpha_pointer(ULONG marker)
{
    BITMAPINFO bmi = {0};
    ICONINFO icon = {0};
    BYTE mask[128] = {0};
    ULONG *bits;
    HCURSOR cursor = NULL;
    int x, y;
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 32; bmi.bmiHeader.biHeight = -32;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    icon.xHotspot = 5; icon.yHotspot = 11;
    icon.hbmMask = CreateBitmap(32,32,1,1,mask);
    icon.hbmColor = CreateDIBSection(NULL,&bmi,DIB_RGB_COLORS,(void **)&bits,NULL,0);
    if (icon.hbmColor && icon.hbmMask && bits) {
        for (y = 0; y < 32; ++y) for (x = 0; x < 32; ++x) {
            ULONG pixel = 0;
            if ((x >= 2 && x <= 12 && y >= 7 && y <= 17) || x == y) pixel = marker;
            else if (x == 1 || y == 30) pixel = 0x80004020UL;
            bits[y*32+x] = pixel;
        }
        cursor = (HCURSOR)CreateIconIndirect(&icon);
    }
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
    return cursor;
}

/* NT5 can round an opaque colour down by one when realizing an alpha
 * cursor. A/B/A must reproduce the same three channel offsets each time. */
static BOOL marker_matches(ULONG actual, ULONG requested, LONG offsets[3], BOOL first)
{
    ULONG channel;
    if ((actual >> 24) != 255) return FALSE;
    for (channel = 0; channel < 3; ++channel) {
        ULONG shift = channel * 8;
        LONG delta = (LONG)((actual >> shift) & 255) - (LONG)((requested >> shift) & 255);
        if (delta != 0 && delta != -1) return FALSE;
        if (!first && delta != offsets[channel]) return FALSE;
        offsets[channel] = delta;
    }
    return TRUE;
}

/* Check the OS-realized source and the independently submitted device
 * resource. Two marker colours identify our pointer even if Windows has
 * expanded it with a shadow before handing it to the display driver. */
static BOOL check_alpha(HDC dc, HCURSOR previous)
{
    const ULONG markers[2] = {0xffcd235dUL, 0xff31b176UL};
    HCURSOR cursors[2];
    ULONG arm = 1, hashes[3] = {0};
    LONG offsets[3] = {0};
    BOOL ok = TRUE;
    int i;
    RB2K_CURSOR_STATS before, after;
    RB2K_CURSOR_SOURCE s;
    cursors[0] = alpha_pointer(markers[0]); cursors[1] = alpha_pointer(markers[1]);
    for (i = 0; i < 3; ++i) {
        ULONG index = i & 1;
        if (!cursors[index] || !snapshot(dc,&before) || ExtEscape(dc, RB2K_CURSOR_SOURCE_ESCAPE,
                sizeof(arm), (LPCSTR)&arm, sizeof(s), (LPSTR)&s) <= 0) { ok = FALSE; break; }
        active = cursors[index]; SetCursor(active); Sleep(80);
        if (!snapshot(dc,&after) || ExtEscape(dc,RB2K_CURSOR_SOURCE_ESCAPE,0,NULL,
                sizeof(s),(LPSTR)&s) <= 0) { ok = FALSE; break; }
        printf("CURSORQA alpha %d visible=%lu size=%lux%lu hot=%lu,%lu marker=%08lx expected=%08lx "
               "partial=%lu partial_color=%lu source=%08lx submitted=%08lx\n",i+1,after.visible,
               s.width,s.height,s.hotX,s.hotY,s.hotPixel,markers[index],
               s.partial,s.partialColor,s.hash,after.hash);
        ok &= after.visible && after.accepted[RB2K_CURSOR_ALPHA] > before.accepted[RB2K_CURSOR_ALPHA] &&
              s.opaque && s.partial && marker_matches(s.hotPixel,markers[index],offsets,i == 0) &&
              after.hash == s.hash &&
              after.width == s.width && after.height == s.height && after.hotX == s.hotX && after.hotY == s.hotY &&
              s.hotX >= 5 && s.hotY >= 11 && s.width >= s.hotX - 5 + 32 && s.height >= s.hotY - 11 + 32;
        hashes[i] = s.hash;
    }
    active = previous; SetCursor(active);
    for (i = 0; i < 2; ++i) if (cursors[i]) DestroyCursor(cursors[i]);
    return ok && hashes[0] != hashes[1] && hashes[0] == hashes[2];
}

int main(void)
{
    WNDCLASSA wc = {0};
    RB2K_CURSOR_STATS first, s;
    HINSTANCE inst = GetModuleHandleA(NULL);
    HCURSOR shape, invert, old;
    HWND window;
    HDC dc;
    RECT client;
    POINT saved, p = {70,70};
    HBRUSH brush;
    ULONG hash, unused, moves, hides;
    int count = 0, ok = 1;
    shape = pointer(inst, FALSE, &hash);
    invert = pointer(inst, TRUE, &unused);
    wc.lpfnWndProc = procedure; wc.hInstance = inst; wc.lpszClassName = "RipplePointerQA";
    if (!shape || !invert || !RegisterClassA(&wc)) return 1;
    GetCursorPos(&saved);
    old = GetCursor(); active = shape;
    window = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "Pointer test", WS_POPUP | WS_VISIBLE,
                             120, 120, 200, 180, NULL, NULL, inst, NULL);
    if (!window) return 1;
    SetForegroundWindow(window);
    dc = GetDC(window);
    if (!dc || !snapshot(dc, &first)) return 1;
    if (!first.enabled) { puts("CURSORQA hardware option off"); return 0; }
    GetClientRect(window, &client);
    brush = CreateSolidBrush(RGB(29,167,91));
    FillRect(dc, &client, brush); DeleteObject(brush);
    ClientToScreen(window, &p);
    SetCursorPos(p.x, p.y);
    do { ++count; } while (ShowCursor(TRUE) < 0 && count < 32);
    SetCursor(shape);
    Sleep(80);
    if (!snapshot(dc, &s)) return 1;
    printf("CURSORQA shape visible=%lu size=%lux%lu hot=%lu,%lu hash=%08lx expected=%08lx mono=%lu failure=%lu\n",
           s.visible,s.width,s.height,s.hotX,s.hotY,s.hash,hash,s.accepted[0],s.failed);
    ok &= s.visible && s.width == 32 && s.height == 32 && s.hotX == 7 && s.hotY == 9 && s.hash == hash;
    moves = s.moves; hides = s.hides;
    SetCursorPos(p.x + 13, p.y + 11); Sleep(80);
    if (!snapshot(dc, &s)) return 1;
    printf("CURSORQA move delta=%lu x=%ld y=%ld expected=%ld,%ld\n",
           s.moves - moves,s.x,s.y,p.x+13,p.y+11);
    ok &= s.visible && s.moves > moves && s.x == p.x + 13 && s.y == p.y + 11;
    ok &= GetPixel(dc, 83, 81) == RGB(29,167,91);
    active = NULL; SetCursor(NULL); Sleep(80);
    if (!snapshot(dc,&s)) return 1;
    printf("CURSORQA hide visible=%lu hide_delta=%lu\n",s.visible,s.hides-hides);
    ok &= !s.visible && s.hides > hides;
    active = invert; SetCursor(active); Sleep(80);
    if (!snapshot(dc,&s)) return 1;
    printf("CURSORQA invert visible=%lu decline_delta=%lu invert_delta=%lu\n",s.visible,
           s.declined-first.declined,s.inverted-first.inverted);
    ok &= !s.visible && s.inverted > first.inverted;
    ok &= check_alpha(dc, invert);
    active = shape; SetCursor(active); Sleep(80);
    if (!snapshot(dc,&s)) return 1;
    ok &= s.visible && s.hash == hash && s.failed == first.failed;
    active = old; SetCursor(old);
    while (count-- > 0) ShowCursor(FALSE);
    SetCursorPos(saved.x,saved.y);
    ReleaseDC(window,dc); DestroyWindow(window);
    DestroyCursor(invert); DestroyCursor(shape);
    printf("CURSORQA %s\n",ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
