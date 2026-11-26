/* Windows 2000 x86 miniport for QEMU virtio-vga. GDI uses the compatibility
 * framebuffer and DirectDraw uses host-rendered offscreen surfaces. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include "rb2krvd.h"
#include "gpu_memory.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define VGA_MMIO_OFFSET 0x400
#define DISPI_OFFSET 0x500
#define DISPI_BYTES 0x16
#define VBE_8BIT_DAC 0x20
#define MIN_FRAME_BYTES (1024UL * 768 * 4)
#define MONITOR_ID 1

enum { VbeId, VbeX, VbeY, VbeBpp, VbeEnable, VbeBank, VbeWidth,
       VbeHeight, VbeOffsetX, VbeOffsetY };
/* Keep the existing mode indices stable across driver updates: new modes
 * go at the end. A mode is offered when one page fits (see mode_fits). */
static const struct { USHORT width, height, bpp; } modes[] = {
    {640, 480, 32}, {800, 600, 32}, {1024, 768, 32},
    {640, 480, 16}, {800, 600, 16}, {1024, 768, 16},
    {640, 480, 8},  {800, 600, 8},  {1024, 768, 8},
    {1152, 864, 32}, {1280, 720, 32}, {1280, 800, 32}, {1280, 960, 32}, {1280, 1024, 32},
    {1440, 900, 32}, {1600, 900, 32}, {1600, 1200, 32}, {1680, 1050, 32},
    {1152, 864, 16}, {1280, 720, 16}, {1280, 800, 16}, {1280, 960, 16}, {1280, 1024, 16},
    {1440, 900, 16}, {1600, 900, 16}, {1600, 1200, 16}, {1680, 1050, 16},
    {1152, 864, 8}, {1280, 720, 8}, {1280, 800, 8}, {1280, 960, 8}, {1280, 1024, 8},
    {1440, 900, 8}, {1600, 900, 8}, {1600, 1200, 8}, {1680, 1050, 8}
,
    /* Only with the desktop in guest memory (RipplePrimaryInMemory): too big for VGA memory. */
    {1920, 1080, 32}, {1920, 1200, 32}
};
/* The largest page in the table, in bytes (1600x1200 at 32 bits). */
#define MAX_PAGE_BYTES (1600UL * 1200 * 4)

PVOID rb_view_current(void);
HANDLE rb_view_open(HANDLE supplied);
void rb_view_close(HANDLE handle);
PVOID rb_view_map(HANDLE process, unsigned __int64 physical, ULONG *bytes);
LONG rb_view_unmap(HANDLE process, PVOID address); /* NTSTATUS */
PVOID rb_view_map_pages(PVOID mdl);
void rb_view_unmap_pages(PVOID owner, PVOID address, PVOID mdl);

typedef struct {
    PVOID address, owner;
    HANDLE supplied, process;
    ULONG refs, bytes;
    BOOLEAN pages;      /* mapped from the desktop's MDL (in guest memory), not a physical range */
} FLIP_VIEW;

typedef struct {
    VIDEO_ACCESS_RANGE fb;
    PUCHAR vga;
    PUSHORT vbe;
    PVOID mappings[8];
    ULONG mode;
    BOOLEAN modeSet;
    FLIP_VIEW view;
    PVOID chainOwner;
    HANDLE chainProcess;
    ULONG flips;
    RB2K_GPU gpu;
} ADAPTER;

/* Native Direct3D 7 is on unless the adapter's registry key has
 * RippleNativeD3D7 = 0; its render targets, depth buffers and textures are
 * host images. Plain 2D offscreen surfaces as host textures are slower than
 * Windows' own in this design (the desktop is guest memory) and cannot be
 * blitted into the back buffer, so they are offered only when the key has
 * RippleDirectDrawGpuSurfaces = 1 (for tests). With native Direct3D 7, the
 * Direct3D 8 driver interface is offered too unless RippleD3D8 = 0. */
static VP_STATUS registry_flag(PVOID ext, PVOID context, PWSTR name, PVOID data, ULONG bytes)
{
    (void)ext; (void)name;
    if (bytes == sizeof(ULONG)) *(BOOLEAN *)context = *(PULONG)data != 0;
    return NO_ERROR;
}

static VP_STATUS find_adapter(PVOID ext, PVOID context, PWSTR args,
                              PVIDEO_PORT_CONFIG_INFO config, PUCHAR again)
{
    ADAPTER *a = ext;
    PCI_COMMON_CONFIG pci;
    VIDEO_ACCESS_RANGE ranges[16];
    VIDEO_ACCESS_RANGE *registers = NULL;
    PHYSICAL_ADDRESS regs, bar2Address;
    ULONG slot = 0, i, bar0, bar2, bar2Type;
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
    bar2Type = bar2 & PCI_ADDRESS_MEMORY_TYPE_MASK;
    if ((bar0 & (PCI_ADDRESS_IO_SPACE | PCI_ADDRESS_MEMORY_TYPE_MASK)) != PCI_TYPE_32BIT ||
        (bar2 & PCI_ADDRESS_IO_SPACE) || bar2Type == PCI_ADDRESS_MEMORY_TYPE_MASK)
        return ERROR_DEV_NOT_EXIST;
    bar0 &= PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    /* A 64-bit BAR can start on a 4 GiB boundary with a zero low dword. */
    bar2Address.LowPart = bar2 & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    bar2Address.HighPart = bar2Type == PCI_TYPE_64BIT ? (LONG)pci.u.type0.BaseAddresses[3] : 0;
    if (!bar0 || !bar2Address.QuadPart)
        return ERROR_DEV_NOT_EXIST;
    for (i = 0; i < ARRAY_COUNT(ranges); ++i) {
        VIDEO_ACCESS_RANGE *r = &ranges[i];
        if (r->RangeInIoSpace || !r->RangeLength) continue;
        if (r->RangeStart.HighPart == 0 &&
            r->RangeStart.LowPart == bar0 && r->RangeLength >= MIN_FRAME_BYTES)
            a->fb = *r;
        if (r->RangeStart.QuadPart == bar2Address.QuadPart &&
            r->RangeLength >= DISPI_OFFSET + DISPI_BYTES)
            registers = r;
    }
    if (!a->fb.RangeLength || !registers) return ERROR_DEV_NOT_EXIST;
    /* BAR2 mirrors VGA ports 3C0-3DF at 400h and the VBE registers at 500h. */
    regs.QuadPart = registers->RangeStart.QuadPart + VGA_MMIO_OFFSET;
    a->vga = VideoPortGetDeviceBase(a, regs, DISPI_OFFSET - VGA_MMIO_OFFSET + DISPI_BYTES, 0);
    if (!a->vga) return ERROR_NOT_ENOUGH_MEMORY;
    a->vbe = (PUSHORT)(a->vga + (DISPI_OFFSET - VGA_MMIO_OFFSET));
    id = VideoPortReadRegisterUshort(a->vbe + VbeId);
    if (id < 0xb0c0 || id > 0xb0c5) {
        VideoPortFreeDeviceBase(a, a->vga);
        a->vga = NULL;
        a->vbe = NULL;
        return ERROR_DEV_NOT_EXIST;
    }
    rb_gpu_find(&a->gpu, a, slot, registers, config);
    a->gpu.nativeD3D7 = TRUE;
    a->gpu.d3d8 = TRUE;
    a->gpu.hostDxt = TRUE;
    VideoPortGetRegistryParameters(a, L"RippleDirectDrawGpuSurfaces", FALSE, registry_flag, &a->gpu.plainSurfaces);
    VideoPortGetRegistryParameters(a, L"RippleGpuDesktop", FALSE, registry_flag, &a->gpu.glEnabled);
    VideoPortGetRegistryParameters(a, L"RippleHardwareCursor", FALSE, registry_flag, &a->gpu.cursorEnabled);
    VideoPortGetRegistryParameters(a, L"RippleNativeD3D7", FALSE, registry_flag, &a->gpu.nativeD3D7);
    VideoPortGetRegistryParameters(a, L"RippleD3D7WaitCurrent", FALSE, registry_flag, &a->gpu.d7WaitCurrent);
    VideoPortGetRegistryParameters(a, L"RippleD3D8", FALSE, registry_flag, &a->gpu.d3d8);
    VideoPortGetRegistryParameters(a, L"RippleHostDxt", FALSE, registry_flag, &a->gpu.hostDxt);
    VideoPortGetRegistryParameters(a, L"RippleVertexShaders", FALSE, registry_flag, &a->gpu.vertexShaders);
    VideoPortGetRegistryParameters(a, L"RippleD3D9", FALSE, registry_flag, &a->gpu.d3d9Ddi);
    VideoPortGetRegistryParameters(a, L"RipplePrimaryInMemory", FALSE, registry_flag, &a->gpu.primaryInMemory);
    /* The GPU desktop composes into the VGA desktop; the two do not mix. */
    if (a->gpu.primaryInMemory) a->gpu.glEnabled = FALSE;
    a->gpu.ddGpuSurfaces = a->gpu.plainSurfaces || a->gpu.nativeD3D7;
    /* Map the scanout pages during adapter discovery, cached like every other
     * view of the framebuffer (rb_map_cached). BAR0 is a PCI framebuffer; its
     * address is not a general-purpose DMA backing entry. */
    if (a->gpu.common) {
        ULONG bytes = a->fb.RangeLength < 2 * MAX_PAGE_BYTES ? a->fb.RangeLength : 2 * MAX_PAGE_BYTES;
        a->gpu.primary.pixels = rb_map_cached((unsigned __int64)a->fb.RangeStart.QuadPart, bytes);
        if (!a->gpu.primary.pixels && bytes != MIN_FRAME_BYTES) {
            bytes = MIN_FRAME_BYTES;
            a->gpu.primary.pixels = rb_map_cached((unsigned __int64)a->fb.RangeStart.QuadPart, bytes);
        }
        if (a->gpu.primary.pixels) a->gpu.primary.bytes = bytes;
        a->gpu.vgaPixels = a->gpu.primary.pixels;
        a->gpu.vgaBytes = a->gpu.primary.bytes;
    }
    config->NumEmulatorAccessEntries = 0;
    config->HardwareStateSize = 0;
    return NO_ERROR;
}

static BOOLEAN initialize(PVOID ext)
{
    ADAPTER *a = ext;
    static WCHAR adapterName[] = L"moose-ripple virtio-vga";
    VideoPortSetRegistryParameters(a, L"HardwareInformation.AdapterString",
                                   adapterName, sizeof(adapterName));
    VideoPortSetRegistryParameters(a, L"HardwareInformation.MemorySize",
                                   &a->fb.RangeLength, sizeof(ULONG));
    return TRUE;
}

/* A mode needs one page of framebuffer; a 32-bit page must also fit the
 * miniport's own framebuffer view and the largest host image. DirectDraw
 * page flipping needs a second page, and scanout_info reports one page
 * where it does not fit: QEMU's virtio-vga has 8 MB, so 32-bit modes above
 * 1280x800 have a desktop but no flip chain. */
/* With RipplePrimaryInMemory, every 32-bit desktop lives in guest memory
 * (gpu_primary.c), up to 1920x1200; the others stay in VGA memory. */
static BOOLEAN in_memory(const ADAPTER *a, ULONG index)
{
    return a->gpu.primaryInMemory && a->gpu.common && index < ARRAY_COUNT(modes) && modes[index].bpp == 32 &&
           (ULONG)modes[index].width * modes[index].height * 4 <= RB2K_MEMORY_PRIMARY_BYTES;
}

static BOOLEAN mode_fits(const ADAPTER *a, ULONG index)
{
    ULONG page;
    if (index >= ARRAY_COUNT(modes)) return FALSE;
    if (in_memory(a, index)) return TRUE;
    if (index < 9) return TRUE;
    page = (ULONG)modes[index].width * modes[index].height * (modes[index].bpp / 8);
    if (page > a->fb.RangeLength) return FALSE;
    return modes[index].bpp != 32 ||
           (page <= RB2K_DD_MAX_BYTES && (!a->gpu.vgaPixels || page <= a->gpu.vgaBytes));
}

static ULONG fitting_modes(const ADAPTER *a)
{
    ULONG i, n = 0;
    for (i = 0; i < ARRAY_COUNT(modes); ++i) n += mode_fits(a, i);
    return n;
}

static void mode_info(const ADAPTER *a, ULONG index, PVIDEO_MODE_INFORMATION m)
{
    VideoPortZeroMemory(m, sizeof(*m));
    if (in_memory(a, index)) m->DriverSpecificAttributeFlags = RB2K_MODE_PRIMARY_IN_MEMORY;
    m->Length = sizeof(*m);
    m->ModeIndex = index;
    m->VisScreenWidth = m->VideoMemoryBitmapWidth = modes[index].width;
    m->VisScreenHeight = m->VideoMemoryBitmapHeight = modes[index].height;
    m->NumberOfPlanes = 1;
    m->BitsPerPlane = modes[index].bpp;
    m->ScreenStride = m->VisScreenWidth * (m->BitsPerPlane / 8);
    m->Frequency = 60;
    m->XMillimeter = 320; m->YMillimeter = 240;
    m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS | VIDEO_MODE_NO_OFF_SCREEN;
    if (m->BitsPerPlane == 8) {
        m->AttributeFlags |= VIDEO_MODE_PALETTE_DRIVEN | VIDEO_MODE_MANAGED_PALETTE;
        m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
    } else if (m->BitsPerPlane == 16) {
        m->NumberRedBits = m->NumberBlueBits = 5; m->NumberGreenBits = 6;
        m->RedMask = 0xf800; m->GreenMask = 0x07e0; m->BlueMask = 0x001f;
    } else {
        m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
        m->RedMask = 0xff0000; m->GreenMask = 0xff00; m->BlueMask = 0xff;
    }
}

static VP_STATUS set_colors(ADAPTER *a, PVIDEO_CLUT clut, ULONG bytes)
{
    ULONG header = FIELD_OFFSET(VIDEO_CLUT, LookupTable), i;
    if (bytes < header || !clut) return ERROR_INSUFFICIENT_BUFFER;
    if (!clut->NumEntries || clut->FirstEntry >= 256 ||
        clut->NumEntries > 256 - clut->FirstEntry) return ERROR_INVALID_PARAMETER;
    if (clut->NumEntries > (bytes - header) / sizeof(clut->LookupTable[0]))
        return ERROR_INSUFFICIENT_BUFFER;
    if (!a->modeSet || modes[a->mode].bpp != 8) return ERROR_NOT_READY;
    VideoPortWriteRegisterUchar(a->vga + 8, (UCHAR)clut->FirstEntry);
    for (i = 0; i < clut->NumEntries; ++i) {
        VIDEO_CLUTDATA *color = &clut->LookupTable[i].RgbArray;
        VideoPortWriteRegisterUchar(a->vga + 9, color->Red);
        VideoPortWriteRegisterUchar(a->vga + 9, color->Green);
        VideoPortWriteRegisterUchar(a->vga + 9, color->Blue);
    }
    return NO_ERROR;
}

/* NT5 may round a user view to the 64 KiB allocation granularity. The only
 * bytes in that padding belong to unused VGA RAM and are scrubbed as well. */
static ULONG view_span(ULONG pageBytes)
{
    return (2 * pageBytes + 0xffffUL) & ~0xffffUL;
}

static VP_STATUS scanout_info(ADAPTER *a, RB2K_DD_FLIP_INFO *info)
{
    ULONG virtualWidth;
    if (a->modeSet && a->gpu.memoryWidth) {
        /* The desktop in guest memory: two pages, each with its own host resource. */
        info->width = modes[a->mode].width;
        info->height = modes[a->mode].height;
        info->pitch = info->width * sizeof(ULONG);
        info->pageBytes = info->pitch * info->height;
        info->pageCount = 2 * info->pageBytes <= a->gpu.memoryBytes ? 2 : 1;
        info->visiblePage = a->gpu.memoryVisible;
        info->yOffset = info->visiblePage * info->height;
        info->flips = a->flips;
        return NO_ERROR;
    }
    if (!a->modeSet || !(VideoPortReadRegisterUshort(a->vbe + VbeEnable) & 1) ||
        VideoPortReadRegisterUshort(a->vbe + VbeBpp) != modes[a->mode].bpp ||
        VideoPortReadRegisterUshort(a->vbe + VbeOffsetX)) return ERROR_NOT_READY;
    info->width = VideoPortReadRegisterUshort(a->vbe + VbeX);
    info->height = VideoPortReadRegisterUshort(a->vbe + VbeY);
    virtualWidth = VideoPortReadRegisterUshort(a->vbe + VbeWidth);
    if (info->width != modes[a->mode].width || info->height != modes[a->mode].height ||
        virtualWidth != info->width) return ERROR_NOT_READY;
    info->pitch = virtualWidth * (modes[a->mode].bpp / 8);
    info->pageBytes = info->pitch * info->height;
    info->pageCount = a->gpu.primary.pixels &&
        view_span(info->pageBytes) <= a->gpu.primary.bytes &&
        VideoPortReadRegisterUshort(a->vbe + VbeHeight) >= 2 * info->height ? 2 : 1;
    info->yOffset = VideoPortReadRegisterUshort(a->vbe + VbeOffsetY);
    info->visiblePage = info->yOffset == 0 ? 0 :
        (info->pageCount == 2 && info->yOffset == info->height ? 1 : (ULONG)-1);
    info->flips = a->flips;
    return NO_ERROR;
}

static VP_STATUS select_page(ADAPTER *a, ULONG page, const RB2K_DD_FLIP_INFO *info)
{
    USHORT offset;
    if (page >= info->pageCount) return ERROR_INVALID_PARAMETER;
    if (a->gpu.memoryWidth) {
        VP_STATUS status;
        if (page == info->visiblePage) return NO_ERROR;
        status = rb_memory_primary_flip(&a->gpu, page);
        if (!status) ++a->flips;
        return status;
    }
    offset = (USHORT)(page * info->height);
    if (offset == info->yOffset) return NO_ERROR;
    VideoPortWriteRegisterUshort(a->vbe + VbeOffsetY, offset);
    if (VideoPortReadRegisterUshort(a->vbe + VbeOffsetY) != offset) return ERROR_WRITE_FAULT;
    ++a->flips;
    return NO_ERROR;
}

static VP_STATUS revoke_view(ADAPTER *a)
{
    VP_STATUS status;
    if (!a->view.address) return NO_ERROR;
    if (a->view.pages) {
        rb_view_unmap_pages(a->view.owner, a->view.address, a->gpu.memoryMdl);
        status = NO_ERROR;
    } else
        status = rb_view_unmap(a->view.process, a->view.address) < 0 ? ERROR_INVALID_PARAMETER : NO_ERROR;
    if (!status) {
        rb_view_close(a->view.process);
        VideoPortZeroMemory(&a->view, sizeof(a->view));
    }
    return status;
}

static void clear_back_page(ADAPTER *a, const RB2K_DD_FLIP_INFO *info)
{
    if (a->gpu.memoryWidth) rb_memory_primary_clear_back(&a->gpu);
    else if (info->pageCount == 2)
        VideoPortZeroDeviceMemory((PUCHAR)a->gpu.primary.pixels + info->pageBytes,
                                  view_span(info->pageBytes) - info->pageBytes);
}

static VP_STATUS acquire_pages(ADAPTER *a)
{
    RB2K_DD_FLIP_INFO info;
    PVOID owner = rb_view_current();
    VP_STATUS status = scanout_info(a, &info);
    if (status) return status;
    if (info.pageCount != 2 || info.visiblePage != 0) return ERROR_NOT_SUPPORTED;
    if (a->chainOwner) return ERROR_BUSY;
    /* A view kept from this process's earlier chain stays: DirectDraw maps once
     * per session and goes on using that address. Another process's goes. */
    if (a->view.address && a->view.owner != owner) {
        status = revoke_view(a);
        if (status) return status;
    }
    a->chainProcess = rb_view_open((HANDLE)-1);
    if (!a->chainProcess) return ERROR_ACCESS_DENIED;
    a->chainOwner = owner;
    clear_back_page(a, &info);
    if (a->gpu.memoryWidth) rb_memory_primary_chain(&a->gpu, TRUE);
    return NO_ERROR;
}

static VP_STATUS release_pages(ADAPTER *a, BOOLEAN keepView)
{
    RB2K_DD_FLIP_INFO info;
    VP_STATUS status = keepView ? NO_ERROR : revoke_view(a);
    if (status) return status;
    if (!scanout_info(a, &info)) {
        status = select_page(a, 0, &info);
        if (!status) clear_back_page(a, &info);
    }
    if (!status && a->gpu.memoryWidth) rb_memory_primary_chain(&a->gpu, FALSE);
    if (!status && a->chainProcess) {
        rb_view_close(a->chainProcess);
        a->chainProcess = NULL;
        a->chainOwner = NULL;
    }
    return status;
}

static VP_STATUS set_mode(ADAPTER *a, ULONG request)
{
    RB2K_DD_FLIP_INFO info;
    BOOLEAN memory;
    ULONG index = request & ~(VIDEO_MODE_NO_ZERO_MEMORY | VIDEO_MODE_MAP_MEM_LINEAR);
    USHORT enable = (request & VIDEO_MODE_NO_ZERO_MEMORY) ? 0xc1 : 0x41;
    VP_STATUS status;
    if (!mode_fits(a, index)) return ERROR_INVALID_PARAMETER;
    if (modes[index].bpp == 8) enable |= VBE_8BIT_DAC;
    status = release_pages(a, FALSE);
    if (status) return status;
    memory = in_memory(a, index);
    /* Leaving the desktop in memory: the transport reset in rb_gpu_surface_reset
     * returns scanout 0 to VGA (unless OpenGL holds the transport). */
    if (!memory && a->gpu.memoryWidth) rb_memory_primary_hide(&a->gpu);
    status = rb_gpu_surface_reset(&a->gpu);
    if (status) return status;
    a->modeSet = FALSE;
    if (memory) {
        /* Scanout 0 shows the host resource; VGA output is not used. */
        VideoPortWriteRegisterUshort(a->vbe + VbeEnable, 0);
        status = rb_memory_primary_show(&a->gpu, modes[index].width, modes[index].height,
                                        (BOOLEAN)!(request & VIDEO_MODE_NO_ZERO_MEMORY));
        if (status) return status;
        a->mode = index;
        a->modeSet = TRUE;
        a->gpu.primary.pixels = a->gpu.memoryPixels;
        a->gpu.primary.bytes = a->gpu.memoryBytes;
        a->gpu.primary.bitsPerPixel = 32;
        a->gpu.primary.width = a->gpu.primary.pitchPixels = modes[index].width;
        a->gpu.primary.height = modes[index].height;
        return NO_ERROR;
    }
    a->gpu.primary.pixels = a->gpu.vgaPixels;
    a->gpu.primary.bytes = a->gpu.vgaBytes;
    VideoPortWriteRegisterUshort(a->vbe + VbeEnable, 0);
    VideoPortWriteRegisterUshort(a->vbe + VbeX, modes[index].width);
    VideoPortWriteRegisterUshort(a->vbe + VbeY, modes[index].height);
    VideoPortWriteRegisterUshort(a->vbe + VbeBpp, modes[index].bpp);
    VideoPortWriteRegisterUshort(a->vbe + VbeEnable, enable);
    VideoPortWriteRegisterUshort(a->vbe + VbeWidth, modes[index].width);
    VideoPortWriteRegisterUshort(a->vbe + VbeOffsetX, 0);
    VideoPortWriteRegisterUshort(a->vbe + VbeOffsetY, 0);
    if (VideoPortReadRegisterUshort(a->vbe + VbeX) != modes[index].width ||
        VideoPortReadRegisterUshort(a->vbe + VbeY) != modes[index].height ||
        VideoPortReadRegisterUshort(a->vbe + VbeBpp) != modes[index].bpp ||
        (VideoPortReadRegisterUshort(a->vbe + VbeEnable) & (VBE_8BIT_DAC | 1)) !=
            (enable & (VBE_8BIT_DAC | 1)))
        return ERROR_INVALID_FUNCTION;
    a->mode = index;
    a->modeSet = TRUE;
    a->gpu.primary.bitsPerPixel = modes[index].bpp;
    a->gpu.primary.width = modes[index].width;
    a->gpu.primary.height = modes[index].height;
    a->gpu.primary.pitchPixels = VideoPortReadRegisterUshort(a->vbe + VbeWidth);
    if (!scanout_info(a, &info)) clear_back_page(a, &info);
    return NO_ERROR;
}

static VP_STATUS share_pages(ADAPTER *a, PVIDEO_REQUEST_PACKET r)
{
    VIDEO_SHARE_MEMORY requested;
    PVIDEO_SHARE_MEMORY_INFORMATION output = r->OutputBuffer;
    RB2K_DD_FLIP_INFO info;
    HANDLE process;
    PVOID address, owner;
    ULONG bytes, mapped;
    VP_STATUS status;
    if (r->InputBufferLength != sizeof(requested) || r->OutputBufferLength < sizeof(*output))
        return ERROR_INVALID_PARAMETER;
    requested = *(PVIDEO_SHARE_MEMORY)r->InputBuffer; /* Buffered input and output may alias. */
    status = scanout_info(a, &info);
    if (status) return status;
    if (info.pageCount != 2) return ERROR_NOT_SUPPORTED;
    bytes = 2 * info.pageBytes;
    if (!requested.ProcessHandle || requested.ViewOffset || requested.RequestedVirtualAddress ||
        requested.ViewSize != bytes) return ERROR_INVALID_PARAMETER;
    owner = rb_view_current();
    /* Windowed DirectDraw surfaces use their own user buffers. Mapping a
     * scanout page here is exclusively for the live flipping chain. */
    if (a->chainOwner != owner) return ERROR_ACCESS_DENIED;
    if (a->view.address) {
        if (a->view.owner != owner || a->view.supplied != requested.ProcessHandle ||
            a->view.bytes != bytes || a->view.refs == (ULONG)-1) return ERROR_BUSY;
        ++a->view.refs;
    } else {
        process = rb_view_open(requested.ProcessHandle);
        if (!process) return ERROR_ACCESS_DENIED;
        if (a->gpu.memoryWidth) {
            /* The desktop in guest memory: all of its pages, into this process (the only
             * one rb_view_open accepts). */
            mapped = a->gpu.memoryBytes;
            address = rb_view_map_pages(a->gpu.memoryMdl);
        } else {
            mapped = view_span(info.pageBytes);
            address = rb_view_map(process, (unsigned __int64)a->fb.RangeStart.QuadPart, &mapped);
        }
        if (!address) { rb_view_close(process); return ERROR_NOT_ENOUGH_MEMORY; }
        a->view.process = process;
        a->view.supplied = requested.ProcessHandle;
        a->view.owner = owner;
        a->view.address = address;
        a->view.bytes = bytes;
        a->view.refs = 1;
        a->view.pages = a->gpu.memoryWidth != 0;
        /* Return only a complete user-space view of the declared pages. */
        if (mapped < bytes || (!a->view.pages && mapped > view_span(info.pageBytes)) ||
            (ULONG)address >= 0x80000000UL || mapped > 0x80000000UL - (ULONG)address) {
            revoke_view(a);
            return ERROR_INVALID_DATA;
        }
    }
    output->SharedViewOffset = 0;
    output->SharedViewSize = bytes;
    output->VirtualAddress = a->view.address;
    r->StatusBlock->Information = sizeof(*output);
    return NO_ERROR;
}

static VP_STATUS unshare_pages(ADAPTER *a, PVIDEO_REQUEST_PACKET r)
{
    PVIDEO_SHARE_MEMORY request = r->InputBuffer;
    if (r->InputBufferLength != sizeof(*request) || r->OutputBufferLength ||
        !request->ProcessHandle || !request->RequestedVirtualAddress ||
        request->ViewOffset || request->ViewSize) return ERROR_INVALID_PARAMETER;
    /* A mode change or another process's chain can already have revoked it. */
    if (!a->view.address || a->view.address != request->RequestedVirtualAddress ||
        a->view.owner != rb_view_current()) return NO_ERROR;
    if (a->view.supplied != request->ProcessHandle) return ERROR_INVALID_HANDLE;
    if (a->view.refs == 1) return revoke_view(a);
    --a->view.refs;
    return NO_ERROR;
}

static VP_STATUS map_frame(ADAPTER *a, PVIDEO_REQUEST_PACKET r)
{
    PVIDEO_MEMORY_INFORMATION m = r->OutputBuffer;
    PVOID base = NULL;
    ULONG size = a->fb.RangeLength, i;
    if (r->InputBufferLength < sizeof(VIDEO_MEMORY) || r->OutputBufferLength < sizeof(*m))
        return ERROR_INSUFFICIENT_BUFFER;
    if (((PVIDEO_MEMORY)r->InputBuffer)->RequestedVirtualAddress)
        return ERROR_INVALID_PARAMETER;
    if (a->modeSet && a->gpu.memoryWidth) {
        /* The desktop in guest memory: its pages are mapped once, in system space. */
        m->VideoRamBase = m->FrameBufferBase = a->gpu.memoryPixels;
        m->VideoRamLength = m->FrameBufferLength = a->gpu.memoryBytes;
        r->StatusBlock->Information = sizeof(*m);
        return NO_ERROR;
    }
    for (i = 0; i < ARRAY_COUNT(a->mappings); ++i) if (!a->mappings[i]) break;
    if (i == ARRAY_COUNT(a->mappings)) return ERROR_NOT_ENOUGH_MEMORY;
    base = rb_map_cached((unsigned __int64)a->fb.RangeStart.QuadPart, size);
    if (!base) return ERROR_NOT_ENOUGH_MEMORY;
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
    if (base == (PVOID)a->gpu.memoryPixels) return NO_ERROR;   /* kept for good (map_frame) */
    for (i = 0; i < ARRAY_COUNT(a->mappings); ++i) {
        if (a->mappings[i] == base) {
            rb_unmap_cached(base, a->fb.RangeLength);
            a->mappings[i] = NULL;
            return NO_ERROR;
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
    case IOCTL_RB2K_DD_FLIP_INFO:
        if (r->InputBufferLength || r->OutputBufferLength < sizeof(RB2K_DD_FLIP_INFO))
            status = ERROR_INVALID_PARAMETER;
        else {
            status = scanout_info(a, r->OutputBuffer);
            if (!status) r->StatusBlock->Information = sizeof(RB2K_DD_FLIP_INFO);
        }
        break;
    case IOCTL_RB2K_DD_FLIP: {
        RB2K_DD_FLIP_INFO info;
        if (r->InputBufferLength != sizeof(ULONG) || r->OutputBufferLength)
            status = ERROR_INVALID_PARAMETER;
        else if (*(PULONG)r->InputBuffer && a->chainOwner != rb_view_current()) status = ERROR_ACCESS_DENIED;
        else {
            status = scanout_info(a, &info);
            if (!status) status = select_page(a, *(PULONG)r->InputBuffer, &info);
        }
        break;
    }
    case IOCTL_RB2K_DD_FLIP_RELEASE:
        /* The owner's view stays until DirectDraw unmaps it, another process
         * takes the pages or the mode changes (a rebuilt chain reuses it). */
        status = r->InputBufferLength || r->OutputBufferLength ? ERROR_INVALID_PARAMETER : release_pages(a, TRUE);
        break;
    case IOCTL_RB2K_DD_FLIP_ACQUIRE:
        status = r->InputBufferLength || r->OutputBufferLength ? ERROR_INVALID_PARAMETER : acquire_pages(a);
        break;
    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY:
        status = share_pages(a, r);
        break;
    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY:
        status = unshare_pages(a, r);
        break;
    case IOCTL_RB2K_DD_QUERY:
    case IOCTL_RB2K_DD_CREATE:
    case IOCTL_RB2K_D3D7_COMPLETED:
    case IOCTL_RB2K_D3D7_FLUSH:
    case IOCTL_RB2K_D3D7_FLIGHT:
    case IOCTL_RB2K_D3D7_PROFILE:
    case IOCTL_RB2K_D3D7_CLEAR:
    case IOCTL_RB2K_D3D7_DRAW:
    case IOCTL_RB2K_D3D7_DRAWS:
    case IOCTL_RB2K_D3D7_TEXTURE:
    case IOCTL_RB2K_D3D7_ZCLEAR:
    case IOCTL_RB2K_D3D7_TEXTURE_FREE:
    case IOCTL_RB2K_D3D8_PROGRAM:
    case IOCTL_RB2K_D3D8_PROGRAM_FREE:
    case IOCTL_RB2K_D7_CUBE_SYNC:
    case IOCTL_RB2K_DD_CREATE_Z16:
    case IOCTL_RB2K_DD_CREATE_Z24S8:
    case IOCTL_RB2K_DD_FILL:
    case IOCTL_RB2K_DD_READ:
    case IOCTL_RB2K_DD_WRITE:
    case IOCTL_RB2K_DD_DESTROY:
    case IOCTL_RB2K_DD_COPY:
    case IOCTL_RB2K_DD_STRETCH:
    case IOCTL_RB2K_DD_PRESENT:
    case IOCTL_RB2K_DD_UNIFORM:
        status = rb_gpu_surface_io(&a->gpu, r);
        break;
    case IOCTL_RB2K_RVD:
    case IOCTL_RB2K_GL_FRAME:
        status = rb_gpu_rvd_io(&a->gpu, r);
        break;
    case IOCTL_RB2K_CURSOR:
        status = rb_cursor_io(&a->gpu, r);
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
            n->NumModes = fitting_modes(a);
            n->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
            r->StatusBlock->Information = sizeof(*n);
            status = NO_ERROR;
        }
        break;
    case IOCTL_VIDEO_QUERY_AVAIL_MODES:
        if (r->OutputBufferLength >= fitting_modes(a) * sizeof(VIDEO_MODE_INFORMATION)) {
            ULONG n = 0;
            for (i = 0; i < ARRAY_COUNT(modes); ++i)
                if (mode_fits(a, i)) mode_info(a, i, (PVIDEO_MODE_INFORMATION)r->OutputBuffer + n++);
            r->StatusBlock->Information = n * sizeof(VIDEO_MODE_INFORMATION);
            status = NO_ERROR;
        }
        break;
    case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        if (!a->modeSet) status = ERROR_INVALID_FUNCTION;
        else if (r->OutputBufferLength >= sizeof(VIDEO_MODE_INFORMATION)) {
            mode_info(a, a->mode, r->OutputBuffer);
            r->StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
            status = NO_ERROR;
        }
        break;
    case IOCTL_VIDEO_SET_CURRENT_MODE:
        if (r->InputBufferLength >= sizeof(VIDEO_MODE))
            status = set_mode(a, ((PVIDEO_MODE)r->InputBuffer)->RequestedMode);
        break;
    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        status = set_colors(a, r->InputBuffer, r->InputBufferLength);
        break;
    case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        status = map_frame(a, r);
        break;
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        status = unmap_frame(a, r);
        break;
    case IOCTL_VIDEO_RESET_DEVICE: {
        VIDEO_X86_BIOS_ARGUMENTS bios;
        status = release_pages(a, FALSE);
        if (status) break;
        /* Text mode shows on VGA only after the transport reset. */
        if (a->gpu.memoryWidth) rb_memory_primary_hide(&a->gpu);
        a->gpu.primary.pixels = a->gpu.vgaPixels;
        a->gpu.primary.bytes = a->gpu.vgaBytes;
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

#ifndef STATUS_REVISION_MISMATCH
#define STATUS_REVISION_MISMATCH 0xC0000059L
#endif

ULONG DriverEntry(PVOID context1, PVOID context2)
{
    /* Windows XP's init data is one ULONG longer. Registered with it, XP keeps
     * adapter settings under Control\Video\{GUID}\0000. The Windows 2000
     * layout selects the legacy Services\rb2kmp\Device0 path on XP. */
    struct {
        VIDEO_HW_INITIALIZATION_DATA w2k;
        ULONG reserved;
    } init;
    ULONG status;
    VideoPortZeroMemory(&init, sizeof(init));
    init.w2k.HwInitDataSize = sizeof(init);
    init.w2k.AdapterInterfaceType = PCIBus;
    init.w2k.HwDeviceExtensionSize = sizeof(ADAPTER);
    init.w2k.HwFindAdapter = find_adapter;
    init.w2k.HwInitialize = initialize;
    init.w2k.HwStartIO = start_io;
    init.w2k.HwGetPowerState = power;
    init.w2k.HwSetPowerState = power;
    init.w2k.HwGetVideoChildDescriptor = child;
    status = VideoPortInitialize(context1, context2, &init.w2k, NULL);
    if (status == STATUS_REVISION_MISMATCH) {
        init.w2k.HwInitDataSize = sizeof(init.w2k);
        status = VideoPortInitialize(context1, context2, &init.w2k, NULL);
    }
    return status;
}
