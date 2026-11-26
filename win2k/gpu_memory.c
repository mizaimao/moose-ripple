/* Guest memory behind OpenGL's host resources (gpu_rvd.c): pages below
 * 4 GiB for the device, mapped cached into the process that asked for them.
 * The pages belong to the driver, so nothing is locked on the program's
 * behalf: if it ends without freeing a buffer, its mapping goes with its
 * address space and the driver frees the pages later (rb_process_gone). */
#include <ntddk.h>
#include "gpu_memory.h"

typedef char run_matches_virtio_entry[sizeof(RB2K_PAGE_RUN) == 16 ? 1 : -1];

/* Time stamps in processor cycles. KeQueryPerformanceCounter reads the ACPI
 * power management timer on these HALs, an emulated port that QEMU answers
 * under the lock its drawing thread holds, so every read could stop the guest
 * until the host finished a batch (4,500 reads a second while 3DMark 2001 SE
 * ran). The cycle counter is read without leaving the guest. Its rate is
 * measured against the performance counter only when a report asks for it,
 * over the whole time since the first stamp. */
LONGLONG rb_counter_ticks(LONGLONG *frequency)
{
    static LONGLONG cycles0, counter0, counterHz;
    LONGLONG cycles = (LONGLONG)__builtin_ia32_rdtsc();
    if (!counter0) {
        LARGE_INTEGER hz, now = KeQueryPerformanceCounter(&hz);
        cycles0 = cycles;
        counter0 = now.QuadPart;
        counterHz = hz.QuadPart;
    }
    if (frequency) {
        LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);
        ULONGLONG c = (ULONGLONG)(cycles - cycles0), q = (ULONGLONG)(now.QuadPart - counter0);
        /* c / (q / counterHz), in two parts so that nothing overflows. */
        *frequency = q ? (LONGLONG)(c / q * (ULONGLONG)counterHz + c % q * (ULONGLONG)counterHz / q) : 0;
    }
    return cycles;
}

PVOID rb_process_ref(void)
{
    PEPROCESS process = PsGetCurrentProcess();
    ObReferenceObject(process);
    return process;
}

void rb_process_deref(PVOID process)
{
    if (process) ObDereferenceObject(process);
}

BOOLEAN rb_process_current(PVOID process)
{
    return process && process == (PVOID)PsGetCurrentProcess();
}

/* A process object is signalled once the process has ended. */
BOOLEAN rb_process_gone(PVOID process)
{
    LARGE_INTEGER now;
    now.QuadPart = 0;
    return process && KeWaitForSingleObject(process, Executive, KernelMode, FALSE, &now) == STATUS_SUCCESS;
}

BOOLEAN rb_pages_alloc(RB2K_PAGES *p, ULONG bytes)
{
    PHYSICAL_ADDRESS low, high, skip;
    PMDL mdl;
    PVOID user = NULL;
    RtlZeroMemory(p, sizeof(*p));
    if (!bytes || (bytes & (PAGE_SIZE - 1))) return FALSE;
    low.QuadPart = 0;
    high.QuadPart = 0xffffffff;
    skip.QuadPart = 0;
    mdl = MmAllocatePagesForMdl(low, high, skip, bytes);
    if (!mdl) return FALSE;
    if (MmGetMdlByteCount(mdl) != bytes) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return FALSE;
    }
    __try {
        user = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
        if (user) RtlZeroMemory(user, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (user) MmUnmapLockedPages(user, mdl);
        user = NULL;
    }
    if (!user) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return FALSE;
    }
    p->mdl = mdl;
    p->user = user;
    p->bytes = bytes;
    p->owner = rb_process_ref();
    return TRUE;
}

/* Physical runs for VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING; 0 if more than max. */
ULONG rb_pages_runs(const RB2K_PAGES *p, RB2K_PAGE_RUN *out, ULONG max)
{
    PPFN_NUMBER pfn = (PPFN_NUMBER)((PMDL)p->mdl + 1);   /* the MDL's page list */
    ULONG pages = p->bytes >> PAGE_SHIFT, i, count = 0;
    for (i = 0; i < pages; ++i) {
        ULONGLONG address = (ULONGLONG)pfn[i] << PAGE_SHIFT;
        if (count && out[count - 1].address + out[count - 1].length == address) {
            out[count - 1].length += PAGE_SIZE;
            continue;
        }
        if (count == max) return 0;
        out[count].address = address;
        out[count].length = PAGE_SIZE;
        out[count].padding = 0;
        ++count;
    }
    return count;
}

/* Unmap only in the owner's own address space; a process that has ended
 * took its mapping with it. Call only after the host has let go. */
void rb_pages_free(RB2K_PAGES *p)
{
    if (!p->mdl) return;
    if (p->user && rb_process_current(p->owner)) MmUnmapLockedPages(p->user, (PMDL)p->mdl);
    MmFreePagesFromMdl((PMDL)p->mdl);
    ExFreePool(p->mdl);
    rb_process_deref(p->owner);
    RtlZeroMemory(p, sizeof(*p));
}

/* A cached kernel view of the framebuffer. The framebuffer is ordinary
 * memory inside the virtual machine, which the host reads coherently.
 * VideoPort maps device memory uncached, and Windows 2000 makes
 * write-combining through MTRRs; KVM on AMD hosts ignores the guest's MTRRs
 * but honours its page attributes, so every framebuffer access went to
 * memory at about 60 MB/s there. */
PVOID rb_map_cached(unsigned __int64 physical, ULONG bytes)
{
    PHYSICAL_ADDRESS at;
    at.QuadPart = (LONGLONG)physical;
    return MmMapIoSpace(at, bytes, MmCached);
}

void rb_unmap_cached(PVOID address, ULONG bytes)
{
    if (address) MmUnmapIoSpace(address, bytes);
}
