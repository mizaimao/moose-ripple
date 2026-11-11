/*
 * gpu-stress: checks the GPU user client's limits (docs/kext-uapi.md) the way Mesa's winsys
 * will use them: thousands of buffers created and destroyed in mixed order, every buffer
 * mapped and told apart, one submit at the maximum command size naming 4096 buffers, then a
 * clear and read-back in the same context to show the context survived. Exit 0 on PASS.
 *   gpu-stress [BUFFERS]      default 6000
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ripple_gpu_uapi.h"
#include "virgl_protocol.h"
#include "virgl_hw.h"

/* Gallium values (virglrenderer 0.10.4, p_defines.h). */
#define PIPE_BUFFER       0
#define PIPE_TEXTURE_2D   2
#define PIPE_CLEAR_COLOR0 (1 << 2)

#define MAX_HANDLES   4096                      /* per SUBMIT */
#define MAX_CMD_BYTES (320 * 1024)              /* per SUBMIT */
#define MESA_CMD_DW   ((64 * 1024) + 1024)      /* Mesa's VIRGL_MAX_CMDBUF_DWORDS */

static io_connect_t conn;
static int failures;

static void die(const char *what, kern_return_t kr)
{
    fprintf(stderr, "FAIL: %s: 0x%x\n", what, kr);
    exit(1);
}

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static uint32_t fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }

static kern_return_t create(uint32_t target, uint32_t format, uint32_t bind, uint32_t w,
                            uint32_t h, uint64_t size, uint32_t *handle)
{
    struct ripple_gpu_buffer_create bc;
    memset(&bc, 0, sizeof(bc));
    bc.size = size;
    bc.params_size = sizeof(struct ripple_virgl_buffer_params);
    struct ripple_virgl_buffer_params *p = (struct ripple_virgl_buffer_params *)bc.params;
    p->target = target;
    p->format = format;
    p->bind = bind;
    p->width = w;
    p->height = h;
    p->depth = 1;
    p->array_size = 1;
    uint64_t out[2];
    uint32_t outCnt = 2;
    kern_return_t kr = IOConnectCallMethod(conn, RIPPLE_GPU_BUFFER_CREATE, NULL, 0, &bc,
                                           sizeof(bc), out, &outCnt, NULL, NULL);
    *handle = kr ? 0 : (uint32_t)out[0];
    return kr;
}

static kern_return_t destroy(uint32_t handle)
{
    uint64_t in = handle;
    return IOConnectCallScalarMethod(conn, RIPPLE_GPU_BUFFER_DESTROY, &in, 1, NULL, NULL);
}

static kern_return_t submit(uint32_t ctx, const uint32_t *handles, uint32_t nh,
                            const uint32_t *cmd, uint32_t ndw)
{
    size_t size = sizeof(struct ripple_gpu_submit) + nh * 4 + ndw * 4;
    uint8_t *sub = malloc(size);
    struct ripple_gpu_submit *s = (struct ripple_gpu_submit *)sub;
    memset(s, 0, sizeof(*s));
    s->context = ctx;
    s->num_handles = nh;
    s->cmd_size = ndw * 4;
    memcpy(sub + sizeof(*s), handles, nh * 4);
    memcpy(sub + sizeof(*s) + nh * 4, cmd, ndw * 4);
    uint64_t fence;
    uint32_t outCnt = 1;
    kern_return_t kr = IOConnectCallMethod(conn, RIPPLE_GPU_SUBMIT, NULL, 0, sub, size, &fence,
                                           &outCnt, NULL, NULL);
    free(sub);
    return kr;
}

/* NOP commands filling exactly ndw dwords. */
static void nops(uint32_t *cmd, uint32_t ndw)
{
    uint32_t n = 0;
    while (n < ndw) {
        uint32_t len = ndw - n - 1;
        if (len > VIRGL_CMD0_MAX_DWORDS)
            len = VIRGL_CMD0_MAX_DWORDS;
        cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, len);
        memset(cmd + n, 0, len * 4);
        n += len;
    }
}

int main(int argc, char **argv)
{
    unsigned nbuf = argc > 1 ? (unsigned)atoi(argv[1]) : 6000;
    kern_return_t kr;
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                   IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
    if (!svc)
        die("no RippleAccelerator service", 0);
    if ((kr = IOServiceOpen(svc, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &conn)))
        die("IOServiceOpen", kr);
    IOObjectRelease(svc);

    uint64_t in[2], out[2];
    uint32_t outCnt = 1;
    in[0] = 0;
    if ((kr = IOConnectCallScalarMethod(conn, RIPPLE_GPU_CONTEXT_CREATE, in, 1, out, &outCnt)))
        die("CONTEXT_CREATE", kr);
    uint32_t ctx = (uint32_t)out[0];

    /* 1. Many small buffers (vertex-buffer sized), then free every third in a scrambled
     *    order and create replacements: exercises the kernel's handle table. */
    uint32_t *h = calloc(nbuf + nbuf / 3 + 1, 4);
    unsigned made = 0;
    for (unsigned i = 0; i < nbuf; i++)
        if (!create(PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER, 4096, 1,
                    4096, &h[i]))
            made++;
    char msg[160];
    snprintf(msg, sizeof(msg), "created %u of %u buffers", made, nbuf);
    check(made == nbuf, msg);

    unsigned freed = 0, bad = 0;
    for (unsigned k = 0; k < nbuf; k++) {
        unsigned i = (k * 7919u) % nbuf;        /* 7919 is prime: visits every index once */
        if (i % 3 == 0 && h[i]) {
            if (destroy(h[i]))
                bad++;
            h[i] = 0;
            freed++;
        }
    }
    unsigned again = 0;
    for (unsigned i = 0; i < nbuf; i += 3)
        if (!create(PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER, 4096, 1,
                    4096, &h[i]))
            again++;
    snprintf(msg, sizeof(msg), "freed %u in mixed order (%u errors), created %u again",
             freed, bad, again);
    check(!bad && again == freed, msg);
    check(destroy(0x7fffffff) != 0, "destroying an unknown handle is refused");

    /* 2. Map every buffer, mark it, and read all marks back: no two share memory. */
    mach_vm_address_t *addr = calloc(nbuf, sizeof(*addr));
    unsigned mapped = 0;
    for (unsigned i = 0; i < nbuf; i++) {
        mach_vm_size_t size = 0;
        if (h[i] && !IOConnectMapMemory64(conn, h[i], mach_task_self(), &addr[i], &size,
                                          kIOMapAnywhere) && size >= 4096) {
            *(volatile uint32_t *)(uintptr_t)addr[i] = h[i] ^ 0x5a5a5a5a;
            mapped++;
        }
    }
    bad = 0;
    for (unsigned i = 0; i < nbuf; i++)
        if (addr[i] && *(volatile uint32_t *)(uintptr_t)addr[i] != (h[i] ^ 0x5a5a5a5a))
            bad++;
    snprintf(msg, sizeof(msg), "mapped %u buffers, %u marks wrong", mapped, bad);
    check(mapped == nbuf && !bad, msg);
    for (unsigned i = 0; i < nbuf; i++)
        if (addr[i])
            IOConnectUnmapMemory64(conn, h[i], mach_task_self(), addr[i]);

    /* 3. One submit as large as Mesa makes (66560 dwords of NOPs) naming 4096 buffers. */
    uint32_t *cmd = malloc(MAX_CMD_BYTES);
    unsigned nh = nbuf < MAX_HANDLES ? nbuf : MAX_HANDLES;
    nops(cmd, MESA_CMD_DW);
    kr = submit(ctx, h, nh, cmd, MESA_CMD_DW);
    snprintf(msg, sizeof(msg), "submit of %u bytes with %u buffers: 0x%x", MESA_CMD_DW * 4, nh, kr);
    check(!kr, msg);
    nops(cmd, MAX_CMD_BYTES / 4);
    kr = submit(ctx, h, 1, cmd, MAX_CMD_BYTES / 4);
    snprintf(msg, sizeof(msg), "submit at the limit (%u bytes): 0x%x", MAX_CMD_BYTES, kr);
    check(!kr, msg);
    kr = submit(ctx, h, 1, cmd, MAX_CMD_BYTES / 4 + 1);
    check(kr != 0, "submit over the limit is refused");
    kr = submit(ctx, h, MAX_HANDLES + 1, cmd, 4);
    check(kr != 0, "submit naming more than 4096 buffers is refused");

    /* 4. The context still works: clear a render target and read it back. */
    uint32_t rt;
    if ((kr = create(PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET,
                     64, 64, 64 * 64 * 4, &rt)))
        die("BUFFER_CREATE render target", kr);
    unsigned n = 0;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE);
    cmd[n++] = 1;
    cmd[n++] = rt;
    cmd[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmd[n++] = 0;
    cmd[n++] = 0;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1));
    cmd[n++] = 1;
    cmd[n++] = 0;
    cmd[n++] = 1;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE);
    cmd[n++] = PIPE_CLEAR_COLOR0;
    cmd[n++] = fbits(0.25f);
    cmd[n++] = fbits(0.5f);
    cmd[n++] = fbits(0.75f);
    cmd[n++] = fbits(1.0f);
    cmd[n++] = 0;
    cmd[n++] = 0;
    cmd[n++] = 0;
    if ((kr = submit(ctx, &rt, 1, cmd, n)))
        die("SUBMIT clear", kr);
    struct ripple_gpu_transfer t;
    memset(&t, 0, sizeof(t));
    t.context = ctx;
    t.buffer = rt;
    t.direction = RIPPLE_GPU_FROM_DEVICE;
    t.width = 64;
    t.height = 64;
    t.depth = 1;
    t.stride = 64 * 4;
    outCnt = 1;
    if ((kr = IOConnectCallMethod(conn, RIPPLE_GPU_TRANSFER, NULL, 0, &t, sizeof(t), out,
                                  &outCnt, NULL, NULL)))
        die("TRANSFER", kr);
    mach_vm_address_t a = 0;
    mach_vm_size_t size = 0;
    if ((kr = IOConnectMapMemory64(conn, rt, mach_task_self(), &a, &size, kIOMapAnywhere)))
        die("IOConnectMapMemory64", kr);
    const uint8_t *px = (const uint8_t *)(uintptr_t)a;
    bad = 0;
    for (unsigned i = 0; i < 64 * 64; i++) {
        const uint8_t *q = px + i * 4;
        if (abs(q[0] - 191) > 1 || abs(q[1] - 128) > 1 || abs(q[2] - 64) > 1 || q[3] != 255)
            bad++;
    }
    snprintf(msg, sizeof(msg), "clear after the big submits: %u of 4096 pixels wrong", bad);
    check(!bad, msg);

    /* DISPLAY with a rectangle beyond the screen must be refused in the kernel: the host sizes
     * its console to the rectangle without looking at the buffer (audit, 2026-11-11). Only
     * run against a kext that has the check: without it this request can end the VM. */
    uint64_t disp[5] = { rt, 0, 0, 0x7fffffff, 0x7fffffff };
    kr = IOConnectCallScalarMethod(conn, RIPPLE_GPU_DISPLAY, disp, 5, NULL, NULL);
    snprintf(msg, sizeof(msg), "DISPLAY of a rectangle beyond the screen refused (0x%x)", kr);
    check(kr == kIOReturnBadArgument, msg);

    /* The surface client (type 0, present with Quartz Extreme on) returns no stale kernel
     * memory: an unlock asked for 4 KB of structure and 16 scalars gets none of the first and
     * zeros for the second. */
    svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                      IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
    io_connect_t surf = 0;
    if (svc && IOServiceOpen(svc, mach_task_self(), 0, &surf) == KERN_SUCCESS) {
        static uint8_t leak[4096];
        uint64_t opt = 0, sc[16];
        uint32_t scCnt = 16;
        size_t leakSize = sizeof(leak);
        memset(leak, 0xA5, sizeof(leak));
        memset(sc, 0xA5, sizeof(sc));
        kr = IOConnectCallMethod(surf, 1 /* ReadUnlockOptions */, &opt, 1, NULL, 0, sc, &scCnt,
                                 leak, &leakSize);
        unsigned nonzero = 0;
        for (unsigned i = 0; i < scCnt && i < 16; i++)
            nonzero += sc[i] != 0;
        snprintf(msg, sizeof(msg), "surface client unlock returns no data (0x%x, %lu structure "
                 "bytes, %u non-zero scalars)", kr, (unsigned long)leakSize, nonzero);
        check(kr == KERN_SUCCESS && leakSize == 0 && nonzero == 0, msg);
        IOServiceClose(surf);
    } else {
        printf("skip: no surface client (Quartz Extreme off)\n");
    }
    if (svc)
        IOObjectRelease(svc);

    /* Closing the connection frees everything (checked in the kernel log by the caller). */
    IOServiceClose(conn);
    printf("%s (%d-bit process, %u buffers)\n", failures ? "FAIL" : "PASS",
           (int)(sizeof(void *) * 8), nbuf);
    return failures ? 1 : 0;
}
