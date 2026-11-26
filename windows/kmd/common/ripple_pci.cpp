/*
 * SPDX-License-Identifier: MIT
 *
 * The driver's glue between the virtio library and its PCI device: the callbacks the library
 * calls for device registers, memory and the PCI configuration space, and the mapping of the
 * device's memory and port ranges.
 *
 * Written for moose-ripple from the interfaces declared in viogpu_pci.h and
 * VirtIO/virtio_pci.h. The upstream file with this job is not used in this repository
 * (see ../README.md).
 */
#include "viogpu.h"

static IVioGpuPCI *
PciOf(void *context)
{
    return static_cast<IVioGpuPCI *>(context);
}

/* ---- Device registers ------------------------------------------------------------------ *
 * The library hands over either an I/O port number (a legacy device, or a port range the
 * platform did not map) or the address of a mapped register. Ports fit in 16 bits. */

static bool
IsPort(ULONG_PTR reg)
{
    return (reg & ~(ULONG_PTR)PORT_MASK) == 0;
}

static u8
ReadByte(ULONG_PTR reg)
{
    return IsPort(reg) ? READ_PORT_UCHAR((PUCHAR)reg) : READ_REGISTER_UCHAR((PUCHAR)reg);
}

static u16
ReadWord(ULONG_PTR reg)
{
    return IsPort(reg) ? READ_PORT_USHORT((PUSHORT)reg) : READ_REGISTER_USHORT((PUSHORT)reg);
}

static u32
ReadDword(ULONG_PTR reg)
{
    return IsPort(reg) ? READ_PORT_ULONG((PULONG)reg) : READ_REGISTER_ULONG((PULONG)reg);
}

static void
WriteByte(ULONG_PTR reg, u8 value)
{
    if (IsPort(reg))
        WRITE_PORT_UCHAR((PUCHAR)reg, value);
    else
        WRITE_REGISTER_UCHAR((PUCHAR)reg, value);
}

static void
WriteWord(ULONG_PTR reg, u16 value)
{
    if (IsPort(reg))
        WRITE_PORT_USHORT((PUSHORT)reg, value);
    else
        WRITE_REGISTER_USHORT((PUSHORT)reg, value);
}

static void
WriteDword(ULONG_PTR reg, u32 value)
{
    if (IsPort(reg))
        WRITE_PORT_ULONG((PULONG)reg, value);
    else
        WRITE_REGISTER_ULONG((PULONG)reg, value);
}

/* ---- Memory ----------------------------------------------------------------------------- */

static void *
AllocContiguousPages(void *context, size_t size)
{
    UNREFERENCED_PARAMETER(context);
    PHYSICAL_ADDRESS highest;
    highest.QuadPart = -1;
    size = ROUND_TO_PAGES(size);
    void *p = MmAllocateContiguousMemory(size, highest);
    if (p)
        RtlZeroMemory(p, size);
    return p;
}

static void
FreeContiguousPages(void *context, void *virt)
{
    UNREFERENCED_PARAMETER(context);
    if (virt)
        MmFreeContiguousMemory(virt);
}

static ULONGLONG
GetPhysicalAddress(void *context, void *virt)
{
    UNREFERENCED_PARAMETER(context);
    return (ULONGLONG)MmGetPhysicalAddress(virt).QuadPart;
}

static void *
AllocNonPagedBlock(void *context, size_t size)
{
    UNREFERENCED_PARAMETER(context);
    void *p = ExAllocatePoolWithTag(NonPagedPool, size, VIOGPUTAG);
    if (p)
        RtlZeroMemory(p, size);
    return p;
}

static void
FreeNonPagedBlock(void *context, void *addr)
{
    UNREFERENCED_PARAMETER(context);
    if (addr)
        ExFreePoolWithTag(addr, VIOGPUTAG);
}

/* ---- PCI configuration space, through the display port driver --------------------------- */

static int
ReadConfig(void *context, int where, void *buffer, ULONG length)
{
    PDXGKRNL_INTERFACE dxgk = PciOf(context)->GetDxgkInterface();
    ULONG got = 0;
    NTSTATUS status = dxgk->DxgkCbReadDeviceSpace(dxgk->DeviceHandle, DXGK_WHICHSPACE_CONFIG, buffer,
                                                  (ULONG)where, length, &got);
    return NT_SUCCESS(status) && got == length ? 0 : -1;
}

static int
ReadConfigByte(void *context, int where, u8 *value)
{
    return ReadConfig(context, where, value, sizeof(*value));
}

static int
ReadConfigWord(void *context, int where, u16 *value)
{
    return ReadConfig(context, where, value, sizeof(*value));
}

static int
ReadConfigDword(void *context, int where, u32 *value)
{
    return ReadConfig(context, where, value, sizeof(*value));
}

/* ---- The device's ranges, interrupts, waiting -------------------------------------------- */

static size_t
GetResourceLength(void *context, int bar)
{
    if (bar < 0 || bar >= PCI_TYPE0_ADDRESSES)
        return 0;
    return PciOf(context)->GetPciResources()->GetBarSize((UINT)bar);
}

static void *
MapAddressRange(void *context, int bar, size_t offset, size_t maxlen)
{
    UNREFERENCED_PARAMETER(maxlen);
    if (bar < 0 || bar >= PCI_TYPE0_ADDRESSES)
        return NULL;
    return PciOf(context)->GetPciResources()->GetMappedAddress((UINT)bar, (ULONG)offset);
}

/* With message-signalled interrupts and a message for each: message 0 is the configuration
 * change, message n + 1 belongs to queue n (the order the adapter's interrupt routine reads
 * them in). With fewer messages than that, which is what Windows 7 allows a display adapter,
 * everything uses message 0. */
static u16
GetMsixVector(void *context, int queue)
{
    if (!PciOf(context)->IsMSIEnabled())
        return VIRTIO_MSI_NO_VECTOR;
    if (PciOf(context)->GetPciResources()->IsSharedMessage())
        return (u16)VIRTIO_GPU_MSIX_CONFIG_VECTOR;
    return queue < 0 ? (u16)VIRTIO_GPU_MSIX_CONFIG_VECTOR : (u16)(queue + 1);
}

static void
Sleep(void *context, unsigned int msecs)
{
    UNREFERENCED_PARAMETER(context);
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        LARGE_INTEGER interval;
        interval.QuadPart = -10000LL * msecs;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    else
    {
        KeStallExecutionProcessor(1000 * msecs);
    }
}

VirtIOSystemOps VioGpuSystemOps = {
    ReadByte,
    ReadWord,
    ReadDword,
    WriteByte,
    WriteWord,
    WriteDword,
    AllocContiguousPages,
    FreeContiguousPages,
    GetPhysicalAddress,
    AllocNonPagedBlock,
    FreeNonPagedBlock,
    ReadConfigByte,
    ReadConfigWord,
    ReadConfigDword,
    GetResourceLength,
    MapAddressRange,
    GetMsixVector,
    Sleep,
};

/* ---- One range of the device (a base address register) ----------------------------------- */

PVOID
CPciBar::GetVA(PDXGKRNL_INTERFACE pDxgkInterface)
{
    if (m_BaseVA != nullptr)
        return m_BaseVA;
    if (m_uSize == 0)
        return nullptr;
    if (m_bPortSpace && m_bIoMapped)
    {
        /* Real I/O ports: the "address" is the port number. */
        m_BaseVA = (PVOID)(ULONG_PTR)m_BasePA.LowPart;
        return m_BaseVA;
    }
    PVOID va = nullptr;
    NTSTATUS status = pDxgkInterface->DxgkCbMapMemory(pDxgkInterface->DeviceHandle, m_BasePA, m_uSize,
                                                      m_bPortSpace ? TRUE : FALSE, FALSE, MmNonCached, &va);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s DxgkCbMapMemory failed with status 0x%X\n", __FUNCTION__, status));
        return nullptr;
    }
    m_BaseVA = va;
    return m_BaseVA;
}

void
CPciBar::Unmap(PDXGKRNL_INTERFACE pDxgkInterface)
{
    if (m_BaseVA != nullptr && !(m_bPortSpace && m_bIoMapped) && pDxgkInterface != nullptr)
        pDxgkInterface->DxgkCbUnmapMemory(pDxgkInterface->DeviceHandle, m_BaseVA);
    m_BaseVA = nullptr;
}

/* ---- All ranges, from what Windows assigned ---------------------------------------------- *
 * The resource list gives addresses and lengths but not which base address register each
 * belongs to. The registers are read from the configuration space and matched by address;
 * a range that matches none takes the next free register in order (Windows lists them in
 * register order). */

static int
FindBar(const ULONG *bars, bool *used, PHYSICAL_ADDRESS start, bool port)
{
    for (int i = 0; i < PCI_TYPE0_ADDRESSES; i++)
    {
        ULONG raw = bars[i];
        bool is_port = (raw & PCI_ADDRESS_IO_SPACE) != 0;
        bool is_64 = !is_port && (raw & PCI_ADDRESS_MEMORY_TYPE_MASK) == PCI_TYPE_64BIT;
        ULONGLONG base = is_port ? (raw & PCI_ADDRESS_IO_ADDRESS_MASK) : (raw & PCI_ADDRESS_MEMORY_ADDRESS_MASK);
        if (is_64 && i + 1 < PCI_TYPE0_ADDRESSES)
            base |= (ULONGLONG)bars[i + 1] << 32;
        if (!used[i] && is_port == port && base != 0 && base == (ULONGLONG)start.QuadPart)
            return i;
        if (is_64)
            i++;
    }
    for (int i = 0; i < PCI_TYPE0_ADDRESSES; i++)
    {
        ULONG raw = bars[i];
        bool is_port = (raw & PCI_ADDRESS_IO_SPACE) != 0;
        bool is_64 = !is_port && (raw & PCI_ADDRESS_MEMORY_TYPE_MASK) == PCI_TYPE_64BIT;
        if (!used[i] && raw != 0 && is_port == port)
            return i;
        if (is_64)
            i++;
    }
    return -1;
}

bool
CPciResources::Init(PDXGKRNL_INTERFACE pDxgkInterface, PCM_RESOURCE_LIST pResList)
{
    m_pDxgkInterface = pDxgkInterface;
    if (pDxgkInterface == nullptr || pResList == nullptr || pResList->Count == 0)
        return false;

    PCI_COMMON_HEADER header;
    ULONG got = 0;
    RtlZeroMemory(&header, sizeof(header));
    NTSTATUS status = pDxgkInterface->DxgkCbReadDeviceSpace(pDxgkInterface->DeviceHandle, DXGK_WHICHSPACE_CONFIG,
                                                            &header, 0, sizeof(header), &got);
    if (!NT_SUCCESS(status) || got != sizeof(header))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s could not read the PCI header, status 0x%X\n", __FUNCTION__, status));
        return false;
    }

    bool used[PCI_TYPE0_ADDRESSES] = {};
    bool any = false;
    m_Messages = 0;
    PCM_PARTIAL_RESOURCE_LIST list = &pResList->List[0].PartialResourceList;
    for (ULONG i = 0; i < list->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &list->PartialDescriptors[i];
        switch (d->Type)
        {
            case CmResourceTypeInterrupt:
                m_InterruptFlags = d->Flags;
                // the translated list has one descriptor for each message
                if (d->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
                    m_Messages++;
                break;
            case CmResourceTypePort:
            case CmResourceTypeMemory:
                {
                    bool port = d->Type == CmResourceTypePort;
                    PHYSICAL_ADDRESS start = port ? d->u.Port.Start : d->u.Memory.Start;
                    ULONG length = port ? d->u.Port.Length : d->u.Memory.Length;
                    int bar = FindBar(header.u.type0.BaseAddresses, used, start, port);
                    if (bar < 0)
                    {
                        DbgPrint(TRACE_LEVEL_WARNING,
                                 ("%s no base address register for the range at 0x%I64X\n",
                                  __FUNCTION__,
                                  start.QuadPart));
                        break;
                    }
                    used[bar] = true;
                    /* A port range the platform maps as memory is reached like memory. */
                    bool io_mapped = !port || (d->Flags & CM_RESOURCE_PORT_MEMORY) == 0;
                    m_Bars[bar] = CPciBar(start, length, port, io_mapped);
                    any = true;
                    DbgPrint(TRACE_LEVEL_INFORMATION,
                             ("%s range %d: %s at 0x%I64X, 0x%X bytes\n",
                              __FUNCTION__,
                              bar,
                              port ? "ports" : "memory",
                              start.QuadPart,
                              length));
                    break;
                }
            default:
                break;
        }
    }
    return any;
}

PVOID
CPciResources::GetMappedAddress(UINT bar, ULONG uOffset)
{
    if (bar >= PCI_TYPE0_ADDRESSES || uOffset >= m_Bars[bar].GetSize())
        return nullptr;
    PVOID va = m_Bars[bar].GetVA(m_pDxgkInterface);
    return va != nullptr ? (PVOID)((PUCHAR)va + uOffset) : nullptr;
}

/* ---- The framebuffer of the device's VGA half -------------------------------------------- */

NTSTATUS
MapFrameBuffer(PHYSICAL_ADDRESS PhysicalAddress, ULONG Length, VOID **VirtualAddress)
{
    if (VirtualAddress == NULL || Length == 0 || PhysicalAddress.QuadPart == 0)
        return STATUS_INVALID_PARAMETER;
    *VirtualAddress = MmMapIoSpace(PhysicalAddress, Length, MmWriteCombined);
    if (*VirtualAddress == NULL)
        *VirtualAddress = MmMapIoSpace(PhysicalAddress, Length, MmNonCached);
    return *VirtualAddress != NULL ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

NTSTATUS
UnmapFrameBuffer(VOID *VirtualAddress, ULONG Length)
{
    if (VirtualAddress == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;
    MmUnmapIoSpace(VirtualAddress, Length);
    return STATUS_SUCCESS;
}
