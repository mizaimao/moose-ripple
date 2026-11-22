/* The first NT5 fixed-function path takes untextured transformed vertices.
 * Positions are IEEE words in the guest. The vertex shader performs the 9x
 * screen-to-clip transform on the host. Colours interpolate in screen space.
 * A batch fits a control packet and needs no separate guest vertex resource. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include <virgl_hw.h>
#include <virgl_protocol.h>

enum { Blend = 2, Depth, Raster, Elements, VertexShader, FragmentShader };
enum { VertexStage = 0, FragmentStage = 1, PipeTriangles = 4 };

static const char vertex_shader[] =
    "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..48]\nDCL ADDR[0]\n"
    "DCL TEMP[0..2]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
    "IMM[0] FLT32 {0x3f000000, 0x3f800000, 0x40000000, 0x00000000}\n"
    "0: UADD TEMP[0].x, SV[0].xxxx, SV[0].xxxx\n"
    "1: UARL ADDR[0].x, TEMP[0].xxxx\n"
    "2: MOV TEMP[1], CONST[ADDR[0].x+1]\n"
    "3: RCP TEMP[2].x, CONST[0].xxxx\n"
    "4: RCP TEMP[2].y, CONST[0].yyyy\n"
    "5: ADD TEMP[1].xy, TEMP[1], IMM[0].xxxx\n"
    "6: MUL TEMP[1].xy, TEMP[1], TEMP[2]\n"
    "7: MAD TEMP[1].xyz, TEMP[1], IMM[0].zzzz, -IMM[0].yyyy\n"
    "8: RCP TEMP[0].w, TEMP[1].wwww\n"
    "9: MUL OUT[0].xyz, TEMP[1], TEMP[0].wwww\n"
    "10: MOV OUT[0].w, TEMP[0].wwww\n"
    "11: MOV OUT[1], CONST[ADDR[0].x+2]\n12: END\n";
static const char fragment_shader[] =
    "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL OUT[0], COLOR\n"
    "0: MOV OUT[0], IN[0]\n1: END\n";

#define CREATE(kind,size) VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT,kind,size)
#define BIND(kind) VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT,kind,1)
static const ULONG pipeline[] = {
    CREATE(VIRGL_OBJECT_BLEND,VIRGL_OBJ_BLEND_SIZE), Blend,0,0,
        VIRGL_OBJ_BLEND_S2_RT_COLORMASK(15),0,0,0,0,0,0,0,
    CREATE(VIRGL_OBJECT_DSA,VIRGL_OBJ_DSA_SIZE),Depth,0,0,0,0,
    CREATE(VIRGL_OBJECT_RASTERIZER, VIRGL_OBJ_RS_SIZE), Raster,
        VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) | VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) |
        VIRGL_OBJ_RS_S0_BOTTOM_EDGE_RULE(1), 0x3f800000, 0, 0, 0x3f800000, 0, 0, 0,
    CREATE(VIRGL_OBJECT_VERTEX_ELEMENTS,VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(0)),Elements,
    BIND(VIRGL_OBJECT_BLEND),Blend, BIND(VIRGL_OBJECT_DSA),Depth,
    BIND(VIRGL_OBJECT_RASTERIZER),Raster, BIND(VIRGL_OBJECT_VERTEX_ELEMENTS),Elements,
    VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER,0,VIRGL_BIND_SHADER_SIZE),VertexShader,VertexStage,
    VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER,0,VIRGL_BIND_SHADER_SIZE),FragmentShader,FragmentStage
};

static BOOLEAN shader(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image, ULONG handle,
                      ULONG stage, const char *text, ULONG size)
{
    ULONG words[6+(sizeof(vertex_shader)+3)/4], n = (size+3)/4;
    if (size > sizeof(vertex_shader)) return FALSE;
    words[0] = CREATE(VIRGL_OBJECT_SHADER,5+n);
    words[1] = handle;
    words[2] = stage;
    words[3] = size;
    words[4] = 300; /* token storage, as in the 9x palette shader emitter */
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

static BOOLEAN prepare(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image, ULONG state)
{
    ULONG words[8], count = 0, object = Depth, bit = 0;
    if (!rb_gpu_prepare_target(gpu, r, image)) return FALSE;
    if (!image->triangleState) {
        if (!shader(gpu,r,image,VertexShader,VertexStage,vertex_shader,sizeof(vertex_shader)) ||
            !shader(gpu,r,image,FragmentShader,FragmentStage,fragment_shader,sizeof(fragment_shader)) ||
            !rb_gpu_frame_words(gpu,r,image,pipeline,sizeof(pipeline)/sizeof(*pipeline))) return FALSE;
        image->triangleState = TRUE;
    }
    if (image->boundDepthState == state) return TRUE;
    if (state & RB2K_Z_ENABLE) {
        ULONG index = state & (RB2K_Z_WRITE | 7);
        /* Each colour context owns at most sixteen enabled DSA objects. */
        object = 16 + index;
        bit = 1UL << index;
        if (!(image->depthStates & bit)) {
            words[count++] = CREATE(VIRGL_OBJECT_DSA, VIRGL_OBJ_DSA_SIZE);
            words[count++] = object;
            words[count++] = VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(1) |
                VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(!!(state & RB2K_Z_WRITE)) |
                VIRGL_OBJ_DSA_S0_DEPTH_FUNC(index & 7);
            words[count++] = 0;
            words[count++] = 0;
            words[count++] = 0;
        }
    }
    words[count++] = BIND(VIRGL_OBJECT_DSA);
    words[count++] = object;
    if (!rb_gpu_frame_words(gpu, r, image, words, count)) return FALSE;
    image->depthStates |= bit;
    image->boundDepthState = state;
    return TRUE;
}

BOOLEAN rb_gpu_triangles(RB2K_GPU *gpu, RB2K_GPU_RESULT *r, RB2K_IMAGE *image,
                         const RB2K_D3D7_VERTEX *vertices, ULONG count, ULONG depthState)
{
    ULONG words[8 + 7 + 8*RB2K_D3D7_VERTICES + 13], *p = words, i;
    ULONG width = dimension_bits(image->width), height = dimension_bits(image->height);
    if (!prepare(gpu,r,image,depthState)) return FALSE;
    *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE,0,VIRGL_SET_VIEWPORT_STATE_SIZE(1));
    *p++ = 0;
    /* The viewport scale and translation each use half the dimensions. */
    *p++ = width - (1UL << 23);
    *p++ = height - (1UL << 23);
    *p++ = 0x3f000000;
    *p++ = width - (1UL << 23);
    *p++ = height - (1UL << 23);
    *p++ = 0x3f000000;
    *p++ = VIRGL_CMD0(VIRGL_CCMD_SET_CONSTANT_BUFFER,0,6+8*count);
    *p++ = VertexStage; *p++ = 0;
    /* Constant zero supplies the shader with its viewport dimensions. */
    *p++ = width; *p++ = height; *p++ = 0; *p++ = 0;
    for (i = 0; i < count; ++i) {
        const RB2K_D3D7_VERTEX *v = vertices + i;
        *p++ = v->x;
        *p++ = v->y;
        *p++ = v->z;
        *p++ = v->rhw;
        *p++ = rb_gpu_channel((v->color >> 16) & 255);
        *p++ = rb_gpu_channel((v->color >> 8) & 255);
        *p++ = rb_gpu_channel(v->color & 255);
        *p++ = rb_gpu_channel(v->color >> 24);
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
