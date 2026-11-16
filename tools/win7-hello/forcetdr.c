/*
 * Brings about a GPU timeout, so that Windows' timeout detection and recovery ("Display driver
 * stopped responding and has recovered") runs with the driver's DxgkDdiResetFromTimeout and
 * DxgkDdiRestartFromTimeout, without keeping the host GPU busy. Writes forcetdr.txt next to it.
 *   forcetdr stall MS   asks a test build of our driver (windows/kmd/build.sh with TEST_TDR=1)
 *                       to hold the next finished command buffer back MS milliseconds; any
 *                       drawing afterwards (move the pointer) then times out
 *   forcetdr [KIND]     asks Windows itself to act as if a timeout had happened (KIND 0, a
 *                       scheduler timeout, or 5, a GPU timeout; as administrator). The retail
 *                       Windows 7 refuses it (0xc00000bb, not supported).
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gdi32's doors to the display kernel, with only the fields used here, taken by name at run
 * time (see dwmprobe.c) */
struct open_gdi { WCHAR name[32]; UINT adapter; LUID luid; UINT source; };
struct escape {
    UINT adapter;
    UINT device;
    UINT type;
    UINT flags;
    void *data;
    UINT size;
    UINT context;
};
struct close_adapter { UINT adapter; };
typedef LONG (WINAPI *kmt_fn)(void *);
#define ESCAPE_DRIVERPRIVATE 0
#define ESCAPE_TDRDBGCTRL 2   /* D3DKMT_ESCAPE_TDRDBGCTRL; the data is a D3DKMT_TDRDBGCTRLTYPE */
#define VIOGPU_TEST_STALL 0x300   /* windows/kmd/shared/viogpum.h */

/* The driver's own escape: type and data length (16 bits each), then the value */
struct viogpu_escape { USHORT type; USHORT length; ULONG value; BYTE rest[56]; };

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    FILE *f = fopen("forcetdr.txt", "wb");
    HMODULE gdi = LoadLibraryA("gdi32.dll");
    kmt_fn kmt_open = (kmt_fn)(void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName");
    kmt_fn kmt_escape = (kmt_fn)(void *)GetProcAddress(gdi, "D3DKMTEscape");
    kmt_fn kmt_close = (kmt_fn)(void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    int stall = cmd && strncmp(cmd, "stall", 5) == 0;
    UINT kind = (cmd && *cmd && !stall) ? (UINT)strtoul(cmd, NULL, 0) : 0;
    struct viogpu_escape ve;
    struct open_gdi og;
    struct escape e;
    DISPLAY_DEVICEW dd;
    DWORD i;
    LONG st;
    (void)inst; (void)prev; (void)show;

    if (!f)
        return 1;
    setvbuf(f, NULL, _IONBF, 0);
    ZeroMemory(&og, sizeof(og));
    for (i = 0;; i++) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0))
            break;
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            lstrcpynW(og.name, dd.DeviceName, 32);
            break;
        }
    }
    if (!kmt_open || !kmt_escape || !kmt_close || kmt_open(&og) < 0) {
        fprintf(f, "the adapter of the primary display (%ls) did not open\r\n", og.name);
        return 1;
    }
    ZeroMemory(&e, sizeof(e));
    e.adapter = og.adapter;
    if (stall) {
        ZeroMemory(&ve, sizeof(ve));
        ve.type = VIOGPU_TEST_STALL;
        ve.length = sizeof(ULONG);
        ve.value = strtoul(cmd + 5, NULL, 0);
        e.type = ESCAPE_DRIVERPRIVATE;
        e.data = &ve;
        e.size = sizeof(ve);
        st = kmt_escape(&e);
        fprintf(f, "next command buffer held back %lu ms on %ls: 0x%08lx\r\n", ve.value, og.name, (unsigned long)st);
    } else {
        e.type = ESCAPE_TDRDBGCTRL;
        e.data = &kind;
        e.size = sizeof(kind);
        st = kmt_escape(&e);
        fprintf(f, "timeout of kind %u asked for on %ls: 0x%08lx\r\n", kind, og.name, (unsigned long)st);
    }
    {
        struct close_adapter c = { og.adapter };
        kmt_close(&c);
    }
    fclose(f);
    return st < 0;
}
