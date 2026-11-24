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
    Raster = 9, Elements = 5, VertexShader = 6, ObjectShader = 7, LitShader = 8, DsaBase = 16, BlendBase = 32,
    SamplerBase = 48, Sampler1Base = 56, ShaderBase = 64, TextureView = 80, TextureView1 = 81,
    PsVertexShader = 82, PsObjectShader = 83, PsViewBase = 84, PsSamplerBase = 96, PsLitShader = 112
};
enum { VertexStage = 0, FragmentStage = 1, PipeTriangles = 4, ShaderText = 2400, LitText = 8192 };
typedef char sampler_handles_fit[Sampler1Base + RB2K_D7_SAMPLER_KEYS <= ShaderBase &&
                                 SamplerBase + RB2K_D7_SAMPLER_KEYS <= Sampler1Base ? 1 : -1];

/* Direct3D 7 values used on the wire (d3dtypes.h). */
enum {
    TopDisable = 1, TopSelect1, TopSelect2, TopModulate, TopModulate2x, TopModulate4x, TopAdd,
    TopAddSigned, TopAddSigned2x, TopSubtract, TopAddSmooth, TopBlendDiffuse, TopBlendTexture,
    TopBlendFactor, TopBlendTexturePm, TopBlendCurrent
};
enum { ArgDiffuse = 0, ArgCurrent, ArgTexture, ArgFactor, ArgSpecular, ArgSelect = 15,
       ArgComplement = 0x10, ArgAlphaReplicate = 0x20 };
enum { CullNone = 1, CullCw, CullCcw };

/* The constant ranges below cover RB2K_D3D7_VERTICES (96) vertices of four
 * constants each: CONST[1..384] here, CONST[15..398] in object_lines. */
typedef char vertex_constants_match[RB2K_D3D7_VERTICES == 96 ? 1 : -1];
static const char vertex_shader[] =
    "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..384]\nDCL ADDR[0]\n"
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
/* Also writes the fog depths GENERIC[3]: camera-space z and distance. */
static const char *const object_lines[] = {
    "UADD TEMP[0].x, SV[0].xxxx, SV[0].xxxx",
    "UADD TEMP[0].x, TEMP[0].xxxx, TEMP[0].xxxx",
    "UADD TEMP[0].x, TEMP[0].xxxx, IMM[1].xxxx",
    "UARL ADDR[0].x, TEMP[0].xxxx",
    "MOV TEMP[1], CONST[ADDR[0].x]",
    "DP4 TEMP[2].x, TEMP[1], CONST[1]",
    "DP4 TEMP[2].y, TEMP[1], CONST[2]",
    "DP4 TEMP[2].z, TEMP[1], CONST[3]",
    "DP4 TEMP[2].w, TEMP[1], CONST[4]",
    "DP4 TEMP[1].x, TEMP[2], CONST[5]",
    "DP4 TEMP[1].y, TEMP[2], CONST[6]",
    "DP4 TEMP[1].z, TEMP[2], CONST[7]",
    "DP4 TEMP[1].w, TEMP[2], CONST[8]",
    "DP4 TEMP[2].x, TEMP[1], CONST[9]",
    "DP4 TEMP[2].y, TEMP[1], CONST[10]",
    "DP4 TEMP[2].z, TEMP[1], CONST[11]",
    "DP4 TEMP[2].w, TEMP[1], CONST[12]",
    "MOV TEMP[5].x, TEMP[1].zzzz",
    "DP3 TEMP[5].y, TEMP[1], TEMP[1]",
    "RSQ TEMP[5].y, TEMP[5].yyyy",
    "RCP TEMP[5].y, TEMP[5].yyyy",
    "MOV TEMP[5].zw, IMM[0].zzzz",
    "MOV OUT[4], TEMP[5]",
    "RCP TEMP[3].x, CONST[0].xxxx",
    "RCP TEMP[3].y, CONST[0].yyyy",
    "MUL TEMP[4].xy, CONST[13].zwww, TEMP[3].xyyy",
    "MAD TEMP[3].zw, CONST[13].xxxy, IMM[0].yyyy, CONST[13].xxzw",
    "ADD TEMP[3].zw, TEMP[3], IMM[0].xxxx",
    "MAD TEMP[4].zw, TEMP[3], TEMP[3].xxxy, -IMM[0].xxxx",
    "MUL TEMP[1].x, TEMP[2].xxxx, TEMP[4].xxxx",
    "MAD OUT[0].x, TEMP[2].wwww, TEMP[4].zzzz, TEMP[1].xxxx",
    "MUL TEMP[1].y, TEMP[2].yyyy, -TEMP[4].yyyy",
    "MAD OUT[0].y, TEMP[2].wwww, TEMP[4].wwww, TEMP[1].yyyy",
    "ADD TEMP[3].x, CONST[14].yyyy, -CONST[14].xxxx",
    "MUL TEMP[3].x, TEMP[3].xxxx, IMM[0].yyyy",
    "MAD TEMP[3].y, CONST[14].xxxx, IMM[0].yyyy, -IMM[0].xxxx",
    "MUL TEMP[1].z, TEMP[2].zzzz, TEMP[3].xxxx",
    "MAD OUT[0].z, TEMP[2].wwww, TEMP[3].yyyy, TEMP[1].zzzz",
    "MOV OUT[0].w, TEMP[2].wwww",
    "MOV OUT[1], CONST[ADDR[0].x+1]",
    "MOV OUT[2], CONST[ADDR[0].x+2]",
    "MOV OUT[3], CONST[ADDR[0].x+3]",
    "END"
};

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
    ULONG n = (size+3)/4, *words;
    if (!size || size > LitText) {
        r->status = ERROR_INVALID_DATA;
        r->stage = RB2K_STAGE_TRANSFER;
        return FALSE;
    }
    words = rb_gpu_frame_reserve(gpu, r, image, 6+n);
    if (!words) return FALSE;
    words[0] = CREATE(VIRGL_OBJECT_SHADER,5+n);
    words[1] = handle;
    words[2] = stage;
    words[3] = size;
    words[4] = 300 + size; /* token storage: the 9x emitter's 300 plus room for longer text */
    words[5] = 0;
    words[5+n] = 0;
    VideoPortMoveMemory(words+6,(PVOID)text,size);
    return TRUE;
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

typedef struct { char *p; ULONG n, line, size; BOOLEAN overflow; } TEXT;

static void put(TEXT *t, const char *s)
{
    while (*s) {
        if (t->n + 1 >= t->size) { t->overflow = TRUE; return; }
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

/* Registers: TEMP[0] and TEMP[5] are the stage zero and one texels, TEMP[6]
 * stage zero's result for stage one's current colour. */
static const char *source(ULONG arg, ULONG stage)
{
    switch (arg & ArgSelect) {
    case ArgTexture: return stage ? "TEMP[5]" : "TEMP[0]";
    case ArgCurrent: return "TEMP[6]";
    case ArgFactor: return "CONST[0]";
    case ArgSpecular: return "IN[1]";
    }
    return "IN[0]";
}

/* Copy one argument, with its modifiers, into the given temporary. */
static void argument(TEXT *t, ULONG arg, const char *slot, ULONG stage)
{
    number(t, t->line++);
    put(t, (arg & ArgComplement) ? ": ADD " : ": MOV ");
    put(t, slot);
    put(t, (arg & ArgComplement) ? ", IMM[0].xxxx, -" : ", ");
    put(t, source(arg, stage));
    if (arg & ArgAlphaReplicate) put(t, ".wwww");
    put(t, "\n");
}

/* Combine TEMP[1] and TEMP[2] into the TEMP[3] channels named by mask. */
static void combine(TEXT *t, ULONG top, const char *mask, ULONG stage)
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
    case TopBlendDiffuse: op(t, "LRP ", d, ", IN[0].wwww, TEMP[1], TEMP[2]", 0); break;
    case TopBlendCurrent:
        op(t, "LRP ", d, stage ? ", TEMP[6].wwww, TEMP[1], TEMP[2]" : ", IN[0].wwww, TEMP[1], TEMP[2]", 0); break;
    case TopBlendTexture:
        op(t, "LRP ", d, stage ? ", TEMP[5].wwww, TEMP[1], TEMP[2]" : ", TEMP[0].wwww, TEMP[1], TEMP[2]", 0); break;
    case TopBlendFactor: op(t, "LRP ", d, ", CONST[0].wwww, TEMP[1], TEMP[2]", 0); break;
    case TopBlendTexturePm:
        op(t, stage ? "ADD TEMP[4], IMM[0].xxxx, -TEMP[5].wwww" : "ADD TEMP[4], IMM[0].xxxx, -TEMP[0].wwww", 0, 0, 0);
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

BOOLEAN rb_gpu_d7_samples(const RB2K_D3D7_DRAW *d, ULONG stage)
{
    if (stage) return d->stage1Color && (uses(d->stage1Color, ArgTexture) || uses(d->stage1Alpha, ArgTexture));
    return uses(d->colorStage, ArgTexture) || uses(d->alphaStage, ArgTexture);
}

/* Stage one may also read the current colour. */
static BOOLEAN stage_valid(ULONG stage, BOOLEAN current)
{
    ULONG top = stage & 255, i;
    if (stage >> 24 || top < TopSelect1 || top > TopBlendCurrent) return FALSE;
    for (i = 8; i <= 16; i += 8) {
        ULONG arg = (stage >> i) & 255;
        if ((arg & ~(ArgSelect | ArgComplement | ArgAlphaReplicate)) ||
            ((arg & ArgSelect) != ArgDiffuse && (arg & ArgSelect) != ArgTexture &&
             (arg & ArgSelect) != ArgFactor && (arg & ArgSelect) != ArgSpecular &&
             (!current || (arg & ArgSelect) != ArgCurrent))) return FALSE;
    }
    return TRUE;
}

/* The fog factor goes to TEMP[4].x, then the colour moves toward the fog
 * colour (CONST[1]) by one minus it. CONST[2] holds start, end, density. */
static void fog_text(TEXT *t, ULONG fog)
{
    ULONG kind = fog & 3, mode = (fog >> RB2K_D7_FOG_MODE_SHIFT) & 3;
    if (kind == RB2K_D7_FOG_ALPHA) op(t, "MOV_SAT TEMP[4].x, IN[1].wwww", 0, 0, 0);
    else {
        if (kind == RB2K_D7_FOG_VERTEX)
            op(t, (fog & RB2K_D7_FOG_RANGE) ? "MOV TEMP[4].y, IN[3].yyyy" : "MOV TEMP[4].y, IN[3].xxxx", 0, 0, 0);
        else op(t, (fog & RB2K_D7_FOG_W) ? "RCP TEMP[4].y, IN[3].wwww" : "MOV TEMP[4].y, IN[3].zzzz", 0, 0, 0);
        if (mode == 3) {   /* linear: (end - d) / (end - start) */
            op(t, "ADD TEMP[4].x, CONST[2].yyyy, -TEMP[4].yyyy", 0, 0, 0);
            op(t, "ADD TEMP[4].z, CONST[2].yyyy, -CONST[2].xxxx", 0, 0, 0);
            op(t, "RCP TEMP[4].z, TEMP[4].zzzz", 0, 0, 0);
            op(t, "MUL_SAT TEMP[4].x, TEMP[4].xxxx, TEMP[4].zzzz", 0, 0, 0);
        } else {           /* exp(-(density d)) or exp(-(density d)^2) */
            op(t, "MUL TEMP[4].x, CONST[2].zzzz, TEMP[4].yyyy", 0, 0, 0);
            if (mode == 2) op(t, "MUL TEMP[4].x, TEMP[4].xxxx, TEMP[4].xxxx", 0, 0, 0);
            op(t, "MUL TEMP[4].x, TEMP[4].xxxx, -IMM[1].xxxx", 0, 0, 0);
            op(t, "EX2 TEMP[4].x, TEMP[4].xxxx", 0, 0, 0);
            op(t, "MOV_SAT TEMP[4].x, TEMP[4].xxxx", 0, 0, 0);
        }
    }
    op(t, "LRP TEMP[3].xyz, TEMP[4].xxxx, TEMP[3], CONST[1]", 0, 0, 0);
}

/* Stage one, when on, samples its texture with the second coordinate pair
 * (IN[2].zw) and combines over stage zero's result. */
static ULONG fragment_text(char *out, ULONG color, ULONG alpha, ULONG specular, ULONG fog,
                           ULONG color1, ULONG alpha1)
{
    TEXT t;
    BOOLEAN sampled = uses(color, ArgTexture) || uses(alpha, ArgTexture);
    BOOLEAN sampled1 = color1 && (uses(color1, ArgTexture) || uses(alpha1, ArgTexture));
    BOOLEAN factor = uses(color, ArgFactor) || uses(alpha, ArgFactor) ||
                     (color1 && (uses(color1, ArgFactor) || uses(alpha1, ArgFactor)));
    ULONG kind = fog & 3;
    t.p = out; t.n = 0; t.line = 0; t.size = ShaderText; t.overflow = FALSE;
    put(&t, "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL IN[1], GENERIC[1], LINEAR\n"
            "DCL IN[2], GENERIC[2], PERSPECTIVE\n");
    if (kind == RB2K_D7_FOG_VERTEX) put(&t, "DCL IN[3], GENERIC[3], PERSPECTIVE\n");
    if (kind == RB2K_D7_FOG_TABLE) put(&t, "DCL IN[3], POSITION, LINEAR\n");
    put(&t, "DCL OUT[0], COLOR\n");
    if (sampled) put(&t, "DCL SAMP[0]\n");
    if (sampled1) put(&t, "DCL SAMP[1]\n");
    if (kind) put(&t, "DCL CONST[0..2]\n");
    else if (factor) put(&t, "DCL CONST[0]\n");
    put(&t, "DCL TEMP[0..7]\nIMM[0] FLT32 {0x3f800000, 0x40000000, 0x40800000, 0x3f000000}\n");
    if (kind) put(&t, "IMM[1] FLT32 {0x3fb8aa3b, 0x00000000, 0x00000000, 0x00000000}\n");   /* log2(e) */
    if (sampled) op(&t, "TEX TEMP[0], IN[2], SAMP[0], 2D", 0, 0, 0);
    if (first_used(color)) argument(&t, (color >> 8) & 255, "TEMP[1]", 0);
    if (second_used(color)) argument(&t, (color >> 16) & 255, "TEMP[2]", 0);
    combine(&t, color & 255, ".xyz", 0);
    if (first_used(alpha)) argument(&t, (alpha >> 8) & 255, "TEMP[1]", 0);
    if (second_used(alpha)) argument(&t, (alpha >> 16) & 255, "TEMP[2]", 0);
    combine(&t, alpha & 255, ".w", 0);
    if (color1) {
        op(&t, "MOV TEMP[6], TEMP[3]", 0, 0, 0);
        if (sampled1) {
            op(&t, "MOV TEMP[7], IN[2].zwzw", 0, 0, 0);
            op(&t, "TEX TEMP[5], TEMP[7], SAMP[1], 2D", 0, 0, 0);
        }
        if (first_used(color1)) argument(&t, (color1 >> 8) & 255, "TEMP[1]", 1);
        if (second_used(color1)) argument(&t, (color1 >> 16) & 255, "TEMP[2]", 1);
        combine(&t, color1 & 255, ".xyz", 1);
        if (first_used(alpha1)) argument(&t, (alpha1 >> 8) & 255, "TEMP[1]", 1);
        if (second_used(alpha1)) argument(&t, (alpha1 >> 16) & 255, "TEMP[2]", 1);
        combine(&t, alpha1 & 255, ".w", 1);
    }
    if (specular) op(&t, "ADD_SAT TEMP[3].xyz, TEMP[3], IN[1]", 0, 0, 0);
    if (kind) fog_text(&t, fog);
    op(&t, "MOV OUT[0], TEMP[3]", 0, 0, 0);
    op(&t, "END", 0, 0, 0);
    return t.overflow ? 0 : t.n + 1;
}

/* ---- vertex shader for Direct3D 7 fixed-function lighting ---- */

/* Constants: 0 target size, 1..12 world, view and projection columns, 13
 * viewport, 14 depth range, 15 global ambient, 16..19 material diffuse,
 * ambient, specular and emissive, 20 (power, normalize, local viewer,
 * specular enabled), 21 and 22 the material sources as flags (diffuse from
 * colour 1 or 2, specular from 1 or 2; ambient, emissive), 23 the light
 * count and first light constant as integers. Vertices follow from 24, five
 * constants each (position, colours 1 and 2, texture coordinate, normal),
 * then each light: diffuse, specular, ambient, position, direction,
 * (range, attenuation 0..2), (theta, phi, falloff), (directional, spot).
 * Lighting is done in camera space, as Direct3D does. Every choice between
 * light types is a CMP, so values a type leaves undefined never mix in. */
enum { LitVertexBase = 24, LitVertexConstants = 5, LitLightConstants = 8,
       LitConstants = LitVertexBase + LitVertexConstants*RB2K_D3D7_VERTICES +
                      LitLightConstants*RB2K_D7_LIGHTS };
static const char *const lit_lines[] = {
    "UMAD TEMP[0].x, SV[0].xxxx, IMM[1].xxxx, IMM[1].yyyy",
    "UARL ADDR[0].x, TEMP[0].xxxx",
    "MOV TEMP[1], CONST[ADDR[0].x]",
    "DP4 TEMP[2].x, TEMP[1], CONST[1]", "DP4 TEMP[2].y, TEMP[1], CONST[2]",
    "DP4 TEMP[2].z, TEMP[1], CONST[3]", "DP4 TEMP[2].w, TEMP[1], CONST[4]",
    "DP4 TEMP[3].x, TEMP[2], CONST[5]", "DP4 TEMP[3].y, TEMP[2], CONST[6]",
    "DP4 TEMP[3].z, TEMP[2], CONST[7]", "DP4 TEMP[3].w, TEMP[2], CONST[8]",
    "DP4 TEMP[4].x, TEMP[3], CONST[9]", "DP4 TEMP[4].y, TEMP[3], CONST[10]",
    "DP4 TEMP[4].z, TEMP[3], CONST[11]", "DP4 TEMP[4].w, TEMP[3], CONST[12]",
    /* The normal in camera space, normalized when asked. */
    "MOV TEMP[5], CONST[ADDR[0].x+4]",
    "DP3 TEMP[6].x, TEMP[5], CONST[1]", "DP3 TEMP[6].y, TEMP[5], CONST[2]",
    "DP3 TEMP[6].z, TEMP[5], CONST[3]",
    "DP3 TEMP[7].x, TEMP[6], CONST[5]", "DP3 TEMP[7].y, TEMP[6], CONST[6]",
    "DP3 TEMP[7].z, TEMP[6], CONST[7]", "MOV TEMP[7].w, IMM[0].zzzz",
    "DP3 TEMP[14].x, TEMP[7], TEMP[7]", "MAX TEMP[14].x, TEMP[14].xxxx, IMM[2].wwww",
    "RSQ TEMP[14].x, TEMP[14].xxxx", "MUL TEMP[14].xyz, TEMP[7], TEMP[14].xxxx",
    "CMP TEMP[7].xyz, -CONST[20].yyyy, TEMP[14], TEMP[7]",
    /* Vertex colours, texture coordinate and the four material colours. */
    "MOV TEMP[8], CONST[ADDR[0].x+1]", "MOV TEMP[9], CONST[ADDR[0].x+2]",
    "MOV TEMP[26], CONST[ADDR[0].x+3]",
    "ADD TEMP[14], TEMP[8], -CONST[16]", "MAD TEMP[10], CONST[21].xxxx, TEMP[14], CONST[16]",
    "ADD TEMP[14], TEMP[9], -CONST[16]", "MAD TEMP[10], CONST[21].yyyy, TEMP[14], TEMP[10]",
    "ADD TEMP[14], TEMP[8], -CONST[18]", "MAD TEMP[11], CONST[21].zzzz, TEMP[14], CONST[18]",
    "ADD TEMP[14], TEMP[9], -CONST[18]", "MAD TEMP[11], CONST[21].wwww, TEMP[14], TEMP[11]",
    "ADD TEMP[14], TEMP[8], -CONST[17]", "MAD TEMP[12], CONST[22].xxxx, TEMP[14], CONST[17]",
    "ADD TEMP[14], TEMP[9], -CONST[17]", "MAD TEMP[12], CONST[22].yyyy, TEMP[14], TEMP[12]",
    "ADD TEMP[14], TEMP[8], -CONST[19]", "MAD TEMP[13], CONST[22].zzzz, TEMP[14], CONST[19]",
    "ADD TEMP[14], TEMP[9], -CONST[19]", "MAD TEMP[13], CONST[22].wwww, TEMP[14], TEMP[13]",
    /* Ambient, diffuse and specular sums; the direction to the viewer. */
    "MOV TEMP[15], CONST[15]", "MOV TEMP[16], IMM[0].zzzz", "MOV TEMP[17], IMM[0].zzzz",
    "DP3 TEMP[14].x, TEMP[3], TEMP[3]", "MAX TEMP[14].x, TEMP[14].xxxx, IMM[2].wwww",
    "RSQ TEMP[14].x, TEMP[14].xxxx", "MUL TEMP[18].xyz, -TEMP[3], TEMP[14].xxxx",
    "CMP TEMP[18].xyz, -CONST[20].zzzz, TEMP[18], IMM[2]", "MOV TEMP[18].w, IMM[0].zzzz",
    "MOV TEMP[0].y, IMM[3].xxxx",
    "BGNLOOP",
    "USGE TEMP[0].z, TEMP[0].yyyy, CONST[23].xxxx",
    "UIF TEMP[0].zzzz", "BRK", "ENDIF",
    "UMAD TEMP[0].w, TEMP[0].yyyy, IMM[1].zzzz, CONST[23].yyyy",
    "UARL ADDR[0].x, TEMP[0].wwww",
    /* Light position and direction in camera space. */
    "DP4 TEMP[19].x, CONST[ADDR[0].x+3], CONST[5]", "DP4 TEMP[19].y, CONST[ADDR[0].x+3], CONST[6]",
    "DP4 TEMP[19].z, CONST[ADDR[0].x+3], CONST[7]",
    "DP3 TEMP[20].x, CONST[ADDR[0].x+4], CONST[5]", "DP3 TEMP[20].y, CONST[ADDR[0].x+4], CONST[6]",
    "DP3 TEMP[20].z, CONST[ADDR[0].x+4], CONST[7]", "MOV TEMP[20].w, IMM[0].zzzz",
    "DP3 TEMP[22].x, TEMP[20], TEMP[20]", "MAX TEMP[22].x, TEMP[22].xxxx, IMM[2].wwww",
    "RSQ TEMP[22].x, TEMP[22].xxxx", "MUL TEMP[20].xyz, TEMP[20], TEMP[22].xxxx",
    /* Point and spot: direction to the light, distance, attenuation, range. */
    "ADD TEMP[21].xyz, TEMP[19], -TEMP[3]", "MOV TEMP[21].w, IMM[0].zzzz",
    "DP3 TEMP[22].x, TEMP[21], TEMP[21]", "MAX TEMP[22].x, TEMP[22].xxxx, IMM[2].wwww",
    "RSQ TEMP[22].y, TEMP[22].xxxx", "MUL TEMP[22].z, TEMP[22].xxxx, TEMP[22].yyyy",
    "MUL TEMP[21].xyz, TEMP[21], TEMP[22].yyyy",
    "MAD TEMP[22].w, CONST[ADDR[0].x+5].zzzz, TEMP[22].zzzz, CONST[ADDR[0].x+5].yyyy",
    "MAD TEMP[22].w, CONST[ADDR[0].x+5].wwww, TEMP[22].xxxx, TEMP[22].wwww",
    "RCP TEMP[22].w, TEMP[22].wwww",
    "SGE TEMP[23].x, CONST[ADDR[0].x+5].xxxx, TEMP[22].zzzz",
    "MUL TEMP[22].w, TEMP[22].wwww, TEMP[23].xxxx",
    /* Spot: ((rho - cos(phi/2)) / (cos(theta/2) - cos(phi/2)))^falloff inside the outer cone. */
    "DP3 TEMP[23].x, -TEMP[21], TEMP[20]",
    "MUL TEMP[23].yz, CONST[ADDR[0].x+6].xxyy, IMM[0].wwww",
    "COS TEMP[23].y, TEMP[23].yyyy", "COS TEMP[23].z, TEMP[23].zzzz",
    "ADD TEMP[23].w, TEMP[23].xxxx, -TEMP[23].zzzz",
    "ADD TEMP[24].x, TEMP[23].yyyy, -TEMP[23].zzzz", "MAX TEMP[24].x, TEMP[24].xxxx, IMM[2].wwww",
    "RCP TEMP[24].x, TEMP[24].xxxx", "MUL_SAT TEMP[23].w, TEMP[23].wwww, TEMP[24].xxxx",
    "MAX TEMP[23].w, TEMP[23].wwww, IMM[2].wwww",
    "POW TEMP[23].w, TEMP[23].wwww, CONST[ADDR[0].x+6].zzzz",
    "SLT TEMP[24].y, TEMP[23].zzzz, TEMP[23].xxxx", "MUL TEMP[23].w, TEMP[23].wwww, TEMP[24].yyyy",
    "CMP TEMP[23].w, -CONST[ADDR[0].x+7].yyyy, TEMP[23].wwww, IMM[0].xxxx",
    "MUL TEMP[22].w, TEMP[22].wwww, TEMP[23].wwww",
    /* Directional: toward the light is minus its direction, no attenuation. */
    "CMP TEMP[21].xyz, -CONST[ADDR[0].x+7].xxxx, -TEMP[20], TEMP[21]",
    "CMP TEMP[22].w, -CONST[ADDR[0].x+7].xxxx, IMM[0].xxxx, TEMP[22].wwww",
    /* Ambient, diffuse, and specular with the halfway vector where N.L > 0. */
    "MAD TEMP[15], CONST[ADDR[0].x+2], TEMP[22].wwww, TEMP[15]",
    "DP3 TEMP[24].x, TEMP[7], TEMP[21]", "MAX TEMP[24].x, TEMP[24].xxxx, IMM[0].zzzz",
    "MUL TEMP[24].y, TEMP[24].xxxx, TEMP[22].wwww",
    "MAD TEMP[16], CONST[ADDR[0].x], TEMP[24].yyyy, TEMP[16]",
    "ADD TEMP[25].xyz, TEMP[21], TEMP[18]", "MOV TEMP[25].w, IMM[0].zzzz",
    "DP3 TEMP[24].z, TEMP[25], TEMP[25]", "MAX TEMP[24].z, TEMP[24].zzzz, IMM[2].wwww",
    "RSQ TEMP[24].z, TEMP[24].zzzz", "MUL TEMP[25].xyz, TEMP[25], TEMP[24].zzzz",
    "DP3 TEMP[24].z, TEMP[7], TEMP[25]", "MAX TEMP[24].z, TEMP[24].zzzz, IMM[2].wwww",
    "POW TEMP[24].z, TEMP[24].zzzz, CONST[20].xxxx",
    "SLT TEMP[24].w, IMM[0].zzzz, TEMP[24].xxxx", "MUL TEMP[24].z, TEMP[24].zzzz, TEMP[24].wwww",
    "MUL TEMP[24].z, TEMP[24].zzzz, TEMP[22].wwww",
    "MAD TEMP[17], CONST[ADDR[0].x+1], TEMP[24].zzzz, TEMP[17]",
    "UADD TEMP[0].y, TEMP[0].yyyy, IMM[1].wwww",
    "ENDLOOP",
    /* Diffuse: Ca*ambient + Cd*diffuse + Ce with the alpha of Cd; specular:
     * Cs*specular when enabled, with the vertex specular alpha (fog). */
    "MUL TEMP[14], TEMP[12], TEMP[15]", "MAD TEMP[14], TEMP[10], TEMP[16], TEMP[14]",
    "ADD TEMP[14], TEMP[14], TEMP[13]", "MOV TEMP[14].w, TEMP[10].wwww", "MOV_SAT OUT[1], TEMP[14]",
    "MUL TEMP[14], TEMP[11], TEMP[17]", "MUL TEMP[14].xyz, TEMP[14], CONST[20].wwww",
    "MOV TEMP[14].w, TEMP[9].wwww", "MOV_SAT OUT[2], TEMP[14]",
    "MOV OUT[3], TEMP[26]",
    /* Fog depths as in object_lines, then its viewport mapping from the clip position. */
    "MOV TEMP[14].x, TEMP[3].zzzz", "DP3 TEMP[14].y, TEMP[3], TEMP[3]", "RSQ TEMP[14].y, TEMP[14].yyyy",
    "RCP TEMP[14].y, TEMP[14].yyyy", "MOV TEMP[14].zw, IMM[0].zzzz", "MOV OUT[4], TEMP[14]",
    "RCP TEMP[27].x, CONST[0].xxxx", "RCP TEMP[27].y, CONST[0].yyyy",
    "MUL TEMP[28].xy, CONST[13].zwww, TEMP[27].xyyy",
    "MAD TEMP[27].zw, CONST[13].xxxy, IMM[0].yyyy, CONST[13].xxzw",
    "ADD TEMP[27].zw, TEMP[27], IMM[0].xxxx",
    "MAD TEMP[28].zw, TEMP[27], TEMP[27].xxxy, -IMM[0].xxxx",
    "MUL TEMP[29].x, TEMP[4].xxxx, TEMP[28].xxxx",
    "MAD OUT[0].x, TEMP[4].wwww, TEMP[28].zzzz, TEMP[29].xxxx",
    "MUL TEMP[29].y, TEMP[4].yyyy, -TEMP[28].yyyy",
    "MAD OUT[0].y, TEMP[4].wwww, TEMP[28].wwww, TEMP[29].yyyy",
    "ADD TEMP[27].x, CONST[14].yyyy, -CONST[14].xxxx", "MUL TEMP[27].x, TEMP[27].xxxx, IMM[0].yyyy",
    "MAD TEMP[27].y, CONST[14].xxxx, IMM[0].yyyy, -IMM[0].xxxx",
    "MUL TEMP[29].z, TEMP[4].zzzz, TEMP[27].xxxx",
    "MAD OUT[0].z, TEMP[4].wwww, TEMP[27].yyyy, TEMP[29].zzzz",
    "MOV OUT[0].w, TEMP[4].wwww",
    "END"
};

static ULONG lit_text(char *out)
{
    TEXT t;
    ULONG i;
    t.p = out; t.n = 0; t.line = 0; t.size = LitText; t.overflow = FALSE;
    put(&t, "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..");
    number(&t, LitConstants - 1);
    put(&t, "]\nDCL ADDR[0]\nDCL TEMP[0..29]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
            "DCL OUT[2], GENERIC[1]\nDCL OUT[3], GENERIC[2]\nDCL OUT[4], GENERIC[3]\n"
            "IMM[0] FLT32 {0x3f800000, 0x40000000, 0x00000000, 0x3f000000}\n"
            "IMM[1] UINT32 {5, 24, 8, 1}\n"
            "IMM[2] FLT32 {0x00000000, 0x00000000, 0xbf800000, 0x0d800000}\n"
            "IMM[3] UINT32 {0, 0, 0, 0}\n");
    for (i = 0; i < sizeof(lit_lines)/sizeof(*lit_lines); ++i) op(&t, lit_lines[i], 0, 0, 0);
    return t.overflow ? 0 : t.n + 1;
}
typedef char lit_layout[LitVertexBase == 24 && LitVertexConstants == 5 && LitLightConstants == 8 ? 1 : -1];

static ULONG object_text(char *out)
{
    TEXT t;
    ULONG i;
    t.p = out; t.n = 0; t.line = 0; t.size = LitText; t.overflow = FALSE;
    put(&t, "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..398]\nDCL ADDR[0]\n"
            "DCL TEMP[0..5]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
            "DCL OUT[2], GENERIC[1]\nDCL OUT[3], GENERIC[2]\nDCL OUT[4], GENERIC[3]\n"
            "IMM[0] FLT32 {0x3f800000, 0x40000000, 0x00000000, 0x00000000}\n"
            "IMM[1] UINT32 {15, 0, 0, 0}\n");
    for (i = 0; i < sizeof(object_lines)/sizeof(*object_lines); ++i) op(&t, object_lines[i], 0, 0, 0);
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
    if (d->fog) {
        ULONG kind = d->fog & 3, mode = (d->fog >> RB2K_D7_FOG_MODE_SHIFT) & 3;
        if ((d->fog & ~(3UL | 3UL << RB2K_D7_FOG_MODE_SHIFT | RB2K_D7_FOG_RANGE | RB2K_D7_FOG_W)) || !kind ||
            (kind == RB2K_D7_FOG_ALPHA ? d->fog != kind : !mode) ||
            (kind == RB2K_D7_FOG_VERTEX && !d->transform) ||
            ((d->fog & RB2K_D7_FOG_RANGE) && kind != RB2K_D7_FOG_VERTEX) ||
            ((d->fog & RB2K_D7_FOG_W) && kind != RB2K_D7_FOG_TABLE)) return FALSE;
    }
    if (d->lighting) {
        ULONG k;
        if (d->lighting != 1 || !d->transform || d->lightCount > RB2K_D7_LIGHTS ||
            (d->lightFlags & ~((0xffUL << RB2K_D7_SOURCE_SHIFT) | RB2K_D7_NORMALIZE | RB2K_D7_LOCAL_VIEWER)))
            return FALSE;
        for (k = 0; k < 4; ++k)
            if (((d->lightFlags >> (RB2K_D7_SOURCE_SHIFT + 2*k)) & 3) > 2) return FALSE;
        for (k = 0; k < d->lightCount; ++k)
            if (d->lights[k][0] < 1 || d->lights[k][0] > 3) return FALSE;   /* point, spot, directional */
    }
    if (!stage_valid(d->colorStage, FALSE) || !stage_valid(d->alphaStage, FALSE)) return FALSE;
    if (d->stage1Color ? !stage_valid(d->stage1Color, TRUE) || !stage_valid(d->stage1Alpha, TRUE)
                       : d->stage1Alpha != 0) return FALSE;
    {
        ULONG addressU = d->sampler1 & 15, addressV = (d->sampler1 >> 4) & 15;
        ULONG mag = (d->sampler1 >> 8) & 15, min = (d->sampler1 >> 12) & 15;
        if (d->sampler1 >> 20 || ((d->sampler1 >> 16) & 15) > 3 || addressU < 1 || addressU > 4 || addressV < 1 || addressV > 4 ||
            mag < 1 || mag > 5 || min < 1 || min > 3) return FALSE;
    }
    if (d->sampler >> 20 || ((d->sampler >> 16) & 15) > 3 || addressU < 1 || addressU > 4 || addressV < 1 || addressV > 4 ||
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

/* Create (or replace) one sampler object. The Direct3D mip filter (point 2,
 * linear 3) becomes Gallium's nearest (0) or linear (1); anything else
 * samples the top level only (2). */
static ULONG sampler_words(ULONG *words, ULONG handle, ULONG state, ULONG sampler, ULONG border)
{
    ULONG n = 0, mag = (sampler >> 8) & 15, min = (sampler >> 12) & 15, mip = (sampler >> 16) & 15;
    if (state == Replaced) { words[n++] = DESTROY(VIRGL_OBJECT_SAMPLER_STATE); words[n++] = handle; }
    words[n++] = CREATE(VIRGL_OBJECT_SAMPLER_STATE, VIRGL_OBJ_SAMPLER_STATE_SIZE);
    words[n++] = handle;
    words[n++] = VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_S(pipe_wrap(sampler & 15)) |
                 VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_T(pipe_wrap((sampler >> 4) & 15)) |
                 VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_R(2) |
                 VIRGL_OBJ_SAMPLE_STATE_S0_MIN_IMG_FILTER(min != 1) |
                 VIRGL_OBJ_SAMPLE_STATE_S0_MIN_MIP_FILTER(mip == 2 ? 0 : mip == 3 ? 1 : 2) |
                 VIRGL_OBJ_SAMPLE_STATE_S0_MAG_IMG_FILTER(mag != 1);
    /* No level-of-detail bias, and every level reachable (minimum 0, maximum
     * 1000.0); a maximum of zero would clamp sampling to the top level. */
    words[n++] = 0; words[n++] = 0; words[n++] = 0x447a0000;
    words[n++] = rb_gpu_channel((border >> 16) & 255);
    words[n++] = rb_gpu_channel((border >> 8) & 255);
    words[n++] = rb_gpu_channel(border & 255);
    words[n++] = rb_gpu_channel(border >> 24);
    return n;
}

/* Depth and alpha test, blend and rasterizer objects, shared by the
 * fixed-function and the pixel shader draws. */
static BOOLEAN fixed_states(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image, ULONG depthState,
                            ULONG alphaTest, ULONG blend, ULONG cull, ULONG transform)
{
    RB2K_D7_STATE *s = &image->d7;
    ULONG words[64], n = 0, slot, key[1], state, i;
    /* Depth and alpha test share one object. */
    key[0] = depthState | (alphaTest << 8);
    slot = cache_slot(&s->dsa, RB2K_D7_DSA_KEYS, key, 1, &state);
    if (state != Hit) {
        ULONG z = depthState;
        if (state == Replaced) { words[n++] = DESTROY(VIRGL_OBJECT_DSA); words[n++] = DsaBase + slot; }
        words[n++] = CREATE(VIRGL_OBJECT_DSA, VIRGL_OBJ_DSA_SIZE);
        words[n++] = DsaBase + slot;
        words[n++] = (z & RB2K_Z_ENABLE ? VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(1) |
                      VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(!!(z & RB2K_Z_WRITE)) |
                      VIRGL_OBJ_DSA_S0_DEPTH_FUNC(z & 7) : 0) |
                     (alphaTest ? VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(1) |
                      VIRGL_OBJ_DSA_S0_ALPHA_FUNC(((alphaTest >> 8) & 15) - 1) : 0);
        words[n++] = 0;
        words[n++] = 0;
        words[n++] = rb_gpu_channel(alphaTest & 255);
    }
    if (s->dsa.bound != slot + 1) {
        words[n++] = BIND(VIRGL_OBJECT_DSA); words[n++] = DsaBase + slot;
        s->dsa.bound = slot + 1;
    }
    key[0] = blend;
    slot = cache_slot(&s->blend, RB2K_D7_BLEND_KEYS, key, 1, &state);
    if (state != Hit) {
        ULONG src = (blend >> 8) & 255, dst = blend & 255, rt = VIRGL_OBJ_BLEND_S2_RT_COLORMASK(15);
        if (src == 12) { src = 5; dst = 6; }        /* both source alpha */
        else if (src == 13) { src = 6; dst = 5; }   /* both inverse source alpha */
        if (blend) rt |= VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(1) |
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
    if (s->boundRaster != cull + 3 * transform) {
        ULONG key = cull + 3 * transform, bit = 1UL << key;
        if (!(s->rasters & bit)) {
            /* Window coordinates follow the Direct3D screen (y down), so a
             * clockwise triangle faces the front, as in Gallium Nine.
             * Transformed draws are clipped to the Direct3D viewport. */
            words[n++] = CREATE(VIRGL_OBJECT_RASTERIZER, VIRGL_OBJ_RS_SIZE);
            words[n++] = Raster + key;
            words[n++] = VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) | VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) |
                         VIRGL_OBJ_RS_S0_SCISSOR(transform) |
                         VIRGL_OBJ_RS_S0_BOTTOM_EDGE_RULE(1) |
                         VIRGL_OBJ_RS_S0_CULL_FACE(cull == CullCw ? 1 : cull == CullCcw ? 2 : 0);
            words[n++] = 0x3f800000; words[n++] = 0; words[n++] = 0;
            words[n++] = 0x3f800000; words[n++] = 0; words[n++] = 0; words[n++] = 0;
            s->rasters |= bit;
        }
        words[n++] = BIND(VIRGL_OBJECT_RASTERIZER); words[n++] = Raster + key;
        s->boundRaster = key;
    }
    return !n || rb_gpu_frame_words(gpu, r, image, words, n);
}

/* Emit the state changes of one draw: depth and alpha test, blend, cull,
 * fragment shader, texture factor, sampler and texture view. */
static BOOLEAN states(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                      const RB2K_D3D7_DRAW *d, const RB2K_TEXTURE *texture, const RB2K_TEXTURE *texture1)
{
    RB2K_D7_STATE *s = &image->d7;
    ULONG words[96], n = 0, slot, key[6], state, i;
    char text[ShaderText];
    if (!s->pipeline) {
        if (!rb_gpu_frame_words(gpu,r,image,pipeline,sizeof(pipeline)/sizeof(*pipeline))) return FALSE;
        s->pipeline = TRUE;
    }
    {
        static char litText[LitText], objectText[LitText];
        static ULONG litSize, objectSize;
        ULONG want = d->lighting ? 4 : d->transform ? 2 : 1;
        ULONG handle = d->lighting ? LitShader : d->transform ? ObjectShader : VertexShader;
        if (!(s->vertexShaders & want)) {
            BOOLEAN ok;
            if (d->lighting) {
                if (!litSize) litSize = lit_text(litText);
                ok = litSize && shader(gpu,r,image,handle,VertexStage,litText,litSize);
                if (!litSize) { r->status = ERROR_INVALID_DATA; r->stage = RB2K_STAGE_TRANSFER; }
            } else if (d->transform) {
                if (!objectSize) objectSize = object_text(objectText);
                ok = objectSize && shader(gpu,r,image,handle,VertexStage,objectText,objectSize);
                if (!objectSize) { r->status = ERROR_INVALID_DATA; r->stage = RB2K_STAGE_TRANSFER; }
            } else ok = shader(gpu,r,image,handle,VertexStage,vertex_shader,sizeof(vertex_shader));
            if (!ok) return FALSE;
            s->vertexShaders |= want;
        }
        if (s->boundVertex != want) {
            ULONG bind[] = { VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER,0,VIRGL_BIND_SHADER_SIZE), handle, VertexStage };
            if (!rb_gpu_frame_words(gpu,r,image,bind,3)) return FALSE;
            s->boundVertex = want;
        }
    }
    if (!fixed_states(gpu, r, image, d->depthState, d->alphaTest, d->blend, d->cull, d->transform)) return FALSE;

    key[0] = d->colorStage; key[1] = d->alphaStage; key[2] = d->specular; key[3] = d->fog;
    key[4] = d->stage1Color; key[5] = d->stage1Alpha;
    slot = cache_slot(&s->shader, RB2K_D7_SHADER_KEYS, key, 6, &state);
    if (state != Hit) {
        ULONG size = fragment_text(text, d->colorStage, d->alphaStage, d->specular, d->fog,
                                   d->stage1Color, d->stage1Alpha);
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
    if (d->fog || uses(d->colorStage, ArgFactor) || uses(d->alphaStage, ArgFactor) ||
        (d->stage1Color && (uses(d->stage1Color, ArgFactor) || uses(d->stage1Alpha, ArgFactor)))) {
        /* The texture factor, then for fog its colour and start, end, density. */
        words[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_CONSTANT_BUFFER,0,d->fog ? 14 : 6);
        words[n++] = FragmentStage; words[n++] = 0;
        words[n++] = rb_gpu_channel((d->factor >> 16) & 255);
        words[n++] = rb_gpu_channel((d->factor >> 8) & 255);
        words[n++] = rb_gpu_channel(d->factor & 255);
        words[n++] = rb_gpu_channel(d->factor >> 24);
        if (d->fog) {
            words[n++] = rb_gpu_channel((d->fogColor >> 16) & 255);
            words[n++] = rb_gpu_channel((d->fogColor >> 8) & 255);
            words[n++] = rb_gpu_channel(d->fogColor & 255);
            words[n++] = 0x3f800000;
            words[n++] = d->fogStart; words[n++] = d->fogEnd; words[n++] = d->fogDensity; words[n++] = 0;
        }
    }
    {
        /* Stage zero uses sampler and view slot zero, stage one slot one. The
         * renderer keeps only the slots up to the last one set, so a draw with
         * stage one sets both slots at once; slot zero then repeats stage
         * one's objects when stage zero has no texture. */
        ULONG views = texture1 ? 2 : texture ? 1 : 0, h[2] = { 0, 0 }, view0;
        BOOLEAN changed = FALSE;
        if (texture) {
            key[0] = d->sampler; key[1] = d->border;
            slot = cache_slot(&s->sampler, RB2K_D7_SAMPLER_KEYS, key, 2, &state);
            if (state != Hit) { n += sampler_words(words + n, SamplerBase + slot, state, d->sampler, d->border); changed = TRUE; }
            h[0] = SamplerBase + slot;
            if (s->viewTexture != texture->id) {
                if (s->viewTexture) { words[n++] = DESTROY(VIRGL_OBJECT_SAMPLER_VIEW); words[n++] = TextureView; }
                words[n++] = CREATE(VIRGL_OBJECT_SAMPLER_VIEW, VIRGL_OBJ_SAMPLER_VIEW_SIZE);
                words[n++] = TextureView; words[n++] = texture->id; words[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
                words[n++] = 0; words[n++] = (texture->levels - 1) << 8;   /* all mipmap levels */
                words[n++] = 0x688; /* identity swizzle */
                s->viewTexture = texture->id;
                changed = TRUE;
            }
        }
        if (texture1) {
            key[0] = d->sampler1; key[1] = d->border1;
            slot = cache_slot(&s->sampler1, RB2K_D7_SAMPLER_KEYS, key, 2, &state);
            if (state != Hit) { n += sampler_words(words + n, Sampler1Base + slot, state, d->sampler1, d->border1); changed = TRUE; }
            h[1] = Sampler1Base + slot;
            if (!texture) h[0] = h[1];
            if (s->viewTexture1 != texture1->id) {
                if (s->viewTexture1) { words[n++] = DESTROY(VIRGL_OBJECT_SAMPLER_VIEW); words[n++] = TextureView1; }
                words[n++] = CREATE(VIRGL_OBJECT_SAMPLER_VIEW, VIRGL_OBJ_SAMPLER_VIEW_SIZE);
                words[n++] = TextureView1; words[n++] = texture1->id; words[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
                words[n++] = 0; words[n++] = (texture1->levels - 1) << 8;   /* all mipmap levels */
                words[n++] = 0x688; /* identity swizzle */
                s->viewTexture1 = texture1->id;
                changed = TRUE;
            }
        }
        view0 = texture ? TextureView : TextureView1;
        if (views && (changed || s->boundSamplers != views || s->boundSampler[0] != h[0] ||
                      (views == 2 && s->boundSampler[1] != h[1]))) {
            words[n++] = VIRGL_CMD0(VIRGL_CCMD_BIND_SAMPLER_STATES,0,VIRGL_BIND_SAMPLER_STATES(views));
            words[n++] = FragmentStage; words[n++] = 0;
            for (i = 0; i < views; ++i) words[n++] = h[i];
            s->boundSamplers = views; s->boundSampler[0] = h[0]; s->boundSampler[1] = h[1];
        }
        if (views && (changed || s->boundViews != views || s->boundView0 != view0)) {
            words[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_SAMPLER_VIEWS,0,VIRGL_SET_SAMPLER_VIEWS_SIZE(views));
            words[n++] = FragmentStage; words[n++] = 0; words[n++] = view0;
            if (views == 2) words[n++] = TextureView1;
            s->boundViews = views; s->boundView0 = view0;
        }
    }
    return !n || rb_gpu_frame_words(gpu, r, image, words, n);
}

BOOLEAN rb_gpu_d7_draw(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                       const RB2K_D3D7_DRAW *d, const RB2K_TEXTURE *texture, const RB2K_TEXTURE *texture1)
{
    /* Viewport 8, scissor 4, constants 7 + 56 (+ 36 lit) + 16 (20 lit) per vertex
     * (+ 32 per light), draw 13. */
    ULONG lit = d->lighting != 0, lights = lit ? d->lightCount : 0, perVertex = lit ? 20 : 16;
    ULONG extra = (d->transform ? 56 : 0) + (lit ? 36 + 32*lights : 0);
    ULONG i, count = d->count, total = 28 + (d->transform ? 4 : 0) + extra + perVertex*count, *p;
    ULONG width = dimension_bits(image->width), height = dimension_bits(image->height);
    if (!rb_gpu_prepare_target(gpu, r, image) || !states(gpu, r, image, d, texture, texture1)) return FALSE;
    p = rb_gpu_frame_reserve(gpu, r, image, total);
    if (!p) return FALSE;
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
    *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_CONSTANT_BUFFER,0,6+extra+perVertex*count);
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
    if (lit) {
        /* Constants 15..23 of lit_lines; Direct3D material sources 1 and 2 are
         * the vertex colours. */
        const ULONG one = 0x3f800000, sources = d->lightFlags >> RB2K_D7_SOURCE_SHIFT;
        ULONG k;
        *p++ = rb_gpu_channel((d->ambient >> 16) & 255);
        *p++ = rb_gpu_channel((d->ambient >> 8) & 255);
        *p++ = rb_gpu_channel(d->ambient & 255);
        *p++ = rb_gpu_channel(d->ambient >> 24);
        for (k = 0; k < 16; ++k) *p++ = d->material[k];
        *p++ = d->material[16];
        *p++ = d->lightFlags & RB2K_D7_NORMALIZE ? one : 0;
        *p++ = d->lightFlags & RB2K_D7_LOCAL_VIEWER ? one : 0;
        *p++ = d->specular ? one : 0;
        for (k = 0; k < 4; ++k) {
            *p++ = ((sources >> 2*k) & 3) == 1 ? one : 0;
            *p++ = ((sources >> 2*k) & 3) == 2 ? one : 0;
        }
        *p++ = lights; *p++ = LitVertexBase + LitVertexConstants*count; *p++ = 0; *p++ = 0;
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
        *p++ = v->u; *p++ = v->v; *p++ = v->u1; *p++ = v->v1;
        if (lit) { *p++ = v->nx; *p++ = v->ny; *p++ = v->nz; *p++ = 0; }
    }
    for (i = 0; i < lights; ++i) {
        /* D3DLIGHT7: type, diffuse, specular, ambient, position, direction,
         * range, falloff, attenuation 0..2, theta, phi. */
        const ULONG *l = d->lights[i];
        ULONG k;
        for (k = 1; k <= 12; ++k) *p++ = l[k];
        *p++ = l[13]; *p++ = l[14]; *p++ = l[15]; *p++ = 0x3f800000;
        *p++ = l[16]; *p++ = l[17]; *p++ = l[18]; *p++ = 0;
        *p++ = l[19]; *p++ = l[21]; *p++ = l[22]; *p++ = l[23];
        *p++ = l[24]; *p++ = l[25]; *p++ = l[20]; *p++ = 0;
        *p++ = l[0] == 3 ? 0x3f800000 : 0; *p++ = l[0] == 2 ? 0x3f800000 : 0; *p++ = 0; *p++ = 0;
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
    image->uniform = FALSE;
    return TRUE;
}

/* ---- Direct3D 8 pixel shader draws ---- */

/* Vertices are constants from CONST[1] (transformed) or CONST[15] (object
 * space, after the matrices and viewport of object_lines): position, diffuse,
 * specular and the coordinate sets the draw carries, CONST[0].z (an integer)
 * apart. The outputs are GENERIC[0..9], the colours and the eight sets, the
 * inputs of the translated pixel shader; a set the draw does not carry reads
 * words of the next vertex, which the program does not use. */
enum { PsVertexConstants = 3 + RB2K_D8_STAGES, PsOutputs = 2 + RB2K_D8_STAGES };
typedef char ps_handles[PsViewBase + RB2K_D8_STAGES <= PsSamplerBase &&
                        PsSamplerBase + RB2K_D8_SAMPLER_KEYS <= PsLitShader && PsOutputs < 11 ? 1 : -1];
static const char *const ps_transformed_lines[] = {
    "UMAD TEMP[0].x, SV[0].xxxx, CONST[0].zzzz, IMM[1].xxxx",
    "UARL ADDR[0].x, TEMP[0].xxxx",
    "MOV TEMP[1], CONST[ADDR[0].x]",
    "RCP TEMP[2].x, CONST[0].xxxx",
    "RCP TEMP[2].y, CONST[0].yyyy",
    "ADD TEMP[1].xy, TEMP[1], IMM[0].xxxx",
    "MUL TEMP[1].xy, TEMP[1], TEMP[2]",
    "MAD TEMP[1].xyz, TEMP[1], IMM[0].zzzz, -IMM[0].yyyy",
    "RCP TEMP[0].w, TEMP[1].wwww",
    "MUL OUT[0].xyz, TEMP[1], TEMP[0].wwww",
    "MOV OUT[0].w, TEMP[0].wwww"
};

/* line: a, the number m, b, the number n and "]". */
static void move_line(char *line, const char *a, ULONG m, const char *b, ULONG n)
{
    while (*a) *line++ = *a++;
    if (m >= 10) *line++ = (char)('0' + m / 10);
    *line++ = (char)('0' + m % 10);
    while (*b) *line++ = *b++;
    if (n >= 10) *line++ = (char)('0' + n / 10);
    *line++ = (char)('0' + n % 10);
    *line++ = ']';
    *line = 0;
}

static ULONG ps_vertex_text(char *out, BOOLEAN object)
{
    TEXT t;
    ULONG i, base = object ? 15 : 1;
    t.p = out; t.n = 0; t.line = 0; t.size = LitText; t.overflow = FALSE;
    put(&t, "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..");
    number(&t, base + PsVertexConstants * RB2K_D8_VERTICES - 1);
    put(&t, "]\nDCL ADDR[0]\nDCL TEMP[0..5]\nDCL OUT[0], POSITION\n");
    for (i = 0; i < PsOutputs + (object ? 1 : 0); ++i) {
        put(&t, "DCL OUT["); number(&t, i + 1); put(&t, "], GENERIC["); number(&t, i); put(&t, "]\n");
    }
    put(&t, object ? "IMM[0] FLT32 {0x3f800000, 0x40000000, 0x00000000, 0x00000000}\n"
                   : "IMM[0] FLT32 {0x3f000000, 0x3f800000, 0x40000000, 0x00000000}\n");
    put(&t, "IMM[1] UINT32 {");
    number(&t, base);
    put(&t, ", 0, 0, 0}\n");
    if (object) {
        /* object_lines without its addressing (0..3) and outputs (39..); its
         * fog depths (17..21) go to GENERIC[10]. */
        op(&t, ps_transformed_lines[0], 0, 0, 0);
        op(&t, ps_transformed_lines[1], 0, 0, 0);
        for (i = 4; i <= 21; ++i) op(&t, object_lines[i], 0, 0, 0);
        op(&t, "MOV OUT[11], TEMP[5]", 0, 0, 0);
        for (i = 23; i <= 38; ++i) op(&t, object_lines[i], 0, 0, 0);
    } else {
        for (i = 0; i < sizeof(ps_transformed_lines) / sizeof(*ps_transformed_lines); ++i)
            op(&t, ps_transformed_lines[i], 0, 0, 0);
    }
    for (i = 1; i <= PsOutputs; ++i) {
        char line[48];
        move_line(line, "MOV OUT[", i, "], CONST[ADDR[0].x+", i);
        op(&t, line, 0, 0, 0);
    }
    op(&t, "END", 0, 0, 0);
    return t.overflow ? 0 : t.n + 1;
}
typedef char object_lines_layout[sizeof(object_lines) / sizeof(*object_lines) == 43 ? 1 : -1];

/* The lighting shader (lit_lines) for pixel shader draws: per vertex the
 * position, diffuse, specular, set 0, the normal and sets 1 to 7 as far as
 * the draw carries them (at least set 0), CONST[0].z apart; the sets out
 * as GENERIC[2..9] and the fog depths as GENERIC[10]. */
enum { PsLitVertexConstants = 4 + RB2K_D8_STAGES };
static BOOLEAN same(const char *a, const char *b)
{
    while (*a && *a == *b) ++a, ++b;
    return !*a && !*b;
}

static ULONG ps_lit_text(char *out)
{
    TEXT t;
    ULONG i, found = 0;
    t.p = out; t.n = 0; t.line = 0; t.size = LitText; t.overflow = FALSE;
    put(&t, "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..");
    number(&t, LitVertexBase + PsLitVertexConstants * RB2K_D8_VERTICES + LitLightConstants * RB2K_D7_LIGHTS - 1);
    put(&t, "]\nDCL ADDR[0]\nDCL TEMP[0..36]\nDCL OUT[0], POSITION\n");
    for (i = 0; i <= PsOutputs; ++i) {
        put(&t, "DCL OUT["); number(&t, i + 1); put(&t, "], GENERIC["); number(&t, i); put(&t, "]\n");
    }
    put(&t, "IMM[0] FLT32 {0x3f800000, 0x40000000, 0x00000000, 0x3f000000}\n"
            "IMM[1] UINT32 {8, 24, 8, 1}\n"
            "IMM[2] FLT32 {0x00000000, 0x00000000, 0xbf800000, 0x0d800000}\n"
            "IMM[3] UINT32 {0, 0, 0, 0}\n");
    for (i = 0; i < sizeof(lit_lines)/sizeof(*lit_lines); ++i) {
        const char *l = lit_lines[i], *fogOut = "MOV OUT[4], TEMP[14]", *coordIn = "MOV TEMP[26], CONST[ADDR[0].x+3]";
        const char *address = "UMAD TEMP[0].x, SV[0].xxxx, IMM[1].xxxx, IMM[1].yyyy";
        char line[48];
        ULONG k;
        if (same(l, address)) {
            op(&t, "UMAD TEMP[0].x, SV[0].xxxx, CONST[0].zzzz, IMM[1].yyyy", 0, 0, 0);
            found |= 4;
            continue;
        }
        if (same(l, fogOut)) {
            op(&t, "MOV OUT[11], TEMP[14]", 0, 0, 0);
            for (k = 1; k < RB2K_D8_STAGES; ++k) {
                move_line(line, "MOV OUT[", 3 + k, "], TEMP[", 29 + k);
                op(&t, line, 0, 0, 0);
            }
            found |= 1;
            continue;
        }
        op(&t, l, 0, 0, 0);
        if (same(l, coordIn)) {
            for (k = 1; k < RB2K_D8_STAGES; ++k) {
                move_line(line, "MOV TEMP[", 29 + k, "], CONST[ADDR[0].x+", 4 + k);
                op(&t, line, 0, 0, 0);
            }
            found |= 2;
        }
    }
    return t.overflow || found != 7 ? 0 : t.n + 1;
}

static BOOLEAN sampler_valid(ULONG sampler)
{
    ULONG addressU = sampler & 15, addressV = (sampler >> 4) & 15;
    ULONG mag = (sampler >> 8) & 15, min = (sampler >> 12) & 15;
    return !(sampler >> 20) && ((sampler >> 16) & 15) <= 3 && addressU >= 1 && addressU <= 4 &&
           addressV >= 1 && addressV <= 4 && mag >= 1 && mag <= 5 && min >= 1 && min <= 3;
}

BOOLEAN rb_gpu_d8_valid(const RB2K_D3D8_DRAW *d)
{
    ULONG k;
    if (d->depthState && (!(d->depthState & RB2K_Z_ENABLE) ||
        d->depthState > (RB2K_Z_ENABLE | RB2K_Z_WRITE | 7))) return FALSE;
    if (d->alphaTest && ((d->alphaTest & ~(RB2K_D7_ON | 0xfff)) ||
        ((d->alphaTest >> 8) & 15) < 1 || ((d->alphaTest >> 8) & 15) > 8)) return FALSE;
    if (d->blend && ((d->blend & ~(RB2K_D7_ON | 0xffff)) ||
        !blend_valid((d->blend >> 8) & 255) || !blend_valid(d->blend & 255))) return FALSE;
    if (d->cull < CullNone || d->cull > CullCcw || (d->constantCount != 8 && d->constantCount != 32)) return FALSE;
    if (d->transform > 1 || (d->transform && (!d->viewport[2] || !d->viewport[3] ||
        d->viewport[0] > 8192 || d->viewport[1] > 8192 || d->viewport[2] > 8192 || d->viewport[3] > 8192)))
        return FALSE;
    for (k = 0; k < RB2K_D8_STAGES; ++k)
        if (d->textures[k] && !sampler_valid(d->samplers[k])) return FALSE;
    if (d->fog) {
        ULONG kind = d->fog & 3, mode = (d->fog >> RB2K_D7_FOG_MODE_SHIFT) & 3;
        if ((d->fog & ~(3UL | 3UL << RB2K_D7_FOG_MODE_SHIFT | RB2K_D7_FOG_RANGE | RB2K_D7_FOG_W)) || !kind ||
            (kind == RB2K_D7_FOG_ALPHA ? d->fog != kind : !mode) ||
            (kind == RB2K_D7_FOG_VERTEX && !d->transform) ||
            ((d->fog & RB2K_D7_FOG_RANGE) && kind != RB2K_D7_FOG_VERTEX) ||
            ((d->fog & RB2K_D7_FOG_W) && kind != RB2K_D7_FOG_TABLE)) return FALSE;
    }
    if (d->lighting) {
        if (d->lighting != 1 || !d->transform || d->lightCount > RB2K_D7_LIGHTS || d->specular > 1 ||
            (d->lightFlags & ~((0xffUL << RB2K_D7_SOURCE_SHIFT) | RB2K_D7_NORMALIZE | RB2K_D7_LOCAL_VIEWER)))
            return FALSE;
        for (k = 0; k < 4; ++k)
            if (((d->lightFlags >> (RB2K_D7_SOURCE_SHIFT + 2*k)) & 3) > 2) return FALSE;
        for (k = 0; k < d->lightCount; ++k)
            if (d->lights[k][0] < 1 || d->lights[k][0] > 3) return FALSE;
    }
    return TRUE;
}

/* program: the TGSI text of the pixel shader; textures: the texture of each
 * stage it samples, NULL for the others. */
BOOLEAN rb_gpu_d8_draw(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image, const RB2K_D3D8_DRAW *d,
                       const char *program, ULONG programBytes, RB2K_TEXTURE *const *textures)
{
    RB2K_D7_STATE *s = &image->d7;
    ULONG words[192], n = 0, slot, key[6], state, i, k, h[RB2K_D8_STAGES], views[RB2K_D8_STAGES];
    ULONG lit = d->lighting != 0, lights = lit ? d->lightCount : 0;
    ULONG stages = 0, fill = 0, count = d->count, extra = (d->transform ? 56 : 0) + (lit ? 36 + 32 * lights : 0);
    ULONG sets = d->coordSets, words8 = RB2K_D8_VERTEX_WORDS(sets);
    ULONG stride = lit ? 4 + (sets ? sets : 1) : 3 + sets, perVertex = 4 * stride;
    ULONG total = 28 + (d->transform ? 4 : 0) + extra + perVertex * count, *p;
    ULONG width = dimension_bits(image->width), height = dimension_bits(image->height);
    gpu->d8Step = 1;
    if (!rb_gpu_prepare_target(gpu, r, image)) return FALSE;
    if (!s->pipeline) {
        if (!rb_gpu_frame_words(gpu,r,image,pipeline,sizeof(pipeline)/sizeof(*pipeline))) return FALSE;
        s->pipeline = TRUE;
    }
    gpu->d8Step = 2;
    {
        static char transformedText[LitText], objectText[LitText], litText[LitText];
        static ULONG transformedSize, objectSize, litSize;
        ULONG want = d->lighting ? 32 : d->transform ? 16 : 8;
        ULONG handle = d->lighting ? PsLitShader : d->transform ? PsObjectShader : PsVertexShader;
        if (!(s->vertexShaders & want)) {
            ULONG *size = d->lighting ? &litSize : d->transform ? &objectSize : &transformedSize;
            char *text = d->lighting ? litText : d->transform ? objectText : transformedText;
            if (!*size) *size = d->lighting ? ps_lit_text(text) : ps_vertex_text(text, d->transform != 0);
            if (!*size) { r->status = ERROR_INVALID_DATA; r->stage = RB2K_STAGE_TRANSFER; return FALSE; }
            if (!shader(gpu, r, image, handle, VertexStage, text, *size)) return FALSE;
            s->vertexShaders |= want;
        }
        if (s->boundVertex != want) {
            ULONG bind[] = { VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER,0,VIRGL_BIND_SHADER_SIZE), handle, VertexStage };
            if (!rb_gpu_frame_words(gpu,r,image,bind,3)) return FALSE;
            s->boundVertex = want;
        }
    }
    gpu->d8Step = 3;
    if (!fixed_states(gpu, r, image, d->depthState, d->alphaTest, d->blend, d->cull, d->transform)) return FALSE;
    /* The program shares the fragment shader table; its key cannot be a
     * fixed-function one, whose colour stage word is never all ones. */
    gpu->d8Step = 4;
    key[0] = 0xffffffff; key[1] = d->program; key[2] = key[3] = key[4] = key[5] = 0;
    slot = cache_slot(&s->shader, RB2K_D7_SHADER_KEYS, key, 6, &state);
    if (state != Hit) {
        if (state == Replaced) {
            words[0] = DESTROY(VIRGL_OBJECT_SHADER); words[1] = ShaderBase + slot;
            if (!rb_gpu_frame_words(gpu, r, image, words, 2)) return FALSE;
        }
        if (!shader(gpu, r, image, ShaderBase + slot, FragmentStage, program, programBytes)) return FALSE;
    }
    if (s->shader.bound != slot + 1) {
        words[n++] = VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER,0,VIRGL_BIND_SHADER_SIZE);
        words[n++] = ShaderBase + slot; words[n++] = FragmentStage;
        s->shader.bound = slot + 1;
    }
    gpu->d8Step = 5;
    /* The program's constants, then for fog its colour and start, end, density. */
    words[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_CONSTANT_BUFFER,0,2 + 4 * d->constantCount + (d->fog ? 8 : 0));
    words[n++] = FragmentStage; words[n++] = 0;
    for (i = 0; i < d->constantCount; ++i) for (k = 0; k < 4; ++k) words[n++] = d->constants[i][k];
    if (d->fog) {
        words[n++] = rb_gpu_channel((d->fogColor >> 16) & 255);
        words[n++] = rb_gpu_channel((d->fogColor >> 8) & 255);
        words[n++] = rb_gpu_channel(d->fogColor & 255);
        words[n++] = 0x3f800000;
        words[n++] = d->fogStart; words[n++] = d->fogEnd; words[n++] = d->fogDensity; words[n++] = 0;
    }
    if (!rb_gpu_frame_words(gpu, r, image, words, n)) return FALSE;
    n = 0;
    /* Each sampled stage gets its sampler and view in its own slot; slots
     * below the last sampled one that the program does not sample repeat a
     * sampled stage's objects. */
    gpu->d8Step = 6;
    for (k = 0; k < RB2K_D8_STAGES; ++k) {
        if (!textures[k]) continue;
        stages = k + 1;
        fill = k;
        key[0] = d->samplers[k]; key[1] = d->borders[k];
        slot = cache_slot(&s->psSampler, RB2K_D8_SAMPLER_KEYS, key, 2, &state);
        if (state != Hit) n += sampler_words(words + n, PsSamplerBase + slot, state, d->samplers[k], d->borders[k]);
        h[k] = PsSamplerBase + slot;
        views[k] = PsViewBase + k;
        if (s->psViews[k] != textures[k]->id) {
            if (s->psViews[k]) { words[n++] = DESTROY(VIRGL_OBJECT_SAMPLER_VIEW); words[n++] = PsViewBase + k; }
            words[n++] = CREATE(VIRGL_OBJECT_SAMPLER_VIEW, VIRGL_OBJ_SAMPLER_VIEW_SIZE);
            words[n++] = PsViewBase + k; words[n++] = textures[k]->id; words[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
            words[n++] = 0; words[n++] = (textures[k]->levels - 1) << 8;   /* all mipmap levels */
            words[n++] = 0x688; /* identity swizzle */
            s->psViews[k] = textures[k]->id;
        }
        /* Nothing to send when the sampler and view are already current. */
        if (n && !rb_gpu_frame_words(gpu, r, image, words, n)) return FALSE;
        n = 0;
    }
    gpu->d8Step = 7;
    if (stages) {
        for (k = 0; k < stages; ++k) if (!textures[k]) { h[k] = h[fill]; views[k] = views[fill]; }
        words[n++] = VIRGL_CMD0(VIRGL_CCMD_BIND_SAMPLER_STATES,0,VIRGL_BIND_SAMPLER_STATES(stages));
        words[n++] = FragmentStage; words[n++] = 0;
        for (k = 0; k < stages; ++k) words[n++] = h[k];
        words[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_SAMPLER_VIEWS,0,VIRGL_SET_SAMPLER_VIEWS_SIZE(stages));
        words[n++] = FragmentStage; words[n++] = 0;
        for (k = 0; k < stages; ++k) words[n++] = views[k];
        if (!rb_gpu_frame_words(gpu, r, image, words, n)) return FALSE;
        n = 0;
        /* The fixed-function draws bind their own slots again next time. */
        s->boundSamplers = s->boundViews = 0;
    }
    gpu->d8Step = 8;
    p = rb_gpu_frame_reserve(gpu, r, image, total);
    if (!p) return FALSE;
    *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE,0,VIRGL_SET_VIEWPORT_STATE_SIZE(1));
    *p++ = 0;
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
    *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_CONSTANT_BUFFER,0,6+extra+perVertex*count);
    *p++ = VertexStage; *p++ = 0;
    *p++ = width; *p++ = height; *p++ = stride; *p++ = 0;
    if (d->transform) {
        ULONG m, row, col;
        for (m = 0; m < 3; ++m)
            for (col = 0; col < 4; ++col)
                for (row = 0; row < 4; ++row) *p++ = d->matrices[m][row * 4 + col];
        for (i = 0; i < 4; ++i) *p++ = integer_bits(d->viewport[i]);
        *p++ = d->viewport[4]; *p++ = d->viewport[5]; *p++ = 0; *p++ = 0;
    }
    if (lit) {
        /* Constants 15..23 of lit_lines, as in rb_gpu_d7_draw. */
        const ULONG one = 0x3f800000, sources = d->lightFlags >> RB2K_D7_SOURCE_SHIFT;
        *p++ = rb_gpu_channel((d->ambient >> 16) & 255);
        *p++ = rb_gpu_channel((d->ambient >> 8) & 255);
        *p++ = rb_gpu_channel(d->ambient & 255);
        *p++ = rb_gpu_channel(d->ambient >> 24);
        for (k = 0; k < 16; ++k) *p++ = d->material[k];
        *p++ = d->material[16];
        *p++ = d->lightFlags & RB2K_D7_NORMALIZE ? one : 0;
        *p++ = d->lightFlags & RB2K_D7_LOCAL_VIEWER ? one : 0;
        *p++ = d->specular ? one : 0;
        for (k = 0; k < 4; ++k) {
            *p++ = ((sources >> 2*k) & 3) == 1 ? one : 0;
            *p++ = ((sources >> 2*k) & 3) == 2 ? one : 0;
        }
        *p++ = lights; *p++ = LitVertexBase + stride * count; *p++ = 0; *p++ = 0;
    }
    for (i = 0; i < count; ++i) {
        const RB2K_D3D8_VERTEX *v = (const RB2K_D3D8_VERTEX *)(d->vertices + i * words8);
        *p++ = v->x; *p++ = v->y; *p++ = v->z; *p++ = v->rhw;
        *p++ = rb_gpu_channel((v->diffuse >> 16) & 255);
        *p++ = rb_gpu_channel((v->diffuse >> 8) & 255);
        *p++ = rb_gpu_channel(v->diffuse & 255);
        *p++ = rb_gpu_channel(v->diffuse >> 24);
        *p++ = rb_gpu_channel((v->specular >> 16) & 255);
        *p++ = rb_gpu_channel((v->specular >> 8) & 255);
        *p++ = rb_gpu_channel(v->specular & 255);
        *p++ = rb_gpu_channel(v->specular >> 24);
        if (lit) {
            /* set 0, normal, sets 1.. */
            if (sets) { *p++ = v->tc[0][0]; *p++ = v->tc[0][1]; *p++ = v->tc[0][2]; *p++ = v->tc[0][3]; }
            else { *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0x3f800000; }
            *p++ = v->nx; *p++ = v->ny; *p++ = v->nz; *p++ = 0;
            for (k = 1; k < sets; ++k) {
                *p++ = v->tc[k][0]; *p++ = v->tc[k][1]; *p++ = v->tc[k][2]; *p++ = v->tc[k][3];
            }
        } else for (k = 0; k < sets; ++k) {
            *p++ = v->tc[k][0]; *p++ = v->tc[k][1]; *p++ = v->tc[k][2]; *p++ = v->tc[k][3];
        }
    }
    for (i = 0; i < lights; ++i) {
        /* As in rb_gpu_d7_draw. */
        const ULONG *l = d->lights[i];
        for (k = 1; k <= 12; ++k) *p++ = l[k];
        *p++ = l[13]; *p++ = l[14]; *p++ = l[15]; *p++ = 0x3f800000;
        *p++ = l[16]; *p++ = l[17]; *p++ = l[18]; *p++ = 0;
        *p++ = l[19]; *p++ = l[21]; *p++ = l[22]; *p++ = l[23];
        *p++ = l[24]; *p++ = l[25]; *p++ = l[20]; *p++ = 0;
        *p++ = l[0] == 3 ? 0x3f800000 : 0; *p++ = l[0] == 2 ? 0x3f800000 : 0; *p++ = 0; *p++ = 0;
    }
    *p++ = VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO,0,VIRGL_DRAW_VBO_SIZE);
    *p++ = 0; *p++ = count; *p++ = PipeTriangles; *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 0;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = count - 1; *p++ = 0;
    image->uniform = FALSE;
    return TRUE;
}
