/*
 * scanout-test: M5's first step (docs/re/m5-plan.md, K2). Puts a GPU buffer on the display
 * through the user client's DISPLAY method, then gives the display back.
 *   1. a render-target buffer the size of the screen, filled from guest memory (row 0 first):
 *      top half red, bottom half blue, then shown (expect red on top if row 0 is the top);
 *   2. after SECONDS, refilled with green and shown again (a "present");
 *   3. after SECONDS more, DISPLAY 0: the desktop comes back.
 *   scanout-test [SECONDS]      default 3
 */
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ripple_gpu_uapi.h"
#include "virgl_hw.h"

#define PIPE_TEXTURE_2D 2

static io_connect_t conn;

static void die(const char *what, kern_return_t kr)
{
    fprintf(stderr, "FAIL: %s: 0x%x\n", what, kr);
    exit(1);
}

static void fill(uint32_t *px, uint32_t w, uint32_t h, int phase)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            px[y * w + x] = phase == 0 ? (y < h / 2 ? 0xffff0000u : 0xff0000ffu)  /* BGRA in memory */
                                       : 0xff00c000u;
}

static void upload(uint32_t ctx, uint32_t buf, uint32_t w, uint32_t h)
{
    struct ripple_gpu_transfer t;
    uint64_t fence;
    uint32_t n = 1;
    memset(&t, 0, sizeof(t));
    t.context = ctx;
    t.buffer = buf;
    t.direction = RIPPLE_GPU_TO_DEVICE;
    t.width = w;
    t.height = h;
    t.depth = 1;
    t.stride = w * 4;
    kern_return_t kr = IOConnectCallMethod(conn, RIPPLE_GPU_TRANSFER, NULL, 0, &t, sizeof(t),
                                           &fence, &n, NULL, NULL);
    if (kr)
        die("TRANSFER", kr);
}

static void show(uint32_t buf, uint32_t w, uint32_t h)
{
    uint64_t in[5] = { buf, 0, 0, w, h };
    kern_return_t kr = IOConnectCallScalarMethod(conn, RIPPLE_GPU_DISPLAY, in, 5, NULL, NULL);
    if (kr)
        die(buf ? "DISPLAY buffer" : "DISPLAY 0", kr);
}

int main(int argc, char **argv)
{
    unsigned secs = argc > 1 ? (unsigned)atoi(argv[1]) : 3;
    uint32_t w = (uint32_t)CGDisplayPixelsWide(CGMainDisplayID());
    uint32_t h = (uint32_t)CGDisplayPixelsHigh(CGMainDisplayID());
    kern_return_t kr;

    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                   IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
    if (!svc)
        die("no RippleAccelerator service", 0);
    if ((kr = IOServiceOpen(svc, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &conn)))
        die("IOServiceOpen", kr);
    IOObjectRelease(svc);

    uint64_t in = 0, out[2];
    uint32_t n = 1;
    if ((kr = IOConnectCallScalarMethod(conn, RIPPLE_GPU_CONTEXT_CREATE, &in, 1, out, &n)))
        die("CONTEXT_CREATE", kr);
    uint32_t ctx = (uint32_t)out[0];

    struct ripple_gpu_buffer_create bc;
    memset(&bc, 0, sizeof(bc));
    bc.size = (uint64_t)w * h * 4;
    bc.params_size = sizeof(struct ripple_virgl_buffer_params);
    struct ripple_virgl_buffer_params *p = (struct ripple_virgl_buffer_params *)bc.params;
    p->target = PIPE_TEXTURE_2D;
    p->format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    p->bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_SCANOUT;
    p->width = w;
    p->height = h;
    p->depth = 1;
    p->array_size = 1;
    n = 2;
    if ((kr = IOConnectCallMethod(conn, RIPPLE_GPU_BUFFER_CREATE, NULL, 0, &bc, sizeof(bc), out,
                                  &n, NULL, NULL)))
        die("BUFFER_CREATE", kr);
    uint32_t buf = (uint32_t)out[0];

    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    if ((kr = IOConnectMapMemory64(conn, buf, mach_task_self(), &addr, &size, kIOMapAnywhere)))
        die("IOConnectMapMemory64", kr);
    uint32_t *px = (uint32_t *)(uintptr_t)addr;

    printf("display %ux%u; showing top red / bottom blue for %u s\n", w, h, secs);
    fflush(stdout);
    fill(px, w, h, 0);
    upload(ctx, buf, w, h);
    show(buf, w, h);
    sleep(secs);

    printf("green, presented again, for %u s\n", secs);
    fflush(stdout);
    fill(px, w, h, 1);
    upload(ctx, buf, w, h);
    show(buf, w, h);
    sleep(secs);

    show(0, 0, 0);
    printf("display given back\n");
    IOConnectUnmapMemory64(conn, buf, mach_task_self(), addr);
    IOServiceClose(conn);
    printf("PASS (no errors; what the screen showed is checked separately)\n");
    return 0;
}
