/* Install the exact PCI display package next to this tool using Windows Setup. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

typedef BOOL (WINAPI *UPDATE_DEVICE)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);

/* NT 5.0 can list the short PCI vendor/device ID among compatible IDs only.
 * NewDev's first argument must match an actual hardware ID exactly. */
static BOOL get_hardware_id(char *result, size_t capacity)
{
    static const char target[] = "PCI\\VEN_1AF4&DEV_1050";
    char ids[4096];
    SP_DEVINFO_DATA device;
    HDEVINFO set;
    DWORD i, bytes, type;
    BOOL found = FALSE;
    set = SetupDiGetClassDevsA(NULL, "PCI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    device.cbSize = sizeof(device);
    for (i = 0; !found && SetupDiEnumDeviceInfo(set, i, &device); ++i) {
        const char *id;
        if (!SetupDiGetDeviceRegistryPropertyA(set, &device, SPDRP_HARDWAREID,
            &type, (PBYTE)ids, sizeof(ids) - 2, &bytes) || type != REG_MULTI_SZ)
            continue;
        ids[bytes] = ids[bytes + 1] = 0;
        for (id = ids; *id; id += strlen(id) + 1) {
            size_t len = strlen(id);
            if (len >= sizeof(target) - 1 && len < capacity &&
                !_strnicmp(id, target, sizeof(target) - 1) &&
                (id[sizeof(target) - 1] == 0 || id[sizeof(target) - 1] == '&')) {
                strcpy(result, id);
                found = TRUE;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

int main(void)
{
    char inf[MAX_PATH], hardware[256];
    char *name;
    HMODULE library;
    UPDATE_DEVICE update;
    BOOL reboot = FALSE;
    DWORD error, length = GetModuleFileNameA(NULL, inf, sizeof(inf));
    if (!length || length >= sizeof(inf)) return 1;
    name = strrchr(inf, '\\');
    if (!name || (size_t)(name + 1 - inf) + sizeof("rb2k.inf") > sizeof(inf))
        return 1;
    strcpy(name + 1, "rb2k.inf");
    if (GetFileAttributesA(inf) == (DWORD)-1) {
        puts("RB2K missing INF");
        return 1;
    }
    if (!get_hardware_id(hardware, sizeof(hardware))) {
        puts("RB2K matching PCI hardware ID absent");
        return 1;
    }
    library = LoadLibraryA("newdev.dll");
    if (!library) return 1;
    update = (UPDATE_DEVICE)GetProcAddress(library, "UpdateDriverForPlugAndPlayDevicesA");
    if (!update) { FreeLibrary(library); return 1; }
    puts("RB2K requesting native display install");
    fflush(stdout);
    if (!update(NULL, hardware, inf, 0, &reboot)) {
        error = GetLastError();
        printf("RB2K install error %08lx\n", error);
        FreeLibrary(library);
        return 1;
    }
    printf("RB2K install succeeded, reboot requested %d\n", reboot);
    FreeLibrary(library);
    return 0;
}
