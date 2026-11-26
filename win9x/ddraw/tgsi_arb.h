/* Pixel shaders for the Windows 9x Direct3D HAL: the TGSI text of win2k/d3d8_ps.c turned into
   an OpenGL ARB_fragment_program, which Mesa compiles for the host. MIT licence. */
#ifndef TGSI_ARB_H
#define TGSI_ARB_H

#define TGSI_ARB_IMMEDIATES 32
#define TGSI_ARB_TEXT_BYTES 12288

/* Where the inputs come from. The translator's GENERIC[0] and [1] are the diffuse and specular
   colours, GENERIC[2+n] texture coordinate set n, GENERIC[10] the fog depths. */
typedef struct {
    unsigned int immediates;                    /* IMM[0..n-1], loaded as program.local[0..n-1] */
    unsigned int imm[TGSI_ARB_IMMEDIATES][4];   /* their bits (IEEE single) */
    unsigned int constants;                     /* CONST[0..n-1], read from program.env[0..n-1] */
    unsigned int line;                          /* where a failure was, counted from 1 */
    unsigned int fog_unit;                      /* in: the texture coordinate set that carries
                                                   GENERIC[10], the fog depth */
} TGSI_ARB;

/* 0 if a line could not be converted (info->line says which), else the length of the text
   written to out, its terminating zero included. */
unsigned int tgsi_to_arb(const char *tgsi, char *out, unsigned int size, TGSI_ARB *info);

#endif
