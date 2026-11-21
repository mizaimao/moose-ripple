/* XP's display class installer puts the INF defaults under Control\Video.
 * Full-screen DirectDraw restores the mode from the current hardware profile. */
#define _WIN32_WINNT 0x0500
#include <windows.h>
#include <setupapi.h>
#include <objbase.h>

static LONG set_profile_defaults(HDEVINFO devices, PSP_DEVINFO_DATA device)
{
    static const WCHAR *const names[] = {
        L"DefaultSettings.XResolution", L"DefaultSettings.YResolution",
        L"DefaultSettings.BitsPerPel", L"DefaultSettings.VRefresh"
    };
    DWORD values[sizeof(names) / sizeof(names[0])];
    WCHAR id[39], path[100];
    HKEY hardware, adapter, profile;
    DWORD i, type, size = sizeof(id);
    GUID guid;
    LONG err;

    hardware = SetupDiOpenDevRegKey(devices, device, DICS_FLAG_GLOBAL, 0,
                                    DIREG_DEV, KEY_QUERY_VALUE);
    if (hardware == INVALID_HANDLE_VALUE) return GetLastError();
    err = RegQueryValueExW(hardware, L"VideoID", NULL, &type, (BYTE *)id, &size);
    RegCloseKey(hardware);
    if (err) return err;
    if (type != REG_SZ || size != sizeof(id) || id[0] != L'{' ||
        id[37] != L'}' || id[38] || FAILED(CLSIDFromString(id, &guid)))
        return ERROR_INVALID_DATA;

    wsprintfW(path, L"System\\CurrentControlSet\\Control\\Video\\%s\\0000", id);
    err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &adapter);
    if (err) return err;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size = sizeof(values[i]);
        err = RegQueryValueExW(adapter, names[i], NULL, &type, (BYTE *)&values[i], &size);
        if (!err && (type != REG_DWORD || size != sizeof(DWORD))) err = ERROR_INVALID_DATA;
        if (err) break;
    }
    RegCloseKey(adapter);
    if (err) return err;

    err = RegCreateKeyExW(HKEY_CURRENT_CONFIG, path, 0, NULL, 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &profile, NULL);
    if (err) return err;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        err = RegQueryValueExW(profile, names[i], NULL, NULL, NULL, NULL);
        if (err == ERROR_FILE_NOT_FOUND)
            err = RegSetValueExW(profile, names[i], 0, REG_DWORD,
                                 (const BYTE *)&values[i], sizeof(DWORD));
        if (err) break;
    }
    RegCloseKey(profile);
    return err;
}

DWORD CALLBACK RippleInstall(DI_FUNCTION action, HDEVINFO devices,
                             PSP_DEVINFO_DATA device, PCOINSTALLER_CONTEXT_DATA context)
{
    DWORD version, err;
    WCHAR message[100];
    if (action != DIF_INSTALLDEVICE) return NO_ERROR;
    version = GetVersion();
    if (LOBYTE(LOWORD(version)) != 5 || HIBYTE(LOWORD(version)) != 1) return NO_ERROR;
    if (!context->PostProcessing) return ERROR_DI_POSTPROCESSING_REQUIRED;
    if (context->InstallResult != NO_ERROR) return context->InstallResult;

    err = set_profile_defaults(devices, device);
    if (err) {
        wsprintfW(message, L"Ripple could not initialize the XP desktop mode (error %lu).", err);
        SetupLogErrorW(message, LogSevWarning);
    }
    return NO_ERROR;
}
