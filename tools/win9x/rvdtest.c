/*
 * rvdtest: talk to the moose-ripple mini-VDD (ripplevd.vxd) from a Win32 program.
 *
 *   rvdtest            prints what the VxD and the host report
 *   rvdtest clear      with a 3D-capable host (virgl): makes a context and a 64x64 render
 *                      target, has the host GPU clear it to orange, reads it back through the
 *                      buffer's memory and paints it in the top left corner of the screen
 *   rvdtest leak       the same, but leaves the context and buffer for the VxD to clean up
 *                      when the program ends
 *
 * Built with Visual C++ 6 for Windows 95/98 (tools/win9x/build-tools.sh). The virgl numbers
 * below are from virglrenderer's protocol (virgl_protocol.h, virgl_hw.h; MIT).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "rvd.h"

typedef unsigned long u32;

#define VIRGL_CMD0(cmd, obj, len)   ((cmd) | ((obj) << 8) | ((u32)(len) << 16))
#define VIRGL_CCMD_CREATE_OBJECT          1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE  5
#define VIRGL_CCMD_CLEAR                  7
#define VIRGL_OBJECT_SURFACE              8
#define VIRGL_FORMAT_B8G8R8A8_UNORM       1
#define VIRGL_BIND_RENDER_TARGET          2
#define PIPE_TEXTURE_2D                   2
#define PIPE_CLEAR_COLOR0                 4

#define W 64
#define H 64

static HANDLE dev;

static int call(DWORD code, void *in, DWORD in_len, void *out, DWORD out_len, const char *what)
{
    DWORD n = 0;
    if (!DeviceIoControl(dev, code, in, in_len, out, out_len, &n, NULL)) {
        printf("%s failed, error %lu\n", what, GetLastError());
        return 0;
    }
    return 1;
}

static u32 fbits(float f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

static int clear_test(int leak)
{
    struct rvd_buffer_create bc;
    struct rvd_buffer buf;
    struct rvd_transfer t;
    struct { struct rvd_submit s; u32 cmd[32]; } sub;
    const u32 surface = 1;
    u32 ctx = 0, i = 0, first;
    BITMAPINFO bi;
    HDC dc;

    if (!call(RVD_CONTEXT_CREATE, "rvdtest", 7, &ctx, sizeof ctx, "context create"))
        return 0;
    memset(&bc, 0, sizeof bc);
    bc.context = ctx;
    bc.size = W * H * 4;
    bc.target = PIPE_TEXTURE_2D;
    bc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    bc.bind = VIRGL_BIND_RENDER_TARGET;
    bc.width = W; bc.height = H; bc.depth = 1; bc.array_size = 1;
    if (!call(RVD_BUFFER_CREATE, &bc, sizeof bc, &buf, sizeof buf, "buffer create"))
        return 0;
    printf("context %lu, buffer %lu: %lu bytes at %p\n", ctx, buf.handle, buf.size, buf.address);

    sub.s.context = ctx;
    sub.cmd[i++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    sub.cmd[i++] = surface; sub.cmd[i++] = buf.handle; sub.cmd[i++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    sub.cmd[i++] = 0; sub.cmd[i++] = 0;                         /* level; first and last layer */
    sub.cmd[i++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    sub.cmd[i++] = 1; sub.cmd[i++] = 0; sub.cmd[i++] = surface; /* 1 colour buffer, no depth */
    sub.cmd[i++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    sub.cmd[i++] = PIPE_CLEAR_COLOR0;
    sub.cmd[i++] = fbits(1.0f); sub.cmd[i++] = fbits(0.5f); sub.cmd[i++] = fbits(0.0f);
    sub.cmd[i++] = fbits(1.0f);                                 /* red, green, blue, alpha */
    sub.cmd[i++] = 0; sub.cmd[i++] = 0; sub.cmd[i++] = 0;       /* depth (a double), stencil */
    sub.s.bytes = i * 4;
    if (!call(RVD_SUBMIT, &sub, sizeof sub.s + i * 4, NULL, 0, "submit"))
        return 0;

    memset(&t, 0, sizeof t);
    t.context = ctx; t.buffer = buf.handle; t.direction = RVD_FROM_HOST;
    t.width = W; t.height = H; t.depth = 1;
    if (!call(RVD_TRANSFER, &t, sizeof t, NULL, 0, "transfer from host"))
        return 0;

    first = *(u32 *)buf.address;                /* the buffer's memory, read in place */
    printf("first pixel 0x%08lx (orange is 0xffff8000)\n", first);

    dc = GetDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    SetDIBitsToDevice(dc, 8, 8, W, H, 0, 0, 0, H, buf.address, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);

    if (!leak) {
        call(RVD_BUFFER_DESTROY, &buf.handle, sizeof buf.handle, NULL, 0, "buffer destroy");
        call(RVD_CONTEXT_DESTROY, &ctx, sizeof ctx, NULL, 0, "context destroy");
    }
    return first == 0xFFFF8000UL;
}

int main(int argc, char **argv)
{
    struct rvd_info info;
    u32 i;

    dev = CreateFile(RVD_DEVICE_NAME, 0, 0, NULL, 0, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        printf("can't open the mini-VDD (%s), error %lu\n", RVD_DEVICE_NAME, GetLastError());
        return 1;
    }
    if (!call(RVD_GET_INFO, NULL, 0, &info, sizeof info, "RVD_GET_INFO"))
        return 1;
    printf("mini-VDD interface %lu, host 3D %s, host display %lux%lu\n",
           info.version, info.virgl ? "yes" : "no", info.display_w, info.display_h);
    printf("largest submission %lu KB, largest buffer %lu MB\n",
           info.max_submit / 1024, info.max_buffer >> 20);
    for (i = 0; i < info.num_caps; i++) {
        static unsigned char blob[4096];
        u32 in[2];
        DWORD n = 0;
        in[0] = info.caps_ids[i];
        in[1] = info.caps_versions[i];
        DeviceIoControl(dev, RVD_GET_CAPS, in, sizeof in, blob, sizeof blob, &n, NULL);
        printf("capability set %lu, version %lu, %lu bytes: read %lu, max texture size %lu\n",
               info.caps_ids[i], info.caps_versions[i], info.caps_sizes[i], n,
               n >= 4 ? *(u32 *)blob : 0);
    }

    if (argc > 1 && (!strcmp(argv[1], "clear") || !strcmp(argv[1], "leak"))) {
        if (!info.virgl) {
            printf("the host has no 3D (virgl): start QEMU with virtio-vga-gl\n");
            return 1;
        }
        if (!clear_test(!strcmp(argv[1], "leak"))) {
            printf("clear test FAILED\n");
            return 1;
        }
        printf("clear test passed: the host GPU drew for Windows\n");
    }
    CloseHandle(dev);
    return 0;
}
