#include "vista_display_memory.h"

#if DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WIN7
VistaDisplayMemory::~VistaDisplayMemory()
{
    if (m_base)
        MmFreeContiguousMemory(m_base);
}

NTSTATUS VistaDisplayMemory::Initialize()
{
    if (m_base)
        return STATUS_SUCCESS;
    PHYSICAL_ADDRESS low = {}, high = {}, boundary = {};
    high.QuadPart = MAXLONGLONG;
    // VidMm maps this physical range to the desktop process. Reserve and clear
    // all of it before exposing it, including padding at the end of a surface.
    for (SIZE_T size = 64 * 1024 * 1024; size >= 8 * 1024 * 1024; size /= 2)
    {
        m_base = static_cast<BYTE *>(MmAllocateContiguousMemorySpecifyCache(size, low, high, boundary, MmWriteCombined));
        if (m_base)
        {
            m_size = size;
            RtlZeroMemory(m_base, m_size);
            DbgPrint(TRACE_LEVEL_WARNING, ("Vista desktop segment: %u MiB\n", (UINT)(m_size >> 20)));
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

void VistaDisplayMemory::Describe(DXGK_SEGMENTDESCRIPTOR *segment) const
{
    RtlZeroMemory(segment, sizeof(*segment));
    // GPU base zero makes SegmentAddress an offset. The CPU has a separate,
    // genuine physical base. The ordinary virtio aperture starts at 0xc0000000.
    segment->CpuTranslatedAddress = MmGetPhysicalAddress(m_base);
    segment->Flags.CpuVisible = TRUE;
    segment->Flags.PopulatedFromSystemMemory = TRUE;
    segment->Size = m_size;
}

BYTE *VistaDisplayMemory::Range(ULONGLONG offset, SIZE_T size) const
{
    return offset <= m_size && size <= m_size - offset ? m_base + (SIZE_T)offset : NULL;
}

BOOLEAN VistaDisplayMemory::PhysicalAt(ULONGLONG offset, SIZE_T size, PHYSICAL_ADDRESS *address) const
{
    BYTE *start = Range(offset, size);
    if (!start)
        return FALSE;
    // One contiguous block (Initialize): the address of its start covers the whole range
    *address = MmGetPhysicalAddress(start);
    return TRUE;
}

NTSTATUS VistaDisplayMemory::Page(DXGKARG_BUILDPAGINGBUFFER *paging)
{
    PAGED_CODE();
    if (paging->Operation == DXGK_OPERATION_DISCARD_CONTENT)
        return paging->DiscardContent.SegmentId == SegmentId ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    if (paging->Operation == DXGK_OPERATION_FILL)
    {
        const auto &fill = paging->Fill;
        if (fill.Destination.SegmentId != SegmentId)
            return STATUS_NOT_SUPPORTED;
        BYTE *dest = Range(fill.Destination.SegmentAddress.QuadPart, fill.FillSize);
        if (!dest || (fill.FillSize & 3))
            return STATUS_INVALID_PARAMETER;
        ULONG *words = reinterpret_cast<ULONG *>(dest);
        for (SIZE_T i = 0; i < fill.FillSize / sizeof(*words); ++i)
            words[i] = fill.FillPattern;
        KeMemoryBarrier();
        return STATUS_SUCCESS;
    }
    if (paging->Operation != DXGK_OPERATION_TRANSFER)
        return STATUS_NOT_SUPPORTED;
    const auto &t = paging->Transfer;
    const bool toSegment = t.Destination.SegmentId == SegmentId;
    if (!toSegment && t.Source.SegmentId != SegmentId)
        return STATUS_NOT_SUPPORTED;
    const auto getAddress = [&](const auto &location) -> BYTE * {
        if (location.SegmentId == SegmentId)
        {
            const ULONGLONG offset = location.SegmentAddress.QuadPart;
            if (offset > m_size || t.TransferOffset > m_size - offset)
                return NULL;
            return Range(offset + t.TransferOffset, t.TransferSize);
        }
        if (location.SegmentId != 0 || !location.pMdl)
            return NULL;
        MDL *mdl = location.pMdl;
        SIZE_T offset = (SIZE_T)t.MdlOffset * PAGE_SIZE;
        SIZE_T length = MmGetMdlByteCount(mdl);
        if (offset > length || t.TransferSize > length - offset)
            return NULL;
        BYTE *base = static_cast<BYTE *>(MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority));
        return base ? base + offset : NULL;
    };
    BYTE *src = getAddress(t.Source);
    BYTE *dst = getAddress(t.Destination);
    if (!src || !dst)
        return STATUS_INVALID_PARAMETER;
    RtlMoveMemory(dst, src, t.TransferSize);
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}
#endif
