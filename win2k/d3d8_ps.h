/* Translation of Direct3D 8 pixel shaders (ps_1_1 to ps_1_3) to Gallium
 * TGSI text for the host. Self-contained C without a runtime library, so
 * the display driver and a native test program share it. */
#ifndef RB2K_D3D8_PS_H
#define RB2K_D3D8_PS_H

/* Version 1 shaders use c0..c7, shader model 2 c0..c31; the fog constants
 * follow them (CONST[8] and [9], or [32] and [33]).
 *
 * Fragment shader inputs, matching the vertex shader of shader draws:
 * GENERIC[0] the diffuse and GENERIC[1] the specular colour, GENERIC[2+n]
 * texture coordinate set n (t0..t3 for version 1, t0..t7 for shader model
 * 2), GENERIC[10] the fog depths. Constants c0..c7 are CONST[0..7]; texture
 * stage n is sampler n. A shader model 2 shader may read RB_PS2_SAMPLERS samplers: the driver
 * that builds this file says how many it binds (16 in the Windows 2000 and XP driver), eight
 * otherwise. The stage programs of the fixed-function pipeline have eight stages. */
#define RB_PS_COORD_SETS 4
#define RB_PS2_COORD_SETS 8
#ifndef RB_PS2_SAMPLERS
#define RB_PS2_SAMPLERS 8
#endif
#define RB_PS_CONSTANTS 8
/* A version 1 program with texbem or texbeml: c0..c7, then each stage n's
 * bump matrix (00, 01, 10, 11) in CONST[8+n] and its luminance scale and
 * offset in CONST[12+n]; the fog constants follow at CONST[16] and [17]. */
#define RB_PS_BUMP_CONSTANTS 16
#define RB_PS2_CONSTANTS 32     /* c0..c31 of shader model 2 */
#define RB_PS_TEXT_BYTES 8192

/* Why a shader was not translated, for diagnostics. */
enum {
    RbPsOk, RbPsVersion, RbPsTruncated, RbPsOpcode, RbPsRegister, RbPsModifier,
    RbPsOverflow, RbPsReasons
};

typedef struct {
    unsigned int samplers;      /* texture stages the shader samples */
    unsigned int coords;        /* texture coordinate sets it reads */
    unsigned int reason;        /* RbPs* */
    unsigned int detail;        /* the opcode or token behind the reason */
    unsigned int constants;     /* vertex shaders: c0..c(n-1) are read (all for relative reads) */
    unsigned int pointSize;     /* vertex shaders: oPts is written (a PSIZE output) */
    /* Vertex shaders with RB_VS_ATTRIBUTES: input IN[k] of the program is element k, packed as
     * RB_VS_ELEMENT(type, stream, offset): the declaration type (0..7: FLOAT1..FLOAT4, D3DCOLOR,
     * UBYTE4, SHORT2, SHORT4), the stream, and the byte offset in that stream's vertex. */
    unsigned int elementCount;
    unsigned int elements[16];
} RB_PS_INFO;
#define RB_VS_ELEMENTS 16
#define RB_VS_ELEMENT(type, stream, offset) ((type) | (stream) << 8 | (offset) << 16)
#define RB_VS_ELEMENT_TYPE(e) ((e) & 0xff)
#define RB_VS_ELEMENT_STREAM(e) (((e) >> 8) & 0xff)
#define RB_VS_ELEMENT_OFFSET(e) ((e) >> 16)

/* Fog applied to the shader's result, encoded as RB2K_D3D7_DRAW.fog: the
 * factor from the specular alpha, from the vertex depths in GENERIC[10]
 * (camera z, distance), or from the pixel depth (z, or w with the W flag);
 * then the D3DFOGMODE, and the range and W flags. CONST[8] holds the fog
 * colour and CONST[9] the start, end and density (CONST[32] and [33] for
 * shader model 2). Zero applies none. */
#define RB_PS_FOG_ALPHA 1
#define RB_PS_FOG_VERTEX 2
#define RB_PS_FOG_TABLE 3
#define RB_PS_FOG_MODE_SHIFT 4
#define RB_PS_FOG_RANGE 0x100
#define RB_PS_FOG_W 0x200
/* Version 1 shaders: the stages whose texture is a cube map, in bits 16-23
 * of the fog argument (shader model 2 declares its samplers). */
#define RB_PS_CUBE_SHIFT 16
/* and the stages whose texture is a volume, in bits 24-31. */
#define RB_PS_VOLUME_SHIFT 24

/* tokens: the shader's token stream, count words long. Returns the length
 * of the text written to out including the terminating zero, or 0. */
unsigned int rb_ps_translate(const unsigned int *tokens, unsigned int count, unsigned int fog, char *out,
                             unsigned int size, RB_PS_INFO *info);

/* Vertex shaders (vs_1_1, vs_2_0) with their Direct3D 8 declaration, as a
 * TGSI vertex shader for the layout in d3d8_ps.c: constants
 * c0..c255 at CONST[RB_VS_CONST_BASE..], then up to RB_VS_VERTICES vertices
 * of at most RB_VS_VERTEX_CONSTANTS constants each. RB_VS_FOG_ALPHA puts
 * oFog into the specular alpha, where fog from the specular alpha finds it. */
#define RB_VS_CONST_BASE 19
#define RB_VS_CONSTANTS 256
#define RB_VS_VERTICES 48
#define RB_VS_VERTEX_CONSTANTS 16
#define RB_VS_FOG_ALPHA 1
/* RB_VS_FLOW: the integer and boolean constants i0..i15 and b0..b15 are in CONST[3..18] (i# as
 * floats in x, y, z: count, start, step; b# as 1 or 0 in w), so vs_2_0 static flow control (rep, loop,
 * if, call, callnz) runs; without it only defi and defb values can drive it. */
#define RB_VS_FLOW 2
/* RB_VS_CLIP: the program also writes clip distances (OUT[12] and OUT[13], CLIPDIST[0] and [1])
 * for six user clip planes in clip space, CONST[RB_VS_CONST_BASE + constants + 0..5] (constants as
 * in info->constants), which the rasterizer enables by D3DRS_CLIPPLANEENABLE. */
#define RB_VS_CLIP 4
/* RB_VS_ATTRIBUTES: the inputs are vertex attributes read from vertex buffers on the host
 * instead of words after the constants: IN[k] for the k-th register the declaration feeds
 * (info->elements, in the declaration's order). No vertices follow the constants, CONST[0].zw
 * is not used, and streamBases is not read. A D3DCOLOR element is to be given to the host as
 * four normalized bytes in memory order (b, g, r, a); the program puts them in order. */
#define RB_VS_ATTRIBUTES 8
/* The IEEE single bits of a whole number (exact below 2^24 in magnitude). */
unsigned int rb_float_bits(int v);
/* A vertex holds the data of each stream the declaration reads, one after the
 * other: streamBases[n] is the word at which stream n starts (NULL: stream 0
 * only, at word 0). */
#define RB_VS_STREAMS 8
unsigned int rb_vs_translate(const unsigned int *tokens, unsigned int count, const unsigned int *decl,
                             unsigned int declCount, const unsigned int *streamBases, unsigned int flags, char *out,
                             unsigned int size, RB_PS_INFO *info);

/* Fixed-function texture stages (D3DTSS colour and alpha operations and
 * arguments, the coordinate index) as a TGSI pixel program with the inputs of
 * translated pixel shaders, for draws with a vertex shader. CONST[0].x holds
 * the texture factor as its ARGB word; fog uses CONST[8] and [9]. Stage n
 * samples with sampler n.
 *
 * transform is D3DTSS_TEXTURETRANSFORMFLAGS: the count of coordinates the
 * texture matrix gives (bits 0-7, 0 for none) and D3DTTFF_PROJECTED (256),
 * which divides them by the last one. The matrix of stage n (n below
 * RB_STAGE_TRANSFORMS) applies per pixel, its columns in CONST[8+4n] to
 * CONST[11+4n]; such a program reads 32 constants and its fog constants
 * follow them (CONST[32] and [33]). A BUMPENVMAP or BUMPENVMAPLUMINANCE stage
 * n (below RB_STAGE_TRANSFORMS, with no texture transform) has its bump matrix
 * (00, 01, 10, 11) in CONST[8+4n] and its luminance scale and offset in
 * CONST[9+4n].x and .y; it moves the next stage's lookup. */
#define RB_STAGE_TRANSFORMS 6
#define RB_TTFF_PROJECTED 256
typedef struct {
    unsigned int colorOp, colorArg1, colorArg2, alphaOp, alphaArg1, alphaArg2, coordIndex, transform;
} RB_STAGE_STATE;
unsigned int rb_stage_translate(const RB_STAGE_STATE *stages, unsigned int count, unsigned int specular,
                                unsigned int fog, char *out, unsigned int size, RB_PS_INFO *info);

#endif
