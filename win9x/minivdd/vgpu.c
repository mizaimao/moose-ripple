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
extern u32   __cdecl vmm_mutex_owner(u32 mutex);
extern u32   __cdecl vmm_queue_refresh(u32 delay_ms);
extern int   __cdecl vmm_rearm_dac(u32 vm);

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
    u32 phys;
    struct vq_desc  *desc;
    struct vq_avail *avail;
    volatile struct vq_used *used;
    u8 *notify;                     /* the queue's notify register (see reg_write) */
    u16 index, last_used, size;
};

/* ---- virtio-gpu (virtio 1.1, 5.7) -------------------------------------------------------------- */
struct gpu_ctrl_hdr { u32 type, flags, fence_lo, fence_hi, ctx_id, padding; };
#define GPU_CMD_GET_DISPLAY_INFO        0x0100
#define GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define GPU_CMD_RESOURCE_UNREF          0x0102
#define GPU_CMD_SET_SCANOUT             0x0103
#define GPU_CMD_RESOURCE_FLUSH          0x0104
#define GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
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
#define GPU_RESP_ERR_INVALID_RESOURCE   0x1203
struct gpu_display_one { u32 x, y, width, height, enabled, flags; };
struct gpu_create_2d { struct gpu_ctrl_hdr hdr; u32 id, format, w, h; };
struct gpu_rect { u32 x, y, w, h; };
struct gpu_set_scanout { struct gpu_ctrl_hdr hdr; struct gpu_rect rect; u32 scanout, id; };
struct gpu_flush { struct gpu_ctrl_hdr hdr; struct gpu_rect rect; u32 id, padding; };
struct gpu_upload_2d { struct gpu_ctrl_hdr hdr; struct gpu_rect rect; u32 off_lo, off_hi, id, pad; };
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

/* Head 0 always belongs to the regular request. Additional pairs carry independent
   fenced submissions and at most one desktop upload. */
#define FENCE_SLOTS (QUEUE_SIZE / 2 - 1)
union fence_request_data {
    struct gpu_ctrl_hdr hdr;
    struct gpu_submit submit;
    struct gpu_upload_2d upload;
};
struct fence_slot { union fence_request_data cmd; struct gpu_ctrl_hdr reply; };
static struct fence_slot *fences;
static u32 fences_phys, fences_busy, async_failed;
static u32 regular_done, regular_bytes;
/* Active until retirement, even after the used head was consumed by another command. */
static struct {
    u32 head, fence, slot, serial, generation, owner, start;
    int result, expired;
} upload;
static u32 cpu_generation, async_uploads, async_shown, upload_test_owner, upload_test_start;
static int upload_test_held, upload_disabled;
static u32 unref_pending, unref_fence;
static void finish_late_unref(void);

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
/* A request that got no answer in time is still the device's: its late answer must not be taken
   for the next request's, and the request buffer must not be written until it comes. Until
   then the interface refuses work (ERROR_TIMEOUT) and keeps what it can't undo yet (zombies,
   below); device_ready() notices the answer and puts things right. */
static u32  stalled = 0;
#define DEFAULT_TIMEOUT_MS 10000
static u32  timeout_ms = DEFAULT_TIMEOUT_MS;    /* RVD_SET_TIMEOUT changes it, for tests */
static u32  timeout_owner = 0;                  /* ...from this process, until it ends */
static u32  info_incomplete = 0;                /* read_host_info() was cut short by a stall */
static struct rvd_info info = { 0 };

/* Who owns what. A context belongs to the process that made it (DeviceIoControl's
   device handle value is the same in every process, so it can't tell them apart); a buffer belongs to
   a context. Context IDs are 1 to MAX_CONTEXTS-1; a buffer's handle (its virgl resource ID) is
   its index in the table plus one. */
#define MAX_CONTEXTS     64
#define MAX_BUFFERS      4096
#define MAX_BUFFER_PAGES 8192                   /* 32 MB behind one buffer */
/* A buffer or context let go while the device was stalled, or whose making timed out: the
   host may still have it (and may still write into the buffer's memory). Its ID and memory
   stay taken until device_ready() has the host forget it. */
#define ZOMBIE           0xFFFFFFFFUL
struct buffer {
    u32 context, mem_handle, pages; u8 *mem;    /* context: 0 free, or ZOMBIE */
    u32 width, height, scanout;
};
static u32 ctx_owner[MAX_CONTEXTS] = { 0 };
static struct buffer *buffers = 0;
static u32 *page_table = 0;                     /* scratch: one entry per page of a new buffer */
static u32 next_buffer = 0;
static u32 lock = 0;
static u32 gpu_page = 0;                        /* currently requested application image */
static u32 presenter = 0;                       /* process whose final handle returns the display to GDI */
static u32 pending_gpu = 0;                     /* latest GPU image awaiting a display update */
static u32 last_gpu_update = 0;
static u32 display_serial, gpu_flips, gpu_updates, timer_updates;
static u32 cpu_owner, last_cpu_sample, cpu_flips, cpu_samples, cpu_uploads;
#define DISPLAY_PERIOD_MS 17
static u32 visible = 0;                         /* last SET_SCANOUT acknowledged */
static u32 takeover = 0;                        /* virtio has taken the display from VGA */
static u32 refresh_pending = 0;
static u32 dos_active;                          /* foreground DOS VM */
static int dos_reported;
u32 vgpu_dac_vm;                                /* VM observed by the mini-VDD DAC callbacks */
static u32 dos_palette[256];                    /* set bit 24 when the RGB value is known */
static u32 dac_rgb, dac_known;
static volatile u32 dac_generation;
static u8 dac_index, dac_component, dac_writing;
static struct {
    u32 vm, generation;
    u8 window_stream, prefix[2];
} dac_resume;
static int is_vga_mode13(void);
static u32 dac_return_vm, dac_return_token, dac_return_time;
static u32 dac_focus_serial;
static u32 display_w, display_h, display_bpp, vram_bytes, display_offset;
static u32 palette[256];
static u8 *vram;

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
    mem = (u8 *)q->desc;
    if (mem) {
        phys = q->phys;
        zero(mem, 3 * 4096);
    } else
        mem = (u8 *)vmm_alloc_contig(3, &phys);
    if (!mem) {
        dbg("rvd: no memory for queue "); dbg_hex(index); dbg("\r\n");
        return 0;
    }
    q->index = index;
    q->phys = phys;
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

static void enqueue_pair(u16 head, u32 req_pa, u32 req_len, u32 resp_pa, u32 resp_len)
{
    struct vq *q = &ctrlq;
    struct vq_desc *d = &q->desc[head];
    d[0].addr_lo = req_pa;  d[0].addr_hi = 0; d[0].len = req_len;
    d[0].flags = VQ_DESC_NEXT; d[0].next = (u16)(head + 1);
    d[1].addr_lo = resp_pa; d[1].addr_hi = 0; d[1].len = resp_len;
    d[1].flags = VQ_DESC_WRITE; d[1].next = 0;
    q->avail->ring[q->avail->idx % q->size] = head;
    q->avail->idx++;
    wr16(q->notify, 0, q->index);
}

/* Fenced work may complete after the flush queued behind it. Only the head ID identifies a
   response, including a late response to a timed-out regular request. */
static void drain_control(void)
{
    struct vq *q = &ctrlq;
    if (q->used->idx == q->last_used)
        return;
    while (q->used->idx != q->last_used) {
        volatile struct vq_used_elem *e = &q->used->ring[q->last_used % q->size];
        u32 id = e->id;
        if (!id) {
            regular_bytes = e->len;
            regular_done = 1;
        } else if (!(id & 1) && id + 1 < q->size && id < QUEUE_SIZE) {
            u32 slot = id / 2 - 1;
            if (fences_busy & (1UL << slot)) {
                struct fence_slot *s = &fences[slot];
                int ok = e->len == sizeof(s->reply) && s->reply.type == GPU_RESP_OK_NODATA &&
                         (s->reply.flags & 1) && s->reply.fence_lo == s->cmd.hdr.fence_lo &&
                         !s->reply.fence_hi;
                if (id == upload.head && !upload.result) {
                    upload.result = ok && s->cmd.hdr.type == GPU_CMD_TRANSFER_TO_HOST_2D &&
                                    s->reply.fence_lo == upload.fence ? 1 : -1;
                } else if (!ok) {
                    if (!async_failed)
                        dbg("rvd: queued render fence failed, switching to waited fences\r\n");
                    async_failed = 1;
                }
                fences_busy &= ~(1UL << slot);
            }
        }
        q->last_used++;
    }
    (void)rd8(isr, 0);
}

/* The main request remains synchronous. Its dedicated descriptors and pages can be reused
   only once its own used-ring entry has arrived, even if a queued fence completes first. */
static u32 command(u32 req_len, u32 resp_len)
{
    struct vq *q = &ctrlq;
    u32 start, spins = 0;
    if (stalled)
        return 0;
    regular_done = 0;
    enqueue_pair(0, req_phys, req_len, resp_phys, resp_len);

    start = vmm_time_ms();
    while (!regular_done) {
        if (q->used->idx != q->last_used && timeout_ms != RVD_TIMEOUT_AT_ONCE) {
            drain_control();
            if (regular_done)
                break;
        }
        if (timeout_ms == RVD_TIMEOUT_AT_ONCE ||
            ((++spins & 0xFFF) == 0 && vmm_time_ms() - start > timeout_ms)) {
            dbg("rvd: no answer yet to request "); dbg_hex(*(u32 *)req_buf);
            dbg(": nothing goes to the device until it comes\r\n");
            stalled = 1;
            return 0;
        }
    }
    return regular_bytes;
}

/* Start a request in req_buf: `bytes` zeroed, type and context set. Never while stalled: the
   request that is still out lives there (every caller checks `stalled` first, or runs only
   after device_ready() said yes and no request since has timed out). */
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
    struct gpu_one_id *r;

    if (stalled)
        return 0;
    r = (struct gpu_one_id *)request(type, ctx, sizeof(*r));
    r->id = id;
    return simple(sizeof(*r));
}

/* virtio 1.0, 3.1.1: reset, announce ourselves, agree on features, set up the queues, go. */
static int map_transport(void)
{
    u32 bar_phys, bar_len, hi, i, raw;
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
    return 1;
}

static int start_transport(void)
{
    u32 i, hi, handle;
    wr8(common, CC_DEVICE_STATUS, 0);
    for (i = 0; i < 100000 && rd8(common, CC_DEVICE_STATUS) != 0; i++)
        ;
    if (rd8(common, CC_DEVICE_STATUS) != 0)
        return 0;
    fences_busy = regular_done = async_failed = 0;
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
    if (!req_buf)
        req_buf = (u8 *)vmm_alloc_contig(REQ_PAGES, &req_phys);
    if (!resp_buf)
        resp_buf = (u8 *)vmm_alloc_contig(RESP_PAGES, &resp_phys);
    if (!buffers)
        buffers = (struct buffer *)vmm_alloc_pages((MAX_BUFFERS * sizeof(struct buffer) + 4095) >> 12, &handle);
    if (!page_table)
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

    info_incomplete = 1;
    request(GPU_CMD_GET_DISPLAY_INFO, 0, sizeof(*resp));
    n = command(sizeof(*resp), sizeof(*resp) + 16 * sizeof(*d));
    if (n && resp->type == GPU_RESP_OK_DISPLAY_INFO) {
        info.display_w = d->width;
        info.display_h = d->height;
    }
    dbg("rvd: host display "); dbg_hex(info.display_w); dbg(" x "); dbg_hex(info.display_h); dbg("\r\n");

    sets = rd32(devcfg, 12);
    info.num_caps = 0;
    /* After a time-out the request buffer is the host's until its answer comes; device_ready()
       reads the rest then. */
    for (i = 0; i < sets && info.num_caps < RVD_MAX_CAPS && !stalled; i++) {
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
    info_incomplete = stalled;
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
    static int started = 0;

    if (started)
        return 1;
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
    if (!gpu.cap[VIRTIO_PCI_CAP_COMMON].length || !map_transport() || !start_transport())
        return 0;
    started = 1;
    read_host_info();
    return 1;
}

/* ---- Contexts and buffers --------------------------------------------------------------------- */
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_GEN_FAILURE       31
#define ERROR_NOT_SUPPORTED     50
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NO_MORE_ITEMS     259
#define ERROR_TIMEOUT           1460            /* the device is stalled (see `stalled`) */

/* What a failed request tells the caller. */
static u32 failure(void)
{
    return stalled ? ERROR_TIMEOUT : ERROR_GEN_FAILURE;
}

static int valid_context(u32 ctx)
{
    return ctx > 0 && ctx < MAX_CONTEXTS && ctx_owner[ctx] != 0 && ctx_owner[ctx] != ZOMBIE;
}

static struct buffer *find_buffer(u32 handle)
{
    if (handle == 0 || handle > MAX_BUFFERS || buffers[handle - 1].context == 0 ||
        buffers[handle - 1].context == ZOMBIE)
        return 0;
    return &buffers[handle - 1];
}

static int queue_refresh(u32 delay)
{
    if (!refresh_pending) {
        refresh_pending = 1;
        if (!vmm_queue_refresh(delay))
            refresh_pending = 0;
    }
    return refresh_pending != 0;
}

/* A resource's backing list contains one entry per physically contiguous run of pages. */
static int attach_backing(u32 id, u8 *mem, u32 pages)
{
    struct gpu_attach_backing *ab;
    struct gpu_mem_entry *e;
    u32 i, n;

    if (!vmm_page_table(mem, pages, page_table))
        return 0;
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
    return simple(sizeof(*ab) + n * sizeof(*e));
}

static int desktop_refresh(int paced);

static void end_cpu_flips(void)
{
    if (cpu_flips >= 10) {
        dbg("rvd: CPU flips "); dbg_hex(cpu_flips); dbg(", mirror samples "); dbg_hex(cpu_samples);
        dbg(", uploads "); dbg_hex(cpu_uploads); dbg(", bytes/upload "); dbg_hex(display_w * display_h * 4);
        dbg(", async submitted "); dbg_hex(async_uploads);
        dbg(", async displayed "); dbg_hex(async_shown); dbg("\r\n");
    }
    if (cpu_owner)
        cpu_generation++;
    async_uploads = async_shown = 0;
    cpu_owner = cpu_flips = cpu_samples = cpu_uploads = 0;
}

static void drop_gpu_page(void)
{
    if (gpu_flips >= 10) {
        dbg("rvd: GPU flips "); dbg_hex(gpu_flips); dbg(", display updates ");
        dbg_hex(gpu_updates); dbg(", from timer "); dbg_hex(timer_updates); dbg("\r\n");
    }
    gpu_flips = gpu_updates = timer_updates = gpu_page = pending_gpu = 0;
}

/* The host forgets the buffer; then its memory is free again. While the device is stalled the
   buffer becomes a zombie instead, its memory kept: the request still out may write into it. */
static void buffer_destroy(struct buffer *b, int detach)
{
    u32 id = (u32)(b - buffers) + 1;

    if (id == gpu_page || id == visible) {
        drop_gpu_page();
        if (!stalled && display_bpp)
            desktop_refresh(0);
        if (takeover)
            queue_refresh(32);
        if (visible == id)
            visible = 0;                        /* this ID may be reassigned after UNREF */
    }
    if (detach)
        one_id(GPU_CMD_CTX_DETACH_RESOURCE, b->context, id);
    one_id(GPU_CMD_RESOURCE_UNREF, 0, id);
    if (stalled) {
        dbg("rvd: buffer "); dbg_hex(id); dbg(" kept until the device answers again\r\n");
        b->context = ZOMBIE;
        return;
    }
    vmm_free_pages(b->mem_handle);
    b->context = 0;
    b->mem = 0;
}

static u32 buffer_create(const struct rvd_buffer_create *c, struct rvd_buffer *out)
{
    struct gpu_resource_create_3d *rc;
    struct buffer *b;
    u32 i, id, pages;

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
    b->pages = pages;                           /* counted by RVD_GET_STATS, zombie or not */

    rc = (struct gpu_resource_create_3d *)request(GPU_CMD_RESOURCE_CREATE_3D, c->context, sizeof(*rc));
    rc->id = id;
    rc->target = c->target;         rc->format = c->format;         rc->bind = c->bind;
    rc->width = c->width;           rc->height = c->height;         rc->depth = c->depth;
    rc->array_size = c->array_size; rc->last_level = c->last_level; rc->nr_samples = c->nr_samples;
    rc->flags = c->flags;
    if (!simple(sizeof(*rc))) {
        if (stalled) {                          /* the host may make it yet */
            b->context = ZOMBIE;
            return ERROR_TIMEOUT;
        }
        vmm_free_pages(b->mem_handle);
        b->mem = 0;
        return ERROR_GEN_FAILURE;
    }

    if (!attach_backing(id, b->mem, pages) ||
        !one_id(GPU_CMD_CTX_ATTACH_RESOURCE, c->context, id)) {
        b->context = c->context;
        buffer_destroy(b, 0);
        return failure();
    }
    b->context = c->context;
    b->width = c->width;
    b->height = c->height;
    b->scanout = (c->bind & (1UL << 18)) && c->target == 2 && c->depth == 1 &&
                 c->array_size == 1 && c->nr_samples <= 1;
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
    if (!stalled) {
        request(GPU_CMD_CTX_DESTROY, ctx, sizeof(struct gpu_ctrl_hdr));
        simple(sizeof(struct gpu_ctrl_hdr));
    }
    ctx_owner[ctx] = stalled ? ZOMBIE : 0;
}

/* Have the host forget the zombies, now that it answers again; stops if it stalls again. */
static void reap(void)
{
    u32 i, n = 0;

    for (i = 0; i < MAX_BUFFERS && !stalled; i++) {
        struct buffer *b = &buffers[i];
        if (b->context != ZOMBIE)
            continue;
        one_id(GPU_CMD_RESOURCE_UNREF, 0, i + 1);   /* an error answer: it was never made */
        if (stalled)
            break;
        vmm_free_pages(b->mem_handle);
        b->context = 0;
        b->mem = 0;
        n++;
    }
    for (i = 1; i < MAX_CONTEXTS && !stalled; i++) {
        if (ctx_owner[i] != ZOMBIE)
            continue;
        request(GPU_CMD_CTX_DESTROY, i, sizeof(struct gpu_ctrl_hdr));
        simple(sizeof(struct gpu_ctrl_hdr));
        if (stalled)
            break;
        ctx_owner[i] = 0;
        n++;
    }
    dbg("rvd: zombies removed: "); dbg_hex(n); dbg("\r\n");
}

/* Can requests go to the device? After a time-out, only once the late answer has come; the
   zombies go then too. Looks once, without waiting. With the test setting RVD_TIMEOUT_AT_ONCE
   the answer is left alone, so a test sees the stalled state for as long as it wants. */
static int device_ready(void)
{
    if (!stalled)
        return 1;
    if (timeout_ms == RVD_TIMEOUT_AT_ONCE)
        return 0;
    drain_control();
    if (!regular_done)
        return 0;
    if (unref_pending)
        finish_late_unref();
    stalled = 0;
    dbg("rvd: the late answer came, the device is back\r\n");
    reap();
    if (!stalled && info_incomplete)
        read_host_info();
    return !stalled;
}

static u32 count_zombies(void)
{
    u32 i, n = 0;

    for (i = 0; buffers && i < MAX_BUFFERS; i++)
        n += buffers[i].context == ZOMBIE;
    for (i = 1; i < MAX_CONTEXTS; i++)
        n += ctx_owner[i] == ZOMBIE;
    return n;
}

/* One desktop can be on screen while a previous upload pins another. A mode change may
   need a third image until the new scanout is acknowledged. Allocate each backing on use. */
#define DESKTOP_SLOTS 3
struct desktop_image {
    u8 *mem;
    u32 handle, w, h;
    int ready, upload, unsafe;
};
static struct desktop_image desktops[DESKTOP_SLOTS];
static unsigned desktop_slot;
static u32 next_fence;
static int release_desktop(unsigned slot);

static void fence_request(struct gpu_ctrl_hdr *h)
{
    h->flags = 1;                                /* VIRTIO_GPU_FLAG_FENCE */
    h->fence_lo = ++next_fence;
}

static struct fence_slot *new_fence(void)
{
    u32 i, slots = ctrlq.size / 2 - 1;
    drain_control();
    if (slots && !async_failed && timeout_ms != RVD_TIMEOUT_AT_ONCE) {
        if (!fences)
            fences = (struct fence_slot *)vmm_alloc_contig(
                                 (FENCE_SLOTS * sizeof(*fences) + 4095) >> 12, &fences_phys);
        for (i = 0; fences && i < slots; i++) {
            struct fence_slot *s = &fences[i];
            if (fences_busy & (1UL << i))
                continue;
            zero(s, sizeof(*s));
            fence_request(&s->cmd.hdr);
            fences_busy |= 1UL << i;
            return s;
        }
    }
    return 0;
}

static void send_fence(struct fence_slot *s, u32 bytes)
{
    u32 i = (u32)(s - fences), pa = fences_phys + i * sizeof(*s);
    enqueue_pair((u16)((i + 1) * 2), pa, bytes, pa + sizeof(s->cmd), sizeof(s->reply));
}

/* The renderer fence pushes out the preceding GL work before the display command. */
static int render_fence(u32 ctx)
{
    struct gpu_submit *submit;
    struct fence_slot *s = new_fence();
    if (s) {
        s->cmd.hdr.type = GPU_CMD_SUBMIT_3D;
        s->cmd.hdr.ctx_id = ctx;
        send_fence(s, sizeof(s->cmd.submit));
        return 1;
    }
    /* Back-pressure: the normal pair always fits, even with all async descriptors in use. */
    submit = (struct gpu_submit *)request(GPU_CMD_SUBMIT_3D, ctx, sizeof(*submit));
    fence_request(&submit->hdr);
    return simple(sizeof(*submit));
}

static int set_scanout(u32 id, u32 w, u32 h)
{
    struct gpu_set_scanout *s;
    if (visible == id)
        return 1;
    s = (struct gpu_set_scanout *)request(GPU_CMD_SET_SCANOUT, 0, sizeof(*s));
    s->id = id;
    s->rect.w = w;
    s->rect.h = h;
    /* Even if the reply is late, QEMU may already have left VGA. */
    takeover = 1;
    if (!simple(sizeof(*s))) {
        visible = 0;
        return 0;
    }
    visible = id;
    return 1;
}

static int flush_scanout(u32 id, u32 w, u32 h, const struct gpu_rect *damage)
{
    struct gpu_flush *f = (struct gpu_flush *)request(GPU_CMD_RESOURCE_FLUSH, 0, sizeof(*f));
    f->id = id;
    f->rect.w = w;
    f->rect.h = h;
    if (damage)
        f->rect = *damage;
    return simple(sizeof(*f));
}

static void release_upload_test(void)
{
    upload_test_owner = 0;
    upload_test_held = 0;
}

/* The used-ring handler never issues a command: it can run inside a head-0 request.
   Flush only in a caller that owns the queue, for the CPU stream that submitted the image. */
static void retire_upload(int show)
{
    struct desktop_image *d;
    u32 id;
    if (upload_test_owner && vmm_time_ms() - upload_test_start >= 60000)
        release_upload_test();
    if (!upload.head || stalled)
        return;
    drain_control();
    if ((!upload.result || upload_test_held) && !upload.expired &&
        vmm_time_ms() - upload.start > timeout_ms) {
        upload.expired = 1;
        dbg("rvd: desktop upload timed out, keeping its backing until completion\r\n");
    }
    if (!upload.result || upload_test_held)
        return;
    d = &desktops[upload.slot];
    id = MAX_BUFFERS + 1 + upload.slot;
    if (upload.result < 0) {
        /* The returned descriptor is ours again but the reply did not prove the host
           finished reading the image. Replace it and require a fenced UNREF to free it. */
        d->unsafe = upload_disabled = 1;
        dbg("rvd: invalid desktop upload fence, using synchronous transfers\r\n");
    } else if (show && !upload.expired && upload.serial == display_serial && upload.generation == cpu_generation &&
               upload.owner == cpu_owner && !gpu_page && !dos_active && display_bpp &&
               desktop_slot == upload.slot && visible == id && flush_scanout(id, d->w, d->h, 0)) {
        if (upload.generation == cpu_generation) {
            async_shown++;
            cpu_uploads++;
            d->upload = 0;
        }
    }
    upload.head = 0;
    if (upload.slot != desktop_slot)
        release_desktop(upload.slot);
}

static int begin_upload(unsigned slot, u32 id, u32 w, u32 h, u32 serial)
{
    struct fence_slot *s;
    if (upload.head || upload_disabled || !queue_refresh(2) || !(s = new_fence()))
        return 0;
    if (display_serial != serial) {
        fences_busy &= ~(1UL << (s - fences));
        return 0;
    }
    s->cmd.upload.hdr.type = GPU_CMD_TRANSFER_TO_HOST_2D;
    s->cmd.upload.rect.w = w; s->cmd.upload.rect.h = h; s->cmd.upload.id = id;
    upload.head = ((u32)(s - fences) + 1) * 2;
    upload.fence = s->cmd.hdr.fence_lo;
    upload.slot = slot;
    upload.serial = serial;
    upload.owner = cpu_owner;
    upload.generation = cpu_generation;
    upload.start = vmm_time_ms();
    upload.result = upload.expired = 0;
    if (upload_test_owner && upload_test_owner == cpu_owner)
        upload_test_held = 1;
    send_fence(s, sizeof(s->cmd.upload));
    async_uploads++;
    return 1;
}

static int fence_ok(u32 bytes, u32 fence)
{
    struct gpu_ctrl_hdr *r = (struct gpu_ctrl_hdr *)resp_buf;
    return bytes == sizeof(*r) && r->type == GPU_RESP_OK_NODATA && (r->flags & 1) &&
           r->fence_lo == fence && !r->fence_hi;
}

/* A quarantined resource's UNREF can itself time out. Inspect head 0's saved reply
   before ordinary device recovery reuses it for zombie cleanup. */
static void finish_late_unref(void)
{
    struct desktop_image *d = &desktops[unref_pending - 1];
    if (fence_ok(regular_bytes, unref_fence)) {
        vmm_free_pages(d->handle);
        zero(d, sizeof(*d));
    }
    unref_pending = 0;
}

static int release_desktop(unsigned slot)
{
    struct desktop_image *d = &desktops[slot];
    struct gpu_ctrl_hdr *r = (struct gpu_ctrl_hdr *)resp_buf;
    if (!d->mem)
        return 1;
    if ((upload.head && upload.slot == slot) || visible == MAX_BUFFERS + 1 + slot)
        return 0;
    if (d->unsafe) {
        struct gpu_one_id *unref;
        if (stalled)
            return 0;
        unref = (struct gpu_one_id *)request(GPU_CMD_RESOURCE_UNREF, 0, sizeof(*unref));
        unref->id = MAX_BUFFERS + 1 + slot;
        fence_request(&unref->hdr);
        unref_fence = unref->hdr.fence_lo;
        if (!fence_ok(command(sizeof(*unref), sizeof(*r)), unref_fence)) {
            if (stalled)
                unref_pending = slot + 1;
            return 0;
        }
    } else {
        /* A failed CREATE may never have reached the renderer. Keep the DMA backing unless
           UNREF succeeds or the host confirms it has no resource by this ID. */
        if (stalled || (!one_id(GPU_CMD_RESOURCE_UNREF, 0, MAX_BUFFERS + 1 + slot) &&
                        (stalled || r->type != GPU_RESP_ERR_INVALID_RESOURCE)))
            return 0;
    }
    vmm_free_pages(d->handle);
    zero(d, sizeof(*d));
    return 1;
}

static int make_desktop(unsigned slot, u32 w, u32 h)
{
    struct desktop_image *d = &desktops[slot];
    u32 id = MAX_BUFFERS + 1 + slot, pages = (w * h * 4 + 4095) >> 12;
    struct gpu_create_2d *r;

    if (upload.head && upload.slot == slot)
        return 0;
    if (d->ready && !d->unsafe && d->w == w && d->h == h)
        return 1;
    if (!pages || pages > MAX_BUFFER_PAGES || visible == id)
        return 0;
    if (!release_desktop(slot))
        return 0;
    d->mem = (u8 *)vmm_alloc_pages(pages, &d->handle);
    if (!d->mem)
        return 0;
    r = (struct gpu_create_2d *)request(GPU_CMD_RESOURCE_CREATE_2D, 0, sizeof(*r));
    r->id = id;
    r->format = 2;                              /* VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM */
    r->w = w; r->h = h;
    if (!simple(sizeof(*r)) || !attach_backing(id, d->mem, pages)) {
        release_desktop(slot);
        return 0;
    }
    d->w = w; d->h = h;
    d->ready = d->upload = 1;
    return 1;
}

static unsigned prepare_desktop(u32 w, u32 h)
{
    unsigned slot;
    if (make_desktop(desktop_slot, w, h))
        return desktop_slot;
    for (slot = 0; slot < DESKTOP_SLOTS && !stalled; slot++)
        if (slot != desktop_slot && make_desktop(slot, w, h))
            return slot;
    return DESKTOP_SLOTS;
}

static int valid_page(u32 w, u32 h, u32 bpp, u32 offset)
{
    u32 row;
    if (!w || !h || w > 4096 || h > 4096 || (w & 7) ||
        (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        return 0;
    row = w * (bpp / 8);
    return offset <= vram_bytes && offset % row == 0 && h <= (vram_bytes - offset) / row;
}

/* The DIB engine, DirectDraw locks and the software cursor all write these same VGA bytes.
   Sample them at a desktop event so writes without any later driver call remain visible.
   Compare the shadow as we copy. A still desktop generates no device commands. */
static int mirror_pixels(struct desktop_image *d, u32 bpp, u32 offset)
{
    const u8 *src = vram + offset;
    u32 *dst = (u32 *)d->mem;
    u32 n = d->w * d->h, pixel;
    int changed = 0;
    while (n--) {
        switch (bpp) {
        case 32: pixel = *(const u32 *)src; src += 4; break;
        case 24: pixel = src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16); src += 3; break;
        case 16: {
            u32 p = *(const u16 *)src, r = p >> 11, g = (p >> 5) & 63, b = p & 31;
            pixel = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            src += 2;
            break;
        }
        default: pixel = palette[*src++]; break;
        }
        if (*dst != pixel) {
            *dst = pixel;
            changed = 1;
        }
        dst++;
    }
    return changed;
}

/* The virtio-vga BAR0 is the VGA's raw RAM. Text characters, attributes and the
   BIOS-loaded font occupy planes 0, 1 and 2 of each four-byte group. */
struct vga_text {
    u32 cols, rows, cw, ch, start, pitch, font[2], cursor, cursor_top, cursor_bottom;
};

static u8 vga_register(u16 port, u8 index)
{
    u8 value;
    __asm {
        pushfd
        cli
        mov dx, port
        in al, dx
        mov ah, al
        mov al, index
        out dx, al
        inc dx
        in al, dx
        mov value, al
        dec dx
        mov al, ah
        out dx, al
        popfd
    }
    return value;
}

static int read_vga_text(struct vga_text *v)
{
    const u16 crtc = 0x3d4;
    u8 gc = vga_register(0x3ce, 6), sr3, overflow, first;
    u32 lines;
    if ((gc & 0x0d) != 0x0c || !vram || vram_bytes < 256UL * 1024)
        return 0;                               /* standard B800 colour text modes */
    v->cols = 1 + vga_register(crtc, 1);
    v->cw = (vga_register(0x3c4, 1) & 1) ? 8 : 9;
    v->ch = 1 + (vga_register(crtc, 9) & 31);
    overflow = vga_register(crtc, 7);
    lines = 1 + vga_register(crtc, 0x12) + ((overflow & 2) << 7) + ((overflow & 0x40) << 3);
    v->rows = lines / v->ch;
    v->start = ((u32)vga_register(crtc, 0x0c) << 8 | vga_register(crtc, 0x0d)) * 4;
    v->pitch = (u32)vga_register(crtc, 0x13) * 8;
    if (v->cols != 80 || v->ch < 8 || v->rows < 1 || v->rows > 60 ||
        lines % v->ch || v->pitch < 4 * v->cols ||
        v->start + (v->rows - 1) * v->pitch + v->cols * 4 > vram_bytes)
        return 0;
    sr3 = vga_register(0x3c4, 3);
    v->font[0] = (((sr3 & 3) * 2) + ((sr3 >> 4) & 1)) * 32768UL;
    v->font[1] = ((((sr3 >> 2) & 3) * 2) + ((sr3 >> 5) & 1)) * 32768UL;
    first = vga_register(crtc, 0x0a);
    v->cursor = (first & 0x20) ? ~0UL :
                ((u32)vga_register(crtc, 0x0e) << 8 | vga_register(crtc, 0x0f)) * 4;
    v->cursor_top = first & 31;
    v->cursor_bottom = vga_register(crtc, 0x0b) & 31;
    return 1;
}

static int mirror_text(struct desktop_image *d, const struct vga_text *v)
{
    static const u32 colours[16] = {
        0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
        0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff
    };
    u32 row, col, y, x, blink = (vmm_time_ms() >> 9) & 1;
    int changed = 0;
    for (row = 0; row < v->rows; row++) {
        for (col = 0; col < v->cols; col++) {
            u32 cell = v->start + row * v->pitch + col * 4;
            u8 ch = vram[cell], attr = vram[cell + 1];
            const u8 *glyph = vram + v->font[(attr >> 3) & 1] + (u32)ch * 128 + 2;
            u32 fg = colours[attr & 15], bg = colours[(attr >> 4) & 7];
            u32 *out = (u32 *)d->mem + row * v->ch * d->w + col * v->cw;
            for (y = 0; y < v->ch; y++, out += d->w) {
                u32 bits = ((attr & 0x80) && blink) ? 0 : (u32)glyph[y * 4] << 1;
                if (ch >= 0xb0 && ch <= 0xdf)
                    bits |= (bits >> 1) & 1;    /* VGA ninth dot for line drawing */
                if (!blink && cell == v->cursor && y >= v->cursor_top && y <= v->cursor_bottom)
                    bits = 511;
                for (x = 0; x < v->cw; x++, bits <<= 1) {
                    u32 pixel = (bits & 256) ? fg : bg;
                    if (out[x] != pixel) {
                        out[x] = pixel;
                        changed = 1;
                    }
                }
            }
        }
    }
    return changed;
}

/* The main VDD owns the DAC ports. Its callbacks let us observe a DOS VM's BIOS
   and application writes, including palette animation, without disturbing an
   interrupted RGB triplet. A palette belongs to the VM that programmed it. */
int __cdecl vgpu_watch_dac(u32 vm, int mode_set)
{
    if (!takeover || !vram || (dos_active && dos_active != vm))
        return 0;
    if (vgpu_dac_vm != vm || mode_set) {
        zero(&dac_resume, sizeof(dac_resume));
        zero(dos_palette, sizeof(dos_palette));
        dac_writing = 0;
        dac_known = 0;
        dac_generation++;
        vgpu_dac_vm = vm;
    }
    return 1;
}

void __cdecl vgpu_forget_dac(u32 vm)
{
    if (dac_resume.vm == vm)
        zero(&dac_resume, sizeof(dac_resume));
    if (vgpu_dac_vm == vm)
        vgpu_dac_vm = 0;
    if (dos_active == vm) {
        dos_active = 0;
        dac_focus_serial = 0;
        display_serial++;
    }
    if (dac_return_vm == vm)
        dac_return_vm = 0;
}

/* QEMU's standard six-bit VGA DAC fills both low bits from bit zero. */
static u32 dac_to_byte(u32 v)
{
    return ((v & 63) << 2) | ((v & 1) * 3);
}

/* Fullscreen restoration can reset the hardware DAC's write index and RGB
   phase. Resume a transaction explicitly indexed by this VM while windowed
   on its first further data write. Selecting the index and replaying at
   most two bytes reconstructs the write latch without committing a colour
   or moving the read index. The main VDD forwards the new guest byte. */
static void resume_dac_write(void)
{
    u32 generation = dac_resume.generation;
    u8 index = dac_index, count = dac_component;
    u8 red = dac_resume.prefix[0], green = dac_resume.prefix[1];
    dac_resume.generation = 0;
    if (generation != display_serial || !dos_active || dos_active != dac_resume.vm ||
        dos_active != vgpu_dac_vm || !dac_writing || count > 2 || !is_vga_mode13() ||
        !vmm_rearm_dac(dos_active))
        return;
    __asm {
        pushfd
        cli
        mov dx,03c8h
        mov al,index
        out dx,al
        cmp count,0
        je resume_done
        inc dx
        mov al,red
        out dx,al
        cmp count,2
        jne resume_done
        mov al,green
        out dx,al
    resume_done:
        popfd
    }
}

void __cdecl vgpu_dac_out(u32 port, u32 value, int physical)
{
    if (port == 0x3c8) {
        dac_resume.generation = 0;
        dac_resume.window_stream = !dos_active && dac_resume.vm && dac_resume.vm == vgpu_dac_vm;
        dac_index = (u8)value;
        dac_component = 0;
        dac_writing = 1;
    } else if (port == 0x3c7) {
        dac_resume.window_stream = 0;
        dac_resume.generation = 0;
        dac_writing = 0;
        /* This very OUT will install a fresh read index and start at red.
           Only here can we bootstrap an unknown palette with physical reads.
           Never touch the write index/cache. The caller forwards the OUT. */
        if (physical && dos_active == vgpu_dac_vm && dac_known < 256) {
            u32 i;
            __asm {
                mov dx, 03c7h
                xor al, al
                out dx, al
            }
            for (i = 0; i < 256; i++) {
                u8 r, g, b;
                __asm {
                    mov dx, 03c9h
                    in al, dx
                    mov r, al
                    in al, dx
                    mov g, al
                    in al, dx
                    mov b, al
                }
                dos_palette[i] = 0x1000000UL | (dac_to_byte(r) << 16) |
                                 (dac_to_byte(g) << 8) | dac_to_byte(b);
            }
            dac_known = 256;
            dac_generation++;
        }
    } else if (port == 0x3c9 && dac_writing) {
        if (dac_resume.generation)
            resume_dac_write();
        if (dac_resume.window_stream && dac_component < 2)
            dac_resume.prefix[dac_component] = (u8)value;
        dac_rgb = ((dac_rgb << 8) & 0xffffff) | dac_to_byte(value);
        if (++dac_component == 3) {
            u32 colour = 0x1000000UL | dac_rgb;
            if (dos_palette[dac_index] != colour) {
                if (!dos_palette[dac_index])
                    dac_known++;
                dos_palette[dac_index] = colour;
                dac_generation++;
            }
            dac_index++;
            dac_component = 0;
        }
    } else if (port == 0x3c9 && dac_known) {
        /* A write without an observed index can change an unknown entry. */
        zero(dos_palette, sizeof(dos_palette));
        dac_known = 0;
        dac_generation++;
    }
}

void __cdecl vgpu_dac_read_data(void)
{
    dac_resume.window_stream = 0;
    dac_resume.generation = 0;
    dac_writing = 0;
}

/* Strict BIOS mode 13h: packed chain-four, 320-byte rows starting at zero,
   400 visible scanlines with each row repeated. Reject planar modes, custom
   stride/start, scanline splits and double-scanning on top of the row repeat. */
static int is_vga_mode13(void)
{
    u8 overflow, scan;
    u32 lines, split;
    if (!vram || vram_bytes < 64000 || (vga_register(0x3ce, 6) & 0x0d) != 5 ||
        (vga_register(0x3ce, 5) & 0x70) != 0x40 ||
        (vga_register(0x3c4, 4) & 0x0e) != 0x0e ||
        vga_register(0x3d4, 1) != 79 || vga_register(0x3d4, 0x13) != 40 ||
        vga_register(0x3d4, 0x0c) || vga_register(0x3d4, 0x0d) ||
        vga_register(0x3d4, 8) || !(vga_register(0x3d4, 0x14) & 0x40))
        return 0;
    overflow = vga_register(0x3d4, 7);
    scan = vga_register(0x3d4, 9);
    lines = 1 + vga_register(0x3d4, 0x12) + ((overflow & 2) << 7) + ((overflow & 0x40) << 3);
    split = vga_register(0x3d4, 0x18) | ((overflow & 0x10) << 4) | ((scan & 0x40) << 3);
    return lines == 400 && (scan & 0x9f) == 1 && split >= 399;
}

/* Some mode sets nest another INT 10h. Re-arm at the outer return, after
   the main VDD has finished its mode-specific trap changes. A consumed
   interrupt or a destroyed VM cannot leave a permanent outstanding callback. */
u32 __cdecl vgpu_arm_dac_return(u32 vm)
{
    u32 now;
    if (!takeover || !vram || (dos_active && dos_active != vm))
        return 0;
    now = vmm_time_ms();
    if (dac_return_vm == vm && now - dac_return_time < 2000)
        return 0;
    if (!++dac_return_token)
        dac_return_token++;
    dac_return_vm = vm;
    dac_return_time = now;
    return dac_return_token;
}

void __cdecl vgpu_dac_return(u32 token, u32 current)
{
    u32 vm = dac_return_vm;
    if (!vm || token != dac_return_token)
        return;
    dac_return_vm = 0;
    if (current == vm && dos_active == vm && vgpu_dac_vm == vm &&
        vmm_time_ms() - dac_return_time < 2000 && is_vga_mode13())
        vmm_rearm_dac(vm);
}

void __cdecl vgpu_focus_dac(u32 serial, u32 vm)
{
    if (serial && serial == dac_focus_serial && serial == display_serial &&
        vm == dos_active && vm == vgpu_dac_vm && is_vga_mode13() && vmm_rearm_dac(vm))
        dac_focus_serial = 0;
}

static int mirror_mode13(struct desktop_image *d, struct gpu_rect *damage, u32 generation)
{
    const u8 *src = vram;
    u32 *row = (u32 *)d->mem;
    u32 y, x, left = 640, right = 0, top = 200, bottom = 0;
    if (dos_active != vgpu_dac_vm)
        return -1;
    for (y = 0; y < 200; y++, row += 1280) {
        u32 row_left = 640, row_right = 0;
        for (x = 0; x < 640; x += 2) {
            u32 colour = dos_palette[*src++];
            if (!colour)
                return -1;
            colour &= 0xffffff;
            if (row[x] != colour || row[x + 1] != colour ||
                row[640 + x] != colour || row[641 + x] != colour) {
                row[x] = row[x + 1] = row[640 + x] = row[641 + x] = colour;
                if (!row_right)
                    row_left = x;
                row_right = x + 2;
            }
        }
        if (row_right) {
            if (row_left < left) left = row_left;
            if (row_right > right) right = row_right;
            if (y < top) top = y;
            bottom = y + 1;
        }
    }
    if (generation != dac_generation)
        return -1;
    if (!right)
        return 0;
    damage->x = left; damage->w = right - left;
    damage->y = top * 2; damage->h = (bottom - top) * 2;
    return 1;
}

static int desktop_refresh(int paced)
{
    static unsigned last_mode13_slot = DESKTOP_SLOTS;
    static u32 last_mode13_serial, last_mode13_palette;
    u32 w = display_w, h = display_h, bpp = display_bpp, offset = display_offset;
    u32 serial = display_serial, start = vmm_time_ms();
    u32 generation = 0;
    int text = 0, mode13 = 0, changed;
    struct vga_text v;
    unsigned slot = desktop_slot, previous_mode13_slot, i;
    struct desktop_image *d;
    struct gpu_rect damage;
    const struct gpu_rect *region = 0;
    struct gpu_upload_2d *t;
    u32 id;

    if (stalled)
        return 0;
    paced = paced && cpu_owner && cpu_samples && !gpu_page && !dos_active;
    retire_upload(paced);
    if (stalled || serial != display_serial)
        return 0;
    /* Ordinary flips may continue selecting pages while this shadow is immutable.
       A handoff builds its synchronous image in an available private slot. */
    if (upload.head && upload.slot == slot && paced && !upload.expired &&
        upload.generation == cpu_generation && upload.owner == cpu_owner && queue_refresh(2))
        return 1;
    /* A scan can modify the shadow before a mode switch, incomplete palette, or
       failed transfer ends this attempt. Only a successful refresh restores the baseline. */
    previous_mode13_slot = last_mode13_slot;
    last_mode13_slot = DESKTOP_SLOTS;
    if (dos_active) {
        if ((text = read_vga_text(&v)) != 0) {
            w = v.cols * v.cw;
            h = v.rows * v.ch;
        } else if ((mode13 = is_vga_mode13()) != 0) {
            w = 640;
            h = 400;
        } else {
            if (dos_reported != 1) {
                dbg("rvd: waiting for supported VGA mode\r\n");
                dos_reported = 1;
            }
            return 0;
        }
        if (dac_focus_serial && dac_focus_serial == serial) {
            /* The first ordinary sample also covers an unscheduled focus
               event. Each acquisition gets at most one successful rearm. */
            if (!mode13 || dos_active != vgpu_dac_vm || vmm_rearm_dac(dos_active))
                dac_focus_serial = 0;
        }
    } else if (!valid_page(w, h, bpp, offset))
        return 0;
    if (!vram) {
        u32 bar = pci_read32(gpu.bdf, 0x10);
        if (!(bar & 0xfffffff0UL) || (bar & 1) ||
            ((bar & 6) == 4 && pci_read32(gpu.bdf, 0x14)))
            return 0;
        vram = (u8 *)vmm_map_phys(bar & 0xfffffff0UL, vram_bytes);
    }
    if (!vram)
        return 0;
    slot = prepare_desktop(w, h);
    if (slot == DESKTOP_SLOTS)
        return 0;
    d = &desktops[slot];
    id = MAX_BUFFERS + 1 + slot;
    if (text)
        changed = mirror_text(d, &v);
    else if (mode13) {
        generation = dac_generation;
        if (previous_mode13_slot != slot || slot != desktop_slot || visible != id ||
            last_mode13_serial != serial)
            d->upload = 1;
        changed = mirror_mode13(d, &damage, generation);
        if (changed > 0 && !d->upload && last_mode13_palette == generation)
            region = &damage;
    } else
        changed = mirror_pixels(d, bpp, offset);
    if (changed)
        d->upload = 1;
    if (changed < 0) {
        if (dos_reported != 4) {
            dbg("rvd: waiting for DOS mode 13h palette\r\n");
            dos_reported = 4;
        }
        return 0;
    }
    /* mirror_mode13 checked that its decoded snapshot used one palette. Later
       DAC writes belong to the next sample, including writes during the fence. */
    if (serial != display_serial)
        return 0;
    if (d->upload) {
        if (paced && visible == id && slot == desktop_slot && begin_upload(slot, id, w, h, serial)) {
            last_cpu_sample = start;
            cpu_samples++;
            return 1;
        }
        if (serial != display_serial)
            return 0;
        t = (struct gpu_upload_2d *)request(GPU_CMD_TRANSFER_TO_HOST_2D, 0, sizeof(*t));
        t->id = id; t->rect.w = w; t->rect.h = h;
        if (region) {
            t->rect = *region;
            t->off_lo = (region->y * w + region->x) * 4;
        }
        fence_request(&t->hdr);
        if (!fence_ok(command(sizeof(*t), sizeof(t->hdr)), t->hdr.fence_lo)) {
            d->unsafe = 1;
            return 0;
        }
    }
    if (serial != display_serial)
        return 0;
    if ((visible != id || d->upload) &&
        (!set_scanout(id, w, h) || !flush_scanout(id, w, h, region))) {
        d->upload = 1;
        return 0;
    }
    if (serial != display_serial)
        return 0;
    if (mode13) {
        last_mode13_serial = serial;
        last_mode13_palette = generation;
        last_mode13_slot = slot;
#ifdef RVD_TRACE_DOS13_DAMAGE
        if (d->upload) {
            dbg("rvd: DOS13 upload x y w h: ");
            dbg_hex(region ? region->x : 0); dbg(" "); dbg_hex(region ? region->y : 0);
            dbg(" "); dbg_hex(region ? region->w : w); dbg(" "); dbg_hex(region ? region->h : h);
            dbg("\r\n");
        }
#endif
    }
    if (cpu_owner) {
        last_cpu_sample = start;
        cpu_samples++;
        if (d->upload)
            cpu_uploads++;
    }
    d->upload = 0;
    if (text && dos_reported != 2) {
        dbg("rvd: displaying DOS text "); dbg_hex(v.cols); dbg(" x "); dbg_hex(v.rows); dbg("\r\n");
        dos_reported = 2;
    } else if (mode13 && dos_reported != 3) {
        dbg("rvd: displaying DOS mode 13h 320x200\r\n");
        dos_reported = 3;
    }
    desktop_slot = slot;
    for (i = 0; i < DESKTOP_SLOTS && !stalled; i++)
        if (i != slot)
            release_desktop(i);
    return !stalled && serial == display_serial;
}

/* Display callbacks merely record the VGA layout. No GPU command runs with the Win16 lock
   in a mode-change or palette callback. Older display drivers never send these messages. */
void __cdecl vgpu_display_mode(u32 bpp, u32 size, u32 bytes)
{
    end_cpu_flips();
    display_serial++;
    dos_active = 0;
    dac_resume.generation = 0;
    dac_focus_serial = 0;
    display_w = size >> 16;
    display_h = size & 0xffff;
    vram_bytes = bytes;
    display_bpp = bpp;
    display_offset = presenter = 0;
    drop_gpu_page();
    if (takeover)
        queue_refresh(1);
}

void __cdecl vgpu_display_colour(u32 index, u32 colour)
{
    palette[index] = colour & 0xffffff;
}

/* SET_SCANOUT takes the console from VGA for this boot. The DOS image uses the same
   transport so the GPU contexts and display resources stay valid across a switch. */
u32 __cdecl vgpu_show_dos(u32 vm)
{
    if (takeover && vram) {
        end_cpu_flips();
        display_serial++;
        dos_active = vm;
        if (dac_resume.vm == vm && dac_resume.window_stream && dac_writing && is_vga_mode13())
            dac_resume.generation = display_serial;
        dac_resume.window_stream = 0;
        dac_focus_serial = vgpu_dac_vm == vm ? display_serial : 0;
        dos_reported = 0;
        queue_refresh(1);
        return dac_focus_serial;
    }
    return 0;
}

void __cdecl vgpu_leave_windows(void)
{
    if (dos_active) {
        zero(&dac_resume, sizeof(dac_resume));
        if (dos_active == vgpu_dac_vm && is_vga_mode13())
            dac_resume.vm = dos_active;
    }
    end_cpu_flips();
    display_serial++;
    dos_active = 0;
    dac_focus_serial = 0;
    display_bpp = presenter = 0;
    drop_gpu_page();
}

static int publish_gpu(u32 id, struct buffer *b)
{
    u32 serial = display_serial, start = vmm_time_ms();
    pending_gpu = 0;
    if (b && b->width == display_w && b->height == display_h && display_bpp &&
        render_fence(b->context) && set_scanout(id, b->width, b->height) &&
        flush_scanout(id, b->width, b->height, 0) && display_serial == serial) {
        end_cpu_flips();
        if (!gpu_page)
            dbg("rvd: presenting GPU image\r\n");
        gpu_page = id;
        last_gpu_update = start;
        gpu_updates++;
        return 1;
    }
    drop_gpu_page();
    if (!stalled && display_bpp)
        desktop_refresh(0);
    if (display_bpp)
        queue_refresh(1);
    return 0;
}

void __cdecl vgpu_desktop_tick(void)
{
    u32 age;
    refresh_pending = 0;
    if (!takeover || (!upload.head && ((!display_bpp && !dos_active) || (gpu_page && !pending_gpu))))
        return;
    if (lock && !vmm_mutex_owner(lock)) {
        vmm_mutex_enter(lock);
        if (upload.head && device_ready())
            retire_upload(1);
        if (takeover && (display_bpp || dos_active) && device_ready()) {
            if (!gpu_page) {
                if (!cpu_owner || vmm_time_ms() - last_cpu_sample >= DISPLAY_PERIOD_MS)
                    desktop_refresh(cpu_owner != 0);
            } else if (pending_gpu == gpu_page && vmm_time_ms() - last_gpu_update >= DISPLAY_PERIOD_MS &&
                     publish_gpu(gpu_page, find_buffer(gpu_page)))
                timer_updates++;
        }
        vmm_mutex_leave(lock);
    }
    if (upload.head)
        queue_refresh(upload.expired || upload_test_held ? 17 : 2);
    else if (takeover && (display_bpp || dos_active)) {
        if (!gpu_page && !cpu_owner)
            queue_refresh(dos_active ? 50 : 32);
        else if (!gpu_page || pending_gpu) {
            age = vmm_time_ms() - (gpu_page ? last_gpu_update : last_cpu_sample);
            queue_refresh(age < DISPLAY_PERIOD_MS ? DISPLAY_PERIOD_MS - age : 1);
        }
    }
}

static u32 present_image(const struct rvd_present *p, u32 process)
{
    struct buffer *b = find_buffer(p->buffer);
    u32 age;
    static int reported = 0;
    if (!lock)                                   /* required for periodic desktop updates */
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!b || b->context != p->context || !valid_context(p->context) ||
        ctx_owner[p->context] != process || !b->scanout ||
        b->width != p->width || b->height != p->height ||
        display_w != p->width || display_h != p->height ||
        !valid_page(display_w, display_h, display_bpp, display_offset)) {
        if (!reported) {
            dbg("rvd: scanout rejected, context "); dbg_hex(p->context);
            dbg(", resource "); dbg_hex(p->buffer);
            dbg(", scanout bind "); dbg_hex(b ? b->scanout : 0);
            dbg(", mode "); dbg_hex(display_w); dbg(" x "); dbg_hex(display_h);
            dbg(" x "); dbg_hex(display_bpp); dbg("\r\n");
            reported = 1;
        }
        return ERROR_INVALID_PARAMETER;
    }
    if (!takeover) {
        if (!queue_refresh(1) || !desktop_refresh(0)) {
            if (takeover && display_bpp)
                queue_refresh(1);
            dbg("rvd: could not prepare desktop scanout\r\n");
            return failure();
        }
        dbg("rvd: desktop scanout ready\r\n");
    }
    presenter = process;
    gpu_flips++;
    /* The resource already on screen can be drawn many times between display refreshes.
       Retain the most recent content there and arm the event even for a single final flip. */
    if (gpu_page == p->buffer && visible == p->buffer) {
        age = vmm_time_ms() - last_gpu_update;
        if (age < DISPLAY_PERIOD_MS) {
            pending_gpu = p->buffer;
            if (queue_refresh(DISPLAY_PERIOD_MS - age))
                return 0;
        }
    }
    return publish_gpu(p->buffer, b) ? 0 : failure();
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

/* The DirectDraw heap (RVD_HEAP): made at the first call, kept until Windows ends. */
static struct rvd_heap heap = { 0 };

/* Handles open, by process. A process can hold more than one: the DirectDraw DLL keeps one for
   RVD_HEAP, Mesa's winsys one for OpenGL. Its GPU contexts end when its last one is closed;
   ending them at the first close took OpenGL's context away from a program that released one
   DirectDraw object and made another (Tomb Raider: The Last Revelation: every allocation after
   that failed). Returns the count after the change; a process not found counts as 0. */
#define MAX_PROCESSES 64
static struct { u32 process, handles; } openers[MAX_PROCESSES];

static u32 opened(u32 process, int change)
{
    u32 i, free_slot = MAX_PROCESSES;

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (openers[i].handles && openers[i].process == process)
            break;
        if (!openers[i].handles && free_slot == MAX_PROCESSES)
            free_slot = i;
    }
    if (i == MAX_PROCESSES) {
        if (change < 0 || free_slot == MAX_PROCESSES)
            return 0;                           /* not counted: treat as the last handle */
        i = free_slot;
        openers[i].process = process;
    }
    openers[i].handles += change;
    return openers[i].handles;
}

/* Keep the logical page current on every Flip, including a flip to page zero. Its pixels
   remain in VGA RAM. Only sample them when the display is due. The event also catches
   writes to a held front buffer after the last Flip. GDI and ownership changes are immediate. */
static u32 show_cpu_page(u32 process, u32 offset, int flip)
{
    u32 previous = display_offset, serial = display_serial, age;
    if (!valid_page(display_w, display_h, display_bpp, offset) || dos_active)
        return ERROR_INVALID_PARAMETER;
    if (stalled)
        return ERROR_TIMEOUT;
    if (flip && cpu_owner == process && !gpu_page && cpu_samples) {
        retire_upload(1);
        if (stalled || serial != display_serial)
            return failure();
        age = vmm_time_ms() - last_cpu_sample;
        if (age < DISPLAY_PERIOD_MS && queue_refresh(DISPLAY_PERIOD_MS - age)) {
            display_offset = offset;
            cpu_flips++;
            return 0;
        }
    }
    if (!flip || cpu_owner != process)
        end_cpu_flips();
    if (flip)
        cpu_owner = process;
    display_offset = offset;
    if (!desktop_refresh(flip)) {
        if (serial == display_serial) {
            display_offset = previous;
            end_cpu_flips();
            if (gpu_page)
                pending_gpu = gpu_page;
            queue_refresh(1);
        }
        return failure();
    }
    if (gpu_page)
        drop_gpu_page();
    if (presenter != process)
        presenter = 0;
    if (flip) {
        /* The HAL closes its temporary ioctl handle after each Flip. A separately open
           device handle lets final process cleanup return the display start to page zero. */
        if (opened(process, 0) > 1)
            presenter = process;
        cpu_flips++;
    }
    queue_refresh(flip ? DISPLAY_PERIOD_MS : 32);
    return 0;
}

/* The last device handle closes even when the presenting program is terminated before its
   DirectDraw cleanup can run. Page zero belongs to GDI; a previous software flip may have
   left both our mirror and the card's display-start register on a different page. Keep the
   other display client's page if it has already taken over. */
static void presenter_closed(u32 process)
{
    if (!presenter || presenter != process)
        return;
    end_cpu_flips();
    presenter = 0;
    display_offset = 0;
    drop_gpu_page();
    if (!display_bpp)
        return;
    __asm {
        pushfd
        cli
        mov dx, 01ceh
        in ax, dx
        mov cx, ax                          ; preserve the VBE index across the two writes
        mov ax, 9                           ; VBE_Y_OFFSET
        out dx, ax
        inc dx
        xor ax, ax
        out dx, ax
        dec dx
        mov ax, cx
        out dx, ax
        popfd
    }
    if (takeover) {
        queue_refresh(1);
        if (!stalled)
            desktop_refresh(0);
    }
}

static u32 dispatch(struct dioc *p)
{
    u32 n = 0, i, err;

    if (p->code == DIOC_CLOSE) {                /* the handle is closed, or its program ended */
        if (opened(p->process, -1) > 0)
            return 0;                           /* the process has another handle open */
        if (upload_test_owner == p->process)
            release_upload_test();
        if (timeout_owner == p->process) {      /* a test setting ends with its program */
            timeout_ms = DEFAULT_TIMEOUT_MS;
            timeout_owner = 0;
        }
        if (common)
            device_ready();                     /* stalled or not, its contexts go */
        presenter_closed(p->process);
        for (i = 1; common && i < MAX_CONTEXTS; i++) {
            if (ctx_owner[i] == p->process)
                context_destroy(i);
        }
        return 0;
    }
    if (p->code == RVD_DESKTOP && !p->in_len) {
        if (!p->out || p->out_len < sizeof(u32))
            return ERROR_INVALID_PARAMETER;
        *(u32 *)p->out = takeover;
        if (p->returned)
            *p->returned = sizeof(u32);
        return 0;
    }
    if (p->code == RVD_GET_STATS) {             /* what is held now: no device needed */
        struct rvd_stats st;
        zero(&st, sizeof(st));
        for (i = 0; buffers && i < MAX_BUFFERS; i++) {
            if (buffers[i].context == 0)
                continue;
            st.buffers += buffers[i].context != ZOMBIE;
            st.buffer_pages += buffers[i].pages;
        }
        for (i = 1; i < MAX_CONTEXTS; i++)
            st.contexts += ctx_owner[i] != 0 && ctx_owner[i] != ZOMBIE;
        st.zombies = count_zombies();
        st.heap_bytes = heap.size;
        n = p->out_len < sizeof(st) ? p->out_len : sizeof(st);
        copy(p->out, &st, n);
        if (p->returned)
            *p->returned = n;
        return 0;
    }
    if (p->code == RVD_HEAP) {                  /* system memory for DirectDraw: no device needed */
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
    if (p->code == RVD_DESKTOP && !takeover) {
        if (p->in_len < sizeof(u32))
            return ERROR_INVALID_PARAMETER;
        if (p->returned)
            *p->returned = 0;
        return 0;                               /* VGA follows its own display-start register */
    }
    if (p->code == RVD_SET_TIMEOUT) {           /* for tests; 0 is the default again */
        if (p->in_len == sizeof(struct rvd_upload_test)) {
            const struct rvd_upload_test *test = (const struct rvd_upload_test *)p->in;
            u32 status = RVD_UPLOAD_SUPPORTED;
            if (!p->in || test->ms || !p->out || p->out_len < sizeof(status) ||
                test->action > RVD_UPLOAD_TIMEOUT)
                return ERROR_INVALID_PARAMETER;
            if (upload_test_owner && vmm_time_ms() - upload_test_start >= 60000)
                release_upload_test();
            if (test->action == RVD_UPLOAD_ARM) {
                if (upload_test_owner)
                    return ERROR_INVALID_PARAMETER;
                upload_test_owner = p->process;
                upload_test_start = vmm_time_ms();
            } else if (upload_test_owner == p->process) {
                if (test->action == RVD_UPLOAD_RELEASE) {
                    release_upload_test();
                    retire_upload(1);
                } else if (test->action == RVD_UPLOAD_TIMEOUT) {
                    if (!upload_test_held || timeout_ms == RVD_TIMEOUT_AT_ONCE)
                        return ERROR_INVALID_PARAMETER;
                    /* Exercise the production deadline check with a real submitted transfer.
                       The test delays retirement even if the used entry already arrived. */
                    upload.start = vmm_time_ms() - timeout_ms - 1;
                    retire_upload(0);
                }
            }
            if (upload.head && !stalled)
                drain_control();
            if (upload_test_owner == p->process) {
                status |= RVD_UPLOAD_ARMED;
                if (upload_test_held)
                    status |= RVD_UPLOAD_HELD | (upload.expired ? RVD_UPLOAD_TIMED_OUT : 0) |
                              (upload.result > 0 ? RVD_UPLOAD_REPLIED : 0);
            }
            *(u32 *)p->out = status;
            if (p->returned)
                *p->returned = sizeof(status);
            return 0;
        }
        if (p->in_len < 4)
            return ERROR_INVALID_PARAMETER;
        if (p->out_len >= 4) {                  /* what is held for the host before this call */
            *(u32 *)p->out = count_zombies();
            n = 4;
        }
        timeout_ms = *(u32 *)p->in ? *(u32 *)p->in : DEFAULT_TIMEOUT_MS;
        timeout_owner = *(u32 *)p->in ? p->process : 0;
        if (p->returned)
            *p->returned = n;
        return 0;
    }
    /* While stalled, requests that need the device are refused straight away; letting go of
       contexts and buffers still works (they become zombies). */
    if (!device_ready() && (p->code == RVD_GET_CAPS || p->code == RVD_CONTEXT_CREATE ||
                            p->code == RVD_BUFFER_CREATE || p->code == RVD_SUBMIT ||
                            p->code == RVD_TRANSFER || p->code == RVD_PRESENT))
        return ERROR_TIMEOUT;
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
            return failure();
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
        if (!simple(sizeof(*r))) {
            if (stalled)                        /* the host may make it yet */
                ctx_owner[i] = ZOMBIE;
            return failure();
        }
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
            return failure();
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
            return failure();
        break;
    }
    case RVD_PRESENT:
        if (p->in_len < sizeof(struct rvd_present))
            return ERROR_INVALID_PARAMETER;
        err = present_image((const struct rvd_present *)p->in, p->process);
        if (err)
            return err;
        break;
    case RVD_DESKTOP: {
        const struct rvd_desktop *d = (const struct rvd_desktop *)p->in;
        if (p->in_len < sizeof(d->offset))
            return ERROR_INVALID_PARAMETER;
        err = show_cpu_page(p->process, d->offset, p->in_len >= sizeof(*d) &&
                            (d->flags & RVD_DESKTOP_FLIP));
        if (err)
            return err;
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
    u32 err;

    if (p->code == DIOC_OPEN) {                /* counted outside the mutex: an open can come
                                                   where blocking is not allowed, and nothing
                                                   in opened() blocks or lets another thread in */
        opened(p->process, 1);
        return 0;
    }
    if (!lock)
        lock = vmm_mutex_create();
    if (lock)
        vmm_mutex_enter(lock);
    err = dispatch(p);
    if (lock)
        vmm_mutex_leave(lock);
    return err;
}
