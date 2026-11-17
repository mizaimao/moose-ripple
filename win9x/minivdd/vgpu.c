/*
 * moose-ripple mini-VDD: the virtio-gpu side (step 3 of docs/win9x/plan.md).
 *
 * 32-bit flat C inside the VxD: no C run-time, no Windows headers. What it needs from VMM comes
 * through small assembly helpers in ripplevd.asm.
 *
 * It finds the card on the PCI bus, brings up the virtio transport (virtio 1.0 over PCI, two
 * queues, answers polled) and gives user-mode drivers what they need to draw with the host's
 * GPU: contexts, buffers (a host GPU object with guest memory behind it), command submission
 * and transfers (rvd.h, docs/win9x/vxd-interface.md). It keeps track of who made what, and
 * removes a program's contexts and buffers when the program closes the device or ends.
 */

/* Everything stays in the VxD's locked code and data: the trap and the interrupt path run at
   any time. (An initialised object lands in data_seg; nothing here is left uninitialised.) */
#pragma code_seg("_LTEXT", "LCODE")
#pragma data_seg("_LDATA", "LCODE")
#pragma const_seg("_LDATA", "LCODE")
#pragma bss_seg("_LDATA", "LCODE")

#include "rvd.h"

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
/* The address and the data port are two steps: interrupts off in between, so that a handler
   reaching configuration space meanwhile can't move the address. */
static u32 pci_read32(u32 bdf, u32 reg)
{
    u32 v;

    __asm pushfd
    __asm cli
    outl(0xCF8, 0x80000000UL | (bdf << 8) | (reg & 0xFC));
    v = inl(0xCFC);
    __asm popfd
    return v;
}

static u8 pci_read8(u32 bdf, u32 reg)
{
    return (u8)(pci_read32(bdf, reg) >> ((reg & 3) * 8));
}

static void pci_write32(u32 bdf, u32 reg, u32 v)
{
    __asm pushfd
    __asm cli
    outl(0xCF8, 0x80000000UL | (bdf << 8) | (reg & 0xFC));
    outl(0xCFC, v);
    __asm popfd
}

/* ---- virtio over PCI (virtio 1.0, 4.1.4): vendor capabilities name the structures ---------- */
#define VIRTIO_PCI_CAP_COMMON  1
#define VIRTIO_PCI_CAP_NOTIFY  2
#define VIRTIO_PCI_CAP_ISR     3
#define VIRTIO_PCI_CAP_DEVICE  4
#define VIRTIO_PCI_CAP_PCI_CFG 5

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
extern void *__cdecl vmm_alloc_pages(u32 pages, u32 *handle);
extern void  __cdecl vmm_free_pages(u32 handle);
extern void *__cdecl vmm_alloc_pageable(u32 pages);
extern int   __cdecl vmm_page_table(void *addr, u32 pages, u32 *entries);
extern u32   __cdecl vmm_time_ms(void);
extern u32   __cdecl vmm_mutex_create(void);
extern void  __cdecl vmm_mutex_enter(u32 mutex);
extern void  __cdecl vmm_mutex_leave(u32 mutex);

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

/* The card's virtio structures are reached in one of two ways. Normally through their memory
   range (BAR), mapped: the structure pointers below are addresses. When the firmware put that
   range above 4 GB, out of reach of a 32-bit Windows (QEMU does with a large host memory range,
   as UTM sets up), through the PCI configuration access capability (virtio 1.0, 4.1.4.7): a
   window in the card's configuration space that reads or writes one register of the range at
   a time. The structure pointers are then their offsets in the range, tagged with WIN_TAG so
   that none is 0. Slower, but only a few registers are touched per request. */
#define WIN_TAG 0x80000000UL
static u32 win_cap = 0;                         /* the capability's offset in configuration
                                                   space, when the window is in use */
static u32 win_ptr = 0;                         /* where the card has that capability (0: none) */

static void win_set(u32 off, u32 len)
{
    pci_write32(gpu.bdf, win_cap + 8, off & ~WIN_TAG);  /* offset in the range */
    pci_write32(gpu.bdf, win_cap + 12, len);            /* 1, 2 or 4 bytes */
}

static u32 reg_read(void *p, u32 o, u32 len)
{
    u8 *a = (u8 *)p + o;
    u32 v;

    if (!win_cap)
        return len == 1 ? *(volatile u8 *)a : len == 2 ? *(volatile u16 *)a : *(volatile u32 *)a;
    win_set((u32)a, len);
    v = pci_read32(gpu.bdf, win_cap + 16);              /* the read happens here */
    return len == 4 ? v : v & ((1UL << (len * 8)) - 1);
}

static void reg_write(void *p, u32 o, u32 len, u32 v)
{
    u8 *a = (u8 *)p + o;

    if (!win_cap) {
        if (len == 1)
            *(volatile u8 *)a = (u8)v;
        else if (len == 2)
            *(volatile u16 *)a = (u16)v;
        else
            *(volatile u32 *)a = v;
        return;
    }
    win_set((u32)a, len);
    pci_write32(gpu.bdf, win_cap + 16, v);              /* and the write here */
}

#define rd8(p, o)      ((u8)reg_read((p), (o), 1))
#define rd16(p, o)     ((u16)reg_read((p), (o), 2))
#define rd32(p, o)     reg_read((p), (o), 4)
#define wr8(p, o, v)   reg_write((p), (o), 1, (u32)(v))
#define wr16(p, o, v)  reg_write((p), (o), 2, (u32)(v))
#define wr32(p, o, v)  reg_write((p), (o), 4, (u32)(v))

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
    u8 *notify;                     /* the queue's notify register (see reg_write) */
    u16 index, last_used, size;
};

/* ---- virtio-gpu (virtio 1.1, 5.7) -------------------------------------------------------------- */
struct gpu_ctrl_hdr { u32 type, flags, fence_lo, fence_hi, ctx_id, padding; };
#define GPU_CMD_GET_DISPLAY_INFO        0x0100
#define GPU_CMD_RESOURCE_UNREF          0x0102
#define GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define GPU_CMD_GET_CAPSET_INFO         0x0108
#define GPU_CMD_GET_CAPSET              0x0109
#define GPU_CMD_CTX_CREATE              0x0200
#define GPU_CMD_CTX_DESTROY             0x0201
#define GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define GPU_CMD_CTX_DETACH_RESOURCE     0x0203
#define GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define GPU_CMD_SUBMIT_3D               0x0207
#define GPU_RESP_OK_NODATA              0x1100
#define GPU_RESP_OK_DISPLAY_INFO        0x1101
#define GPU_RESP_OK_CAPSET_INFO         0x1102
#define GPU_RESP_OK_CAPSET              0x1103
struct gpu_display_one { u32 x, y, width, height, enabled, flags; };
/* One u32 after the header, then padding: UNREF, CTX_ATTACH/DETACH_RESOURCE, GET_CAPSET_INFO */
struct gpu_one_id { struct gpu_ctrl_hdr hdr; u32 id, padding; };
struct gpu_ctx_create { struct gpu_ctrl_hdr hdr; u32 name_len, context_init; char name[64]; };
struct gpu_resource_create_3d {
    struct gpu_ctrl_hdr hdr;
    u32 id, target, format, bind, width, height, depth, array_size, last_level, nr_samples,
        flags, padding;
};
struct gpu_attach_backing { struct gpu_ctrl_hdr hdr; u32 id, entries; };
struct gpu_mem_entry { u32 addr_lo, addr_hi, length, padding; };
struct gpu_submit { struct gpu_ctrl_hdr hdr; u32 size, padding; };
struct gpu_transfer_3d {
    struct gpu_ctrl_hdr hdr;
    u32 x, y, z, w, h, d;
    u32 offset_lo, offset_hi, id, level, stride, layer_stride;
};
struct gpu_get_capset { struct gpu_ctrl_hdr hdr; u32 id, version; };
struct gpu_capset_info { struct gpu_ctrl_hdr hdr; u32 id, max_version, max_size, padding; };

static void *common = 0, *isr = 0, *devcfg = 0, *notify_base = 0;
static struct vq ctrlq = { 0 }, cursorq = { 0 };
/* Memory the device reads and writes, physically contiguous, allocated once: one buffer for
   requests (a command submission is the largest) and one for answers. */
#define REQ_PAGES   128
#define RESP_PAGES  4
#define REQ_BYTES   (REQ_PAGES * 4096UL)
#define RESP_BYTES  (RESP_PAGES * 4096UL)
static u8  *req_buf = 0, *resp_buf = 0;
static u32  req_phys = 0, resp_phys = 0;
static u32  features = 0;
static u32  dead = 0;                           /* the device stopped answering */
static struct rvd_info info = { 0 };

/* Who owns what. A context belongs to the process that made it (DeviceIoControl's
   device handle value is the same in every process, so it can't tell them apart); a buffer belongs to
   a context. Context IDs are 1 to MAX_CONTEXTS-1; a buffer's handle (its virgl resource ID) is
   its index in the table plus one. */
#define MAX_CONTEXTS     64
#define MAX_BUFFERS      4096
#define MAX_BUFFER_PAGES 8192                   /* 32 MB behind one buffer */
struct buffer { u32 context, mem_handle, pages; u8 *mem; };
static u32 ctx_owner[MAX_CONTEXTS] = { 0 };
static struct buffer *buffers = 0;
static u32 *page_table = 0;                     /* scratch: one entry per page of a new buffer */
static u32 next_buffer = 0;

int vgpu_has_virgl(void)
{
    return (features & VIRTIO_GPU_F_VIRGL) != 0;
}

static void copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if ((((u32)d | (u32)s | n) & 3) == 0) {
        u32 *d4 = (u32 *)d;
        const u32 *s4 = (const u32 *)s;
        for (n >>= 2; n; n--)
            *d4++ = *s4++;
        return;
    }
    while (n--)
        *d++ = *s++;
}

static void zero(void *dst, u32 n)
{
    u8 *d = (u8 *)dst;
    while (n--)
        *d++ = 0;
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
    q->notify = (u8 *)notify_base + rd16(common, CC_QUEUE_NOTIFY_OFF) * gpu.notify_multiplier;
    wr16(common, CC_QUEUE_ENABLE, 1);
    return 1;
}

/* One request (in req_buf) with its answer (in resp_buf); waits, polling, until the device has
   used the buffers. Returns the number of bytes the device wrote, 0 on a time-out (after which
   the device counts as dead: its late answer would be taken for the next request's). */
static u32 command(u32 req_len, u32 resp_len)
{
    struct vq *q = &ctrlq;
    u32 start, spins = 0;
    u16 head = 0;

    if (dead)
        return 0;
    q->desc[0].addr_lo = req_phys;        q->desc[0].addr_hi = 0;
    q->desc[0].len = req_len;             q->desc[0].flags = VQ_DESC_NEXT; q->desc[0].next = 1;
    q->desc[1].addr_lo = resp_phys;       q->desc[1].addr_hi = 0;
    q->desc[1].len = resp_len;            q->desc[1].flags = VQ_DESC_WRITE; q->desc[1].next = 0;
    q->avail->ring[q->avail->idx % q->size] = head;
    q->avail->idx++;
    wr16(q->notify, 0, q->index);

    start = vmm_time_ms();
    while (q->used->idx == q->last_used) {
        if ((++spins & 0xFFF) == 0 && vmm_time_ms() - start > 10000) {
            dbg("rvd: the device did not answer request "); dbg_hex(*(u32 *)req_buf); dbg("\r\n");
            dead = 1;
            return 0;
        }
    }
    q->last_used++;
    (void)rd8(isr, 0);                          /* reading it takes the interrupt request back */
    return q->used->ring[(u16)(q->last_used - 1) % q->size].len;
}

/* Start a request in req_buf: `bytes` zeroed, type and context set. */
static void *request(u32 type, u32 ctx, u32 bytes)
{
    struct gpu_ctrl_hdr *h = (struct gpu_ctrl_hdr *)req_buf;
    zero(req_buf, bytes);
    h->type = type;
    h->ctx_id = ctx;
    return req_buf;
}

/* Send a request whose answer is just "ok". */
static int simple(u32 req_len)
{
    struct gpu_ctrl_hdr *resp = (struct gpu_ctrl_hdr *)resp_buf;
    u32 type = *(u32 *)req_buf;

    if (!command(req_len, sizeof(*resp)))
        return 0;
    if (resp->type != GPU_RESP_OK_NODATA) {
        dbg("rvd: request "); dbg_hex(type); dbg(" answered "); dbg_hex(resp->type); dbg("\r\n");
        return 0;
    }
    return 1;
}

static int one_id(u32 type, u32 ctx, u32 id)
{
    struct gpu_one_id *r = (struct gpu_one_id *)request(type, ctx, sizeof(*r));
    r->id = id;
    return simple(sizeof(*r));
}

/* virtio 1.0, 3.1.1: reset, announce ourselves, agree on features, set up the queues, go. */
static int transport_init(void)
{
    u32 bar_phys, bar_len, hi, i, handle, raw;
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
    if (i > 5)
        return 0;
    raw = pci_read32(gpu.bdf, 0x10 + i * 4);
    bar_phys = raw & 0xFFFFFFF0UL;
    /* A 64-bit range with an upper half, or none assigned: mapping the lower half would hit
       ordinary memory (address 0 is the real-mode interrupt table). The window instead. */
    if ((raw & 1) || !bar_phys ||
        ((raw & 6) == 4 && (i == 5 || pci_read32(gpu.bdf, 0x14 + i * 4) != 0))) {
        if (!win_ptr) {
            dbg("rvd: the card's registers are out of reach\r\n");
            return 0;
        }
        win_cap = win_ptr;
        pci_write32(gpu.bdf, win_cap + 4, i);           /* which range; the rest is read-only */
        bar = (u8 *)WIN_TAG;
        dbg("rvd: the card's registers through its configuration window\r\n");
    } else {
        bar = (u8 *)vmm_map_phys(bar_phys, bar_len);
        if (!bar)
            return 0;
    }
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
        wr8(common, CC_DEVICE_STATUS, 0);
        common = 0;                     /* nothing may use the card now */
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
        wr8(common, CC_DEVICE_STATUS, 0);
        common = 0;
        return 0;
    }
    req_buf    = (u8 *)vmm_alloc_contig(REQ_PAGES, &req_phys);
    resp_buf   = (u8 *)vmm_alloc_contig(RESP_PAGES, &resp_phys);
    buffers    = (struct buffer *)vmm_alloc_pages((MAX_BUFFERS * sizeof(struct buffer) + 4095) >> 12, &handle);
    page_table = (u32 *)vmm_alloc_pages((MAX_BUFFER_PAGES * 4 + 4095) >> 12, &handle);
    if (!req_buf || !resp_buf || !buffers || !page_table)
        dbg("rvd: no memory for the buffers\r\n");
    if (!req_buf || !resp_buf || !buffers || !page_table ||
        !setup_queue(&ctrlq, 0) || !setup_queue(&cursorq, 1)) {
        wr8(common, CC_DEVICE_STATUS, 0);
        common = 0;
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

/* What the host offers: its display size and its capability sets. */
static void read_host_info(void)
{
    struct gpu_ctrl_hdr *resp = (struct gpu_ctrl_hdr *)resp_buf;
    struct gpu_display_one *d = (struct gpu_display_one *)(resp + 1);
    struct gpu_capset_info *ci = (struct gpu_capset_info *)resp_buf;
    u32 i, n, sets;

    info.version = RVD_INTERFACE_VERSION;
    info.virgl = vgpu_has_virgl();
    info.max_submit = REQ_BYTES - sizeof(struct gpu_submit);
    info.max_buffer = MAX_BUFFER_PAGES * 4096UL;

    request(GPU_CMD_GET_DISPLAY_INFO, 0, sizeof(*resp));
    n = command(sizeof(*resp), sizeof(*resp) + 16 * sizeof(*d));
    if (n && resp->type == GPU_RESP_OK_DISPLAY_INFO) {
        info.display_w = d->width;
        info.display_h = d->height;
    }
    dbg("rvd: host display "); dbg_hex(info.display_w); dbg(" x "); dbg_hex(info.display_h); dbg("\r\n");

    sets = rd32(devcfg, 12);
    for (i = 0; i < sets && info.num_caps < RVD_MAX_CAPS; i++) {
        struct gpu_one_id *r = (struct gpu_one_id *)request(GPU_CMD_GET_CAPSET_INFO, 0, sizeof(*r));
        r->id = i;
        if (!command(sizeof(*r), sizeof(*ci)) || ci->hdr.type != GPU_RESP_OK_CAPSET_INFO)
            continue;
        info.caps_ids[info.num_caps] = ci->id;
        info.caps_versions[info.num_caps] = ci->max_version;
        info.caps_sizes[info.num_caps] = ci->max_size;
        info.num_caps++;
        dbg("rvd: capability set "); dbg_hex(ci->id); dbg(", version "); dbg_hex(ci->max_version);
        dbg(", bytes "); dbg_hex(ci->max_size); dbg("\r\n");
    }
}

/* Called once from MiniVDD_Dynamic_Init. Returns 0 if there is no virtio-gpu card (the mini-VDD
   still loads: QEMU's standard VGA has the same mode-setting ports and no virtio). */
/* This driver polls and never takes the card's interrupt, and nobody else answers it either: an
   interrupt the card raises outside a request (a configuration change, say) would hold the line
   until the next request, and on a line shared with another device that device's handler would
   run again and again. So the card is told not to raise it at all (PCI command register,
   interrupt disable). Checked again at every new context, in case Windows' PCI code rewrote the
   register after start-up. The status half of the register is written as zeroes, which clears
   nothing. */
static void quiet_interrupt(void)
{
    u32 cmd = pci_read32(gpu.bdf, 0x04);

    if (cmd & 0x400UL)
        return;
    pci_write32(gpu.bdf, 0x04, (cmd & 0xFFFFUL) | 0x400UL);
    dbg("rvd: the card's interrupt switched off\r\n");
}

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
    quiet_interrupt();
    for (i = 0; i < 6; i++)
        gpu.bar[i] = pci_read32(bdf, 0x10 + i * 4) & 0xFFFFFFF0UL;
    dbg("rvd: virtio-gpu at device "); dbg_hex(bdf);
    dbg(", interrupt line "); dbg_hex(pci_read8(bdf, 0x3C)); dbg("\r\n");

    if (!(pci_read32(bdf, 0x04) & 0x00100000UL))    /* status: capabilities list */
        return 0;
    for (ptr = pci_read8(bdf, 0x34) & 0xFC; ptr; ptr = pci_read8(bdf, ptr + 1) & 0xFC) {
        u32 type;
        if (pci_read8(bdf, ptr) != 0x09)            /* vendor-specific */
            continue;
        type = pci_read8(bdf, ptr + 3);
        if (type == VIRTIO_PCI_CAP_PCI_CFG) {
            win_ptr = ptr;                          /* the window (see reg_read) */
            continue;
        }
        if (type < 1 || type > 4)
            continue;
        gpu.cap[type].bar    = pci_read8(bdf, ptr + 4);
        gpu.cap[type].offset = pci_read32(bdf, ptr + 8);
        gpu.cap[type].length = pci_read32(bdf, ptr + 12);
        if (type == VIRTIO_PCI_CAP_NOTIFY)
            gpu.notify_multiplier = pci_read32(bdf, ptr + 16);
    }
    if (!gpu.cap[VIRTIO_PCI_CAP_COMMON].length || !transport_init())
        return 0;
    read_host_info();
    return 1;
}

/* ---- Contexts and buffers --------------------------------------------------------------------- */
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_GEN_FAILURE       31
#define ERROR_NOT_SUPPORTED     50
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NO_MORE_ITEMS     259

static int valid_context(u32 ctx)
{
    return ctx > 0 && ctx < MAX_CONTEXTS && ctx_owner[ctx] != 0;
}

static struct buffer *find_buffer(u32 handle)
{
    if (handle == 0 || handle > MAX_BUFFERS || buffers[handle - 1].context == 0)
        return 0;
    return &buffers[handle - 1];
}

/* The host forgets the buffer; then its memory is free again. */
static void buffer_destroy(struct buffer *b, int detach)
{
    u32 id = (u32)(b - buffers) + 1;

    if (detach)
        one_id(GPU_CMD_CTX_DETACH_RESOURCE, b->context, id);
    one_id(GPU_CMD_RESOURCE_UNREF, 0, id);
    if (!dead)                                  /* a dead device may still hold the pages */
        vmm_free_pages(b->mem_handle);
    b->context = 0;
    b->mem = 0;
}

static u32 buffer_create(const struct rvd_buffer_create *c, struct rvd_buffer *out)
{
    struct gpu_resource_create_3d *rc;
    struct gpu_attach_backing *ab;
    struct gpu_mem_entry *e;
    struct buffer *b;
    u32 i, id, pages, n;

    if (!valid_context(c->context))
        return ERROR_INVALID_PARAMETER;
    pages = (c->size + 4095) >> 12;
    if (pages == 0)
        pages = 1;
    if (c->size > MAX_BUFFER_PAGES * 4096UL)
        return ERROR_INVALID_PARAMETER;
    for (i = 0; i < MAX_BUFFERS; i++) {
        b = &buffers[(next_buffer + i) % MAX_BUFFERS];
        if (b->context == 0)
            break;
    }
    if (i == MAX_BUFFERS)
        return ERROR_NO_MORE_ITEMS;
    id = (u32)(b - buffers) + 1;

    b->mem = (u8 *)vmm_alloc_pages(pages, &b->mem_handle);
    if (!b->mem)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!vmm_page_table(b->mem, pages, page_table)) {
        vmm_free_pages(b->mem_handle);
        return ERROR_GEN_FAILURE;
    }

    rc = (struct gpu_resource_create_3d *)request(GPU_CMD_RESOURCE_CREATE_3D, c->context, sizeof(*rc));
    rc->id = id;
    rc->target = c->target;         rc->format = c->format;         rc->bind = c->bind;
    rc->width = c->width;           rc->height = c->height;         rc->depth = c->depth;
    rc->array_size = c->array_size; rc->last_level = c->last_level; rc->nr_samples = c->nr_samples;
    rc->flags = c->flags;
    if (!simple(sizeof(*rc))) {
        vmm_free_pages(b->mem_handle);
        return ERROR_GEN_FAILURE;
    }

    /* The memory behind it: one entry per run of pages that follow each other physically. */
    ab = (struct gpu_attach_backing *)request(GPU_CMD_RESOURCE_ATTACH_BACKING, 0, sizeof(*ab));
    ab->id = id;
    e = (struct gpu_mem_entry *)(ab + 1);
    for (i = 0, n = 0; i < pages; i++) {
        u32 phys = page_table[i] & 0xFFFFF000UL;
        if (n && e[n - 1].addr_lo + e[n - 1].length == phys) {
            e[n - 1].length += 4096;
        } else {
            e[n].addr_lo = phys; e[n].addr_hi = 0; e[n].length = 4096; e[n].padding = 0;
            n++;
        }
    }
    ab->entries = n;
    if (!simple(sizeof(*ab) + n * sizeof(*e)) ||
        !one_id(GPU_CMD_CTX_ATTACH_RESOURCE, c->context, id)) {
        b->context = c->context;
        buffer_destroy(b, 0);
        return ERROR_GEN_FAILURE;
    }
    b->context = c->context;
    b->pages = pages;
    next_buffer = id % MAX_BUFFERS;
    out->handle = id;
    out->size = pages << 12;
    out->address = b->mem;
    return 0;
}

static void context_destroy(u32 ctx)
{
    u32 i, n = 0;

    for (i = 0; i < MAX_BUFFERS; i++) {
        if (buffers[i].context == ctx) {
            buffer_destroy(&buffers[i], 0);
            n++;
        }
    }
    dbg("rvd: context "); dbg_hex(ctx); dbg(" ends, buffers left to remove: "); dbg_hex(n); dbg("\r\n");
    request(GPU_CMD_CTX_DESTROY, ctx, sizeof(struct gpu_ctrl_hdr));
    simple(sizeof(struct gpu_ctrl_hdr));
    ctx_owner[ctx] = 0;
}

/* ---- The interface for user-mode drivers: DeviceIoControl on \\.\RIPPLEVD (rvd.h) ------------- */
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

static u32 dispatch(struct dioc *p)
{
    u32 n = 0, i, err;

    if (p->code == DIOC_OPEN)
        return 0;
    if (p->code == DIOC_CLOSE) {                /* the handle is closed, or its program ended */
        for (i = 1; common && i < MAX_CONTEXTS; i++) {
            if (ctx_owner[i] == p->process)
                context_destroy(i);
        }
        return 0;
    }
    if (p->code == RVD_HEAP) {                  /* system memory for DirectDraw: no device needed */
        static struct rvd_heap heap;
        if (p->out_len < sizeof(heap))
            return ERROR_INVALID_PARAMETER;
        if (!heap.address && p->in_len >= 4) {
            u32 pages = (*(u32 *)p->in + 4095) >> 12;
            for (; pages >= 256 && !heap.address; pages /= 2) {   /* less if memory is short */
                heap.address = vmm_alloc_pageable(pages);
                heap.size = heap.address ? pages << 12 : 0;
            }
            dbg("rvd: DirectDraw heap of "); dbg_hex(heap.size); dbg(" bytes at ");
            dbg_hex((u32)heap.address); dbg("\r\n");
        }
        copy(p->out, &heap, sizeof(heap));
        if (p->returned)
            *p->returned = sizeof(heap);
        return 0;
    }
    if (!common)
        return ERROR_NOT_SUPPORTED;
    switch (p->code) {
    case RVD_GET_INFO:
        if (p->out_len < sizeof(info))
            return ERROR_INVALID_PARAMETER;
        copy(p->out, &info, sizeof(info));
        n = sizeof(info);
        break;
    case RVD_GET_CAPS: {
        struct gpu_get_capset *r;
        u32 got;
        if (p->in_len < 8)
            return ERROR_INVALID_PARAMETER;
        r = (struct gpu_get_capset *)request(GPU_CMD_GET_CAPSET, 0, sizeof(*r));
        r->id = ((u32 *)p->in)[0];
        r->version = ((u32 *)p->in)[1];
        got = command(sizeof(*r), RESP_BYTES);
        if (got <= sizeof(struct gpu_ctrl_hdr) ||
            ((struct gpu_ctrl_hdr *)resp_buf)->type != GPU_RESP_OK_CAPSET)
            return ERROR_GEN_FAILURE;
        n = got - sizeof(struct gpu_ctrl_hdr);
        if (n > p->out_len)
            n = p->out_len;
        copy(p->out, resp_buf + sizeof(struct gpu_ctrl_hdr), n);
        break;
    }
    case RVD_CONTEXT_CREATE: {
        struct gpu_ctx_create *r;
        if (p->out_len < 4)
            return ERROR_INVALID_PARAMETER;
        quiet_interrupt();
        for (i = 1; i < MAX_CONTEXTS && ctx_owner[i]; i++)
            ;
        if (i == MAX_CONTEXTS)
            return ERROR_NO_MORE_ITEMS;
        r = (struct gpu_ctx_create *)request(GPU_CMD_CTX_CREATE, i, sizeof(*r));
        r->name_len = p->in_len < sizeof(r->name) ? p->in_len : sizeof(r->name) - 1;
        copy(r->name, p->in, r->name_len);
        if (!simple(sizeof(*r)))
            return ERROR_GEN_FAILURE;
        ctx_owner[i] = p->process;
        dbg("rvd: context "); dbg_hex(i); dbg(" for process "); dbg_hex(p->process); dbg("\r\n");
        *(u32 *)p->out = i;
        n = 4;
        break;
    }
    case RVD_CONTEXT_DESTROY:
        if (p->in_len < 4 || !valid_context(*(u32 *)p->in))
            return ERROR_INVALID_PARAMETER;
        context_destroy(*(u32 *)p->in);
        break;
    case RVD_BUFFER_CREATE:
        if (p->in_len < sizeof(struct rvd_buffer_create) || p->out_len < sizeof(struct rvd_buffer))
            return ERROR_INVALID_PARAMETER;
        err = buffer_create((const struct rvd_buffer_create *)p->in, (struct rvd_buffer *)p->out);
        if (err)
            return err;
        n = sizeof(struct rvd_buffer);
        break;
    case RVD_BUFFER_DESTROY: {
        struct buffer *b;
        if (p->in_len < 4 || !(b = find_buffer(*(u32 *)p->in)))
            return ERROR_INVALID_PARAMETER;
        buffer_destroy(b, 1);
        break;
    }
    case RVD_SUBMIT: {
        const struct rvd_submit *s = (const struct rvd_submit *)p->in;
        struct gpu_submit *r;
        if (p->in_len < sizeof(*s) || s->bytes > p->in_len - sizeof(*s) ||
            s->bytes > info.max_submit || (s->bytes & 3) || !valid_context(s->context))
            return ERROR_INVALID_PARAMETER;
        r = (struct gpu_submit *)request(GPU_CMD_SUBMIT_3D, s->context, sizeof(*r));
        r->size = s->bytes;
        copy(r + 1, s + 1, s->bytes);
        if (!simple(sizeof(*r) + s->bytes))
            return ERROR_GEN_FAILURE;
        break;
    }
    case RVD_TRANSFER: {
        const struct rvd_transfer *t = (const struct rvd_transfer *)p->in;
        struct gpu_transfer_3d *r;
        if (p->in_len < sizeof(*t) || !valid_context(t->context) || !find_buffer(t->buffer))
            return ERROR_INVALID_PARAMETER;
        r = (struct gpu_transfer_3d *)request(t->direction == RVD_FROM_HOST ?
                GPU_CMD_TRANSFER_FROM_HOST_3D : GPU_CMD_TRANSFER_TO_HOST_3D, t->context, sizeof(*r));
        r->x = t->x; r->y = t->y; r->z = t->z; r->w = t->width; r->h = t->height; r->d = t->depth;
        r->offset_lo = t->offset;
        r->id = t->buffer;
        r->level = t->level;
        r->stride = t->stride;
        r->layer_stride = t->layer_stride;
        if (!simple(sizeof(*r)))
            return ERROR_GEN_FAILURE;
        break;
    }
    default:
        return ERROR_NOT_SUPPORTED;
    }
    if (p->returned)
        *p->returned = n;
    return 0;
}

/* One request at a time. The request and answer buffers, the page-table scratch and the tables
   above are shared by every caller, and a caller can block inside a request (allocating memory
   may page), which lets another thread in. The mutex is made at the first call, in a thread:
   nothing between the test and the store can block, so two threads can't both make one. */
u32 __cdecl vgpu_ioctl(struct dioc *p)
{
    static u32 lock = 0;
    u32 err;

    if (p->code == DIOC_OPEN)
        return 0;
    if (!lock)
        lock = vmm_mutex_create();
    if (lock)
        vmm_mutex_enter(lock);
    err = dispatch(p);
    if (lock)
        vmm_mutex_leave(lock);
    return err;
}
