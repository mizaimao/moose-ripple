/*
 * rvdtest: talk to the moose-ripple mini-VDD (ripplevd.vxd) from a Win32 program.
 *
 *   rvdtest            prints what the VxD and the host report
 *   rvdtest clear      with a 3D-capable host (virgl): has the host GPU clear a 64x64 texture,
 *                      reads it back and paints it in the top left corner of the screen
 *
 * Built with Visual C++ 6 for Windows 95/98 (tools/win9x/build-tools.sh). The virtio-gpu and
 * virgl numbers below are from the virtio specification (5.7) and virglrenderer's protocol
 * (virgl_protocol.h, virgl_hw.h; MIT).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long u32;

/* ---- The VxD's interface (win9x/minivdd/vgpu.c) ---------------------------------------------- */
#define RVD_INFO        1
#define RVD_COMMAND     2
#define RVD_AREA_READ   3
#define RVD_AREA_WRITE  4
struct rvd_info {
    u32 version, virgl, capsets, display_w, display_h, area_phys, area_bytes, max_request;
};

/* ---- virtio-gpu -------------------------------------------------------------------------------- */
struct hdr { u32 type, flags, fence_lo, fence_hi, ctx_id, padding; };
#define CMD_GET_DISPLAY_INFO        0x0100
#define CMD_RESOURCE_ATTACH_BACKING 0x0106
#define CMD_GET_CAPSET_INFO         0x0108
#define CMD_CTX_CREATE              0x0200
#define CMD_CTX_DESTROY             0x0201
#define CMD_CTX_ATTACH_RESOURCE     0x0202
#define CMD_RESOURCE_CREATE_3D      0x0204
#define CMD_TRANSFER_FROM_HOST_3D   0x0206
#define CMD_SUBMIT_3D               0x0207
#define CMD_RESOURCE_UNREF          0x0102
#define RESP_OK_NODATA              0x1100

/* ---- virgl ------------------------------------------------------------------------------------- */
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

static int command(void *req, u32 req_len, void *resp, u32 resp_len, const char *what)
{
    DWORD n = 0;
    if (!DeviceIoControl(dev, RVD_COMMAND, req, req_len, resp, resp_len, &n, NULL)) {
        printf("%s: DeviceIoControl failed, error %lu\n", what, GetLastError());
        return 0;
    }
    if (resp_len == sizeof(struct hdr) && ((struct hdr *)resp)->type != RESP_OK_NODATA) {
        printf("%s: the host answered 0x%lx\n", what, ((struct hdr *)resp)->type);
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

static int clear_test(const struct rvd_info *info)
{
    struct hdr resp;
    const u32 ctx = 7, res = 77, surface = 1;
    static u32 pixels[W * H];
    u32 off = 0;
    DWORD n;
    int ok;

    { struct { struct hdr h; u32 nlen, pad; char name[64]; } c;
      memset(&c, 0, sizeof c); c.h.type = CMD_CTX_CREATE; c.h.ctx_id = ctx;
      strcpy(c.name, "rvdtest"); c.nlen = 7;
      if (!command(&c, sizeof c, &resp, sizeof resp, "context create")) return 0; }

    { struct { struct hdr h; u32 id, target, format, bind, width, height, depth, array_size,
                              last_level, nr_samples, flags, pad; } c;
      memset(&c, 0, sizeof c); c.h.type = CMD_RESOURCE_CREATE_3D; c.h.ctx_id = ctx;
      c.id = res; c.target = PIPE_TEXTURE_2D; c.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
      c.bind = VIRGL_BIND_RENDER_TARGET; c.width = W; c.height = H; c.depth = 1; c.array_size = 1;
      if (!command(&c, sizeof c, &resp, sizeof resp, "resource create")) return 0; }

    { struct { struct hdr h; u32 id, entries; u32 addr_lo, addr_hi, length, pad; } c;
      memset(&c, 0, sizeof c); c.h.type = CMD_RESOURCE_ATTACH_BACKING;
      c.id = res; c.entries = 1; c.addr_lo = info->area_phys; c.length = W * H * 4;
      if (!command(&c, sizeof c, &resp, sizeof resp, "attach backing")) return 0; }

    { struct { struct hdr h; u32 id, pad; } c;
      memset(&c, 0, sizeof c); c.h.type = CMD_CTX_ATTACH_RESOURCE; c.h.ctx_id = ctx; c.id = res;
      if (!command(&c, sizeof c, &resp, sizeof resp, "attach to context")) return 0; }

    { struct { struct hdr h; u32 size, pad; u32 cmd[32]; } c;
      u32 i = 0;
      memset(&c, 0, sizeof c); c.h.type = CMD_SUBMIT_3D; c.h.ctx_id = ctx;
      c.cmd[i++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
      c.cmd[i++] = surface; c.cmd[i++] = res; c.cmd[i++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
      c.cmd[i++] = 0; c.cmd[i++] = 0;                       /* level; first and last layer */
      c.cmd[i++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
      c.cmd[i++] = 1; c.cmd[i++] = 0; c.cmd[i++] = surface; /* 1 colour buffer, no depth */
      c.cmd[i++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
      c.cmd[i++] = PIPE_CLEAR_COLOR0;
      c.cmd[i++] = fbits(1.0f); c.cmd[i++] = fbits(0.5f); c.cmd[i++] = fbits(0.0f);
      c.cmd[i++] = fbits(1.0f);                             /* red, green, blue, alpha */
      c.cmd[i++] = 0; c.cmd[i++] = 0; c.cmd[i++] = 0;       /* depth (a double), stencil */
      c.size = i * 4;
      if (!command(&c, sizeof(struct hdr) + 8 + i * 4, &resp, sizeof resp, "submit")) return 0; }

    { struct { struct hdr h; u32 x, y, z, w, h_, d; u32 off_lo, off_hi; u32 id, level, stride,
                              layer_stride; } c;
      memset(&c, 0, sizeof c); c.h.type = CMD_TRANSFER_FROM_HOST_3D; c.h.ctx_id = ctx;
      c.w = W; c.h_ = H; c.d = 1; c.id = res; c.stride = W * 4;
      if (!command(&c, sizeof c, &resp, sizeof resp, "transfer from host")) return 0; }

    ok = DeviceIoControl(dev, RVD_AREA_READ, &off, sizeof off, pixels, sizeof pixels, &n, NULL);
    printf("read back %lu bytes, first pixel 0x%08lx (orange is 0xffff8000)\n",
           ok ? n : 0, pixels[0]);

    { BITMAPINFO bi; HDC dc = GetDC(NULL);
      memset(&bi, 0, sizeof bi);
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
      bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
      SetDIBitsToDevice(dc, 8, 8, W, H, 0, 0, 0, H, pixels, &bi, DIB_RGB_COLORS);
      ReleaseDC(NULL, dc); }

    { struct { struct hdr h; u32 id, pad; } c;
      memset(&c, 0, sizeof c); c.h.type = CMD_RESOURCE_UNREF; c.id = res;
      command(&c, sizeof c, &resp, sizeof resp, "resource unref"); }
    { struct hdr c;
      memset(&c, 0, sizeof c); c.type = CMD_CTX_DESTROY; c.ctx_id = ctx;
      command(&c, sizeof c, &resp, sizeof resp, "context destroy"); }
    return pixels[0] == 0xFFFF8000UL;
}

int main(int argc, char **argv)
{
    struct rvd_info info;
    DWORD n;

    dev = CreateFile("\\\\.\\RIPPLEVD", 0, 0, NULL, 0, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        printf("can't open the mini-VDD (\\\\.\\RIPPLEVD), error %lu\n", GetLastError());
        return 1;
    }
    if (!DeviceIoControl(dev, RVD_INFO, NULL, 0, &info, sizeof info, &n, NULL)) {
        printf("RVD_INFO failed, error %lu\n", GetLastError());
        return 1;
    }
    printf("mini-VDD interface %lu, host 3D %s, %lu capability sets, host display %lux%lu\n",
           info.version, info.virgl ? "yes" : "no", info.capsets, info.display_w, info.display_h);
    printf("transfer area: %lu KB at physical 0x%08lx; largest request %lu KB\n",
           info.area_bytes / 1024, info.area_phys, info.max_request / 1024);

    { struct hdr req; struct { struct hdr h; u32 d[16 * 6]; } resp;
      memset(&req, 0, sizeof req); req.type = CMD_GET_DISPLAY_INFO;
      if (command(&req, sizeof req, &resp, sizeof resp, "display info"))
          printf("through RVD_COMMAND: answer 0x%lx, display %lux%lu\n",
                 resp.h.type, resp.d[2], resp.d[3]); }

    if (argc > 1 && !strcmp(argv[1], "clear")) {
        if (!info.virgl) {
            printf("the host has no 3D (virgl): start QEMU with virtio-vga-gl\n");
            return 1;
        }
        if (!clear_test(&info)) {
            printf("clear test FAILED\n");
            return 1;
        }
        printf("clear test passed: the host GPU drew for Windows\n");
    }
    CloseHandle(dev);
    return 0;
}
