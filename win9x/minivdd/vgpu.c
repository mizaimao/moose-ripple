/*
 * moose-ripple mini-VDD: the virtio-gpu side (step 3 of docs/win9x/plan.md, work in progress).
 *
 * 32-bit flat C inside the VxD: no C run-time, no Windows headers. What it needs from VMM comes
 * through small assembly helpers in ripplevd.asm.
 *
 * So far: find the card on the PCI bus and read where its virtio structures are.
 */

/* Everything stays in the VxD's locked code and data: the trap and the interrupt path run at
   any time. (An initialised object lands in data_seg; nothing here is left uninitialised.) */
#pragma code_seg("_LTEXT", "LCODE")
#pragma data_seg("_LDATA", "LCODE")
#pragma const_seg("_LDATA", "LCODE")

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

/* ---- Ring 0 basics: port I/O and QEMU's debug port ------------------------------------------- */
static u32 inl(u16 port)
{
    u32 v;
    __asm {
        mov dx, port
        in  eax, dx
        mov v, eax
    }
    return v;
}

static void outl(u16 port, u32 v)
{
    __asm {
        mov dx, port
        mov eax, v
        out dx, eax
    }
}

static void dbg_putc(char c)
{
    __asm {
        mov al, c
        out 0E9h, al
    }
}

static void dbg(const char *s)
{
    while (*s)
        dbg_putc(*s++);
}

static void dbg_hex(u32 v)
{
    int i;
    for (i = 28; i >= 0; i -= 4)
        dbg_putc("0123456789abcdef"[(v >> i) & 15]);
}

/* ---- PCI configuration space, mechanism 1 ---------------------------------------------------- */
static u32 pci_read32(u32 bdf, u32 reg)
{
    outl(0xCF8, 0x80000000UL | (bdf << 8) | (reg & 0xFC));
    return inl(0xCFC);
}

static u8 pci_read8(u32 bdf, u32 reg)
{
    return (u8)(pci_read32(bdf, reg) >> ((reg & 3) * 8));
}

/* ---- virtio over PCI (virtio 1.0, 4.1.4): vendor capabilities name the structures ---------- */
#define VIRTIO_PCI_CAP_COMMON  1
#define VIRTIO_PCI_CAP_NOTIFY  2
#define VIRTIO_PCI_CAP_ISR     3
#define VIRTIO_PCI_CAP_DEVICE  4

struct vgpu {
    u32 bdf;                    /* bus, device, function: 0 when no card was found */
    u32 bar[6];
    struct { u32 bar, offset, length; } cap[5];
    u32 notify_multiplier;
};

static struct vgpu gpu = { 0 };

/* Called once from MiniVDD_Dynamic_Init. Returns 0 if there is no virtio-gpu card (the mini-VDD
   still loads: QEMU's standard VGA has the same mode-setting ports and no virtio). */
int __cdecl vgpu_init(void)
{
    u32 bdf, id, ptr, i;

    for (bdf = 0; bdf < 0x100; bdf++) {
        id = pci_read32(bdf, 0x00);
        if (id == 0x10501AF4UL)
            break;
    }
    if (bdf == 0x100) {
        dbg("rvd: no virtio-gpu card\r\n");
        return 0;
    }
    gpu.bdf = bdf;
    for (i = 0; i < 6; i++)
        gpu.bar[i] = pci_read32(bdf, 0x10 + i * 4) & 0xFFFFFFF0UL;
    dbg("rvd: virtio-gpu at device "); dbg_hex(bdf);
    dbg(", command "); dbg_hex(pci_read32(bdf, 0x04) & 0xFFFF);
    dbg(", interrupt line "); dbg_hex(pci_read8(bdf, 0x3C)); dbg("\r\n");

    if (!(pci_read32(bdf, 0x04) & 0x00100000UL))    /* status: capabilities list */
        return 0;
    for (ptr = pci_read8(bdf, 0x34) & 0xFC; ptr; ptr = pci_read8(bdf, ptr + 1) & 0xFC) {
        u32 type;
        if (pci_read8(bdf, ptr) != 0x09)            /* vendor-specific */
            continue;
        type = pci_read8(bdf, ptr + 3);
        if (type < 1 || type > 4)
            continue;
        gpu.cap[type].bar    = pci_read8(bdf, ptr + 4);
        gpu.cap[type].offset = pci_read32(bdf, ptr + 8);
        gpu.cap[type].length = pci_read32(bdf, ptr + 12);
        if (type == VIRTIO_PCI_CAP_NOTIFY)
            gpu.notify_multiplier = pci_read32(bdf, ptr + 16);
        dbg("rvd:   structure "); dbg_hex(type);
        dbg(" in range "); dbg_hex(gpu.cap[type].bar);
        dbg(" ("); dbg_hex(gpu.bar[gpu.cap[type].bar]);
        dbg(") at "); dbg_hex(gpu.cap[type].offset);
        dbg(", length "); dbg_hex(gpu.cap[type].length); dbg("\r\n");
    }
    return gpu.cap[VIRTIO_PCI_CAP_COMMON].length != 0;
}
