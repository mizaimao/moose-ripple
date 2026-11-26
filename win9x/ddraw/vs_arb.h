/*
 * Direct3D vertex shaders (vs_1_1, and vs_2_0 without flow control) with their Direct3D 8
 * vertex declaration, as an ARB vertex program. MIT licence, see LICENSE.
 *
 * The program reads the declared registers v0..v15 from generic attributes (vertex.attrib[n]),
 * c0..c255 from program.env, and from program.local[0] the target's half pixel (x: 1/width,
 * y: 1/height) and [1] the table fog's end / (end - start), 1 / (end - start), density.
 * oPos becomes OpenGL's clip position as the fixed-function programs make it (tnl_arb.c).
 */
#ifndef VS_ARB_H
#define VS_ARB_H

#define VS_ARB_INPUTS 16
#define VS_ARB_CONSTANTS 256
#define VS_ARB_DEFS 64
#define VS_ARB_TEXT_BYTES 32768

/* the declaration's types (D3DVSDT_*) */
#define VS_ARB_FLOAT1 0
#define VS_ARB_FLOAT4 3
#define VS_ARB_COLOR  4                     /* D3DCOLOR: BGRA bytes, 0..1 */
#define VS_ARB_UBYTE4 5
#define VS_ARB_SHORT2 6
#define VS_ARB_SHORT4 7

/* key: what the program does besides the shader */
#define VS_ARB_FOG        1                 /* fog: the factor out (oFog's, or table fog's) */
#define VS_ARB_FOG_COORD  2                 /* into OpenGL's fog coordinate, else the fog unit's
                                               texture coordinates (unit in bits 8-11) */
#define VS_ARB_FOG_SPEC   4                 /* the specular colour times the factor */
#define VS_ARB_TABLE_SHIFT 4                /* bits 4-5: table fog (D3DFOG_EXP 1, EXP2 2,
                                               LINEAR 3) from the depth instead of oFog */
#define VS_ARB_UNIT_SHIFT 8

typedef struct {
    unsigned int inputs;                    /* bit n: vn comes from the stream */
    unsigned char type[VS_ARB_INPUTS];      /* its declaration type */
    unsigned char offset[VS_ARB_INPUTS];    /* its bytes into the vertex */
    unsigned int stride;                    /* the bytes the declaration lays out */
    unsigned int defs;                      /* def constants, loaded over c# when it draws */
    struct { unsigned short reg; float v[4]; } def[VS_ARB_DEFS];
    unsigned int reason, detail;            /* why it was not translated (VS_ARB_*) */
} VS_ARB;

enum { VS_ARB_OK, VS_ARB_VERSION, VS_ARB_OPCODE, VS_ARB_REGISTER, VS_ARB_DECLARATION,
       VS_ARB_TRUNCATED, VS_ARB_OVERFLOW };

/* The program text into out (size bytes) and the layout into info; the length with the final
   0, or 0 if the shader can't be translated (info->reason says why). */
unsigned int vs_to_arb(const unsigned int *code, unsigned int codeWords, const unsigned int *decl,
                       unsigned int declWords, unsigned int key, char *out, unsigned int size,
                       VS_ARB *info);

#endif
