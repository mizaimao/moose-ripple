/*
 * moose-ripple mini-VDD: the virtio-gpu side (step 3 of docs/win9x/plan.md, work in progress).
 *
 * 32-bit flat C inside the VxD: no C run-time, no Windows headers. What it needs from VMM comes
 * through small assembly helpers in ripplevd.asm.
 *
 * So far: find the card on the PCI bus, bring up the virtio transport (virtio 1.0 over PCI, two
 * queues, answers polled) and ask the host for its display size.
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

/* ---- From ripplevd.asm ---------------------------------------------------------------------- */
extern void *__cdecl vmm_map_phys(u32 phys, u32 bytes);
extern void *__cdecl vmm_alloc_contig(u32 pages, u32 *phys);
extern u32   __cdecl vmm_time_ms(void);

/* ---- The virtio structures in the card's memory (virtio 1.0, 4.1.4.3) ------------------------ */
/* common configuration, offsets */
#define CC_DEVICE_FEATURE_SELECT 0x00
#define CC_DEVICE_FEATURE        0x04
#define CC_DRIVER_FEATURE_SELECT 0x08
#define CC_DRIVER_FEATURE        0x0C
#define CC_NUM_QUEUES            0x12
#define CC_DEVICE_STATUS         0x14
#define CC_QUEUE_SELECT          0x16
#define CC_QUEUE_SIZE            0x18
#define CC_QUEUE_ENABLE          0x1C
#define CC_QUEUE_NOTIFY_OFF      0x1E
#define CC_QUEUE_DESC            0x20
#define CC_QUEUE_DRIVER          0x28
#define CC_QUEUE_DEVICE          0x30

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8

#define VIRTIO_F_VERSION_1_HI   1UL             /* feature bit 32 */
#define VIRTIO_GPU_F_VIRGL      1UL             /* feature bit 0: the host renders 3D (virgl) */

#define rd8(p, o)      (*(volatile u8  *)((u8 *)(p) + (o)))
#define rd16(p, o)     (*(volatile u16 *)((u8 *)(p) + (o)))
#define rd32(p, o)     (*(volatile u32 *)((u8 *)(p) + (o)))
#define wr8(p, o, v)   (*(volatile u8  *)((u8 *)(p) + (o)) = (u8)(v))
#define wr16(p, o, v)  (*(volatile u16 *)((u8 *)(p) + (o)) = (u16)(v))
#define wr32(p, o, v)  (*(volatile u32 *)((u8 *)(p) + (o)) = (u32)(v))

/* ---- A virtqueue (virtio 1.0, 2.4): one page each for the three parts ---------------------- */
#define QUEUE_SIZE 64

struct vq_desc { u32 addr_lo, addr_hi, len; u16 flags, next; };
#define VQ_DESC_NEXT  1
#define VQ_DESC_WRITE 2                         /* the device writes this buffer */

struct vq_avail { u16 flags, idx, ring[QUEUE_SIZE]; };
#define VQ_AVAIL_NO_INTERRUPT 1
struct vq_used_elem { u32 id, len; };
struct vq_used  { u16 flags, idx; struct vq_used_elem ring[QUEUE_SIZE]; };

struct vq {
    struct vq_desc  *desc;
    struct vq_avail *avail;
    volatile struct vq_used *used;
    volatile u16 *notify;
    u16 index, last_used, size;
};

/* ---- virtio-gpu (virtio 1.1, 5.7): the few things needed so far ------------------------------ */
struct gpu_ctrl_hdr { u32 type, flags, fence_lo, fence_hi, ctx_id, padding; };
#define GPU_CMD_GET_DISPLAY_INFO   0x0100
#define GPU_RESP_OK_DISPLAY_INFO   0x1101
struct gpu_display_one { u32 x, y, width, height, enabled, flags; };

static void *common = 0, *isr = 0, *devcfg = 0, *notify_base = 0;
static struct vq ctrlq = { 0 }, cursorq = { 0 };
/* Memory the device reads and writes, physically contiguous, allocated once:
   a request buffer, a page for answers, and a transfer area that user-mode drivers attach to
   host resources as their guest-side storage. */
#define REQ_PAGES   16
#define AREA_PAGES  512
static u8  *req_buf = 0, *resp_page = 0, *area = 0;
static u32  req_phys = 0, resp_phys = 0, area_phys = 0;
static u32  display_w = 0, display_h = 0;
static u32  features = 0;

int vgpu_has_virgl(void)
{
    return (features & VIRTIO_GPU_F_VIRGL) != 0;
}

static int setup_queue(struct vq *q, u16 index)
{
    u32 phys;
    u8 *mem;

    wr16(common, CC_QUEUE_SELECT, index);
    q->size = rd16(common, CC_QUEUE_SIZE);      /* the device's largest; ours is the smaller */
    if (q->size > QUEUE_SIZE)
        q->size = QUEUE_SIZE;
    if (q->size < 2) {
        dbg("rvd: queue "); dbg_hex(index); dbg(" has no room\r\n");
        return 0;
    }
    mem = (u8 *)vmm_alloc_contig(3, &phys);
    if (!mem) {
        dbg("rvd: no memory for queue "); dbg_hex(index); dbg("\r\n");
        return 0;
    }
    q->index = index;
    q->last_used = 0;
    q->desc  = (struct vq_desc *)mem;
    q->avail = (struct vq_avail *)(mem + 4096);
    q->used  = (volatile struct vq_used *)(mem + 8192);
    q->avail->flags = VQ_AVAIL_NO_INTERRUPT;    /* answers are polled for now */
    wr16(common, CC_QUEUE_SIZE, q->size);
    wr32(common, CC_QUEUE_DESC, phys);          wr32(common, CC_QUEUE_DESC + 4, 0);
    wr32(common, CC_QUEUE_DRIVER, phys + 4096); wr32(common, CC_QUEUE_DRIVER + 4, 0);
    wr32(common, CC_QUEUE_DEVICE, phys + 8192); wr32(common, CC_QUEUE_DEVICE + 4, 0);
    q->notify = (volatile u16 *)((u8 *)notify_base +
                                 rd16(common, CC_QUEUE_NOTIFY_OFF) * gpu.notify_multiplier);
    wr16(common, CC_QUEUE_ENABLE, 1);
    return 1;
}

/* One request (in req_buf) with its answer (in resp_page); waits, polling, until the device has
   used the buffers. Returns the number of bytes the device wrote, 0 on a time-out. */
static u32 command(u32 req_len, u32 resp_len)
{
    struct vq *q = &ctrlq;
    u32 start, spins = 0;
    u16 head = 0;

    q->desc[0].addr_lo = req_phys;        q->desc[0].addr_hi = 0;
    q->desc[0].len = req_len;             q->desc[0].flags = VQ_DESC_NEXT; q->desc[0].next = 1;
    q->desc[1].addr_lo = resp_phys;       q->desc[1].addr_hi = 0;
    q->desc[1].len = resp_len;            q->desc[1].flags = VQ_DESC_WRITE; q->desc[1].next = 0;
    q->avail->ring[q->avail->idx % q->size] = head;
    q->avail->idx++;
    *q->notify = q->index;

    start = vmm_time_ms();
    while (q->used->idx == q->last_used) {
        if ((++spins & 0xFFF) == 0 && vmm_time_ms() - start > 2000)
            return 0;
    }
    q->last_used++;
    (void)rd8(isr, 0);                          /* reading it takes the interrupt request back */
    return q->used->ring[(u16)(q->last_used - 1) % q->size].len;
}

/* virtio 1.0, 3.1.1: reset, announce ourselves, agree on features, set up the queues, go. */
static int transport_init(void)
{
    u32 bar_phys, bar_len, hi, i;
    u8 *bar;

    i = gpu.cap[VIRTIO_PCI_CAP_COMMON].bar;
    for (bar_len = 0, hi = 1; hi <= 4; hi++) {       /* all four are in one memory range here */
        u32 end;
        if (gpu.cap[hi].bar != i)
            return 0;
        end = gpu.cap[hi].offset + gpu.cap[hi].length;
        if (end > bar_len)
            bar_len = end;
    }
    bar_phys = gpu.bar[i];
    bar = (u8 *)vmm_map_phys(bar_phys, bar_len);
    if (!bar)
        return 0;
    common      = bar + gpu.cap[VIRTIO_PCI_CAP_COMMON].offset;
    notify_base = bar + gpu.cap[VIRTIO_PCI_CAP_NOTIFY].offset;
    isr         = bar + gpu.cap[VIRTIO_PCI_CAP_ISR].offset;
    devcfg      = bar + gpu.cap[VIRTIO_PCI_CAP_DEVICE].offset;

    wr8(common, CC_DEVICE_STATUS, 0);
    for (i = 0; i < 100000 && rd8(common, CC_DEVICE_STATUS) != 0; i++)
        ;
    wr8(common, CC_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
    wr8(common, CC_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    wr32(common, CC_DEVICE_FEATURE_SELECT, 0);
    features = rd32(common, CC_DEVICE_FEATURE);
    wr32(common, CC_DEVICE_FEATURE_SELECT, 1);
    hi = rd32(common, CC_DEVICE_FEATURE);
    if (!(hi & VIRTIO_F_VERSION_1_HI)) {
        dbg("rvd: the card is not a virtio 1.0 device\r\n");
        return 0;
    }
    features &= VIRTIO_GPU_F_VIRGL;
    wr32(common, CC_DRIVER_FEATURE_SELECT, 0);
    wr32(common, CC_DRIVER_FEATURE, features);
    wr32(common, CC_DRIVER_FEATURE_SELECT, 1);
    wr32(common, CC_DRIVER_FEATURE, VIRTIO_F_VERSION_1_HI);
    wr8(common, CC_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(rd8(common, CC_DEVICE_STATUS) & STATUS_FEATURES_OK)) {
        dbg("rvd: the card refused our features\r\n");
        return 0;
    }
    req_buf   = (u8 *)vmm_alloc_contig(REQ_PAGES, &req_phys);
    resp_page = (u8 *)vmm_alloc_contig(1, &resp_phys);
    area      = (u8 *)vmm_alloc_contig(AREA_PAGES, &area_phys);
    if (!req_buf || !resp_page || !area)
        dbg("rvd: no contiguous memory for the buffers\r\n");
    if (!req_buf || !resp_page || !area || !setup_queue(&ctrlq, 0) || !setup_queue(&cursorq, 1)) {
        wr8(common, CC_DEVICE_STATUS, 0);
        return 0;
    }
    wr8(common, CC_DEVICE_STATUS,
        STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    (void)rd8(isr, 0);
    dbg("rvd: virtio-gpu ready, 3D "); dbg(vgpu_has_virgl() ? "yes" : "no");
    dbg(", scanouts "); dbg_hex(rd32(devcfg, 8));
    dbg(", capability sets "); dbg_hex(rd32(devcfg, 12)); dbg("\r\n");
    return 1;
}

/* A first exchange with the host: what does it show? */
static void show_display_info(void)
{
    struct gpu_ctrl_hdr *req = (struct gpu_ctrl_hdr *)req_buf;
    struct gpu_ctrl_hdr *resp = (struct gpu_ctrl_hdr *)resp_page;
    struct gpu_display_one *d = (struct gpu_display_one *)(resp + 1);
    u32 i, n;

    for (i = 0; i < sizeof(*req) / 4; i++)
        ((u32 *)req)[i] = 0;
    req->type = GPU_CMD_GET_DISPLAY_INFO;
    n = command(sizeof(*req), sizeof(*resp) + 16 * sizeof(*d));
    if (!n || resp->type != GPU_RESP_OK_DISPLAY_INFO) {
        dbg("rvd: no answer to GET_DISPLAY_INFO, bytes "); dbg_hex(n);
        dbg(", type "); dbg_hex(resp->type); dbg("\r\n");
        return;
    }
    display_w = d->width;
    display_h = d->height;
    dbg("rvd: host display "); dbg_hex(d->width); dbg(" x "); dbg_hex(d->height);
    dbg(", enabled "); dbg_hex(d->enabled); dbg("\r\n");
}

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
    if (!gpu.cap[VIRTIO_PCI_CAP_COMMON].length || !transport_init())
        return 0;
    show_display_info();
    return 1;
}

/* ---- The interface for user-mode drivers: DeviceIoControl on \\.\RIPPLEVD -------------------- */
/* What VMM hands over with the W32_DEVICEIOCONTROL message (in ESI). */
struct dioc {
    u32   internal1, vm, internal2, code;
    void *in;   u32 in_len;
    void *out;  u32 out_len;
    u32  *returned;
    void *overlapped;
    u32   device, process;
};
#define DIOC_OPEN   0UL
#define DIOC_CLOSE  0xFFFFFFFFUL

/* The private codes. Everything else about virtio-gpu is decided in user mode:
   RVD_COMMAND carries a complete virtio-gpu control request and returns the device's answer. */
#define RVD_INFO        1   /* out: struct rvd_info */
#define RVD_COMMAND     2   /* in: the request; out: the answer */
#define RVD_AREA_READ   3   /* in: u32 offset; out: bytes from the transfer area */
#define RVD_AREA_WRITE  4   /* in: u32 offset, then bytes for the transfer area */

struct rvd_info {
    u32 version;            /* of this interface: 1 */
    u32 virgl;              /* the host renders 3D */
    u32 capsets;            /* number of capability sets */
    u32 display_w, display_h;
    u32 area_phys, area_bytes;      /* the transfer area, for RESOURCE_ATTACH_BACKING */
    u32 max_request;
};

#define ERROR_NOT_SUPPORTED     50
#define ERROR_INVALID_PARAMETER 87
#define ERROR_GEN_FAILURE       31

static void copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--)
        *d++ = *s++;
}

u32 __cdecl vgpu_ioctl(struct dioc *p)
{
    u32 n = 0;

    if (p->code == DIOC_OPEN || p->code == DIOC_CLOSE)
        return 0;
    if (!common)
        return ERROR_NOT_SUPPORTED;
    switch (p->code) {
    case RVD_INFO: {
        struct rvd_info i;
        if (p->out_len < sizeof(i))
            return ERROR_INVALID_PARAMETER;
        i.version = 1;
        i.virgl = vgpu_has_virgl();
        i.capsets = rd32(devcfg, 12);
        i.display_w = display_w;
        i.display_h = display_h;
        i.area_phys = area_phys;
        i.area_bytes = AREA_PAGES * 4096UL;
        i.max_request = REQ_PAGES * 4096UL;
        copy(p->out, &i, sizeof(i));
        n = sizeof(i);
        break;
    }
    case RVD_COMMAND:
        if (p->in_len < sizeof(struct gpu_ctrl_hdr) || p->in_len > REQ_PAGES * 4096UL ||
            p->out_len < sizeof(struct gpu_ctrl_hdr) || p->out_len > 4096)
            return ERROR_INVALID_PARAMETER;
        copy(req_buf, p->in, p->in_len);
        n = command(p->in_len, p->out_len);
        if (!n)
            return ERROR_GEN_FAILURE;
        copy(p->out, resp_page, n);
        break;
    case RVD_AREA_READ: {
        u32 off;
        if (p->in_len < 4)
            return ERROR_INVALID_PARAMETER;
        off = *(u32 *)p->in;
        if (off > AREA_PAGES * 4096UL || p->out_len > AREA_PAGES * 4096UL - off)
            return ERROR_INVALID_PARAMETER;
        copy(p->out, area + off, p->out_len);
        n = p->out_len;
        break;
    }
    case RVD_AREA_WRITE: {
        u32 off, len;
        if (p->in_len < 4)
            return ERROR_INVALID_PARAMETER;
        off = *(u32 *)p->in;
        len = p->in_len - 4;
        if (off > AREA_PAGES * 4096UL || len > AREA_PAGES * 4096UL - off)
            return ERROR_INVALID_PARAMETER;
        copy(area + off, (u8 *)p->in + 4, len);
        break;
    }
    default:
        return ERROR_NOT_SUPPORTED;
    }
    if (p->returned)
        *p->returned = n;
    return 0;
}
