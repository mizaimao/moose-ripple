/*
 * surface-probe: what WindowServer does when a window's surface is bound the way a hardware GL
 * driver binds it (docs/re/ioaccelerator-interface.md, trace 3). It makes a window, adds a
 * surface to it and calls CGSBindSurface(cid, wid, sid, KIND, MODE, DISPLAY), then asks our
 * kext whether an accelerator surface with that ID exists now and where it is
 * (RIPPLE_GPU_SURFACE_INFO): WindowServer creates it, the app does not.
 *   surface-probe [-k KIND] [-m MODE] [-d DISPLAY] [-p] [SECONDS]
 * Defaults: kind 2, mode 0x24, the main display's ID, 8 s. The window moves after 3 s and the
 * kext is asked again, to see the region follow.
 * -p: also show a picture in the surface the way a driver would at a swap: a GPU buffer the
 * size of the surface (top half red, bottom half blue, a white square in the top left corner,
 * a green bar that moves down) is handed to the kext with RIPPLE_GPU_SURFACE_PRESENT ten times
 * a second, and the window stays where it is.
 * Built without ARC (10.6).
 */
#import <Cocoa/Cocoa.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ripple_gpu_uapi.h"

extern int CGSMainConnectionID(void);
extern int CGSAddSurface(int cid, int wid, int *sid);
extern int CGSRemoveSurface(int cid, int wid, int sid);
extern int CGSSetSurfaceBounds(int cid, int wid, int sid, CGRect bounds);
extern int CGSOrderSurface(int cid, int wid, int sid, int place, int relative);
extern int CGSBindSurface(int cid, int wid, int sid, int kind, int mode, unsigned last);

static io_connect_t gpu;

enum { PW = 400, PH = 300 };
static uint32_t pctx, pbuf;
static uint32_t *ppix;

/* A context and a 400x300 BGRA texture with its memory mapped here. */
static int present_setup(void)
{
    uint64_t in[1] = { 0 }, out[2] = { 0, 0 };
    uint32_t n = 1;
    if (IOConnectCallScalarMethod(gpu, RIPPLE_GPU_CONTEXT_CREATE, in, 1, out, &n) != KERN_SUCCESS)
        return 0;
    pctx = (uint32_t)out[0];
    struct ripple_gpu_buffer_create bc;
    memset(&bc, 0, sizeof(bc));
    bc.size = PW * PH * 4;
    bc.params_size = sizeof(struct ripple_virgl_buffer_params);
    struct ripple_virgl_buffer_params *p = (struct ripple_virgl_buffer_params *)bc.params;
    p->target = 2;                  /* 2D texture */
    p->format = 1;                  /* B8G8R8A8 */
    p->bind = (1 << 1) | (1 << 3);  /* render target, sampler view */
    p->width = PW;
    p->height = PH;
    p->depth = 1;
    p->array_size = 1;
    n = 2;
    if (IOConnectCallMethod(gpu, RIPPLE_GPU_BUFFER_CREATE, NULL, 0, &bc, sizeof(bc), out, &n,
                            NULL, NULL) != KERN_SUCCESS)
        return 0;
    pbuf = (uint32_t)out[0];
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gpu, pbuf, mach_task_self(), &addr, &size, kIOMapAnywhere) != KERN_SUCCESS)
        return 0;
    ppix = (uint32_t *)(uintptr_t)addr;
    return 1;
}

/* Draw frame `f` (first row = top of the picture), upload it and hand it to the kext. */
static int present_frame(int sid, int f)
{
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            uint32_t c = y < PH / 2 ? 0xffff0000 : 0xff0000ff;      /* red over blue (BGRA) */
            if (x < 40 && y < 40)
                c = 0xffffffff;
            if (y >= (f * 6) % PH && y < (f * 6) % PH + 10)
                c = 0xff00ff00;
            ppix[y * PW + x] = c;
        }
    struct ripple_gpu_transfer t;
    memset(&t, 0, sizeof(t));
    t.context = pctx;
    t.buffer = pbuf;
    t.direction = RIPPLE_GPU_TO_DEVICE;
    t.width = PW;
    t.height = PH;
    t.depth = 1;
    uint64_t out[1] = { 0 };
    uint32_t n = 1;
    if (IOConnectCallMethod(gpu, RIPPLE_GPU_TRANSFER, NULL, 0, &t, sizeof(t), out, &n, NULL, NULL) != KERN_SUCCESS)
        return -1;
    uint64_t in[4] = { (uint32_t)sid, pbuf, pctx, RIPPLE_GPU_PRESENT_TOP_FIRST };
    n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(gpu, RIPPLE_GPU_SURFACE_PRESENT, in, 4, out, &n);
    return kr == KERN_SUCCESS ? (int)out[0] : -2;
}

static void ask(const char *when, int sid)
{
    struct ripple_gpu_surface_info si;
    uint64_t in = (uint32_t)sid;
    size_t size = sizeof(si);

    memset(&si, 0, sizeof(si));
    kern_return_t kr = gpu ? IOConnectCallMethod(gpu, RIPPLE_GPU_SURFACE_INFO, &in, 1, NULL, 0,
                                                 NULL, NULL, &si, &size) : KERN_FAILURE;
    if (kr != KERN_SUCCESS)
        printf("%s: the kext has no surface 0x%x (0x%x)\n", when, sid, kr);
    else
        printf("%s: kext surface 0x%x: flags 0x%x, %ux%u at %d,%d on a %ux%u screen, shape calls %u\n",
               when, si.id, si.flags, si.width, si.height, si.x, si.y, si.screen_width,
               si.screen_height, si.shape_seq);
    fflush(stdout);
}

/* -t: the host command the kext uses, sent from here between two buffers of our own, and the
 * result read back: does a copy with the rows turned over work in a new context? */
static uint32_t make_buffer(uint32_t **pix)
{
    struct ripple_gpu_buffer_create bc;
    uint64_t out[2] = { 0, 0 };
    uint32_t n = 2;
    memset(&bc, 0, sizeof(bc));
    bc.size = PW * PH * 4;
    bc.params_size = sizeof(struct ripple_virgl_buffer_params);
    struct ripple_virgl_buffer_params *p = (struct ripple_virgl_buffer_params *)bc.params;
    p->target = 2;
    p->format = 1;
    p->bind = (1 << 1) | (1 << 3);
    p->width = PW;
    p->height = PH;
    p->depth = 1;
    p->array_size = 1;
    if (IOConnectCallMethod(gpu, RIPPLE_GPU_BUFFER_CREATE, NULL, 0, &bc, sizeof(bc), out, &n,
                            NULL, NULL) != KERN_SUCCESS)
        return 0;
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gpu, (uint32_t)out[0], mach_task_self(), &addr, &size, kIOMapAnywhere) != KERN_SUCCESS)
        return 0;
    *pix = (uint32_t *)(uintptr_t)addr;
    return (uint32_t)out[0];
}

static int transfer(uint32_t buf, int from_device)
{
    struct ripple_gpu_transfer t;
    uint64_t out[1] = { 0 };
    uint32_t n = 1;
    memset(&t, 0, sizeof(t));
    t.context = pctx;
    t.buffer = buf;
    t.direction = from_device ? RIPPLE_GPU_FROM_DEVICE : RIPPLE_GPU_TO_DEVICE;
    t.width = PW;
    t.height = PH;
    t.depth = 1;
    return IOConnectCallMethod(gpu, RIPPLE_GPU_TRANSFER, NULL, 0, &t, sizeof(t), out, &n, NULL, NULL) == KERN_SUCCESS;
}

static int self_test(void)
{
    uint64_t in[2] = { 0, 0 }, out[2] = { 0, 0 };
    uint32_t n = 1, *a = NULL, *b = NULL;
    if (IOConnectCallScalarMethod(gpu, RIPPLE_GPU_CONTEXT_CREATE, in, 1, out, &n) != KERN_SUCCESS)
        return 1;
    pctx = (uint32_t)out[0];
    uint32_t ha = make_buffer(&a), hb = make_buffer(&b);
    if (!ha || !hb)
        return 1;
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++)
            a[y * PW + x] = y < PH / 2 ? 0xffff0000 : 0xff0000ff;
    printf("upload: %d\n", transfer(ha, 0));
    uint32_t keep_top = a[10], keep_bottom = a[(PH - 1) * PW + 10];
    memset(a, 0, PW * PH * 4);
    printf("read the source back: %d; top row 0x%08x (was 0x%08x), bottom row 0x%08x (was 0x%08x)\n",
           transfer(ha, 1), a[10], keep_top, a[(PH - 1) * PW + 10], keep_bottom);
    {
        /* A plain copy first (RESOURCE_COPY_REGION), into the lower half of b. */
        struct { struct ripple_gpu_submit s; uint32_t handles[2]; uint32_t cmd[14]; } r;
        uint64_t o[1] = { 0 };
        uint32_t on = 1;
        memset(&r, 0, sizeof(r));
        r.s.context = pctx;
        r.s.num_handles = 2;
        r.s.cmd_size = sizeof(r.cmd);
        r.handles[0] = ha;
        r.handles[1] = hb;
        r.cmd[0] = 17 | (13 << 16);
        r.cmd[1] = hb; r.cmd[3] = 0; r.cmd[4] = 0;
        r.cmd[6] = ha; r.cmd[8] = 0; r.cmd[9] = 0; r.cmd[11] = PW; r.cmd[12] = PH; r.cmd[13] = 1;
        kern_return_t k = IOConnectCallMethod(gpu, RIPPLE_GPU_SUBMIT, NULL, 0, &r, sizeof(r), o, &on, NULL, NULL);
        uint64_t wi[2] = { o[0], 1000 };
        on = 1;
        IOConnectCallScalarMethod(gpu, RIPPLE_GPU_WAIT, wi, 2, o, &on);
        printf("plain copy: submit 0x%x, read back %d: top row 0x%08x, bottom row 0x%08x\n", k,
               transfer(hb, 1), b[10], b[(PH - 1) * PW + 10]);
        memset(b, 0, PW * PH * 4);
    }
    struct { struct ripple_gpu_submit s; uint32_t handles[2]; uint32_t cmd[22]; } m;
    memset(&m, 0, sizeof(m));
    m.s.context = pctx;
    m.s.num_handles = 2;
    m.s.cmd_size = sizeof(m.cmd);
    m.handles[0] = ha;
    m.handles[1] = hb;
    uint32_t *c = m.cmd;
    c[0] = 16 | (21 << 16);
    c[1] = 0xf;
    c[4] = hb; c[6] = 1; c[7] = 0; c[8] = 0; c[10] = PW; c[11] = PH; c[12] = 1;
    c[13] = ha; c[15] = 1; c[16] = 0; c[17] = PH; c[19] = PW; c[20] = (uint32_t)-PH; c[21] = 1;
    n = 1;
    kern_return_t kr = IOConnectCallMethod(gpu, RIPPLE_GPU_SUBMIT, NULL, 0, &m, sizeof(m), out, &n, NULL, NULL);
    printf("submit of the copy: 0x%x\n", kr);
    in[0] = out[0];
    in[1] = 1000;
    n = 1;
    IOConnectCallScalarMethod(gpu, RIPPLE_GPU_WAIT, in, 2, out, &n);
    printf("read back: %d\n", transfer(hb, 1));
    printf("copy, top row: 0x%08x (source bottom row was 0x%08x); bottom row: 0x%08x (source top 0x%08x)\n",
           b[10], a[(PH - 1) * PW + 10], b[(PH - 1) * PW + 10], a[10]);
    return 0;
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int kind = 2, mode = 0x24;
    unsigned last = CGMainDisplayID();
    double seconds = 8;
    int present = 0, test = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc)
            kind = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            mode = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-p"))
            present = 1;
        else if (!strcmp(argv[i], "-t"))
            test = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            last = (unsigned)strtoul(argv[++i], NULL, 0);
        else
            seconds = atof(argv[i]);
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                   IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
    if (!svc || IOServiceOpen(svc, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &gpu) != KERN_SUCCESS) {
        printf("no connection to the kext's GPU client; going on without asking it\n");
        gpu = 0;
    }

    if (test)
        return gpu ? self_test() : 1;

    NSRect content = NSMakeRect(300, 300, 400, 300);
    NSWindow *win = [[NSWindow alloc] initWithContentRect:content
                                                styleMask:NSTitledWindowMask
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [win setTitle:@"surface-probe"];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];

    int cid = CGSMainConnectionID(), wid = (int)[win windowNumber], sid = 0;
    int e = CGSAddSurface(cid, wid, &sid);
    printf("CGSAddSurface(cid %d, wid %d) = %d, surface %d (0x%x)\n", cid, wid, e, sid, sid);
    /* Surface bounds count from the top left of the window's frame: below the title bar. */
    e = CGSSetSurfaceBounds(cid, wid, sid, CGRectMake(0, [win frame].size.height - 300, 400, 300));
    printf("CGSSetSurfaceBounds = %d\n", e);
    e = CGSOrderSurface(cid, wid, sid, 1, 0);
    printf("CGSOrderSurface = %d\n", e);
    ask("before binding", sid);
    e = CGSBindSurface(cid, wid, sid, kind, mode, last);
    printf("CGSBindSurface(kind %d, mode 0x%x, 0x%x) = %d\n", kind, mode, last, e);
    fflush(stdout);
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    ask("after binding", sid);

    if (present) {
        if (!gpu || !present_setup()) {
            printf("no GPU buffer for presenting\n");
            return 1;
        }
        int shown = 0, not_shown = 0, failed = 0, frames = (int)(seconds * 10);
        for (int f = 0; f < frames; f++) {
            int r = present_frame(sid, f);
            if (r == 1) shown++; else if (r == 0) not_shown++; else failed++;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        }
        printf("presented %d frames: %d shown, %d refused (not a plain region), %d failed\n",
               frames, shown, not_shown, failed);
        ask("after presenting", sid);
        CGSRemoveSurface(cid, wid, sid);
        [pool drain];
        return 0;
    }

    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.5]];
    [win setFrameOrigin:NSMakePoint(600, 150)];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
    ask("after moving the window", sid);

    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds > 4.5 ? seconds - 4.5 : 0.1]];
    e = CGSRemoveSurface(cid, wid, sid);
    printf("CGSRemoveSurface = %d\n", e);
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    ask("after removing the surface", sid);
    [pool drain];
    return 0;
}
