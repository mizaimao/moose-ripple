/* Direct3D 9 vertex declarations and vertex shader functions said in Direct3D 8's terms, for
 * drivers that draw through their Direct3D 8 vertex shader path: shared by the Windows 2000/XP
 * driver and the Windows 9x driver. Self-contained C89 without a run-time library and without
 * floating point. MIT licence.
 *
 * A DirectX 9 runtime on the Direct3D 9 driver interface creates vertex declarations
 * (D3DVERTEXELEMENT9) and shader functions separately and makes one of each current. Direct3D 8
 * knew one object: declaration tokens that give each piece of vertex data an input register,
 * and the code. */
#ifndef RB_D3D9_DECL_H
#define RB_D3D9_DECL_H

/* D3DVERTEXELEMENT9; the list ends with an element whose stream is 0xff, which the count
 * includes. */
typedef struct {
    unsigned short stream, offset;
    unsigned char type, method, usage, usageIndex;
} RB_D9_ELEMENT;

/* The Direct3D 8 declaration and code for a declaration and a shader function, into out:
 * declaration tokens (each element at the input register the function's dcl statement gives
 * its usage; skips over gaps and over elements the function does not read), then the code
 * without its input dcl statements. streams: the streams the driver reads. Returns 1 with the
 * words of each, or 0 when the pair has something Direct3D 8 cannot say (a vertex type beyond
 * SHORT4, a tessellation method, an offset that is not a multiple of four, a function newer
 * than vs_2_0) or out is too small; room of 4 x count + 2 x streams + words + 2 is enough. */
int rb_d9_translate(const RB_D9_ELEMENT *elements, unsigned long count,
                    const unsigned long *tokens, unsigned long words, unsigned long streams,
                    unsigned long *out, unsigned long room,
                    unsigned long *declWords, unsigned long *codeWords);

/* A declaration that is a plain vertex format: stream 0 only, in the order and sizes of an
 * FVF code (position or transformed position, normal, point size, diffuse, specular, the
 * separate fog value of a transformed vertex as D3DFVF_FOG, texture coordinate sets). Returns the
 * FVF code, or 0 when it is not one. */
unsigned long rb_d9_decl_fvf(const RB_D9_ELEMENT *elements, unsigned long count);

/* A declaration for the fixed-function pipeline as Direct3D 8 declaration tokens without code:
 * each element at the register Direct3D 8 gives its usage (D3DVSDE_*: position 0, blend weight
 * 1, blend indices 2, normal 3, point size 4, diffuse 5, specular 6, texture coordinate sets 7
 * to 14), from any stream; other usages are skipped. Returns the words written, or 0 when the
 * declaration has no untransformed position or something Direct3D 8 cannot say. */
unsigned long rb_d9_decl_fixed(const RB_D9_ELEMENT *elements, unsigned long count,
                               unsigned long streams, unsigned long *out, unsigned long room);

#endif
