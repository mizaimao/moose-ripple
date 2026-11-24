/* Translation of Direct3D 8 pixel shaders (ps_1_1 to ps_1_3) to Gallium
 * TGSI text for the host. Self-contained C without a runtime library, so
 * the display driver and a native test program share it. */
#ifndef RB2K_D3D8_PS_H
#define RB2K_D3D8_PS_H

/* Fragment shader inputs, matching the vertex shader of shader draws:
 * GENERIC[0] the diffuse and GENERIC[1] the specular colour, GENERIC[2+n]
 * texture coordinate set n. Constants c0..c7 are CONST[0..7]; texture stage
 * n is sampler n. */
#define RB_PS_COORD_SETS 4
#define RB_PS_CONSTANTS 8
#define RB_PS_TEXT_BYTES 4096

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
} RB_PS_INFO;

/* tokens: the shader's token stream, count words long. Returns the length
 * of the text written to out including the terminating zero, or 0. */
unsigned int rb_ps_translate(const unsigned int *tokens, unsigned int count, char *out,
                             unsigned int size, RB_PS_INFO *info);

#endif
