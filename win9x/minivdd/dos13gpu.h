/* Included in vgpu.c. All commands run at its serialized display event.
   The host keeps these four resources; inline writes need no DMA backing. */
#define PAL_CTX MAX_CONTEXTS
enum {
    PAL_INDEX = MAX_BUFFERS + DESKTOP_SLOTS + 1, PAL_LUT, PAL_TARGET, PAL_VERTICES,
    PAL_BLEND = 1, PAL_DSA, PAL_RASTER, PAL_VS, PAL_FS, PAL_ELEMENTS, PAL_SAMPLER,
    PAL_SURFACE, PAL_INDEX_VIEW, PAL_LUT_VIEW
};
/* From virgl_protocol.h and virgl_hw.h. */
enum {
    VC_CREATE = 1, VC_BIND = 2, VC_DESTROY = 3, VC_VIEWPORT = 4, VC_FRAMEBUFFER = 5,
    VC_VERTICES = 6, VC_DRAW = 8, VC_WRITE = 9, VC_VIEWS = 10, VC_SAMPLERS = 18,
    VC_SHADER = 31, VO_BLEND = 1, VO_RASTER = 2, VO_DSA = 3, VO_SHADER = 4,
    VO_ELEMENTS = 5, VO_VIEW = 6, VO_SAMPLER = 7, VO_SURFACE = 8,
    VF_BGRX = 2, VF_RG32F = 29, VF_R8 = 64, VF_RGBA = 67,
    VB_RENDER = 2, VB_SAMPLE = 8, VB_VERTEX = 16, VB_SCANOUT = 1UL << 18
};
#define VC(c,o,n) ((c) | ((o) << 8) | ((u32)(n) << 16))

static struct {
    u8 *indices;                       /* the last complete input, unrelated to host DMA */
    u32 pages, colours[256], serial, generation;
    unsigned resources;                /* may exist, including a CREATE whose reply was late */
    int context, available, ready, valid, disabled;
} pal;
#ifdef RVD_TRACE_DOS13_DAMAGE
static const char *pal_stage;
#define PAL_STAGE(s) (pal_stage = (s))
#else
#define PAL_STAGE(s) ((void)0)
#endif

/* No allocation is reused while visible or after an ambiguous SET_SCANOUT.
   A rejected teardown keeps its IDs reserved and is retried following a handoff. */
static void pal_release(void)
{
    struct gpu_ctrl_hdr *reply = (struct gpu_ctrl_hdr *)resp_buf;
    if (!pal.indices || !visible || visible == PAL_TARGET || stalled)
        return;
    pal.ready = pal.valid = 0;
    if (pal.context) {
        request(GPU_CMD_CTX_DESTROY, PAL_CTX, sizeof(*reply));
        if (!simple(sizeof(*reply)) && (stalled || reply->type != 0x1204)) /* invalid context */
            return;
        pal.context = 0;
    }
    while (pal.resources) {
        if (!one_id(GPU_CMD_RESOURCE_UNREF, 0, PAL_INDEX + pal.resources - 1) &&
            (stalled || reply->type != GPU_RESP_ERR_INVALID_RESOURCE))
            return;
        pal.resources--;
    }
    vmm_free_pages(pal.pages);
    pal.indices = 0;
}

static int pal_caps(void)
{
    struct gpu_get_capset *q;
    struct gpu_ctrl_hdr *reply = (struct gpu_ctrl_hdr *)resp_buf;
    u32 i, bytes, *v = (u32 *)(reply + 1);
    if (pal.available)
        return 1;
    for (i = 0; i < info.num_caps; i++) {
        if (info.caps_ids[i] != 1 && info.caps_ids[i] != 2)
            continue;
        PAL_STAGE("caps");
        q = (struct gpu_get_capset *)request(GPU_CMD_GET_CAPSET, 0, sizeof(*q));
        q->id = info.caps_ids[i]; q->version = info.caps_versions[i];
        bytes = command(sizeof(*q), RESP_BYTES);
        if (stalled)
            return 0;
        /* Common v1 prefix: max_version, sampler[16], render[16]. The vertexbuffer
           mask reports exceptional R11G11B10_FLOAT support, not ordinary RG32F vertices. */
#ifdef RVD_TRACE_DOS13_DAMAGE
        if (bytes >= sizeof(*reply) + 65 * 4 && reply->type == GPU_RESP_OK_CAPSET) {
            dbg("rvd: indexed caps id sampler[2] render[0] vertex[0]: ");
            dbg_hex(info.caps_ids[i]); dbg(" "); dbg_hex(v[3]); dbg(" ");
            dbg_hex(v[17]); dbg(" "); dbg_hex(v[49]); dbg("\r\n");
        }
#endif
        if (bytes >= sizeof(*reply) + 33 * 4 && reply->type == GPU_RESP_OK_CAPSET &&
            (v[1 + VF_R8 / 32] & (1UL << (VF_R8 % 32))) &&
            (v[1 + VF_RGBA / 32] & (1UL << (VF_RGBA % 32))) &&
            (v[17 + VF_BGRX / 32] & (1UL << (VF_BGRX % 32)))) {
            pal.available = 1;
            return 1;
        }
    }
    return 0;
}

static int pal_resource(u32 id, u32 target, u32 format, u32 bind, u32 w, u32 h)
{
    struct gpu_resource_create_3d *r = (struct gpu_resource_create_3d *)
        request(GPU_CMD_RESOURCE_CREATE_3D, PAL_CTX, sizeof(*r));
    pal.resources++;
    r->id = id; r->target = target; r->format = format; r->bind = bind;
    r->width = w; r->height = h; r->depth = r->array_size = 1;
    PAL_STAGE("resource creation");
    if (!simple(sizeof(*r)))
        return 0;
    PAL_STAGE("context resource attachment");
    return one_id(GPU_CMD_CTX_ATTACH_RESOURCE, PAL_CTX, id);
}

static u32 *pal_begin(void)
{
    struct gpu_submit *s = (struct gpu_submit *)request(GPU_CMD_SUBMIT_3D, PAL_CTX, sizeof(*s));
    return (u32 *)(s + 1);
}

static int pal_submit(u32 *end)
{
    struct gpu_submit *s = (struct gpu_submit *)req_buf;
    u32 fence, bytes;
    s->size = (u32)((u8 *)end - (u8 *)(s + 1));
    fence_request(&s->hdr);
    fence = s->hdr.fence_lo;
    bytes = command(sizeof(*s) + s->size, sizeof(s->hdr));
    if (fence_ok(bytes, fence))
        return 1;
#ifdef RVD_TRACE_DOS13_DAMAGE
    if (!stalled) {
        struct gpu_ctrl_hdr *r = (struct gpu_ctrl_hdr *)resp_buf;
        dbg("rvd: indexed fence bytes flags expected actual-lo actual-hi: ");
        dbg_hex(bytes); dbg(" "); dbg_hex(r->flags); dbg(" "); dbg_hex(fence);
        dbg(" "); dbg_hex(r->fence_lo); dbg(" "); dbg_hex(r->fence_hi); dbg("\r\n");
    }
#endif
    return 0;
}

static u32 *pal_words(u32 *p, const u32 *src, u32 bytes)
{
    copy(p, src, bytes);
    return p + bytes / 4;
}

static u32 *pal_shader(u32 *p, u32 handle, u32 stage, const char *code, u32 bytes)
{
    u32 n = (bytes + 3) / 4;
    *p++ = VC(VC_CREATE, VO_SHADER, 5 + n);
    *p++ = handle; *p++ = stage; *p++ = bytes; *p++ = 300; *p++ = 0;
    p[n - 1] = 0;
    copy(p, code, bytes);
    return p + n;
}

/* The command contains tightly packed rows even when the guest image has a wider stride.
   The largest possible command is 64,000 index bytes plus a 1,024-byte palette and draw. */
static u32 *pal_write(u32 *p, u32 id, const struct gpu_rect *r, u32 bpp, const u8 *src, u32 pitch)
{
    u32 y, row = r->w * bpp, bytes = row * r->h, n = (bytes + 3) / 4;
    u8 *out;
    *p++ = VC(VC_WRITE, 0, 11 + n);
    *p++ = id; *p++ = 0; *p++ = 2; /* level zero, PIPE_MAP_WRITE */
    *p++ = row; *p++ = bytes;
    *p++ = r->x; *p++ = r->y; *p++ = 0;
    *p++ = r->w; *p++ = r->h; *p++ = 1;
    p[n - 1] = 0;
    if (row == pitch)
        copy(p, src, bytes);
    else
        for (y = 0, out = (u8 *)p; y < r->h; y++, out += row, src += pitch)
            copy(out, src, row);
    return p + n;
}

static int pal_setup(void)
{
    /* TGSI immediates are IEEE-754 bit patterns. The vertex stage maps [-1,1]
       to [0,1]. The fragment stage takes a normalized index to the center
       of its LUT texel: index * (255/256) + 1/512, at vertical center 0.5. */
    static const char vs[] =
        "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
        "IMM[0] FLT32 {0x3f000000, 0x3f000000, 0x00000000, 0x3f800000}\n"
        "0: MOV OUT[0], IN[0]\n1: MAD OUT[1], IN[0], IMM[0].xxxx, IMM[0].xxxx\n2: END\n";
    static const char fs[] =
        "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n"
        "DCL SAMP[0]\nDCL SAMP[1]\nDCL TEMP[0..1]\n"
        "IMM[0] FLT32 {0x3f7f0000, 0x3b000000, 0x3f000000, 0x3f800000}\n"
        "0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
        "1: MAD TEMP[1].x, TEMP[0].xxxx, IMM[0].xxxx, IMM[0].yyyy\n"
        "2: MOV TEMP[1].yzw, IMM[0].zzzz\n"
        "3: TEX OUT[0], TEMP[1], SAMP[1], 2D\n4: END\n";
    static const u32 pipeline[] = {
        VC(VC_CREATE,VO_BLEND,11),PAL_BLEND,0,0,0xfu<<27,0,0,0,0,0,0,0,
        VC(VC_CREATE,VO_DSA,5),PAL_DSA,0,0,0,0,
        VC(VC_CREATE,VO_RASTER,9),PAL_RASTER,(1UL<<1)|(1UL<<29)|(1UL<<30),
            0x3f800000,0,0,0x3f800000,0,0,0,
        VC(VC_CREATE,VO_ELEMENTS,5),PAL_ELEMENTS,0,0,0,VF_RG32F,
        /* Nearest min/mag, no mipmapping, clamp to edge on all three axes. */
        VC(VC_CREATE,VO_SAMPLER,9),PAL_SAMPLER,2|(2<<3)|(2<<6)|(2<<11),0,0,0,0,0,0,0,
        VC(VC_BIND,VO_BLEND,1),PAL_BLEND, VC(VC_BIND,VO_DSA,1),PAL_DSA,
        VC(VC_BIND,VO_RASTER,1),PAL_RASTER, VC(VC_BIND,VO_ELEMENTS,1),PAL_ELEMENTS,
        VC(VC_SHADER,0,2),PAL_VS,0, VC(VC_SHADER,0,2),PAL_FS,1,
        VC(VC_SAMPLERS,0,4),1,0,PAL_SAMPLER,PAL_SAMPLER,
        /* Viewport scale and translate are (320, 200, 0.5), covering 640x400. */
        VC(VC_VIEWPORT,0,7),0,0x43a00000,0x43480000,0x3f000000,0x43a00000,0x43480000,0x3f000000,
        VC(VC_CREATE,VO_SURFACE,5),PAL_SURFACE,PAL_TARGET,VF_BGRX,0,0,
        VC(VC_FRAMEBUFFER,0,3),1,0,PAL_SURFACE, VC(VC_VERTICES,0,3),8,0,PAL_VERTICES
    };
    static const u32 corners[] = {
        0xbf800000,0xbf800000,0x3f800000,0xbf800000,0xbf800000,0x3f800000,0x3f800000,0x3f800000
    };
    static const struct gpu_rect vertex_box = { 0, 0, 32, 1 };
    struct gpu_ctx_create *ctx;
    u32 *p;
    if (!pal_caps())
        return 0;
    PAL_STAGE("guest index shadow allocation");
    pal.indices = (u8 *)vmm_alloc_pages(16, &pal.pages);
    if (!pal.indices)
        return 0;
    pal.context = 1;
    PAL_STAGE("context creation");
    ctx = (struct gpu_ctx_create *)request(GPU_CMD_CTX_CREATE, PAL_CTX, sizeof(*ctx));
    copy(ctx->name, "ripple-dos13", 12); ctx->name_len = 12;
    if (!simple(sizeof(*ctx)) ||
        !pal_resource(PAL_INDEX, 2, VF_R8, VB_SAMPLE, 320, 200) ||
        !pal_resource(PAL_LUT, 2, VF_RGBA, VB_SAMPLE, 256, 1) ||
        !pal_resource(PAL_TARGET, 2, VF_BGRX, VB_RENDER | VB_SCANOUT, 640, 400) ||
        !pal_resource(PAL_VERTICES, 0, VF_R8, VB_VERTEX, 32, 1))
        return 0;
    p = pal_begin();
    p = pal_shader(p, PAL_VS, 0, vs, sizeof(vs));
    p = pal_shader(p, PAL_FS, 1, fs, sizeof(fs));
    p = pal_words(p, pipeline, sizeof(pipeline));
    p = pal_write(p, PAL_VERTICES, &vertex_box, 1, (const u8 *)corners, sizeof(corners));
    PAL_STAGE("shader and vertex setup");
    pal.ready = pal_submit(p);
    return pal.ready;
}

/* Positive: coherent frame submitted or unchanged. Zero: use the regular 2D
   mirror. Negative: the source is in transition; the next display event retries. */
static int pal_present(u32 serial)
{
    /* Recreate and bind both views for each draw. QEMU/virglrenderer combinations
       tested by the Windows gamma driver can otherwise leave the LUT on the wrong unit. */
    static const u32 draw[] = {
        VC(VC_CREATE,VO_VIEW,6),PAL_INDEX_VIEW,PAL_INDEX,VF_R8,0,0,0x688,
        VC(VC_CREATE,VO_VIEW,6),PAL_LUT_VIEW,PAL_LUT,VF_RGBA,0,0,0x688,
        VC(VC_VIEWS,0,4),1,0,PAL_INDEX_VIEW,PAL_LUT_VIEW,
        VC(VC_DRAW,0,12),0,4,5,0,1,0,0,0,0,0,3,0,
        VC(VC_VIEWS,0,4),1,0,0,0, VC(VC_DESTROY,VO_VIEW,1),PAL_INDEX_VIEW,
        VC(VC_DESTROY,VO_VIEW,1),PAL_LUT_VIEW
    };
    /* Each inline write adds a command dword and eleven parameter dwords. */
    typedef char pal_frame_fits_request[
        sizeof(struct gpu_submit) + 2 * 12 * 4 + 320 * 200 + 256 * 4 + sizeof(draw)
        <= REQ_BYTES ? 1 : -1];
    struct gpu_rect r, table, damage;
    const u8 *src = vram;
    u8 *dst;
    u32 x, y, left = 320, right = 0, top = 200, bottom = 0;
    u32 first = 256, last = 0, generation, *p;
    int full;
    if (pal.disabled || !vgpu_has_virgl() || info_incomplete)
        return 0;
    if (!dos_active || dos_active != vgpu_dac_vm)
        return -1;
    if (!pal.ready && pal.indices) {
        /* Previous teardown may still own some IDs after a late reply. */
        pal_release();
        if (pal.indices || stalled)
            return 0;
    }
    if (!pal.ready && !pal_setup())
        goto failed;
    generation = dac_generation;
    full = !pal.valid || pal.serial != serial || visible != PAL_TARGET;
    pal.valid = 0; /* an interrupted scan may already have changed our cached input */
    if (full || pal.generation != generation) {
        for (x = 0; x < 256; x++) {
            u32 rgb = dos_palette[x], rgba = rgb ? 0xff000000UL | ((rgb & 255) << 16) |
                (rgb & 0xff00) | ((rgb >> 16) & 255) : 0;
            if (full || pal.colours[x] != rgba) {
                if (!last) first = x;
                last = x + 1;
                pal.colours[x] = rgba;
            }
        }
    }
    dst = pal.indices;
    for (y = 0; y < 200; y++) {
        u32 row_left = 320, row_right = 0;
        for (x = 0; x < 320; x++, dst++) {
            u8 index = *src++;
            if (!pal.colours[index])
                return -1;
            if (full || *dst != index) {
                *dst = index;
                if (!row_right) row_left = x;
                row_right = x + 1;
            }
        }
        if (row_right) {
            if (row_left < left) left = row_left;
            if (row_right > right) right = row_right;
            if (y < top) top = y;
            bottom = y + 1;
        }
    }
    if (generation != dac_generation || serial != display_serial)
        return -1;
    if (right || last) {
        p = pal_begin();
        if (right) {
            r.x = left; r.y = top; r.w = right - left; r.h = bottom - top;
            p = pal_write(p, PAL_INDEX, &r, 1, pal.indices + top * 320 + left, 320);
            damage.x = left * 2; damage.y = top * 2; damage.w = r.w * 2; damage.h = r.h * 2;
        }
        if (last) {
            table.x = first; table.y = 0; table.w = last - first; table.h = 1;
            p = pal_write(p, PAL_LUT, &table, 4, (const u8 *)&pal.colours[first], table.w * 4);
        }
        p = pal_words(p, draw, sizeof(draw));
        PAL_STAGE("indexed image draw");
        if (!pal_submit(p))
            goto failed;
        if (serial != display_serial)
            return -1;
        PAL_STAGE("indexed SET_SCANOUT");
        if (!set_scanout(PAL_TARGET, 640, 400))
            goto failed;
        if (serial != display_serial)
            return -1;
        PAL_STAGE("indexed RESOURCE_FLUSH");
        if (!flush_scanout(PAL_TARGET, 640, 400, last ? 0 : &damage))
            goto failed;
#ifdef RVD_TRACE_DOS13_DAMAGE
        dbg("rvd: DOS13 GPU bytes index LUT: ");
        dbg_hex(right ? (right - left) * (bottom - top) : 0);
        dbg(" "); dbg_hex(last ? (last - first) * 4 : 0); dbg("\r\n");
#endif
    }
    if (serial != display_serial)
        return -1;
    pal.serial = serial; pal.generation = generation; pal.valid = 1;
    return 1;
failed:
    pal.disabled = 1; /* leave possible late commands and their IDs alone until handoff */
#ifdef RVD_TRACE_DOS13_DAMAGE
    dbg("rvd: indexed failure stage "); dbg(pal_stage ? pal_stage : "capset unavailable");
    dbg(" resources "); dbg_hex(pal.resources); dbg(" request ");
    dbg_hex(((struct gpu_ctrl_hdr *)req_buf)->type);
    if (!stalled) { dbg(" reply "); dbg_hex(((struct gpu_ctrl_hdr *)resp_buf)->type); }
    dbg("\r\n");
#endif
    dbg("rvd: using 2D DOS mirror after indexed presenter failure\r\n");
    return 0;
}
#undef VC
#undef PAL_STAGE
