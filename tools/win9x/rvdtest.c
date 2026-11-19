/*
 * rvdtest: talk to the moose-ripple mini-VDD (ripplevd.vxd) from a Win32 program.
 *
 *   rvdtest            prints what the VxD and the host report
 *   rvdtest clear      with a 3D-capable host (virgl): makes a context and a 64x64 render
 *                      target, has the host GPU clear it to orange, reads it back through the
 *                      buffer's memory and paints it in the top left corner of the screen
 *   rvdtest leak       the same, but leaves the context and buffer for the VxD to clean up
 *                      when the program ends
 *   rvdtest stall      has a request time out (RVD_SET_TIMEOUT, given up as soon as it is
 *                      sent: a 16 MB read-back), lets go of its buffer and context while the
 *                      device is held stalled, checks that requests are refused and both are
 *                      kept for the host, then lets the VxD find the device back, checks they
 *                      are gone, and runs the clear test
 *   rvdtest async      after GPU scanout was used in this boot, hold a CPU display upload
 *                      across a mode switch, then time one out and check recovery. Uses
 *                      1024x768 and 640x480 at 16 bits; restores the original desktop mode.
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

/* Capability sets 1 and 2 (virtio_gpu.h) hold virgl's union virgl_caps: the first word is
   max_version. Set 2 is struct virgl_caps_v2, whose max_texture_2d_size lies 484 bytes in: the
   308-byte struct virgl_caps_v1, then 176 bytes. */
#define VIRTIO_GPU_CAPSET_VIRGL           1
#define VIRTIO_GPU_CAPSET_VIRGL2          2
#define VIRGL_CAPS_V2_MAX_TEXTURE_2D_SIZE 484UL

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

#ifndef ERROR_TIMEOUT
#define ERROR_TIMEOUT 1460
#endif
#define BIG 2048                                /* a 16 MB texture: its read-back takes a while */

/* Sets the VxD's request time-out; returns how many contexts and buffers it held for the host
   before the call (~0 if the call failed). */
static u32 set_timeout(u32 ms)
{
    u32 held = ~0UL;
    call(RVD_SET_TIMEOUT, &ms, sizeof ms, &held, sizeof held, "set timeout");
    return held;
}

static int stall_test(void)
{
    struct rvd_buffer_create bc;
    struct rvd_buffer buf;
    struct rvd_transfer t;
    u32 ctx = 0, probe, held, refused = 0;
    DWORD n, err = 0, start;

    if (!call(RVD_CONTEXT_CREATE, "rvdtest", 7, &ctx, sizeof ctx, "context create"))
        return 0;
    memset(&bc, 0, sizeof bc);
    bc.context = ctx;
    bc.size = BIG * BIG * 4;
    bc.target = PIPE_TEXTURE_2D;
    bc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    bc.bind = VIRGL_BIND_RENDER_TARGET;
    bc.width = BIG; bc.height = BIG; bc.depth = 1; bc.array_size = 1;
    if (!call(RVD_BUFFER_CREATE, &bc, sizeof bc, &buf, sizeof buf, "buffer create"))
        return 0;
    memset(&t, 0, sizeof t);
    /* A read-back: the host waits for its GPU first. */
    t.context = ctx; t.buffer = buf.handle; t.direction = RVD_FROM_HOST;
    t.width = BIG; t.height = BIG; t.depth = 1;

    /* The VxD gives up on the read-back as soon as it is sent and, while this setting lasts,
       leaves the answer alone: the device stays stalled. */
    set_timeout(RVD_TIMEOUT_AT_ONCE);
    err = DeviceIoControl(dev, RVD_TRANSFER, &t, sizeof t, NULL, 0, &n, NULL) ? 0 : GetLastError();
    if (err != ERROR_TIMEOUT) {
        set_timeout(0);
        printf("the read-back gave error %lu, not a time-out\n", err);
        call(RVD_BUFFER_DESTROY, &buf.handle, sizeof buf.handle, NULL, 0, "buffer destroy");
        call(RVD_CONTEXT_DESTROY, &ctx, sizeof ctx, NULL, 0, "context destroy");
        return 0;
    }
    printf("the read-back timed out\n");

    /* Stalled: letting go works (the buffer and context become zombies, held for the host);
       requests that need the device are refused. */
    if (!call(RVD_BUFFER_DESTROY, &buf.handle, sizeof buf.handle, NULL, 0, "buffer destroy") ||
        !call(RVD_CONTEXT_DESTROY, &ctx, sizeof ctx, NULL, 0, "context destroy")) {
        set_timeout(0);
        return 0;
    }
    err = DeviceIoControl(dev, RVD_CONTEXT_CREATE, "probe", 5, &probe, sizeof probe, &n, NULL) ?
          0 : GetLastError();
    if (!err)
        call(RVD_CONTEXT_DESTROY, &probe, sizeof probe, NULL, 0, "context destroy");
    held = set_timeout(0);                      /* the VxD looks for the answer again */
    printf("while stalled: a new context %s; held for the host: %lu (2 expected)\n",
           err == ERROR_TIMEOUT ? "refused" : "NOT refused", held);
    if (err != ERROR_TIMEOUT || held != 2)
        return 0;

    for (start = GetTickCount(); GetTickCount() - start < 20000; Sleep(0)) {
        if (DeviceIoControl(dev, RVD_CONTEXT_CREATE, "probe", 5, &probe, sizeof probe, &n, NULL)) {
            call(RVD_CONTEXT_DESTROY, &probe, sizeof probe, NULL, 0, "context destroy");
            held = set_timeout(0);
            printf("the device is back after %lu ms (%lu requests refused meanwhile); held "
                   "for the host: %lu (0 expected)\n", GetTickCount() - start, refused, held);
            return held == 0 && clear_test(0);
        }
        if (GetLastError() != ERROR_TIMEOUT) {
            printf("context create failed, error %lu\n", GetLastError());
            return 0;
        }
        refused++;
    }
    printf("the device did not come back in 20 s\n");
    return 0;
}

/* The two boundary markers also go to QEMU's debug console, even with stdout redirected. */
static void async_message(const char *s)
{
    const char *p = s;
    while (*p) {
        char byte_value = *p++;
        __asm { mov al, byte_value }
        __asm { out 0E9h, al }
    }
    __asm { mov al, 10 }
    __asm { out 0E9h, al }
    puts(s);
}

static int upload_control(u32 action, u32 *state)
{
    struct rvd_upload_test test;
    DWORD n = 0;
    test.ms = 0;
    test.action = action;
    *state = 0;
    return DeviceIoControl(dev, RVD_SET_TIMEOUT, &test, sizeof test, state, sizeof *state,
                           &n, NULL) && n == sizeof *state;
}

static int display_mode(DEVMODE *dm)
{
    HDC dc;
    int matches;
    if (ChangeDisplaySettings(dm, 0) != DISP_CHANGE_SUCCESSFUL)
        return 0;
    dc = GetDC(NULL);
    if (!dc)
        return 0;
    matches = (DWORD)GetDeviceCaps(dc, HORZRES) == dm->dmPelsWidth &&
              (DWORD)GetDeviceCaps(dc, VERTRES) == dm->dmPelsHeight &&
              (DWORD)(GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES)) == dm->dmBitsPerPel;
    ReleaseDC(NULL, dc);
    return matches;
}

/* Paint a small part of the visible VGA page and notify the VxD just as the HAL does.
   A non-flip request immediately shows the GDI page after a change of mode. */
static int paint_page(DWORD colour, int flip)
{
    struct rvd_desktop page;
    HDC dc = GetDC(NULL);
    int ok;
    if (!dc)
        return 0;
    ok = PatBlt(dc, 8, 8, 64, 64, colour);
    ReleaseDC(NULL, dc);
    page.offset = 0;
    page.flags = flip ? RVD_DESKTOP_FLIP : 0;
    return ok && call(RVD_DESKTOP, &page, sizeof page, NULL, 0, "select CPU page");
}

static int hold_upload(void)
{
    DWORD start;
    u32 state;
    int i = 0;
    if (!paint_page(WHITENESS, 1) || !upload_control(RVD_UPLOAD_ARM, &state) ||
        !(state & RVD_UPLOAD_ARMED))
        return 0;
    for (start = GetTickCount(); GetTickCount() - start < 3000; Sleep(20)) {
        if (!paint_page(i++ & 1 ? WHITENESS : BLACKNESS, 1) ||
            !upload_control(RVD_UPLOAD_QUERY, &state))
            return 0;
        if (state & RVD_UPLOAD_HELD)
            return 1;
    }
    return 0;
}

static int release_upload(void)
{
    DWORD start;
    u32 state;
    if (!upload_control(RVD_UPLOAD_RELEASE, &state))
        return 0;
    for (start = GetTickCount(); GetTickCount() - start < 3000; Sleep(10)) {
        if (!upload_control(RVD_UPLOAD_QUERY, &state))
            return 0;
        if (!(state & (RVD_UPLOAD_ARMED | RVD_UPLOAD_HELD)))
            return 1;
    }
    return 0;
}

/* A fresh capture after the timeout can only be armed on a newly queued transfer.
   Wait for its own device reply before releasing the test hold. */
static int confirm_new_upload(void)
{
    u32 state;
    DWORD start;
    if (!hold_upload())
        return 0;
    for (start = GetTickCount(); GetTickCount() - start < 3000; Sleep(10)) {
        if (!upload_control(RVD_UPLOAD_QUERY, &state) || !(state & RVD_UPLOAD_HELD))
            return 0;
        if (state & RVD_UPLOAD_REPLIED)
            return release_upload();
    }
    return 0;
}

static int async_test(void)
{
    DEVMODE saved, mode;
    struct rvd_stats before, after;
    u32 state, takeover, ctx;
    int ok = 0, released;
    const char *stage;

    if (!upload_control(RVD_UPLOAD_QUERY, &state) || !(state & RVD_UPLOAD_SUPPORTED)) {
        async_message("async test unavailable: driver has no asynchronous upload test");
        return 2;
    }
    if (!call(RVD_DESKTOP, NULL, 0, &takeover, sizeof takeover, "scanout takeover") || !takeover) {
        async_message("async test requires one GPU-presented frame earlier in this boot");
        return 2;
    }
    memset(&saved, 0, sizeof saved);
    saved.dmSize = sizeof saved;
    if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &saved) ||
        !call(RVD_GET_STATS, NULL, 0, &before, sizeof before, "initial resource counts"))
        return 1;
    memset(&mode, 0, sizeof mode);
    mode.dmSize = sizeof mode;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    mode.dmPelsWidth = 1024; mode.dmPelsHeight = 768; mode.dmBitsPerPel = 16;
    stage = "async test FAILED: cannot hold the initial 1024x768 CPU upload";
    if (!display_mode(&mode) || !hold_upload())
        goto done;
    async_message("async test: upload held before 1024x768 to 640x480 switch");
    mode.dmPelsWidth = 640; mode.dmPelsHeight = 480;
    stage = "async test FAILED: 640x480 display could not use a different backing";
    if (!display_mode(&mode) || !paint_page(BLACKNESS, 0) ||
        !upload_control(RVD_UPLOAD_QUERY, &state) || !(state & RVD_UPLOAD_HELD) ||
        !release_upload())
        goto done;
    stage = "async test FAILED: cannot force a held 640x480 upload to time out";
    if (!hold_upload() || !upload_control(RVD_UPLOAD_TIMEOUT, &state) ||
        (state & (RVD_UPLOAD_HELD | RVD_UPLOAD_TIMED_OUT)) != (RVD_UPLOAD_HELD | RVD_UPLOAD_TIMED_OUT))
        goto done;
    async_message("async test: held 640x480 CPU upload timed out");
    stage = "async test FAILED: unrelated GPU commands stopped with the CPU upload";
    if (!call(RVD_CONTEXT_CREATE, "probe", 5, &ctx, sizeof ctx, "context during upload timeout") ||
        !call(RVD_CONTEXT_DESTROY, &ctx, sizeof ctx, NULL, 0, "release probe context"))
        goto done;
    mode.dmPelsWidth = 1024; mode.dmPelsHeight = 768;
    stage = "async test FAILED: mode return with a timed-out upload";
    if (!display_mode(&mode) || !paint_page(WHITENESS, 0) ||
        !upload_control(RVD_UPLOAD_QUERY, &state) || !(state & RVD_UPLOAD_HELD) ||
        !release_upload())
        goto done;
    stage = "async test FAILED: no new CPU upload replied after the timed-out one";
    if (!confirm_new_upload())
        goto done;
    async_message("async test: a fresh CPU upload received its own reply after timeout");
    stage = "async test FAILED: CPU display or 3D clear after upload release";
    if (!paint_page(BLACKNESS, 1) || !clear_test(0) ||
        !call(RVD_GET_STATS, NULL, 0, &after, sizeof after, "final resource counts") ||
        before.zombies != after.zombies || before.contexts != after.contexts ||
        before.buffers != after.buffers || before.buffer_pages != after.buffer_pages)
        goto done;
    ok = 1;
done:
    if (!ok)
        async_message(stage);
    released = release_upload();
    if (!display_mode(&saved) || !released) {
        async_message("async test FAILED: restore upload handling and desktop mode");
        ok = 0;
    }
    if (ok)
        async_message("async test passed: mode change, timeout, recovery and desktop return");
    return ok ? 0 : 1;
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
        printf("capability set %lu, version %lu, %lu bytes: read %lu",
               info.caps_ids[i], info.caps_versions[i], info.caps_sizes[i], n);
        if ((info.caps_ids[i] == VIRTIO_GPU_CAPSET_VIRGL ||
             info.caps_ids[i] == VIRTIO_GPU_CAPSET_VIRGL2) && n >= 4)
            printf(", max version %lu", *(u32 *)blob);
        if (info.caps_ids[i] == VIRTIO_GPU_CAPSET_VIRGL2 &&
            n >= VIRGL_CAPS_V2_MAX_TEXTURE_2D_SIZE + 4)
            printf(", largest 2D texture %lu",
                   *(u32 *)(blob + VIRGL_CAPS_V2_MAX_TEXTURE_2D_SIZE));
        printf("\n");
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
    if (argc > 1 && !strcmp(argv[1], "stall")) {
        if (!info.virgl) {
            printf("the host has no 3D (virgl): start QEMU with virtio-vga-gl\n");
            return 1;
        }
        if (!stall_test()) {
            printf("stall test FAILED\n");
            return 1;
        }
        printf("stall test passed: 3D works again after a request timed out\n");
    }
    if (argc > 1 && !strcmp(argv[1], "async")) {
        int result = async_test();
        CloseHandle(dev);
        return result;
    }
    CloseHandle(dev);
    return 0;
}
