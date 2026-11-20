/* Windows 2000 x86 miniport for QEMU virtio-vga. GDI uses the compatibility
 * framebuffer and DirectDraw uses host-rendered offscreen surfaces. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define DISPI_OFFSET 0x500
#define DISPI_BYTES 0x16
#define MIN_FRAME_BYTES (1024UL * 768 * 4)
#define MONITOR_ID 1

enum { VbeId, VbeX, VbeY, VbeBpp, VbeEnable, VbeBank, VbeWidth,
       VbeHeight, VbeOffsetX, VbeOffsetY };
static const struct { USHORT width, height; } modes[] = {
    {640, 480}, {800, 600}, {1024, 768}
};

typedef struct {
    VIDEO_ACCESS_RANGE fb;
    PUSHORT vbe;
    PVOID mappings[8];
    ULONG mode;
    BOOLEAN modeSet;
    RB2K_GPU gpu;
} ADAPTER;

static VP_STATUS find_adapter(PVOID ext, PVOID context, PWSTR args,
                              PVIDEO_PORT_CONFIG_INFO config, PUCHAR again)
{
    ADAPTER *a = ext;
    PCI_COMMON_CONFIG pci;
    VIDEO_ACCESS_RANGE ranges[16];
    VIDEO_ACCESS_RANGE *registers = NULL;
    PHYSICAL_ADDRESS regs;
    ULONG slot = 0, i, bar0, bar2;
    VP_STATUS status;
    USHORT id;
    (void)context; (void)args;
    *again = FALSE;
    if (config->Length < sizeof(*config)) return ERROR_INVALID_PARAMETER;

    VideoPortZeroMemory(ranges, sizeof(ranges));
    status = VideoPortGetAccessRanges(a, 0, NULL, ARRAY_COUNT(ranges),
                                      ranges, NULL, NULL, &slot);
    if (status != NO_ERROR) return status;
    if (VideoPortGetBusData(a, PCIConfiguration, slot, &pci, 0, PCI_COMMON_HDR_LENGTH)
        != PCI_COMMON_HDR_LENGTH || pci.VendorID != 0x1af4 || pci.DeviceID != 0x1050 ||
        pci.BaseClass != 3 || pci.SubClass != 0) return ERROR_DEV_NOT_EXIST;
    bar0 = pci.u.type0.BaseAddresses[0];
    bar2 = pci.u.type0.BaseAddresses[2];
    /* BAR0 is 32-bit. BAR2 is often 64-bit on this x86 machine. Match both
     * to bus-assigned memory ranges, never to an assumed physical address. */
    if ((bar0 & 7) != 0 || (bar2 & 1) || !(bar0 & ~15UL) || !(bar2 & ~15UL))
        return ERROR_DEV_NOT_EXIST;
    for (i = 0; i < ARRAY_COUNT(ranges); ++i) {
        VIDEO_ACCESS_RANGE *r = &ranges[i];
        if (r->RangeInIoSpace || !r->RangeLength) continue;
        if (r->RangeStart.HighPart == 0 &&
            r->RangeStart.LowPart == (bar0 & ~15UL) && r->RangeLength >= MIN_FRAME_BYTES)
            a->fb = *r;
        if (r->RangeStart.LowPart == (bar2 & ~15UL) &&
            (ULONG)r->RangeStart.HighPart ==
            ((bar2 & 6) == 4 ? pci.u.type0.BaseAddresses[3] : 0) &&
            r->RangeLength >= DISPI_OFFSET + DISPI_BYTES)
            registers = r;
    }
    if (!a->fb.RangeLength || !registers) return ERROR_DEV_NOT_EXIST;
    regs.QuadPart = registers->RangeStart.QuadPart + DISPI_OFFSET;
    a->vbe = VideoPortGetDeviceBase(a, regs, DISPI_BYTES, 0);
    if (!a->vbe) return ERROR_NOT_ENOUGH_MEMORY;
    id = VideoPortReadRegisterUshort(a->vbe + VbeId);
    if (id < 0xb0c0 || id > 0xb0c5) {
        VideoPortFreeDeviceBase(a, a->vbe);
        a->vbe = NULL;
        return ERROR_DEV_NOT_EXIST;
    }
    rb_gpu_find(&a->gpu, a, slot, registers, config);
    config->NumEmulatorAccessEntries = 0;
    config->HardwareStateSize = 0;
    return NO_ERROR;
}

static BOOLEAN initialize(PVOID ext)
{
    ADAPTER *a = ext;
    static WCHAR adapterName[] = L"Ripple NT5 QEMU virtio-vga framebuffer";
    VideoPortSetRegistryParameters(a, L"HardwareInformation.AdapterString",
                                   adapterName, sizeof(adapterName));
    VideoPortSetRegistryParameters(a, L"HardwareInformation.MemorySize",
                                   &a->fb.RangeLength, sizeof(ULONG));
    return TRUE;
}

static void mode_info(ULONG index, PVIDEO_MODE_INFORMATION m)
{
    VideoPortZeroMemory(m, sizeof(*m));
    m->Length = sizeof(*m);
    m->ModeIndex = index;
    m->VisScreenWidth = m->VideoMemoryBitmapWidth = modes[index].width;
    m->VisScreenHeight = m->VideoMemoryBitmapHeight = modes[index].height;
    m->ScreenStride = m->VisScreenWidth * 4;
    m->NumberOfPlanes = 1;
    m->BitsPerPlane = 32;
    m->Frequency = 60;
    m->XMillimeter = 320; m->YMillimeter = 240;
    m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
    m->RedMask = 0xff0000; m->GreenMask = 0xff00; m->BlueMask = 0xff;
    m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS | VIDEO_MODE_NO_OFF_SCREEN;
}

static VP_STATUS set_mode(ADAPTER *a, ULONG request)
{
    ULONG index = request & ~(VIDEO_MODE_NO_ZERO_MEMORY | VIDEO_MODE_MAP_MEM_LINEAR);
    USHORT enable = (request & VIDEO_MODE_NO_ZERO_MEMORY) ? 0xc1 : 0x41;
    VP_STATUS status;
    if (index >= ARRAY_COUNT(modes)) return ERROR_INVALID_PARAMETER;
    status = rb_gpu_surface_reset(&a->gpu);
    if (status) return status;
    VideoPortWriteRegisterUshort(a->vbe + VbeEnable, 0);
    VideoPortWriteRegisterUshort(a->vbe + VbeX, modes[index].width);
    VideoPortWriteRegisterUshort(a->vbe + VbeY, modes[index].height);
    VideoPortWriteRegisterUshort(a->vbe + VbeBpp, 32);
    VideoPortWriteRegisterUshort(a->vbe + VbeEnable, enable);
    VideoPortWriteRegisterUshort(a->vbe + VbeWidth, modes[index].width);
    VideoPortWriteRegisterUshort(a->vbe + VbeOffsetX, 0);
    VideoPortWriteRegisterUshort(a->vbe + VbeOffsetY, 0);
    if (VideoPortReadRegisterUshort(a->vbe + VbeX) != modes[index].width ||
        VideoPortReadRegisterUshort(a->vbe + VbeY) != modes[index].height ||
        VideoPortReadRegisterUshort(a->vbe + VbeBpp) != 32 ||
        !(VideoPortReadRegisterUshort(a->vbe + VbeEnable) & 1))
        return ERROR_INVALID_FUNCTION;
    a->mode = index;
    a->modeSet = TRUE;
    return NO_ERROR;
}

static VP_STATUS map_frame(ADAPTER *a, PVIDEO_REQUEST_PACKET r)
{
    PVIDEO_MEMORY_INFORMATION m = r->OutputBuffer;
    PVOID base = NULL;
    ULONG size = a->fb.RangeLength, io = 0, i;
    VP_STATUS status;
    if (r->InputBufferLength < sizeof(VIDEO_MEMORY) || r->OutputBufferLength < sizeof(*m))
        return ERROR_INSUFFICIENT_BUFFER;
    if (((PVIDEO_MEMORY)r->InputBuffer)->RequestedVirtualAddress)
        return ERROR_INVALID_PARAMETER;
    for (i = 0; i < ARRAY_COUNT(a->mappings); ++i) if (!a->mappings[i]) break;
    if (i == ARRAY_COUNT(a->mappings)) return ERROR_NOT_ENOUGH_MEMORY;
    status = VideoPortMapMemory(a, a->fb.RangeStart, &size, &io, &base);
    if (status != NO_ERROR) return status;
    a->mappings[i] = base;
    m->VideoRamBase = m->FrameBufferBase = base;
    m->VideoRamLength = m->FrameBufferLength = size;
    r->StatusBlock->Information = sizeof(*m);
    return NO_ERROR;
}

static VP_STATUS unmap_frame(ADAPTER *a, PVIDEO_REQUEST_PACKET r)
{
    PVOID base;
    ULONG i;
    if (r->InputBufferLength < sizeof(VIDEO_MEMORY)) return ERROR_INSUFFICIENT_BUFFER;
    base = ((PVIDEO_MEMORY)r->InputBuffer)->RequestedVirtualAddress;
    if (!base) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < ARRAY_COUNT(a->mappings); ++i) {
        if (a->mappings[i] == base) {
            VP_STATUS status = VideoPortUnmapMemory(a, base, NULL);
            if (status == NO_ERROR) a->mappings[i] = NULL;
            return status;
        }
    }
    return ERROR_INVALID_PARAMETER;
}

static BOOLEAN start_io(PVOID ext, PVIDEO_REQUEST_PACKET r)
{
    ADAPTER *a = ext;
    ULONG i;
    VP_STATUS status = ERROR_INSUFFICIENT_BUFFER;
    r->StatusBlock->Information = 0;
    switch (r->IoControlCode) {
    case IOCTL_RB2K_DD_QUERY:
    case IOCTL_RB2K_DD_CREATE:
    case IOCTL_RB2K_DD_FILL:
    case IOCTL_RB2K_DD_READ:
    case IOCTL_RB2K_DD_WRITE:
    case IOCTL_RB2K_DD_DESTROY:
        status = rb_gpu_surface_io(&a->gpu, r);
        break;
    case IOCTL_RB2K_GPU_PROBE:
        if (r->InputBufferLength) status = ERROR_INVALID_PARAMETER;
        else if (r->OutputBufferLength >= sizeof(RB2K_GPU_RESULT)) {
            rb_gpu_probe(&a->gpu, r->OutputBuffer);
            r->StatusBlock->Information = sizeof(RB2K_GPU_RESULT);
            status = NO_ERROR;
        }
        break;
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        if (r->OutputBufferLength >= sizeof(VIDEO_NUM_MODES)) {
            PVIDEO_NUM_MODES n = r->OutputBuffer;
            n->NumModes = ARRAY_COUNT(modes);
            n->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
            r->StatusBlock->Information = sizeof(*n);
            status = NO_ERROR;
        }
        break;
    case IOCTL_VIDEO_QUERY_AVAIL_MODES:
        if (r->OutputBufferLength >= ARRAY_COUNT(modes) * sizeof(VIDEO_MODE_INFORMATION)) {
            for (i = 0; i < ARRAY_COUNT(modes); ++i)
                mode_info(i, (PVIDEO_MODE_INFORMATION)r->OutputBuffer + i);
            r->StatusBlock->Information = ARRAY_COUNT(modes) * sizeof(VIDEO_MODE_INFORMATION);
            status = NO_ERROR;
        }
        break;
    case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        if (!a->modeSet) status = ERROR_INVALID_FUNCTION;
        else if (r->OutputBufferLength >= sizeof(VIDEO_MODE_INFORMATION)) {
            mode_info(a->mode, r->OutputBuffer);
            r->StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
            status = NO_ERROR;
        }
        break;
    case IOCTL_VIDEO_SET_CURRENT_MODE:
        if (r->InputBufferLength >= sizeof(VIDEO_MODE))
            status = set_mode(a, ((PVIDEO_MODE)r->InputBuffer)->RequestedMode);
        break;
    case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        status = map_frame(a, r);
        break;
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        status = unmap_frame(a, r);
        break;
    case IOCTL_VIDEO_RESET_DEVICE: {
        VIDEO_X86_BIOS_ARGUMENTS bios;
        status = rb_gpu_surface_reset(&a->gpu);
        if (status) break;
        VideoPortZeroMemory(&bios, sizeof(bios));
        bios.Eax = 3;
        status = VideoPortInt10(a, &bios);
        if (status == NO_ERROR) a->modeSet = FALSE;
        break;
    }
    default:
        status = ERROR_INVALID_FUNCTION;
    }
    r->StatusBlock->Status = status;
    return TRUE;
}

static VP_STATUS power(PVOID ext, ULONG id, PVIDEO_POWER_MANAGEMENT pm)
{
    (void)ext;
    if (id != DISPLAY_ADAPTER_HW_ID && id != MONITOR_ID) return ERROR_DEV_NOT_EXIST;
    return pm->PowerState == VideoPowerOn ? NO_ERROR : ERROR_INVALID_FUNCTION;
}

static ULONG child(PVOID ext, PVIDEO_CHILD_ENUM_INFO info, PVIDEO_CHILD_TYPE type,
                   PUCHAR desc, PULONG id, PULONG unused)
{
    (void)ext; (void)desc;
    *unused = 0;
    if (info->ChildIndex == DISPLAY_ADAPTER_HW_ID) {
        *type = VideoChip; *id = DISPLAY_ADAPTER_HW_ID;
    } else if (info->ChildIndex == 1) {
        *type = Monitor; *id = MONITOR_ID;
    } else return VIDEO_ENUM_NO_MORE_DEVICES;
    return VIDEO_ENUM_MORE_DEVICES;
}

ULONG DriverEntry(PVOID context1, PVOID context2)
{
    VIDEO_HW_INITIALIZATION_DATA init;
    VideoPortZeroMemory(&init, sizeof(init));
    init.HwInitDataSize = sizeof(init);
    init.AdapterInterfaceType = PCIBus;
    init.HwDeviceExtensionSize = sizeof(ADAPTER);
    init.HwFindAdapter = find_adapter;
    init.HwInitialize = initialize;
    init.HwStartIO = start_io;
    init.HwGetPowerState = power;
    init.HwSetPowerState = power;
    init.HwGetVideoChildDescriptor = child;
    return VideoPortInitialize(context1, context2, &init, NULL);
}
