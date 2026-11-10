/*
 * virgl-clear: M3's acceptance test. Through the GPU user client (docs/kext-uapi.md): create a
 * context and a 64x64 render-target buffer, submit a virgl command stream that clears it to
 * (0.25, 0.5, 0.75, 1.0), copy it back from the GPU, map it and check the pixels.
 * Exit 0 on PASS.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ripple_gpu_uapi.h"
#include "virgl_protocol.h"
#include "virgl_hw.h"

/* Gallium values the virgl protocol uses (virglrenderer 0.10.4,
 * src/gallium/include/pipe/p_defines.h): enum pipe_texture_target, PIPE_CLEAR_COLOR0. */
#define PIPE_TEXTURE_2D   2
#define PIPE_CLEAR_COLOR0 (1 << 2)

#define W 64
#define H 64

static io_connect_t conn;

static void die(const char *what, kern_return_t kr)
{
    fprintf(stderr, "FAIL: %s: 0x%x\n", what, kr);
    exit(1);
}

static uint32_t fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }

int main(void)
{
    kern_return_t kr;
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                   IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
    if (!svc)
        die("no RippleAccelerator service", 0);
    kr = IOServiceOpen(svc, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &conn);
    IOObjectRelease(svc);
    if (kr)
        die("IOServiceOpen", kr);

    struct ripple_gpu_info info;
    size_t infoSize = sizeof(info);
    if ((kr = IOConnectCallStructMethod(conn, RIPPLE_GPU_GET_INFO, NULL, 0, &info, &infoSize)))
        die("GET_INFO", kr);
    printf("device \"%s\", API %u, backend %u, %u capability blobs\n", info.name,
           info.api_version, info.backend, info.num_caps);
    if (info.backend != RIPPLE_GPU_BACKEND_VIRGL)
        die("not a virgl device", 0);

    uint64_t in[2], out[2];
    uint32_t outCnt = 1;
    in[0] = 0;
    if ((kr = IOConnectCallScalarMethod(conn, RIPPLE_GPU_CONTEXT_CREATE, in, 1, out, &outCnt)))
        die("CONTEXT_CREATE", kr);
    uint32_t ctx = (uint32_t)out[0];

    struct ripple_gpu_buffer_create bc;
    memset(&bc, 0, sizeof(bc));
    bc.size = W * H * 4;
    bc.params_size = sizeof(struct ripple_virgl_buffer_params);
    struct ripple_virgl_buffer_params *p = (struct ripple_virgl_buffer_params *)bc.params;
    p->target = PIPE_TEXTURE_2D;
    p->format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    p->bind = VIRGL_BIND_RENDER_TARGET;
    p->width = W;
    p->height = H;
    p->depth = 1;
    p->array_size = 1;
    outCnt = 2;
    if ((kr = IOConnectCallMethod(conn, RIPPLE_GPU_BUFFER_CREATE, NULL, 0, &bc, sizeof(bc),
                                  out, &outCnt, NULL, NULL)))
        die("BUFFER_CREATE", kr);
    uint32_t buf = (uint32_t)out[0];
    printf("context %u, buffer %u (%llu bytes)\n", ctx, buf, (unsigned long long)out[1]);

    /* Commands: a surface on the buffer, make it the colour buffer, clear it. */
    uint32_t cmd[64];
    unsigned n = 0;
    const uint32_t surface = 1;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE);
    cmd[n++] = surface;
    cmd[n++] = buf;
    cmd[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmd[n++] = 0;                               /* level */
    cmd[n++] = 0;                               /* first | last layer << 16 */
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1));
    cmd[n++] = 1;                               /* nr_cbufs */
    cmd[n++] = 0;                               /* no depth/stencil surface */
    cmd[n++] = surface;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE);
    cmd[n++] = PIPE_CLEAR_COLOR0;
    cmd[n++] = fbits(0.25f);
    cmd[n++] = fbits(0.5f);
    cmd[n++] = fbits(0.75f);
    cmd[n++] = fbits(1.0f);
    cmd[n++] = 0;                               /* depth: double 0.0 */
    cmd[n++] = 0;
    cmd[n++] = 0;                               /* stencil */

    uint8_t sub[sizeof(struct ripple_gpu_submit) + 4 + sizeof(cmd)];
    struct ripple_gpu_submit *s = (struct ripple_gpu_submit *)sub;
    memset(s, 0, sizeof(*s));
    s->context = ctx;
    s->num_handles = 1;
    s->cmd_size = n * 4;
    memcpy(sub + sizeof(*s), &buf, 4);
    memcpy(sub + sizeof(*s) + 4, cmd, n * 4);
    outCnt = 1;
    if ((kr = IOConnectCallMethod(conn, RIPPLE_GPU_SUBMIT, NULL, 0, sub, sizeof(*s) + 4 + n * 4,
                                  out, &outCnt, NULL, NULL)))
        die("SUBMIT", kr);
    printf("submitted %u command words, fence %llu\n", n, (unsigned long long)out[0]);

    struct ripple_gpu_transfer t;
    memset(&t, 0, sizeof(t));
    t.context = ctx;
    t.buffer = buf;
    t.direction = RIPPLE_GPU_FROM_DEVICE;
    t.width = W;
    t.height = H;
    t.depth = 1;
    t.stride = W * 4;
    outCnt = 1;
    if ((kr = IOConnectCallMethod(conn, RIPPLE_GPU_TRANSFER, NULL, 0, &t, sizeof(t),
                                  out, &outCnt, NULL, NULL)))
        die("TRANSFER", kr);
    in[0] = out[0];
    in[1] = 1000;
    outCnt = 1;
    if ((kr = IOConnectCallScalarMethod(conn, RIPPLE_GPU_WAIT, in, 2, out, &outCnt)) || !out[0])
        die("WAIT", kr);

    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    if ((kr = IOConnectMapMemory64(conn, buf, mach_task_self(), &addr, &size, kIOMapAnywhere)))
        die("IOConnectMapMemory64", kr);
    const uint8_t *px = (const uint8_t *)(uintptr_t)addr;
    unsigned bad = 0;
    for (unsigned i = 0; i < W * H; i++) {
        const uint8_t *q = px + i * 4;          /* B, G, R, A */
        if (abs(q[0] - 191) > 1 || abs(q[1] - 128) > 1 || abs(q[2] - 64) > 1 || q[3] != 255)
            bad++;
    }
    const uint8_t *c = px + (32 * W + 32) * 4;
    printf("pixel (32,32): B %u G %u R %u A %u (expect about 191 128 64 255); %u of %u pixels wrong\n",
           c[0], c[1], c[2], c[3], bad, W * H);

    IOConnectUnmapMemory64(conn, buf, mach_task_self(), addr);
    in[0] = buf;
    IOConnectCallScalarMethod(conn, RIPPLE_GPU_BUFFER_DESTROY, in, 1, NULL, NULL);
    in[0] = ctx;
    IOConnectCallScalarMethod(conn, RIPPLE_GPU_CONTEXT_DESTROY, in, 1, NULL, NULL);
    IOServiceClose(conn);
    printf("%s (%d-bit process)\n", bad ? "FAIL" : "PASS", (int)(sizeof(void *) * 8));
    return bad ? 1 : 0;
}
