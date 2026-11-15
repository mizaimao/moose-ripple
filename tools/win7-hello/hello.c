/*
 * A minimal Windows kernel driver for the toolchain check (docs/windows/toolchain.md): it
 * prints one line through DbgPrint when it loads and one when it unloads. Built off Windows
 * by build.sh, test-signed by sign.sh, loaded with `sc create ... type= kernel`.
 * MIT licence (LICENSE at the repository root).
 */
#include <ntddk.h>

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD HelloUnload;

VOID HelloUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("moose-ripple hello: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    RTL_OSVERSIONINFOW v = { sizeof(v) };
    UNREFERENCED_PARAMETER(RegistryPath);
    RtlGetVersion(&v);
    DbgPrint("moose-ripple hello: loaded on Windows %lu.%lu build %lu\n",
             v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    DriverObject->DriverUnload = HelloUnload;
    return STATUS_SUCCESS;
}
