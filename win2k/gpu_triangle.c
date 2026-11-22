/* The NT5 fixed-function path takes transformed Direct3D 7 vertices with
 * one texture stage. Positions are IEEE words in the guest. The vertex
 * shader performs the 9x screen-to-clip transform on the host. Colours
 * interpolate in screen space and texture coordinates with perspective.
 * A batch fits a control packet and needs no separate guest vertex resource.
 * Each colour context keeps small tables of the state objects and fragment
 * shaders it has created, keyed by validated Direct3D values. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include <virgl_hw.h>
#include <virgl_protocol.h>

enum {
    Raster = 9, Elements = 5, VertexShader = 6, ObjectShader = 7, DsaBase = 16, BlendBase = 32,
    SamplerBase = 48, ShaderBase = 64, TextureView = 80
};
enum { VertexStage = 0, FragmentStage = 1, PipeTriangles = 4, ShaderText = 2400 };

/* Direct3D 7 values used on the wire (d3dtypes.h). */
enum {
    TopDisable = 1, TopSelect1, TopSelect2, TopModulate, TopModulate2x, TopModulate4x, TopAdd,
    TopAddSigned, TopAddSigned2x, TopSubtract, TopAddSmooth, TopBlendDiffuse, TopBlendTexture,
    TopBlendFactor, TopBlendTexturePm, TopBlendCurrent
};
enum { ArgDiffuse = 0, ArgCurrent, ArgTexture, ArgFactor, ArgSpecular, ArgSelect = 15,
       ArgComplement = 0x10, ArgAlphaReplicate = 0x20 };
enum { CullNone = 1, CullCw, CullCcw };

static const char vertex_shader[] =
    "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..96]\nDCL ADDR[0]\n"
    "DCL TEMP[0..2]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
    "DCL OUT[2], GENERIC[1]\nDCL OUT[3], GENERIC[2]\n"
    "IMM[0] FLT32 {0x3f000000, 0x3f800000, 0x40000000, 0x00000000}\n"
    "0: UADD TEMP[0].x, SV[0].xxxx, SV[0].xxxx\n"
    "1: UADD TEMP[0].x, TEMP[0].xxxx, TEMP[0].xxxx\n"
    "2: UARL ADDR[0].x, TEMP[0].xxxx\n"
    "3: MOV TEMP[1], CONST[ADDR[0].x+1]\n"
    "4: RCP TEMP[2].x, CONST[0].xxxx\n"
    "5: RCP TEMP[2].y, CONST[0].yyyy\n"
    "6: ADD TEMP[1].xy, TEMP[1], IMM[0].xxxx\n"
    "7: MUL TEMP[1].xy, TEMP[1], TEMP[2]\n"
    "8: MAD TEMP[1].xyz, TEMP[1], IMM[0].zzzz, -IMM[0].yyyy\n"
    "9: RCP TEMP[0].w, TEMP[1].wwww\n"
    "10: MUL OUT[0].xyz, TEMP[1], TEMP[0].wwww\n"
    "11: MOV OUT[0].w, TEMP[0].wwww\n"
    "12: MOV OUT[1], CONST[ADDR[0].x+2]\n"
    "13: MOV OUT[2], CONST[ADDR[0].x+3]\n"
    "14: MOV OUT[3], CONST[ADDR[0].x+4]\n15: END\n";

/* Object-space input: world, view and projection (CONST[1..12], each the
 * columns of a Direct3D row-major matrix), then the Direct3D viewport
 * (CONST[13] x, y, width, height; CONST[14] minimum and maximum z) mapped
 * into the full-target clip space the transformed path uses. The +1 is the
 * half-pixel shift of Direct3D's integer pixel centres. Vertices follow
 * from CONST[15], four constants each as in the transformed path. */
static const char object_shader[] =
    "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..110]\nDCL ADDR[0]\n"
    "DCL TEMP[0..4]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
    "DCL OUT[2], GENERIC[1]\nDCL OUT[3], GENERIC[2]\n"
    "IMM[0] FLT32 {0x3f800000, 0x40000000, 0x00000000, 0x00000000}\n"
    "IMM[1] UINT32 {15, 0, 0, 0}\n"
    "0: UADD TEMP[0].x, SV[0].xxxx, SV[0].xxxx\n"
    "1: UADD TEMP[0].x, TEMP[0].xxxx, TEMP[0].xxxx\n"
    "2: UADD TEMP[0].x, TEMP[0].xxxx, IMM[1].xxxx\n"
    "3: UARL ADDR[0].x, TEMP[0].xxxx\n"
    "4: MOV TEMP[1], CONST[ADDR[0].x]\n"
    "5: DP4 TEMP[2].x, TEMP[1], CONST[1]\n"
    "6: DP4 TEMP[2].y, TEMP[1], CONST[2]\n"
    "7: DP4 TEMP[2].z, TEMP[1], CONST[3]\n"
    "8: DP4 TEMP[2].w, TEMP[1], CONST[4]\n"
    "9: DP4 TEMP[1].x, TEMP[2], CONST[5]\n"
    "10: DP4 TEMP[1].y, TEMP[2], CONST[6]\n"
    "11: DP4 TEMP[1].z, TEMP[2], CONST[7]\n"
    "12: DP4 TEMP[1].w, TEMP[2], CONST[8]\n"
    "13: DP4 TEMP[2].x, TEMP[1], CONST[9]\n"
    "14: DP4 TEMP[2].y, TEMP[1], CONST[10]\n"
    "15: DP4 TEMP[2].z, TEMP[1], CONST[11]\n"
    "16: DP4 TEMP[2].w, TEMP[1], CONST[12]\n"
    "17: RCP TEMP[3].x, CONST[0].xxxx\n"
    "18: RCP TEMP[3].y, CONST[0].yyyy\n"
    "19: MUL TEMP[4].xy, CONST[13].zwww, TEMP[3].xyyy\n"
    "20: MAD TEMP[3].zw, CONST[13].xxxy, IMM[0].yyyy, CONST[13].xxzw\n"
    "21: ADD TEMP[3].zw, TEMP[3], IMM[0].xxxx\n"
    "22: MAD TEMP[4].zw, TEMP[3], TEMP[3].xxxy, -IMM[0].xxxx\n"
    "23: MUL TEMP[1].x, TEMP[2].xxxx, TEMP[4].xxxx\n"
    "24: MAD OUT[0].x, TEMP[2].wwww, TEMP[4].zzzz, TEMP[1].xxxx\n"
    "25: MUL TEMP[1].y, TEMP[2].yyyy, -TEMP[4].yyyy\n"
    "26: MAD OUT[0].y, TEMP[2].wwww, TEMP[4].wwww, TEMP[1].yyyy\n"
    "27: ADD TEMP[3].x, CONST[14].yyyy, -CONST[14].xxxx\n"
    "28: MUL TEMP[3].x, TEMP[3].xxxx, IMM[0].yyyy\n"
    "29: MAD TEMP[3].y, CONST[14].xxxx, IMM[0].yyyy, -IMM[0].xxxx\n"
    "30: MUL TEMP[1].z, TEMP[2].zzzz, TEMP[3].xxxx\n"
    "31: MAD OUT[0].z, TEMP[2].wwww, TEMP[3].yyyy, TEMP[1].zzzz\n"
    "32: MOV OUT[0].w, TEMP[2].wwww\n"
    "33: MOV OUT[1], CONST[ADDR[0].x+1]\n"
    "34: MOV OUT[2], CONST[ADDR[0].x+2]\n"
    "35: MOV OUT[3], CONST[ADDR[0].x+3]\n36: END\n";

#define CREATE(kind,size) VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT,kind,size)
#define DESTROY(kind) VIRGL_CMD0(VIRGL_CCMD_DESTROY_OBJECT,kind,1)
#define BIND(kind) VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT,kind,1)
static const ULONG pipeline[] = {
    CREATE(VIRGL_OBJECT_VERTEX_ELEMENTS,VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(0)),Elements,
    BIND(VIRGL_OBJECT_VERTEX_ELEMENTS),Elements
};

static BOOLEAN shader(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image, ULONG handle,
                      ULONG stage, const char *text, ULONG size)
{
    ULONG words[6+(ShaderText+3)/4], n = (size+3)/4;
    if (size > ShaderText) {
        r->status = ERROR_INVALID_DATA;
        r->stage = RB2K_STAGE_TRANSFER;
        return FALSE;
    }
    words[0] = CREATE(VIRGL_OBJECT_SHADER,5+n);
    words[1] = handle;
    words[2] = stage;
    words[3] = size;
    words[4] = 300 + size; /* token storage: the 9x emitter's 300 plus room for longer text */
    words[5] = 0;
    words[5+n] = 0;
    VideoPortMoveMemory(words+6,(PVOID)text,size);
    return rb_gpu_frame_words(gpu,r,image,words,6+n);
}

/* Encode a positive integer dimension as an IEEE single without enabling
 * floating point in the miniport. The largest supported dimension is 1024. */
static ULONG dimension_bits(ULONG value)
{
    ULONG shift = 0, v = value;
    while (v >>= 1) ++shift;
    return ((127+shift)<<23) | ((value ^ (1UL<<shift)) << (23-shift));
}

/* An unsigned integer below 2^23 as an IEEE single, zero included. */
static ULONG integer_bits(ULONG value)
{
    return value ? dimension_bits(value) : 0;
}

BOOLEAN rb_gpu_prepare_target(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image)
{
    ULONG fb[] = {
        CREATE(VIRGL_OBJECT_SURFACE,VIRGL_OBJ_SURFACE_SIZE),1,image->id,VIRGL_FORMAT_B8G8R8A8_UNORM,0,0,
        VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE,0,VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)),1,0,1
    };
    if (!image->defined) {
        /* Uncovered pixels of a fresh target are logically zero. */
        if (!rb_gpu_draw_image(gpu,r,image->id,0,!image->hasSurface,image,
                               image->depthBuffer ? RB2K_D7_DEPTH_VIEW : 0)) return FALSE;
        image->defined = image->uniform = image->hasSurface = TRUE;
        image->color = 0;
    } else if (!image->hasSurface) {
        if (!rb_gpu_frame_words(gpu,r,image,fb,sizeof(fb)/sizeof(*fb))) return FALSE;
        image->hasSurface = TRUE;
    }
    return TRUE;
}

/* ---- fragment shader text for Direct3D texture stage zero ---- */

typedef struct { char *p; ULONG n, line; BOOLEAN overflow; } TEXT;

static void put(TEXT *t, const char *s)
{
    while (*s) {
        if (t->n + 1 >= ShaderText) { t->overflow = TRUE; return; }
        t->p[t->n++] = *s++;
    }
    t->p[t->n] = 0;
}

static void number(TEXT *t, ULONG v)
{
    char digits[11];
    ULONG i = sizeof(digits);
    digits[--i] = 0;
    do { digits[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(t, digits + i);
}

/* One numbered instruction line. */
static void op(TEXT *t, const char *a, const char *b, const char *c, const char *d)
{
    number(t, t->line++);
    put(t, ": ");
    put(t, a);
    if (b) put(t, b);
    if (c) put(t, c);
    if (d) put(t, d);
    put(t, "\n");
}

static const char *source(ULONG arg)
{
    switch (arg & ArgSelect) {
    case ArgTexture: return "TEMP[0]";
    case ArgFactor: return "CONST[0]";
    case ArgSpecular: return "IN[1]";
    }
    return "IN[0]";
}

/* Copy one argument, with its modifiers, into the given temporary. */
static void argument(TEXT *t, ULONG arg, const char *slot)
{
    number(t, t->line++);
    put(t, (arg & ArgComplement) ? ": ADD " : ": MOV ");
    put(t, slot);
    put(t, (arg & ArgComplement) ? ", IMM[0].xxxx, -" : ", ");
    put(t, source(arg));
    if (arg & ArgAlphaReplicate) put(t, ".wwww");
    put(t, "\n");
}

/* Combine TEMP[1] and TEMP[2] into the TEMP[3] channels named by mask. */
static void combine(TEXT *t, ULONG top, const char *mask)
{
    char d[16] = "TEMP[3]";
    ULONG i = 7;
    while (*mask && i < sizeof(d)-1) d[i++] = *mask++;
    d[i] = 0;
    switch (top) {
    case TopSelect1: op(t, "MOV ", d, ", TEMP[1]", 0); break;
    case TopSelect2: op(t, "MOV ", d, ", TEMP[2]", 0); break;
    case TopModulate: op(t, "MUL ", d, ", TEMP[1], TEMP[2]", 0); break;
    case TopModulate2x:
        op(t, "MUL TEMP[4], TEMP[1], TEMP[2]", 0, 0, 0);
        op(t, "ADD_SAT ", d, ", TEMP[4], TEMP[4]", 0); break;
    case TopModulate4x:
        op(t, "MUL TEMP[4], TEMP[1], TEMP[2]", 0, 0, 0);
        op(t, "MUL_SAT ", d, ", TEMP[4], IMM[0].zzzz", 0); break;
    case TopAdd: op(t, "ADD_SAT ", d, ", TEMP[1], TEMP[2]", 0); break;
    case TopAddSigned:
        op(t, "ADD TEMP[4], TEMP[1], TEMP[2]", 0, 0, 0);
        op(t, "ADD_SAT ", d, ", TEMP[4], -IMM[0].wwww", 0); break;
    case TopAddSigned2x:
        op(t, "ADD TEMP[4], TEMP[1], TEMP[2]", 0, 0, 0);
        op(t, "ADD TEMP[4], TEMP[4], -IMM[0].wwww", 0, 0, 0);
        op(t, "ADD_SAT ", d, ", TEMP[4], TEMP[4]", 0); break;
    case TopSubtract: op(t, "ADD_SAT ", d, ", TEMP[1], -TEMP[2]", 0); break;
    case TopAddSmooth:
        op(t, "ADD TEMP[4], IMM[0].xxxx, -TEMP[1]", 0, 0, 0);
        op(t, "MAD_SAT ", d, ", TEMP[2], TEMP[4], TEMP[1]", 0); break;
    case TopBlendDiffuse:
    case TopBlendCurrent: op(t, "LRP ", d, ", IN[0].wwww, TEMP[1], TEMP[2]", 0); break;
    case TopBlendTexture: op(t, "LRP ", d, ", TEMP[0].wwww, TEMP[1], TEMP[2]", 0); break;
    case TopBlendFactor: op(t, "LRP ", d, ", CONST[0].wwww, TEMP[1], TEMP[2]", 0); break;
    case TopBlendTexturePm:
        op(t, "ADD TEMP[4], IMM[0].xxxx, -TEMP[0].wwww", 0, 0, 0);
        op(t, "MAD_SAT ", d, ", TEMP[2], TEMP[4], TEMP[1]", 0); break;
    }
}

static BOOLEAN first_used(ULONG stage) { return (stage & 255) != TopSelect2; }
static BOOLEAN second_used(ULONG stage) { return (stage & 255) != TopSelect1; }

static BOOLEAN uses(ULONG stage, ULONG arg)
{
    ULONG top = stage & 255;
    if ((top == TopBlendTexture || top == TopBlendTexturePm) && arg == ArgTexture) return TRUE;
    if (top == TopBlendFactor && arg == ArgFactor) return TRUE;
    return (first_used(stage) && ((stage >> 8) & ArgSelect) == arg) ||
           (second_used(stage) && ((stage >> 16) & ArgSelect) == arg);
}

BOOLEAN rb_gpu_d7_samples(const RB2K_D3D7_DRAW *d)
{
    return uses(d->colorStage, ArgTexture) || uses(d->alphaStage, ArgTexture);
}

static BOOLEAN stage_valid(ULONG stage)
{
    ULONG top = stage & 255, i;
    if (stage >> 24 || top < TopSelect1 || top > TopBlendCurrent) return FALSE;
    for (i = 8; i <= 16; i += 8) {
        ULONG arg = (stage >> i) & 255;
        if ((arg & ~(ArgSelect | ArgComplement | ArgAlphaReplicate)) ||
            ((arg & ArgSelect) != ArgDiffuse && (arg & ArgSelect) != ArgTexture &&
             (arg & ArgSelect) != ArgFactor && (arg & ArgSelect) != ArgSpecular)) return FALSE;
    }
    return TRUE;
}

static ULONG fragment_text(char *out, ULONG color, ULONG alpha, ULONG specular)
{
    TEXT t;
    BOOLEAN sampled = uses(color, ArgTexture) || uses(alpha, ArgTexture);
    t.p = out; t.n = 0; t.line = 0; t.overflow = FALSE;
    put(&t, "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL IN[1], GENERIC[1], LINEAR\n"
            "DCL IN[2], GENERIC[2], PERSPECTIVE\nDCL OUT[0], COLOR\n");
    if (sampled) put(&t, "DCL SAMP[0]\n");
    if (uses(color, ArgFactor) || uses(alpha, ArgFactor)) put(&t, "DCL CONST[0]\n");
    put(&t, "DCL TEMP[0..4]\nIMM[0] FLT32 {0x3f800000, 0x40000000, 0x40800000, 0x3f000000}\n");
    if (sampled) op(&t, "TEX TEMP[0], IN[2], SAMP[0], 2D", 0, 0, 0);
    if (first_used(color)) argument(&t, (color >> 8) & 255, "TEMP[1]");
    if (second_used(color)) argument(&t, (color >> 16) & 255, "TEMP[2]");
    combine(&t, color & 255, ".xyz");
    if (first_used(alpha)) argument(&t, (alpha >> 8) & 255, "TEMP[1]");
    if (second_used(alpha)) argument(&t, (alpha >> 16) & 255, "TEMP[2]");
    combine(&t, alpha & 255, ".w");
    if (specular) op(&t, "ADD_SAT TEMP[3].xyz, TEMP[3], IN[1]", 0, 0, 0);
    op(&t, "MOV OUT[0], TEMP[3]", 0, 0, 0);
    op(&t, "END", 0, 0, 0);
    return t.overflow ? 0 : t.n + 1;
}

/* ---- validation and state objects ---- */

static BOOLEAN blend_valid(ULONG factor) { return factor >= 1 && factor <= 13; }

BOOLEAN rb_gpu_d7_valid(const RB2K_D3D7_DRAW *d)
{
    ULONG addressU = d->sampler & 15, addressV = (d->sampler >> 4) & 15;
    ULONG mag = (d->sampler >> 8) & 15, min = (d->sampler >> 12) & 15;
    if (d->depthState && (!(d->depthState & RB2K_Z_ENABLE) ||
        d->depthState > (RB2K_Z_ENABLE | RB2K_Z_WRITE | 7))) return FALSE;
    if (d->alphaTest && ((d->alphaTest & ~(RB2K_D7_ON | 0xfff)) ||
        ((d->alphaTest >> 8) & 15) < 1 || ((d->alphaTest >> 8) & 15) > 8)) return FALSE;
    if (d->blend && ((d->blend & ~(RB2K_D7_ON | 0xffff)) ||
        !blend_valid((d->blend >> 8) & 255) || !blend_valid(d->blend & 255))) return FALSE;
    if (d->cull < CullNone || d->cull > CullCcw || d->specular > 1) return FALSE;
    if (d->transform > 1 || (d->transform && (!d->viewport[2] || !d->viewport[3] ||
        d->viewport[0] > 8192 || d->viewport[1] > 8192 || d->viewport[2] > 8192 || d->viewport[3] > 8192)))
        return FALSE;
    if (!stage_valid(d->colorStage) || !stage_valid(d->alphaStage)) return FALSE;
    if (d->sampler >> 16 || addressU < 1 || addressU > 4 || addressV < 1 || addressV > 4 ||
        mag < 1 || mag > 5 || min < 1 || min > 3) return FALSE;
    return TRUE;
}

/* Find or make room for a key. The state is Hit, Fresh (an unused handle)
 * or Replaced (the caller destroys that handle's old object first). */
enum { Hit, Fresh, Replaced };
static ULONG cache_slot(RB2K_D7_CACHE *c, ULONG limit, const ULONG *key, ULONG words, ULONG *state)
{
    ULONG i, j;
    for (i = 0; i < c->used; ++i) {
        for (j = 0; j < words && c->keys[i][j] == key[j]; ++j) {}
        if (j == words) { *state = Hit; return i; }
    }
    if (c->used < limit) { i = c->used++; *state = Fresh; }
    else {
        i = c->next;
        c->next = (c->next + 1) % limit;
        *state = Replaced;
        if (c->bound == i + 1) c->bound = 0;
    }
    for (j = 0; j < words; ++j) c->keys[i][j] = key[j];
    return i;
}

static ULONG pipe_blend(ULONG factor)
{
    /* The render target has no alpha channel for Direct3D: destination
     * alpha is one, and the saturated source factor is therefore zero. */
    static const UCHAR map[] = {
        0, 0x11, 0x01, 0x02, 0x12, 0x03, 0x13, 0x01, 0x11, 0x05, 0x15, 0x11, 0x03, 0x13
    };
    return map[factor];
}

static ULONG pipe_wrap(ULONG address)
{
    static const UCHAR map[] = { 0, 0, 4, 2, 3 }; /* wrap, mirror, clamp, border */
    return map[address];
}

/* Emit the state changes of one draw: depth and alpha test, blend, cull,
 * fragment shader, texture factor, sampler and texture view. */
static BOOLEAN states(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                      const RB2K_D3D7_DRAW *d, const RB2K_TEXTURE *texture)
{
    RB2K_D7_STATE *s = &image->d7;
    ULONG words[64], n = 0, slot, key[3], state, i;
    char text[ShaderText];
    if (!s->pipeline) {
        if (!rb_gpu_frame_words(gpu,r,image,pipeline,sizeof(pipeline)/sizeof(*pipeline))) return FALSE;
        s->pipeline = TRUE;
    }
    {
        ULONG want = d->transform ? 2 : 1, handle = d->transform ? ObjectShader : VertexShader;
        if (!(s->vertexShaders & want)) {
            if (d->transform ? !shader(gpu,r,image,handle,VertexStage,object_shader,sizeof(object_shader)) :
                               !shader(gpu,r,image,handle,VertexStage,vertex_shader,sizeof(vertex_shader)))
                return FALSE;
            s->vertexShaders |= want;
        }
        if (s->boundVertex != want) {
            ULONG bind[] = { VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER,0,VIRGL_BIND_SHADER_SIZE), handle, VertexStage };
            if (!rb_gpu_frame_words(gpu,r,image,bind,3)) return FALSE;
            s->boundVertex = want;
        }
    }
    /* Depth and alpha test share one object. */
    key[0] = d->depthState | (d->alphaTest << 8);
    slot = cache_slot(&s->dsa, RB2K_D7_DSA_KEYS, key, 1, &state);
    if (state != Hit) {
        ULONG z = d->depthState;
        if (state == Replaced) { words[n++] = DESTROY(VIRGL_OBJECT_DSA); words[n++] = DsaBase + slot; }
        words[n++] = CREATE(VIRGL_OBJECT_DSA, VIRGL_OBJ_DSA_SIZE);
        words[n++] = DsaBase + slot;
        words[n++] = (z & RB2K_Z_ENABLE ? VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(1) |
                      VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(!!(z & RB2K_Z_WRITE)) |
                      VIRGL_OBJ_DSA_S0_DEPTH_FUNC(z & 7) : 0) |
                     (d->alphaTest ? VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(1) |
                      VIRGL_OBJ_DSA_S0_ALPHA_FUNC(((d->alphaTest >> 8) & 15) - 1) : 0);
        words[n++] = 0;
        words[n++] = 0;
        words[n++] = rb_gpu_channel(d->alphaTest & 255);
    }
    if (s->dsa.bound != slot + 1) {
        words[n++] = BIND(VIRGL_OBJECT_DSA); words[n++] = DsaBase + slot;
        s->dsa.bound = slot + 1;
    }
    key[0] = d->blend;
    slot = cache_slot(&s->blend, RB2K_D7_BLEND_KEYS, key, 1, &state);
    if (state != Hit) {
        ULONG src = (d->blend >> 8) & 255, dst = d->blend & 255, rt = VIRGL_OBJ_BLEND_S2_RT_COLORMASK(15);
        if (src == 12) { src = 5; dst = 6; }        /* both source alpha */
        else if (src == 13) { src = 6; dst = 5; }   /* both inverse source alpha */
        if (d->blend) rt |= VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(1) |
            VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(pipe_blend(src)) |
            VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(pipe_blend(dst)) |
            VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(pipe_blend(src)) |
            VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(pipe_blend(dst));
        if (state == Replaced) { words[n++] = DESTROY(VIRGL_OBJECT_BLEND); words[n++] = BlendBase + slot; }
        words[n++] = CREATE(VIRGL_OBJECT_BLEND, VIRGL_OBJ_BLEND_SIZE);
        words[n++] = BlendBase + slot;
        words[n++] = 0; words[n++] = 0; words[n++] = rt;
        for (i = 1; i < VIRGL_MAX_COLOR_BUFS; ++i) words[n++] = 0;
    }
    if (s->blend.bound != slot + 1) {
        words[n++] = BIND(VIRGL_OBJECT_BLEND); words[n++] = BlendBase + slot;
        s->blend.bound = slot + 1;
    }
    if (s->boundRaster != d->cull + 3 * d->transform) {
        ULONG key = d->cull + 3 * d->transform, bit = 1UL << key;
        if (!(s->rasters & bit)) {
            /* Window coordinates follow the Direct3D screen (y down), so a
             * clockwise triangle faces the front, as in Gallium Nine.
             * Transformed draws are clipped to the Direct3D viewport. */
            words[n++] = CREATE(VIRGL_OBJECT_RASTERIZER, VIRGL_OBJ_RS_SIZE);
            words[n++] = Raster + key;
            words[n++] = VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) | VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) |
                         VIRGL_OBJ_RS_S0_SCISSOR(d->transform) |
                         VIRGL_OBJ_RS_S0_BOTTOM_EDGE_RULE(1) |
                         VIRGL_OBJ_RS_S0_CULL_FACE(d->cull == CullCw ? 1 : d->cull == CullCcw ? 2 : 0);
            words[n++] = 0x3f800000; words[n++] = 0; words[n++] = 0;
            words[n++] = 0x3f800000; words[n++] = 0; words[n++] = 0; words[n++] = 0;
            s->rasters |= bit;
        }
        words[n++] = BIND(VIRGL_OBJECT_RASTERIZER); words[n++] = Raster + key;
        s->boundRaster = key;
    }
    if (n && !rb_gpu_frame_words(gpu, r, image, words, n)) return FALSE;
    n = 0;

    key[0] = d->colorStage; key[1] = d->alphaStage; key[2] = d->specular;
    slot = cache_slot(&s->shader, RB2K_D7_SHADER_KEYS, key, 3, &state);
    if (state != Hit) {
        ULONG size = fragment_text(text, d->colorStage, d->alphaStage, d->specular);
        if (!size) { r->status = ERROR_INVALID_DATA; r->stage = RB2K_STAGE_TRANSFER; return FALSE; }
        if (state == Replaced) {
            words[0] = DESTROY(VIRGL_OBJECT_SHADER); words[1] = ShaderBase + slot;
            if (!rb_gpu_frame_words(gpu, r, image, words, 2)) return FALSE;
        }
        if (!shader(gpu, r, image, ShaderBase + slot, FragmentStage, text, size)) return FALSE;
    }
    if (s->shader.bound != slot + 1) {
        words[n++] = VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER,0,VIRGL_BIND_SHADER_SIZE);
        words[n++] = ShaderBase + slot; words[n++] = FragmentStage;
        s->shader.bound = slot + 1;
    }
    if (uses(d->colorStage, ArgFactor) || uses(d->alphaStage, ArgFactor)) {
        words[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_CONSTANT_BUFFER,0,6);
        words[n++] = FragmentStage; words[n++] = 0;
        words[n++] = rb_gpu_channel((d->factor >> 16) & 255);
        words[n++] = rb_gpu_channel((d->factor >> 8) & 255);
        words[n++] = rb_gpu_channel(d->factor & 255);
        words[n++] = rb_gpu_channel(d->factor >> 24);
    }
    if (texture) {
        key[0] = d->sampler; key[1] = d->border;
        slot = cache_slot(&s->sampler, RB2K_D7_SAMPLER_KEYS, key, 2, &state);
        if (state != Hit) {
            ULONG mag = (d->sampler >> 8) & 15, min = (d->sampler >> 12) & 15;
            if (state == Replaced) {
                words[n++] = DESTROY(VIRGL_OBJECT_SAMPLER_STATE); words[n++] = SamplerBase + slot;
            }
            words[n++] = CREATE(VIRGL_OBJECT_SAMPLER_STATE, VIRGL_OBJ_SAMPLER_STATE_SIZE);
            words[n++] = SamplerBase + slot;
            /* Only the top level is uploaded: no mipmap filter. */
            words[n++] = VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_S(pipe_wrap(d->sampler & 15)) |
                         VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_T(pipe_wrap((d->sampler >> 4) & 15)) |
                         VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_R(2) |
                         VIRGL_OBJ_SAMPLE_STATE_S0_MIN_IMG_FILTER(min != 1) |
                         VIRGL_OBJ_SAMPLE_STATE_S0_MIN_MIP_FILTER(2) |
                         VIRGL_OBJ_SAMPLE_STATE_S0_MAG_IMG_FILTER(mag != 1);
            words[n++] = 0; words[n++] = 0; words[n++] = 0;
            words[n++] = rb_gpu_channel((d->border >> 16) & 255);
            words[n++] = rb_gpu_channel((d->border >> 8) & 255);
            words[n++] = rb_gpu_channel(d->border & 255);
            words[n++] = rb_gpu_channel(d->border >> 24);
        }
        if (s->sampler.bound != slot + 1) {
            words[n++] = VIRGL_CMD0(VIRGL_CCMD_BIND_SAMPLER_STATES,0,VIRGL_BIND_SAMPLER_STATES(1));
            words[n++] = FragmentStage; words[n++] = 0; words[n++] = SamplerBase + slot;
            s->sampler.bound = slot + 1;
        }
        if (s->viewTexture != texture->id) {
            if (s->viewTexture) { words[n++] = DESTROY(VIRGL_OBJECT_SAMPLER_VIEW); words[n++] = TextureView; }
            words[n++] = CREATE(VIRGL_OBJECT_SAMPLER_VIEW, VIRGL_OBJ_SAMPLER_VIEW_SIZE);
            words[n++] = TextureView; words[n++] = texture->id; words[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
            words[n++] = 0; words[n++] = 0; words[n++] = 0x688; /* identity swizzle */
            words[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_SAMPLER_VIEWS,0,VIRGL_SET_SAMPLER_VIEWS_SIZE(1));
            words[n++] = FragmentStage; words[n++] = 0; words[n++] = TextureView;
            s->viewTexture = texture->id;
        }
    }
    return !n || rb_gpu_frame_words(gpu, r, image, words, n);
}

BOOLEAN rb_gpu_d7_draw(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                       const RB2K_D3D7_DRAW *d, const RB2K_TEXTURE *texture)
{
    ULONG words[4 + 8 + 7 + 56 + 16*RB2K_D3D7_VERTICES + 13], *p = words, i, count = d->count;
    ULONG width = dimension_bits(image->width), height = dimension_bits(image->height);
    if (!rb_gpu_prepare_target(gpu, r, image) || !states(gpu, r, image, d, texture)) return FALSE;
    *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE,0,VIRGL_SET_VIEWPORT_STATE_SIZE(1));
    *p++ = 0;
    /* The viewport scale and translation each use half the dimensions. */
    *p++ = width - (1UL << 23);
    *p++ = height - (1UL << 23);
    *p++ = 0x3f000000;
    *p++ = width - (1UL << 23);
    *p++ = height - (1UL << 23);
    *p++ = 0x3f000000;
    if (d->transform) {
        ULONG x0 = d->viewport[0], y0 = d->viewport[1];
        ULONG x1 = d->viewport[0] + d->viewport[2], y1 = d->viewport[1] + d->viewport[3];
        if (x1 > image->width) x1 = image->width;
        if (y1 > image->height) y1 = image->height;
        if (x0 > x1) x0 = x1;
        if (y0 > y1) y0 = y1;
        *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_SCISSOR_STATE,0,VIRGL_SET_SCISSOR_STATE_SIZE(1));
        *p++ = 0; *p++ = x0 | y0 << 16; *p++ = x1 | y1 << 16;
    }
    *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_CONSTANT_BUFFER,0,6+(d->transform ? 56 : 0)+16*count);
    *p++ = VertexStage; *p++ = 0;
    /* Constant zero supplies the shader with its viewport dimensions. */
    *p++ = width; *p++ = height; *p++ = 0; *p++ = 0;
    if (d->transform) {
        ULONG m, row, col;
        /* Each matrix as its four columns, so DP4 gives a row vector times it. */
        for (m = 0; m < 3; ++m)
            for (col = 0; col < 4; ++col)
                for (row = 0; row < 4; ++row) *p++ = d->matrices[m][row * 4 + col];
        for (i = 0; i < 4; ++i) *p++ = integer_bits(d->viewport[i]);
        *p++ = d->viewport[4]; *p++ = d->viewport[5]; *p++ = 0; *p++ = 0;
    }
    for (i = 0; i < count; ++i) {
        const RB2K_D3D7_VERTEX *v = d->vertices + i;
        *p++ = v->x; *p++ = v->y; *p++ = v->z; *p++ = v->rhw;
        *p++ = rb_gpu_channel((v->diffuse >> 16) & 255);
        *p++ = rb_gpu_channel((v->diffuse >> 8) & 255);
        *p++ = rb_gpu_channel(v->diffuse & 255);
        *p++ = rb_gpu_channel(v->diffuse >> 24);
        *p++ = rb_gpu_channel((v->specular >> 16) & 255);
        *p++ = rb_gpu_channel((v->specular >> 8) & 255);
        *p++ = rb_gpu_channel(v->specular & 255);
        *p++ = rb_gpu_channel(v->specular >> 24);
        *p++ = v->u; *p++ = v->v; *p++ = 0; *p++ = 0x3f800000;
    }
    *p++ = VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO,0,VIRGL_DRAW_VBO_SIZE);
    *p++ = 0;                  /* first vertex */
    *p++ = count;
    *p++ = PipeTriangles;
    *p++ = 0;                  /* unindexed */
    *p++ = 1;                  /* one instance */
    *p++ = 0;                  /* index bias */
    *p++ = 0;                  /* first instance */
    *p++ = 0;                  /* primitive restart */
    *p++ = 0;                  /* restart index */
    *p++ = 0;                  /* minimum index */
    *p++ = count - 1;          /* maximum index */
    *p++ = 0;                  /* stream-output count */
    if (!rb_gpu_frame_words(gpu,r,image,words,p-words)) return FALSE;
    image->uniform = FALSE;
    return TRUE;
}
