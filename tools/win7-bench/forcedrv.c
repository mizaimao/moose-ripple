/* forcedrv INF [HARDWARE-ID]: puts the driver of a setup file on the devices with that
 * hardware ID, also where Windows ranks another driver higher (its own Standard VGA driver
 * against a test-signed package). Without an ID: the virtio GPU. Run elevated.
 * Build: x86_64-w64-mingw32-clang -municode -O2 -o forcedrv.exe forcedrv.c -lnewdev
 * MIT licence (LICENSE at the repository root). */
#include <windows.h>
#include <stdio.h>

BOOL WINAPI UpdateDriverForPlugAndPlayDevicesW(HWND parent, LPCWSTR hardwareId, LPCWSTR fullInfPath,
                                               DWORD installFlags, PBOOL rebootRequired);
#define INSTALLFLAG_FORCE 1

int wmain(int argc, wchar_t **argv)
{
    wchar_t inf[MAX_PATH];
    const wchar_t *id = argc > 2 ? argv[2] : L"PCI\\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01";
    BOOL reboot = FALSE;

    if (argc < 2 || !GetFullPathNameW(argv[1], MAX_PATH, inf, NULL)) {
        printf("FORCEDRV usage: forcedrv INF [HARDWARE-ID]\n");
        return 2;
    }
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, id, inf, INSTALLFLAG_FORCE, &reboot)) {
        printf("FORCEDRV failed, error 0x%08lx\n", GetLastError());
        return 1;
    }
    printf("FORCEDRV done, restart %s\n", reboot ? "needed" : "not needed");
    return 0;
}
