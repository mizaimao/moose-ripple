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

/* Map the scanout pages into a process the way VideoPortMapMemory does, but
 * cached: VideoPort maps device memory uncached (see rb_map_cached). */
PVOID rb_view_map(HANDLE process, unsigned __int64 physical, ULONG *bytes)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    HANDLE section;
    LARGE_INTEGER offset;
    SIZE_T size = *bytes;
    PVOID address = NULL;
    NTSTATUS status;
    RtlInitUnicodeString(&name, L"\\Device\\PhysicalMemory");
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenSection(&section, SECTION_MAP_READ | SECTION_MAP_WRITE, &attributes)))
        return NULL;
    offset.QuadPart = (LONGLONG)physical;
    status = ZwMapViewOfSection(section, process, &address, 0, size, &offset, &size,
                                ViewUnmap, 0, PAGE_READWRITE);
    ZwClose(section);
    if (!NT_SUCCESS(status)) return NULL;
    if (offset.QuadPart != (LONGLONG)physical) {
        ZwUnmapViewOfSection(process, address);
        return NULL;
    }
    *bytes = (ULONG)size;
    return address;
}

/* A process that has ended took the view with its address space. */
NTSTATUS rb_view_unmap(HANDLE process, PVOID address)
{
    NTSTATUS status = ZwUnmapViewOfSection(process, address);
    return status == STATUS_PROCESS_IS_TERMINATING ? STATUS_SUCCESS : status;
}
