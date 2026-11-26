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
 * stage n is sampler n (s0..s7 for shader model 2). */
#define RB_PS_COORD_SETS 4
#define RB_PS2_COORD_SETS 8
#define RB_PS2_SAMPLERS 8
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
} RB_PS_INFO;

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
 * samples with sampler n. */
typedef struct {
    unsigned int colorOp, colorArg1, colorArg2, alphaOp, alphaArg1, alphaArg2, coordIndex;
} RB_STAGE_STATE;
unsigned int rb_stage_translate(const RB_STAGE_STATE *stages, unsigned int count, unsigned int specular,
                                unsigned int fog, char *out, unsigned int size, RB_PS_INFO *info);

#endif
