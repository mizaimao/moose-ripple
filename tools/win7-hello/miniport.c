/*
 * A skeleton WDDM 1.1 display miniport for the toolchain check (docs/windows/toolchain.md):
 * DriverEntry fills DRIVER_INITIALIZATION_DATA (Windows 7 interface version) with stubs and
 * registers with the DirectX kernel through DxgkInitialize from the WDK's displib.lib. It only
 * has to link and pass a structural check: installed, it would take the screen away.
 * MIT licence (LICENSE at the repository root).
 */
#include <ntddk.h>
#include <dispmprt.h>

DRIVER_INITIALIZE DriverEntry;

/* Every callback is this stub: nothing here is ever meant to run. */
static NTSTATUS APIENTRY SkeletonStub(VOID)
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA init;
    PVOID *fn;

    RtlZeroMemory(&init, sizeof(init));
    init.Version = DXGKDDI_INTERFACE_VERSION_WIN7;
    /* All members after Version are callback pointers. */
    for (fn = (PVOID *)&init.DxgkDdiAddDevice; fn < (PVOID *)(&init + 1); fn++)
        *fn = (PVOID)SkeletonStub;
    return DxgkInitialize(DriverObject, RegistryPath, &init);
}
