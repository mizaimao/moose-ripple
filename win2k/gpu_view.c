/* Retain the actual requesting process across DirectDraw mode changes. A
 * current-process pseudo-handle alone cannot revoke a view from another thread. */
#include <ntddk.h>

/* Exported by the Windows 2000 kernel and its import library. The base DDK
 * declares this entry point only in the file-system kit. */
NTKERNELAPI NTSTATUS NTAPI ObOpenObjectByPointer(PVOID object, ULONG attributes,
    PACCESS_STATE state, ACCESS_MASK access, POBJECT_TYPE type,
    KPROCESSOR_MODE mode, PHANDLE handle);

PVOID rb_view_current(void)
{
    return PsGetCurrentProcess();
}

HANDLE rb_view_open(HANDLE supplied)
{
    PVOID object;
    HANDLE handle = NULL;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(supplied, 0, NULL, KernelMode, &object, NULL)))
        return NULL;
    if (object == PsGetCurrentProcess())
        ObOpenObjectByPointer(object, OBJ_KERNEL_HANDLE, NULL, 0x0008 /* PROCESS_VM_OPERATION */,
                              NULL, KernelMode, &handle);
    ObDereferenceObject(object);
    return handle;
}

void rb_view_close(HANDLE handle)
{
    ZwClose(handle);
}
