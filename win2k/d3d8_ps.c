/* Direct3D pixel shaders 1.0 to 1.3 and 2.0 as Gallium TGSI text.
 *
 * Registers: r0 and r1 are TEMP[0] and TEMP[1], texture registers t0..t3
 * TEMP[8..11], v0 and v1 IN[0] and IN[1], texture coordinate set n IN[2+n],
 * c0..c7 CONST[0..7] unless a def gives the value, which becomes an IMM.
 * TEMP[12..14] hold modified sources, TEMP[15] a comparison, TEMP[16] and
 * TEMP[17] the results of an instruction and of the one co-issued with it,
 * and TEMP[18] the dot products of the texm3x2 and texm3x3 sequences (with
 * TEMP[13] the eye vector and TEMP[15] scratch of a texm3x3 reflection).
 *
 * Each arithmetic result is shifted, then saturated or clamped to [-1, 1]
 * (the device reports a MaxPixelShaderValue of 1), then written through the
 * write mask. A co-issued pair reads its sources before either writes.
 * Texture coordinates read by texcoord are saturated, as in Direct3D. */
#include "d3d8_ps.h"

enum {
    OpNop = 0, OpMov = 1, OpAdd = 2, OpSub = 3, OpMad = 4, OpMul = 5, OpDp3 = 8, OpDp4 = 9,
    OpLrp = 18, OpTexcoord = 64, OpTexkill = 65, OpTex = 66, OpTexbem = 67, OpTexbeml = 68,
    OpTexreg2ar = 69, OpTexreg2gb = 70, OpTexm3x2pad = 71, OpTexm3x2tex = 72, OpTexm3x3pad = 73,
    OpTexm3x3tex = 74, OpTexm3x3spec = 76, OpTexm3x3vspec = 77, OpCnd = 80, OpDef = 81,
    OpTexreg2rgb = 82, OpTexdp3tex = 83, OpTexm3x2depth = 84, OpTexdp3 = 85, OpTexm3x3 = 86,
    OpTexdepth = 87, OpCmp = 88, OpBem = 89, OpPhase = 0xfffd, OpComment = 0xfffe, OpEnd = 0xffff
};
enum { RegTemp = 0, RegInput = 1, RegConst = 2, RegTexture = 3 };
enum { ModNone, ModNeg, ModBias, ModBiasNeg, ModSign, ModSignNeg, ModComp, ModX2, ModX2Neg };
#define COISSUE 0x40000000u

typedef struct {
    char *p;
    unsigned int n, size, line;
    int overflow;
} TEXT;

typedef struct { char text[40]; int neg; } OPERAND;

typedef struct {
    TEXT t;
    unsigned int minor;
    unsigned int defined[8];        /* constants given by def, one bit each */
    unsigned char defImm[256];      /* the immediate holding each */
    int vertex;                     /* translating a vertex shader */
    unsigned int cube;              /* version 1: stages whose texture is a cube map */
    unsigned int volume;            /* version 1: stages whose texture is a volume */
    unsigned int version;           /* major version of a vertex shader */
    unsigned char samplerKind[16];  /* shader model 2: D3DSAMPLER_TEXTURE_TYPE of each sampler */
    unsigned int written;           /* r0, r1 and t0..t3 written so far: bits 0-1, 8-11 */
    unsigned int pads;              /* texm3x2/texm3x3 dot products so far */
    unsigned int bump;              /* version 1: texbem or texbeml read the bump constants */
    const unsigned int *streamBases;  /* vertex shaders: the word each stream starts at, or NULL */
    unsigned int attributes;          /* vertex shaders: inputs are vertex attributes (RB_VS_ATTRIBUTES) */
    unsigned int stream;              /* the stream the declaration is at */
    /* Results waiting to be written: register, write mask, result slot. */
    char pendDest[2][24];
    unsigned int pendMask[2], pending;
    /* Vertex shader flow control: whether integer and boolean constants are given (RB_VS_FLOW),
     * where each label's code starts, the loops open (counter TEMP[56+n], aL TEMP[60+n]) with their
     * kind and integer constant, the ifs open, the subroutines being inlined, whether the current
     * instruction reads c[aL + n], and the defi and defb registers with their immediates. */
    unsigned int flow, labelAt[16], loops, loopIsLoop[4], ifs, calls, relLoop, defInt, defBool;
    char loopInt[4][24];
    unsigned char defIntImm[16], defBoolImm[16];
    RB_PS_INFO *info;
} PS;

static void put(TEXT *t, const char *s)
{
    while (*s) {
        if (t->n + 1 >= t->size) { t->overflow = 1; return; }
        t->p[t->n++] = *s++;
    }
    t->p[t->n] = 0;
}

static void number(TEXT *t, unsigned int v)
{
    char d[11];
    unsigned int i = sizeof(d);
    d[--i] = 0;
    do { d[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(t, d + i);
}

static void hex(TEXT *t, unsigned int v)
{
    static const char digits[] = "0123456789abcdef";
    char d[11];
    int i;
    d[0] = '0'; d[1] = 'x'; d[10] = 0;
    for (i = 0; i < 8; ++i) d[9 - i] = digits[(v >> (4 * i)) & 15];
    put(t, d);
}

/* Small string building into a fixed buffer. */
static void cat(char *dst, unsigned int size, const char *s)
{
    unsigned int n = 0;
    while (n < size && dst[n]) ++n;
    while (*s && n + 1 < size) dst[n++] = *s++;
    if (n < size) dst[n] = 0;
}

static void cat_number(char *dst, unsigned int size, unsigned int v)
{
    char d[11];
    unsigned int i = sizeof(d);
    d[--i] = 0;
    do { d[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    cat(dst, size, d + i);
}

/* One numbered instruction: the parts are concatenated. */
static void line(PS *s, const char *a, const char *b, const char *c, const char *d, const char *e,
                 const char *f, const char *g)
{
    const char *parts[7];
    unsigned int i;
    parts[0] = a; parts[1] = b; parts[2] = c; parts[3] = d; parts[4] = e; parts[5] = f; parts[6] = g;
    number(&s->t, s->t.line++);
    put(&s->t, ": ");
    for (i = 0; i < 7; ++i) if (parts[i]) put(&s->t, parts[i]);
    put(&s->t, "\n");
}

static unsigned int fail(PS *s, unsigned int reason, unsigned int detail)
{
    if (s->info->reason == RbPsOk) { s->info->reason = reason; s->info->detail = detail; }
    return 0;
}

static int defined(const PS *s, unsigned int c) { return c < 256 && (s->defined[c >> 5] >> (c & 31)) & 1; }
static void define(PS *s, unsigned int c) { if (c < 256) s->defined[c >> 5] |= 1u << (c & 31); }

static unsigned int reg_type(unsigned int token) { return (token >> 28) & 7; }
static unsigned int reg_num(unsigned int token) { return token & 0x7ff; }

/* The TGSI register of a Direct3D register; 0 when it does not exist. */
static int reg_name(PS *s, unsigned int type, unsigned int num, char *out, unsigned int size)
{
    out[0] = 0;
    switch (type) {
    case RegTemp:
        if (num > (s->minor == 4 ? 5u : 1u)) return 0;   /* r0..r5 in version 1.4 */
        cat(out, size, "TEMP["); cat_number(out, size, num); cat(out, size, "]");
        return 1;
    case RegInput:
        if (num > 1) return 0;
        cat(out, size, "IN["); cat_number(out, size, num); cat(out, size, "]");
        return 1;
    case RegConst:
        if (num >= RB_PS_CONSTANTS) return 0;
        if (defined(s, num)) { cat(out, size, "IMM["); cat_number(out, size, s->defImm[num]); }
        else { cat(out, size, "CONST["); cat_number(out, size, num); }
        cat(out, size, "]");
        return 1;
    case RegTexture:
        /* Version 1.4 reads t# only as texld and texcrd coordinates (texture14). */
        if (num >= RB_PS_COORD_SETS || s->minor == 4) return 0;
        cat(out, size, "TEMP["); cat_number(out, size, 8 + num); cat(out, size, "]");
        return 1;
    }
    return 0;
}

static void swizzle(unsigned int token, char *out, unsigned int size)
{
    static const char xyzw[] = "xyzw";
    char sw[6];
    unsigned int k, bits = (token >> 16) & 0xff;
    if (bits == 0xe4) return;               /* .xyzw */
    sw[0] = '.';
    for (k = 0; k < 4; ++k) sw[1 + k] = xyzw[(bits >> (2 * k)) & 3];
    sw[5] = 0;
    cat(out, size, sw);
}

/* A source operand, with any modifier other than negation applied first
 * into the scratch register for its slot. */
static int source(PS *s, unsigned int token, unsigned int slot, OPERAND *o)
{
    char name[24], scratch[16];
    unsigned int mod = (token >> 24) & 15;
    if (!(token & 0x80000000u)) return fail(s, RbPsRegister, token);
    if (!reg_name(s, reg_type(token), reg_num(token), name, sizeof(name))) return fail(s, RbPsRegister, token);
    swizzle(token, name, sizeof(name));
    o->text[0] = 0;
    o->neg = mod == ModNeg || mod == ModBiasNeg || mod == ModSignNeg || mod == ModX2Neg;
    if (mod == ModNone || mod == ModNeg) { cat(o->text, sizeof(o->text), name); return 1; }
    scratch[0] = 0;
    cat(scratch, sizeof(scratch), "TEMP[");
    cat_number(scratch, sizeof(scratch), 12 + slot);
    cat(scratch, sizeof(scratch), "]");
    switch (mod) {
    case ModBias: case ModBiasNeg:
        line(s, "ADD ", scratch, ", ", name, ", -IMM[0].xxxx", 0, 0);
        break;
    case ModSign: case ModSignNeg:
        line(s, "MAD ", scratch, ", ", name, ", IMM[0].zzzz, -IMM[0].yyyy", 0, 0);
        break;
    case ModComp:
        line(s, "ADD ", scratch, ", IMM[0].yyyy, -", name, 0, 0, 0);
        break;
    case ModX2: case ModX2Neg:
        if (s->minor < 4) return fail(s, RbPsModifier, token);
        line(s, "ADD ", scratch, ", ", name, ", ", name, 0);
        break;
    default:
        return fail(s, RbPsModifier, token);
    }
    cat(o->text, sizeof(o->text), scratch);
    return 1;
}

static const char *render(const OPERAND *o, char *buf, unsigned int size)
{
    buf[0] = 0;
    if (o->neg) cat(buf, size, "-");
    cat(buf, size, o->text);
    return buf;
}

static void mask_text(unsigned int mask, char *out, unsigned int size)
{
    static const char xyzw[] = "xyzw";
    unsigned int k, n = 1;
    char m[6];
    out[0] = 0;
    if (mask == 15) return;
    m[0] = '.';
    for (k = 0; k < 4; ++k) if (mask & (1u << k)) m[n++] = xyzw[k];
    m[n] = 0;
    cat(out, size, m);
}

/* Write the results waiting from the last instruction and the one co-issued with it. */
static void flush(PS *s)
{
    unsigned int k;
    for (k = 0; k < 2; ++k) {
        char mask[6], result[16];
        if (!(s->pending & (1u << k))) continue;
        mask_text(s->pendMask[k], mask, sizeof(mask));
        result[0] = 0;
        cat(result, sizeof(result), k ? "TEMP[17]" : "TEMP[16]");
        line(s, "MOV ", s->pendDest[k], mask, ", ", result, 0, 0);
    }
    s->pending = 0;
}

/* Shift and clamp the result in slot, then queue its write to the destination. */
static int finish(PS *s, unsigned int dest, unsigned int slot)
{
    const char *r = slot ? "TEMP[17]" : "TEMP[16]";
    unsigned int shift = (dest >> 24) & 15, sat = (dest >> 20) & 1, mask = (dest >> 16) & 15;
    static const char *const factors[16] = {
        0, "IMM[0].zzzz", "IMM[0].wwww", "IMM[1].xxxx", 0, 0, 0, 0,
        0, 0, 0, 0, 0, "IMM[1].zzzz", "IMM[1].yyyy", "IMM[0].xxxx"
    };
    if ((dest >> 20) & 14) return fail(s, RbPsModifier, dest);
    if (shift) {
        if (!factors[shift] || (shift == 3 || shift == 13 ? s->minor < 4 : 0)) return fail(s, RbPsModifier, dest);
        line(s, "MUL ", r, ", ", r, ", ", factors[shift], 0);
    }
    if (sat) line(s, "MOV_SAT ", r, ", ", r, 0, 0, 0);
    else {
        line(s, "MIN ", r, ", ", r, ", IMM[0].yyyy", 0, 0);
        line(s, "MAX ", r, ", ", r, ", IMM[1].wwww", 0, 0);
    }
    if (!reg_name(s, reg_type(dest), reg_num(dest), s->pendDest[slot], sizeof(s->pendDest[slot])) ||
        (reg_type(dest) != RegTemp && reg_type(dest) != RegTexture)) return fail(s, RbPsRegister, dest);
    s->pendMask[slot] = mask ? mask : 15;
    s->pending |= 1u << slot;
    return 1;
}

static int arithmetic(PS *s, unsigned int op, const unsigned int *p, unsigned int slot)
{
    OPERAND a, b, c;
    char ba[48], bb[48], bc[48];
    const char *r = slot ? "TEMP[17]" : "TEMP[16]";
    unsigned int sources = op == OpMov ? 1 : op == OpMad || op == OpLrp || op == OpCnd || op == OpCmp ? 3 : 2;
    if (!source(s, p[1], 0, &a)) return 0;
    if (sources > 1 && !source(s, p[2], 1, &b)) return 0;
    if (sources > 2 && !source(s, p[3], 2, &c)) return 0;
    switch (op) {
    case OpMov: line(s, "MOV ", r, ", ", render(&a, ba, 48), 0, 0, 0); break;
    case OpAdd: line(s, "ADD ", r, ", ", render(&a, ba, 48), ", ", render(&b, bb, 48), 0); break;
    case OpSub:
        b.neg = !b.neg;
        line(s, "ADD ", r, ", ", render(&a, ba, 48), ", ", render(&b, bb, 48), 0);
        break;
    case OpMul: line(s, "MUL ", r, ", ", render(&a, ba, 48), ", ", render(&b, bb, 48), 0); break;
    case OpDp3: line(s, "DP3 ", r, ", ", render(&a, ba, 48), ", ", render(&b, bb, 48), 0); break;
    case OpDp4:
        if (s->minor < 2) return fail(s, RbPsOpcode, op);
        line(s, "DP4 ", r, ", ", render(&a, ba, 48), ", ", render(&b, bb, 48), 0);
        break;
    case OpMad:
        line(s, "MAD ", r, ", ", render(&a, ba, 48), ", ", render(&b, bb, 48), 0);
        s->t.n -= 1;            /* reopen the line for the third operand */
        s->t.p[s->t.n] = 0;
        put(&s->t, ", "); put(&s->t, render(&c, bc, 48)); put(&s->t, "\n");
        break;
    case OpLrp:
        line(s, "LRP ", r, ", ", render(&a, ba, 48), ", ", render(&b, bb, 48), 0);
        s->t.n -= 1;
        s->t.p[s->t.n] = 0;
        put(&s->t, ", "); put(&s->t, render(&c, bc, 48)); put(&s->t, "\n");
        break;
    case OpCnd:
        /* src1 where src0 > 0.5, else src2. */
        line(s, "ADD TEMP[15], IMM[0].xxxx, ", a.neg ? "" : "-", a.text, 0, 0, 0, 0);
        line(s, "CMP ", r, ", TEMP[15], ", render(&b, bb, 48), ", ", render(&c, bc, 48), 0);
        break;
    case OpCmp:
        /* src1 where src0 >= 0, else src2. */
        if (s->minor < 2) return fail(s, RbPsOpcode, op);
        line(s, "CMP ", r, ", ", render(&a, ba, 48), ", ", render(&c, bc, 48), 0);
        s->t.n -= 1;
        s->t.p[s->t.n] = 0;
        put(&s->t, ", "); put(&s->t, render(&b, bb, 48)); put(&s->t, "\n");
        break;
    default:
        return fail(s, RbPsOpcode, op);
    }
    return finish(s, p[0], slot);
}

/* Texture register n of a texture instruction's destination. */
static int texture_dest(PS *s, unsigned int token, unsigned int *n)
{
    if (reg_type(token) != RegTexture || reg_num(token) >= RB_PS_COORD_SETS || ((token >> 16) & 15) != 15 ||
        (token & 0x0ff00000u)) return fail(s, RbPsRegister, token);
    *n = reg_num(token);
    return 1;
}

static int texture_source(PS *s, unsigned int token, unsigned int *m)
{
    if (reg_type(token) != RegTexture || reg_num(token) >= RB_PS_COORD_SETS || ((token >> 24) & 15))
        return fail(s, RbPsRegister, token);
    *m = reg_num(token);
    return 1;
}

static void names(unsigned int n, char *tex, char *coord, char *samp)
{
    tex[0] = coord[0] = samp[0] = 0;
    cat(tex, 16, "TEMP["); cat_number(tex, 16, 8 + n); cat(tex, 16, "]");
    cat(coord, 16, "IN["); cat_number(coord, 16, 2 + n); cat(coord, 16, "]");
    cat(samp, 16, "SAMP["); cat_number(samp, 16, n); cat(samp, 16, "]");
}

/* The texture register a texm3x2, texm3x3 or texdp3 instruction reads.
 * Version 1 allows the sign scale (_bx2) on it, applied into TEMP[12]. */
static int texture_operand(PS *s, unsigned int token, unsigned int *m, char *texm, char *coordm, char *sampm)
{
    unsigned int mod = (token >> 24) & 15;
    if (reg_type(token) != RegTexture || reg_num(token) >= RB_PS_COORD_SETS || (mod && mod != ModSign))
        return fail(s, RbPsRegister, token);
    *m = reg_num(token);
    names(*m, texm, coordm, sampm);
    if (mod == ModSign) {
        line(s, "MAD TEMP[12], ", texm, ", IMM[0].zzzz, -IMM[0].yyyy", 0, 0, 0, 0);
        texm[0] = 0;
        cat(texm, 16, "TEMP[12]");
    }
    return 1;
}

/* The texture target of stage n: a cube map, a volume or a 2D texture. */
static const char *stage_target(const PS *s, unsigned int n)
{
    return (s->cube >> n) & 1 ? ", CUBE" : (s->volume >> n) & 1 ? ", 3D" : ", 2D";
}

static int texture_op(PS *s, unsigned int op, const unsigned int *p)
{
    unsigned int n = 0, m = 0;
    char tex[16], coord[16], samp[16], texm[16], coordm[16], sampm[16];
    if (!texture_dest(s, p[0], &n)) return 0;
    names(n, tex, coord, samp);
    switch (op) {
    case OpTex:
        s->info->samplers |= 1u << n; s->info->coords |= 1u << n;
        line(s, "TEX ", tex, ", ", coord, ", ", samp, (s->cube >> n) & 1 ? ", CUBE" : (s->volume >> n) & 1 ? ", 3D" : ", 2D");
        break;
    case OpTexcoord:
        s->info->coords |= 1u << n;
        line(s, "MOV_SAT ", tex, ".xyz, ", coord, 0, 0, 0);
        line(s, "MOV ", tex, ".w, IMM[0].yyyy", 0, 0, 0, 0);
        break;
    case OpTexkill:
        s->info->coords |= 1u << n;
        line(s, "KILL_IF ", coord, ".xyzz", 0, 0, 0, 0);
        return 1;               /* writes nothing */
    case OpTexreg2ar:
    case OpTexreg2gb:
    case OpTexreg2rgb:
        if (op == OpTexreg2rgb && s->minor < 2) return fail(s, RbPsOpcode, op);
        if (!texture_source(s, p[1], &m)) return 0;
        names(m, texm, coordm, sampm);
        s->info->samplers |= 1u << n;
        line(s, "MOV TEMP[18], ", texm, op == OpTexreg2ar ? ".wxxx" : op == OpTexreg2gb ? ".yzzz" : 0, 0, 0, 0, 0);
        line(s, "TEX ", tex, ", TEMP[18], ", samp, (s->cube >> n) & 1 ? ", CUBE" : (s->volume >> n) & 1 ? ", 3D" : ", 2D", 0, 0);
        break;
    case OpTexm3x2pad:
    case OpTexm3x3pad:
        if (!texture_operand(s, p[1], &m, texm, coordm, sampm)) return 0;
        s->info->coords |= 1u << n;
        if (s->pads > 1) return fail(s, RbPsOpcode, op);
        line(s, "DP3 TEMP[18].", s->pads ? "y" : "x", ", ", coord, ", ", texm, 0);
        ++s->pads;
        return 1;
    case OpTexm3x2tex:
    case OpTexdp3tex:
        if (op == OpTexdp3tex && s->minor < 2) return fail(s, RbPsOpcode, op);
        if (op == OpTexm3x2tex && s->pads != 1) return fail(s, RbPsOpcode, op);
        if (!texture_operand(s, p[1], &m, texm, coordm, sampm)) return 0;
        s->info->coords |= 1u << n; s->info->samplers |= 1u << n;
        if (op == OpTexdp3tex) {
            line(s, "DP3 TEMP[18].x, ", coord, ", ", texm, 0, 0, 0);
            line(s, "MOV TEMP[18].y, IMM[2].xxxx", 0, 0, 0, 0, 0, 0);
        } else line(s, "DP3 TEMP[18].y, ", coord, ", ", texm, 0, 0, 0);
        line(s, "TEX ", tex, ", TEMP[18], ", samp, (s->cube >> n) & 1 ? ", CUBE" : (s->volume >> n) & 1 ? ", 3D" : ", 2D", 0, 0);
        s->pads = 0;
        break;
    case OpTexm3x3tex:
        /* The third row, then a lookup with the vector (a cube map or a volume). */
        if (s->pads != 2) return fail(s, RbPsOpcode, op);
        if (!texture_operand(s, p[1], &m, texm, coordm, sampm)) return 0;
        s->info->coords |= 1u << n; s->info->samplers |= 1u << n;
        line(s, "DP3 TEMP[18].z, ", coord, ", ", texm, 0, 0, 0);
        line(s, "TEX ", tex, ", TEMP[18], ", samp, stage_target(s, n), 0, 0);
        s->pads = 0;
        break;
    case OpTexm3x3spec:
    case OpTexm3x3vspec: {
        /* The third row gives the normal N; the eye vector E is a constant
         * (spec) or the w of the three rows' coordinates (vspec). The cube map
         * is looked up with the reflection 2 N (N.E) / (N.N) - E. */
        char eye[24];
        if (s->pads != 2 || n < 2) return fail(s, RbPsOpcode, op);
        if (!texture_operand(s, p[1], &m, texm, coordm, sampm)) return 0;
        s->info->coords |= 1u << n; s->info->samplers |= 1u << n;
        line(s, "DP3 TEMP[18].z, ", coord, ", ", texm, 0, 0, 0);
        eye[0] = 0;
        if (op == OpTexm3x3spec) {
            if (reg_type(p[2]) != RegConst || ((p[2] >> 24) & 15) ||
                !reg_name(s, RegConst, reg_num(p[2]), eye, sizeof(eye))) return fail(s, RbPsRegister, p[2]);
        } else {
            char c0[16], c1[16], t0[16], t1[16], s0[16], s1[16];
            names(n - 2, t0, c0, s0);
            names(n - 1, t1, c1, s1);
            line(s, "MOV TEMP[13].x, ", c0, ".wwww", 0, 0, 0, 0);
            line(s, "MOV TEMP[13].y, ", c1, ".wwww", 0, 0, 0, 0);
            line(s, "MOV TEMP[13].z, ", coord, ".wwww", 0, 0, 0, 0);
            cat(eye, sizeof(eye), "TEMP[13]");
        }
        line(s, "DP3 TEMP[15].x, TEMP[18], ", eye, 0, 0, 0, 0, 0);
        line(s, "DP3 TEMP[15].y, TEMP[18], TEMP[18]", 0, 0, 0, 0, 0, 0);
        line(s, "RCP TEMP[15].y, TEMP[15].yyyy", 0, 0, 0, 0, 0, 0);
        line(s, "MUL TEMP[15].x, TEMP[15].xxxx, TEMP[15].yyyy", 0, 0, 0, 0, 0, 0);
        line(s, "MUL TEMP[15].x, TEMP[15].xxxx, IMM[0].zzzz", 0, 0, 0, 0, 0, 0);
        line(s, "MAD TEMP[18].xyz, TEMP[18], TEMP[15].xxxx, -", eye, 0, 0, 0, 0, 0);
        line(s, "TEX ", tex, ", TEMP[18], ", samp, ", CUBE", 0, 0);
        s->pads = 0;
        break;
    }
    case OpTexm3x3:
        if (s->minor < 2 || s->pads != 2) return fail(s, RbPsOpcode, op);
        if (!texture_operand(s, p[1], &m, texm, coordm, sampm)) return 0;
        s->info->coords |= 1u << n;
        line(s, "DP3 TEMP[18].z, ", coord, ", ", texm, 0, 0, 0);
        line(s, "MOV ", tex, ".xyz, TEMP[18]", 0, 0, 0, 0);
        line(s, "MOV ", tex, ".w, IMM[0].yyyy", 0, 0, 0, 0);
        s->pads = 0;
        break;
    case OpTexdp3:
        if (s->minor < 2) return fail(s, RbPsOpcode, op);
        if (!texture_operand(s, p[1], &m, texm, coordm, sampm)) return 0;
        s->info->coords |= 1u << n;
        line(s, "DP3 ", tex, ", ", coord, ", ", texm, 0);
        break;
    case OpTexbem:
    case OpTexbeml: {
        /* Stage n's coordinates moved by stage m's du and dv through stage n's
         * bump matrix (CONST[8+n]: 00, 01, 10, 11): u + du 00 + dv 10,
         * v + du 01 + dv 11. texbeml then scales the colour by m's
         * luminance times the scale plus the offset (CONST[12+n]). */
        char mat[16], lum[16];
        if (!texture_source(s, p[1], &m)) return 0;
        if (m >= n) return fail(s, RbPsRegister, p[1]);
        names(m, texm, coordm, sampm);
        mat[0] = lum[0] = 0;
        cat(mat, 16, "CONST["); cat_number(mat, 16, RB_PS_CONSTANTS + n); cat(mat, 16, "]");
        cat(lum, 16, "CONST["); cat_number(lum, 16, RB_PS_CONSTANTS + 4 + n); cat(lum, 16, "]");
        s->info->coords |= 1u << n; s->info->samplers |= 1u << n;
        line(s, "MAD TEMP[18], ", texm, ".xxxx, ", mat, ".xyxy, ", coord, 0);
        line(s, "MAD TEMP[18].xy, ", texm, ".yyyy, ", mat, ".zwzw, TEMP[18]", 0, 0);
        line(s, "TEX ", tex, ", TEMP[18], ", samp, stage_target(s, n), 0, 0);
        if (op == OpTexbeml) {
            line(s, "MAD_SAT TEMP[15].x, ", texm, ".zzzz, ", lum, ".xxxx, ", lum, ".yyyy");
            line(s, "MUL ", tex, ".xyz, ", tex, ", TEMP[15].xxxx", 0, 0);
        }
        break;
    }
    default:
        /* Cube and depth lookups need texture kinds or states this
         * device does not offer. */
        return fail(s, RbPsOpcode, op);
    }
    s->written |= 1u << (8 + n);
    return 1;
}

/* Version 1.4 texture instructions. texld rN, src samples sampler N at src: a coordinate set t#,
 * or in the second phase a register r# (a dependent read); texcrd rN, t# copies the coordinates;
 * texkill takes r# or t#. A t# source may be divided by its z or w (_dz, _dw), and selects xyz or
 * xyw by its swizzle. */
enum { ModDz = 9, ModDw = 10 };
static int texture14(PS *s, unsigned int op, const unsigned int *p)
{
    unsigned int n = reg_num(p[0]), m, mod;
    char dst[16], src[32], samp[16], mask[6];
    dst[0] = src[0] = samp[0] = 0;
    if (op == OpTexkill) {
        if (reg_type(p[0]) == RegTexture && n < 6) {
            s->info->coords |= 1u << n;
            cat(src, sizeof(src), "IN["); cat_number(src, sizeof(src), 2 + n); cat(src, sizeof(src), "]");
        } else if (reg_type(p[0]) == RegTemp && n < 6) {
            cat(src, sizeof(src), "TEMP["); cat_number(src, sizeof(src), n); cat(src, sizeof(src), "]");
        } else return fail(s, RbPsRegister, p[0]);
        line(s, "KILL_IF ", src, ".xyzz", 0, 0, 0, 0);
        return 1;
    }
    m = reg_num(p[1]);
    mod = (p[1] >> 24) & 15;
    if (reg_type(p[0]) != RegTemp || n > 5 || (p[0] & 0x0ff00000u)) return fail(s, RbPsRegister, p[0]);
    cat(dst, sizeof(dst), "TEMP["); cat_number(dst, sizeof(dst), n); cat(dst, sizeof(dst), "]");
    if (reg_type(p[1]) == RegTexture && m < 6) {
        s->info->coords |= 1u << m;
        cat(src, sizeof(src), "IN["); cat_number(src, sizeof(src), 2 + m); cat(src, sizeof(src), "]");
    } else if (reg_type(p[1]) == RegTemp && m < 6 && op == OpTex && !mod) {
        cat(src, sizeof(src), "TEMP["); cat_number(src, sizeof(src), m); cat(src, sizeof(src), "]");
    } else return fail(s, RbPsRegister, p[1]);
    if (mod == ModDz || mod == ModDw) {
        /* x and y divided by z or w, into TEMP[12]. */
        line(s, "RCP TEMP[13].x, ", src, mod == ModDz ? ".zzzz" : ".wwww", 0, 0, 0, 0);
        line(s, "MUL TEMP[12].xy, ", src, ".xyyy, TEMP[13].xxxx", 0, 0, 0, 0);
        line(s, "MOV TEMP[12].zw, IMM[0].yyyy", 0, 0, 0, 0, 0, 0);
        src[0] = 0;
        cat(src, sizeof(src), "TEMP[12]");
    } else if (mod) return fail(s, RbPsModifier, p[1]);
    else swizzle(p[1], src, sizeof(src));
    if (op == OpTex) {
        s->info->samplers |= 1u << n;
        cat(samp, sizeof(samp), "SAMP["); cat_number(samp, sizeof(samp), n); cat(samp, sizeof(samp), "]");
        line(s, "TEX ", dst, ", ", src, ", ", samp, stage_target(s, n));
    } else {
        mask_text((p[0] >> 16) & 15 ? (p[0] >> 16) & 15 : 15, mask, sizeof(mask));
        line(s, "MOV ", dst, mask, ", ", src, 0, 0);
    }
    return 1;
}

/* Version 1.4 bem rN.xy, src0, src1 (first phase): src0 moved by the bump matrix of stage N times
 * src1, the matrix (00, 01, 10, 11) in CONST[RB_PS_CONSTANTS + N] as for texbem. */
static int bem(PS *s, const unsigned int *p)
{
    OPERAND a, b;
    char ba[48], bb[48], m[16];
    unsigned int n = reg_num(p[0]);
    if (reg_type(p[0]) != RegTemp || n >= RB_PS_COORD_SETS) return fail(s, RbPsRegister, p[0]);
    if (!source(s, p[1], 0, &a) || !source(s, p[2], 1, &b)) return 0;
    m[0] = 0;
    cat(m, sizeof(m), "CONST["); cat_number(m, sizeof(m), RB_PS_CONSTANTS + n); cat(m, sizeof(m), "]");
    line(s, "MOV TEMP[14], ", render(&b, bb, sizeof(bb)), 0, 0, 0, 0, 0);   /* src1 may carry a swizzle */
    line(s, "MUL TEMP[15].xy, ", m, ".xyyy, TEMP[14].xxxx", 0, 0, 0, 0);
    line(s, "MAD TEMP[15].xy, ", m, ".zwww, TEMP[14].yyyy, TEMP[15]", 0, 0, 0, 0);
    line(s, "ADD TEMP[16].xy, ", render(&a, ba, sizeof(ba)), ", TEMP[15]", 0, 0, 0, 0);
    return finish(s, p[0], 0);
}

static int parameters(unsigned int op, unsigned int minor)
{
    switch (op) {
    case OpNop: case OpPhase: return 0;
    case OpTexcoord: case OpTex: return minor == 4 ? 2 : 1;   /* texcrd and texld in version 1.4 */
    case OpTexkill: case OpTexdepth: return 1;
    case OpMov: case OpTexbem: case OpTexbeml: case OpTexreg2ar: case OpTexreg2gb: case OpTexm3x2pad:
    case OpTexm3x2tex: case OpTexm3x3pad: case OpTexm3x3tex: case OpTexm3x3vspec: case OpTexreg2rgb:
    case OpTexdp3tex: case OpTexm3x2depth: case OpTexdp3: case OpTexm3x3: return 2;
    case OpAdd: case OpSub: case OpMul: case OpDp3: case OpDp4: case OpTexm3x3spec: case OpBem: return 3;
    case OpMad: case OpLrp: case OpCnd: case OpCmp: return 4;
    case OpDef: return 5;
    }
    return -1;
}

static int texture_family(unsigned int op) { return op >= OpTexcoord && op <= OpTexm3x3vspec ? 1 :
    op == OpTexreg2rgb || op == OpTexdp3tex || op == OpTexm3x2depth || op == OpTexdp3 || op == OpTexm3x3 ||
    op == OpTexdepth; }

/* Visit every instruction; pass 0 collects def constants, pass 1 translates. */
static int walk(PS *s, const unsigned int *tok, unsigned int count, int pass, unsigned int *imm)
{
    unsigned int i = 1;
    while (i < count) {
        unsigned int t = tok[i], op = t & 0xffff;
        int n;
        if (t == OpEnd) return 1;
        if (op == OpComment) { i += 1 + ((t >> 16) & 0x7fff); continue; }
        n = parameters(op, s->minor);
        if (n < 0 || (t & 0x80000000u) || ((t & COISSUE) && texture_family(op)))
            return fail(s, RbPsOpcode, t);
        if (i + 1 + (unsigned int)n > count) return fail(s, RbPsTruncated, i);
        if (pass == 0) {
            if (op == OpDef) {
                unsigned int c = reg_num(tok[i + 1]);
                if (reg_type(tok[i + 1]) != RegConst || c >= RB_PS_CONSTANTS) return fail(s, RbPsRegister, tok[i + 1]);
                define(s, c);
                s->defImm[c] = (unsigned char)(*imm)++;
                put(&s->t, "IMM["); number(&s->t, s->defImm[c]); put(&s->t, "] FLT32 {");
                hex(&s->t, tok[i + 2]); put(&s->t, ", "); hex(&s->t, tok[i + 3]); put(&s->t, ", ");
                hex(&s->t, tok[i + 4]); put(&s->t, ", "); hex(&s->t, tok[i + 5]); put(&s->t, "}\n");
            }
        } else if (op != OpDef && op != OpNop) {
            const unsigned int *p = tok + i + 1;
            if (!(t & COISSUE)) flush(s);
            if (s->minor == 4 && op == OpPhase) { i += 1; continue; }   /* registers simply carry over */
            if (s->minor == 4 && op == OpBem) { if (!bem(s, p)) return 0; i += 4; continue; }
            if (op == OpPhase || op == OpBem || op == OpTexdepth) return fail(s, RbPsOpcode, op);
            if (s->minor == 4 && (op == OpTex || op == OpTexcoord || op == OpTexkill)) { if (!texture14(s, op, p)) return 0; }
            else if (s->minor == 4 && texture_family(op)) return fail(s, RbPsOpcode, op);
            else if (texture_family(op)) { if (!texture_op(s, op, p)) return 0; }
            else {
                if (!arithmetic(s, op, p, (t & COISSUE) ? 1 : 0)) return 0;
                if (reg_type(p[0]) == RegTemp) s->written |= 1u << reg_num(p[0]);
            }
        }
        i += 1 + (unsigned int)n;
    }
    return fail(s, RbPsTruncated, count);
}

/* Blend the clamped result with the fog colour, as Direct3D does after a
 * version 1 pixel shader. TEMP[2] holds the factor; log2e is IMM[log2e]. */
static void fog_line(PS *s, const char *a, const char *b, const char *c, const char *d, const char *e)
{
    line(s, a, b, c, d, e, 0, 0);
}

/* color: the result register; base: the fog colour constant, followed by
 * the start, end and density. */
static void fog_lines(PS *s, unsigned int fog, unsigned int log2e, const char *color, unsigned int base)
{
    unsigned int kind = fog & 3, mode = (fog >> RB_PS_FOG_MODE_SHIFT) & 3;
    char imm[24], c0[16], c1[16];
    imm[0] = c0[0] = c1[0] = 0;
    cat(imm, sizeof(imm), "-IMM["); cat_number(imm, sizeof(imm), log2e); cat(imm, sizeof(imm), "].xxxx");
    cat(c0, sizeof(c0), "CONST["); cat_number(c0, sizeof(c0), base); cat(c0, sizeof(c0), "]");
    cat(c1, sizeof(c1), "CONST["); cat_number(c1, sizeof(c1), base + 1); cat(c1, sizeof(c1), "]");
    fog_line(s, "MOV_SAT ", color, ", ", color, 0);
    if (kind == RB_PS_FOG_ALPHA) fog_line(s, "MOV_SAT TEMP[2].x, IN[1].wwww", 0, 0, 0, 0);
    else {
        if (kind == RB_PS_FOG_VERTEX)
            fog_line(s, (fog & RB_PS_FOG_RANGE) ? "MOV TEMP[2].y, IN[10].yyyy" : "MOV TEMP[2].y, IN[10].xxxx", 0, 0, 0, 0);
        else fog_line(s, (fog & RB_PS_FOG_W) ? "RCP TEMP[2].y, IN[10].wwww" : "MOV TEMP[2].y, IN[10].zzzz", 0, 0, 0, 0);
        if (mode == 3) {   /* linear: (end - d) / (end - start) */
            fog_line(s, "ADD TEMP[2].x, ", c1, ".yyyy, -TEMP[2].yyyy", 0, 0);
            fog_line(s, "ADD TEMP[2].z, ", c1, ".yyyy, -", c1, ".xxxx");
            fog_line(s, "RCP TEMP[2].z, TEMP[2].zzzz", 0, 0, 0, 0);
            fog_line(s, "MUL_SAT TEMP[2].x, TEMP[2].xxxx, TEMP[2].zzzz", 0, 0, 0, 0);
        } else {           /* exp(-(density d)) or exp(-(density d)^2) */
            fog_line(s, "MUL TEMP[2].x, ", c1, ".zzzz, TEMP[2].yyyy", 0, 0);
            if (mode == 2) fog_line(s, "MUL TEMP[2].x, TEMP[2].xxxx, TEMP[2].xxxx", 0, 0, 0, 0);
            fog_line(s, "MUL TEMP[2].x, TEMP[2].xxxx, ", imm, 0, 0, 0);
            fog_line(s, "EX2 TEMP[2].x, TEMP[2].xxxx", 0, 0, 0, 0);
            fog_line(s, "MOV_SAT TEMP[2].x, TEMP[2].xxxx", 0, 0, 0, 0);
        }
    }
    line(s, "LRP ", color, ".xyz, TEMP[2].xxxx, ", color, ", ", c0, 0);
}

/* ---- shader model 2 ----
 * Registers: r0..r11 are TEMP[0..11], v0 and v1 IN[0] and IN[1], t# the
 * coordinates IN[2+#], c0..c31 CONST[0..31] unless defined, s# SAMP[#] and
 * oC0 TEMP[20]. TEMP[17..19] hold intermediate values of expanded
 * instructions. Arithmetic is unclamped, as in Direct3D 9. */
enum {
    Op2Rcp = 6, Op2Rsq = 7, Op2Min = 10, Op2Max = 11, Op2Exp = 14, Op2Log = 15, Op2Frc = 19, Op2M4x4 = 20,
    Op2M4x3 = 21, Op2M3x4 = 22, Op2M3x3 = 23, Op2M3x2 = 24, Op2Dcl = 31, Op2Pow = 32, Op2Crs = 33,
    Op2Abs = 35, Op2Nrm = 36, Op2Sincos = 37, Op2Dp2add = 90
};
enum { Reg2Temp = 0, Reg2Input = 1, Reg2Const = 2, Reg2Texture = 3, Reg2ColorOut = 8, Reg2Sampler = 10 };
/* Vertex shader register types. */
enum { RegVsAddr = 3, RegVsRastOut = 4, RegVsAttrOut = 5, RegVsTexOut = 6, RegVsConstInt = 7, RegVsConstBool = 14,
       RegVsLoop = 15, RegVsLabel = 18 };
enum { Mod2Abs = 11, Mod2AbsNeg = 12 };

static unsigned int reg2_type(unsigned int t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }

static int vreg_name(PS *s, unsigned int token, unsigned int add, char *out, unsigned int size);

static int reg2_name(PS *s, unsigned int token, unsigned int add, char *out, unsigned int size)
{
    unsigned int type = reg2_type(token), num = reg_num(token) + add;
    out[0] = 0;
    if (s->vertex) return vreg_name(s, token, add, out, size);
    if (token & 0x2000) return fail(s, RbPsRegister, token);   /* relative addressing */
    switch (type) {
    case Reg2Temp:
        if (num > 11) return fail(s, RbPsRegister, token);
        cat(out, size, "TEMP["); cat_number(out, size, num); cat(out, size, "]");
        return 1;
    case Reg2Input:
        if (num > 1) return fail(s, RbPsRegister, token);
        cat(out, size, "IN["); cat_number(out, size, num); cat(out, size, "]");
        return 1;
    case Reg2Const:
        if (num >= RB_PS2_CONSTANTS) return fail(s, RbPsRegister, token);
        if (defined(s, num)) { cat(out, size, "IMM["); cat_number(out, size, s->defImm[num]); }
        else { cat(out, size, "CONST["); cat_number(out, size, num); }
        cat(out, size, "]");
        return 1;
    case Reg2Texture:
        if (num >= RB_PS2_COORD_SETS) return fail(s, RbPsRegister, token);
        cat(out, size, "IN["); cat_number(out, size, 2 + num); cat(out, size, "]");
        s->info->coords |= 1u << num;
        return 1;
    case Reg2ColorOut:
        if (num) return fail(s, RbPsRegister, token);
        cat(out, size, "TEMP[20]");
        return 1;
    }
    return fail(s, RbPsRegister, token);
}

/* A source with its swizzle, negation and absolute value. scalar: take one
 * component (a replicate swizzle's, else w) for every channel. */
static int source2(PS *s, unsigned int token, unsigned int add, int scalar, char *out, unsigned int size)
{
    char name[32];
    unsigned int mod = (token >> 24) & 15, swz = (token >> 16) & 0xff;
    if (!(token & 0x80000000u)) return fail(s, RbPsRegister, token);
    if (!reg2_name(s, token, add, name, sizeof(name))) return 0;
    if (scalar) {
        static const char *const rep[4] = { ".xxxx", ".yyyy", ".zzzz", ".wwww" };
        cat(name, sizeof(name), rep[swz == 0xe4 ? 3 : swz & 3]);
    } else swizzle(token, name, sizeof(name));
    out[0] = 0;
    switch (mod) {
    case ModNone: cat(out, size, name); return 1;
    case ModNeg: cat(out, size, "-"); cat(out, size, name); return 1;
    case Mod2Abs: cat(out, size, "|"); cat(out, size, name); cat(out, size, "|"); return 1;
    case Mod2AbsNeg: cat(out, size, "-|"); cat(out, size, name); cat(out, size, "|"); return 1;
    }
    return fail(s, RbPsModifier, token);
}

/* The destination register with its write mask; sat gets "_SAT" or "". */
static int dest2(PS *s, unsigned int token, char *out, unsigned int size, const char **sat)
{
    char mask[6];
    unsigned int type = reg2_type(token);
    if (!(token & 0x80000000u) || ((token >> 24) & 15) || ((token >> 20) & 8) || (token & 0x2000) ||
        (s->vertex ? type != Reg2Temp && type != RegVsRastOut && type != RegVsAttrOut && type != RegVsTexOut
                   : type != Reg2Temp && type != Reg2ColorOut)) return fail(s, RbPsRegister, token);
    if (!reg2_name(s, token, 0, out, size)) return 0;
    mask_text((token >> 16) & 15 ? (token >> 16) & 15 : 15, mask, sizeof(mask));
    cat(out, size, mask);
    *sat = (token >> 20) & 1 ? "_SAT " : " ";   /* partial precision and centroid are ignored */
    return 1;
}

/* |x| of an operand that may carry a negation or an absolute value already. */
static void absolute(const char *a, char *out, unsigned int size)
{
    const char *x = a[0] == '-' ? a + 1 : a;
    out[0] = 0;
    if (x[0] == '|') cat(out, size, x);
    else { cat(out, size, "|"); cat(out, size, x); cat(out, size, "|"); }
}

static int vertex_instruction(PS *s, unsigned int op, const unsigned int *p, unsigned int n);

static int instruction2(PS *s, unsigned int op, unsigned int control, const unsigned int *p, unsigned int n)
{
    char d[40], a[48], b[48], c[48], name[24];
    const char *sat = " ";
    unsigned int k, rows;
    if (s->vertex) {
        int done = vertex_instruction(s, op, p, n);
        if (done >= 0) return done;
    }
    switch (op) {
    case OpNop: case OpDef: case Op2Dcl: return 1;
    case OpTexkill:
        if (n < 1 || !reg2_name(s, p[0], 0, name, sizeof(name))) return 0;
        line(s, "KILL_IF ", name, ".xyzz", 0, 0, 0, 0);
        return 1;
    case OpTex: {
        static const char *const ops[3] = { "TEX", "TXP", "TXB" };
        /* Only 2D textures exist on this device so far. */
        static const char *const targets[5] = { 0, 0, "2D", "CUBE", "3D" };   /* D3DSTT_2D, CUBE, VOLUME */
        unsigned int sampler;
        if (n < 3 || control > 2 || reg2_type(p[2]) != Reg2Sampler) return fail(s, RbPsOpcode, op);
        sampler = reg_num(p[2]);
        if (sampler >= RB_PS2_SAMPLERS || !targets[s->samplerKind[sampler] < 5 ? s->samplerKind[sampler] : 0])
            return fail(s, RbPsRegister, p[2]);
        if (!dest2(s, p[0], d, sizeof(d), &sat) || !source2(s, p[1], 0, 0, a, sizeof(a))) return 0;
        name[0] = 0;
        cat(name, sizeof(name), "SAMP["); cat_number(name, sizeof(name), sampler); cat(name, sizeof(name), "], ");
        line(s, ops[control], sat, d, ", ", a, ", ", name);
        s->t.n -= 1; s->t.p[s->t.n] = 0;
        put(&s->t, targets[s->samplerKind[sampler]]); put(&s->t, "\n");
        s->info->samplers |= 1u << sampler;
        return 1;
    }
    }
    if (n < 2 || !dest2(s, p[0], d, sizeof(d), &sat)) return 0;
    switch (op) {
    case OpMov: case Op2Abs: case Op2Frc:
        if (!source2(s, p[1], 0, 0, a, sizeof(a))) return 0;
        if (op == Op2Abs) absolute(a, b, sizeof(b));
        line(s, op == Op2Frc ? "FRC" : "MOV", sat, d, ", ", op == Op2Abs ? b : a, 0, 0);
        return 1;
    case Op2Rcp: case Op2Rsq: case Op2Exp: case Op2Log: {
        static const char *const names[] = { "RCP", "RSQ", "EX2", "LG2" };
        unsigned int which = op == Op2Rcp ? 0 : op == Op2Rsq ? 1 : op == Op2Exp ? 2 : 3;
        if (!source2(s, p[1], 0, 1, a, sizeof(a))) return 0;
        if (which == 1 || which == 3) {   /* of the absolute value */
            absolute(a, b, sizeof(b));
            line(s, names[which], sat, d, ", ", b, 0, 0);
        } else line(s, names[which], sat, d, ", ", a, 0, 0);
        return 1;
    }
    case Op2Pow:
        if (n < 3 || !source2(s, p[1], 0, 1, a, sizeof(a)) || !source2(s, p[2], 0, 1, b, sizeof(b))) return 0;
        absolute(a, c, sizeof(c));
        line(s, "POW", sat, d, ", ", c, ", ", b);
        return 1;
    case OpAdd: case OpSub: case OpMul: case OpDp3: case OpDp4: case Op2Min: case Op2Max: {
        static const char *const names[] = { "ADD", "ADD", "MUL", "DP3", "DP4", "MIN", "MAX" };
        unsigned int which = op == OpAdd ? 0 : op == OpSub ? 1 : op == OpMul ? 2 : op == OpDp3 ? 3 :
                             op == OpDp4 ? 4 : op == Op2Min ? 5 : 6;
        if (n < 3 || !source2(s, p[1], 0, 0, a, sizeof(a)) || !source2(s, p[2], 0, 0, b, sizeof(b))) return 0;
        if (op == OpSub) {
            c[0] = 0;
            if (b[0] == '-') cat(c, sizeof(c), b + 1); else { cat(c, sizeof(c), "-"); cat(c, sizeof(c), b); }
            line(s, names[which], sat, d, ", ", a, ", ", c);
        } else line(s, names[which], sat, d, ", ", a, ", ", b);
        return 1;
    }
    case OpMad: case OpLrp: case OpCmp: case Op2Dp2add:
        if (n < 4 || !source2(s, p[1], 0, 0, a, sizeof(a)) || !source2(s, p[2], 0, 0, b, sizeof(b)) ||
            !source2(s, p[3], 0, op == Op2Dp2add, c, sizeof(c))) return 0;
        if (op == Op2Dp2add) {
            line(s, "MOV TEMP[17], ", a, 0, 0, 0, 0, 0);
            line(s, "MOV TEMP[18], ", b, 0, 0, 0, 0, 0);
            line(s, "MUL TEMP[19].x, TEMP[17].xxxx, TEMP[18].xxxx", 0, 0, 0, 0, 0, 0);
            line(s, "MAD TEMP[19].x, TEMP[17].yyyy, TEMP[18].yyyy, TEMP[19].xxxx", 0, 0, 0, 0, 0, 0);
            line(s, "ADD", sat, d, ", TEMP[19].xxxx, ", c, 0, 0);
            return 1;
        }
        line(s, op == OpMad ? "MAD" : op == OpLrp ? "LRP" : "CMP", sat, d, ", ", a, ", ", op == OpCmp ? c : b);
        s->t.n -= 1; s->t.p[s->t.n] = 0;
        put(&s->t, ", "); put(&s->t, op == OpCmp ? b : c); put(&s->t, "\n");
        return 1;
    case Op2Nrm:
        if (!source2(s, p[1], 0, 0, a, sizeof(a))) return 0;
        line(s, "MOV TEMP[17], ", a, 0, 0, 0, 0, 0);
        line(s, "DP3 TEMP[19].x, TEMP[17], TEMP[17]", 0, 0, 0, 0, 0, 0);
        line(s, "RSQ TEMP[19].x, TEMP[19].xxxx", 0, 0, 0, 0, 0, 0);
        line(s, "MUL", sat, d, ", TEMP[17], TEMP[19].xxxx", 0, 0, 0);
        return 1;
    case Op2Crs:
        if (n < 3 || !source2(s, p[1], 0, 0, a, sizeof(a)) || !source2(s, p[2], 0, 0, b, sizeof(b))) return 0;
        line(s, "MOV TEMP[17], ", a, 0, 0, 0, 0, 0);
        line(s, "MOV TEMP[18], ", b, 0, 0, 0, 0, 0);
        line(s, "MUL TEMP[19], TEMP[17].yzxx, TEMP[18].zxyy", 0, 0, 0, 0, 0, 0);
        line(s, "MAD TEMP[19], -TEMP[17].zxyy, TEMP[18].yzxx, TEMP[19]", 0, 0, 0, 0, 0, 0);
        line(s, "MOV", sat, d, ", TEMP[19]", 0, 0, 0);
        return 1;
    case Op2Sincos:
        /* x is the cosine and y the sine; shader model 2 passes two more constants. */
        if (!source2(s, p[1], 0, 1, a, sizeof(a))) return 0;
        line(s, "COS TEMP[19].x, ", a, 0, 0, 0, 0, 0);
        line(s, "SIN TEMP[19].y, ", a, 0, 0, 0, 0, 0);
        line(s, "MOV", sat, d, ", TEMP[19]", 0, 0, 0);
        return 1;
    case Op2M4x4: case Op2M4x3: case Op2M3x4: case Op2M3x3: case Op2M3x2: {
        static const char *const xyzw[4] = { "TEMP[19].x, ", "TEMP[19].y, ", "TEMP[19].z, ", "TEMP[19].w, " };
        int four = op == Op2M4x4 || op == Op2M4x3;
        rows = op == Op2M4x4 || op == Op2M3x4 ? 4 : op == Op2M3x2 ? 2 : 3;
        if (n < 3 || !source2(s, p[1], 0, 0, a, sizeof(a))) return 0;
        line(s, "MOV TEMP[17], ", a, 0, 0, 0, 0, 0);
        for (k = 0; k < rows; ++k) {
            if (!source2(s, p[2], k, 0, b, sizeof(b))) return 0;
            line(s, four ? "DP4 " : "DP3 ", xyzw[k], "TEMP[17], ", b, 0, 0, 0);
        }
        line(s, "MOV", sat, d, ", TEMP[19]", 0, 0, 0);
        return 1;
    }
    }
    return fail(s, RbPsOpcode, op);
}

/* Pass 0: declarations and def constants; pass 1: instructions. */
static int walk2(PS *s, const unsigned int *tok, unsigned int count, int pass, unsigned int *imm)
{
    unsigned int i = 1;
    while (i < count) {
        unsigned int t = tok[i], op = t & 0xffff, n = (t >> 24) & 15;
        if (t == OpEnd) return 1;
        if (op == OpComment) { i += 1 + ((t >> 16) & 0x7fff); continue; }
        if (t & 0x90000000u) return fail(s, RbPsOpcode, t);    /* predication and reserved bits */
        if (i + 1 + n > count) return fail(s, RbPsTruncated, i);
        if (pass == 0) {
            if (op == OpDef) {
                unsigned int c = reg_num(tok[i + 1]);
                if (n != 5 || reg2_type(tok[i + 1]) != Reg2Const || c >= RB_PS2_CONSTANTS)
                    return fail(s, RbPsRegister, tok[i + 1]);
                define(s, c);
                s->defImm[c] = (unsigned char)(*imm)++;
                put(&s->t, "IMM["); number(&s->t, s->defImm[c]); put(&s->t, "] FLT32 {");
                hex(&s->t, tok[i + 2]); put(&s->t, ", "); hex(&s->t, tok[i + 3]); put(&s->t, ", ");
                hex(&s->t, tok[i + 4]); put(&s->t, ", "); hex(&s->t, tok[i + 5]); put(&s->t, "}\n");
            } else if (op == Op2Dcl) {
                if (n != 2) return fail(s, RbPsOpcode, t);
                if (reg2_type(tok[i + 2]) == Reg2Sampler && reg_num(tok[i + 2]) < 16)
                    s->samplerKind[reg_num(tok[i + 2])] = (unsigned char)((tok[i + 1] >> 27) & 15);
            }
        } else if (!instruction2(s, op, (t >> 16) & 0xff, tok + i + 1, n)) return 0;
        i += 1 + n;
    }
    return fail(s, RbPsTruncated, count);
}

static unsigned int translate2(PS *s, const unsigned int *tokens, unsigned int count, unsigned int fog)
{
    unsigned int imm = 3, k, log2e;
    put(&s->t, "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL IN[1], GENERIC[1], LINEAR\n");
    for (k = 0; k < RB_PS2_COORD_SETS; ++k) {
        put(&s->t, "DCL IN["); number(&s->t, 2 + k); put(&s->t, "], GENERIC["); number(&s->t, 2 + k);
        put(&s->t, "], PERSPECTIVE\n");
    }
    if ((fog & 3) == RB_PS_FOG_VERTEX) put(&s->t, "DCL IN[10], GENERIC[10], PERSPECTIVE\n");
    if ((fog & 3) == RB_PS_FOG_TABLE) put(&s->t, "DCL IN[10], POSITION, LINEAR\n");
    put(&s->t, "DCL OUT[0], COLOR\n");
    for (k = 0; k < RB_PS2_SAMPLERS; ++k) { put(&s->t, "DCL SAMP["); number(&s->t, k); put(&s->t, "]\n"); }
    put(&s->t, fog ? "DCL CONST[0..33]\n" : "DCL CONST[0..31]\n");
    put(&s->t, "DCL TEMP[0..20]\n"
               "IMM[0] FLT32 {0x3f000000, 0x3f800000, 0x40000000, 0x40800000}\n"
               "IMM[1] FLT32 {0x41000000, 0x3e800000, 0x3e000000, 0xbf800000}\n"
               "IMM[2] FLT32 {0x00000000, 0x00000000, 0x00000000, 0x00000000}\n");
    if (!walk2(s, tokens, count, 0, &imm)) return 0;
    log2e = imm;
    if (fog) { put(&s->t, "IMM["); number(&s->t, log2e); put(&s->t, "] FLT32 {0x3fb8aa3b, 0x00000000, 0x00000000, 0x00000000}\n"); }
    for (k = 0; k < 12; ++k) {
        char r[12];
        r[0] = 0; cat(r, sizeof(r), "TEMP["); cat_number(r, sizeof(r), k); cat(r, sizeof(r), "]");
        line(s, "MOV ", r, ", IMM[2]", 0, 0, 0, 0);
    }
    line(s, "MOV TEMP[20], IMM[2]", 0, 0, 0, 0, 0, 0);
    if (!walk2(s, tokens, count, 1, &imm)) return 0;
    if (fog) fog_lines(s, fog, log2e, "TEMP[20]", RB_PS2_CONSTANTS);
    line(s, "MOV OUT[0], TEMP[20]", 0, 0, 0, 0, 0, 0);
    line(s, "END", 0, 0, 0, 0, 0, 0);
    if (s->t.overflow) return fail(s, RbPsOverflow, s->t.n);
    return s->t.n + 1;
}

/* Whether a version 1 program has texbem or texbeml, which read the bump
 * constants after c7 (the fog constants then follow those). */
static int uses_bump(const unsigned int *tokens, unsigned int count)
{
    unsigned int i;
    for (i = 1; i < count; ++i) {
        unsigned int op = tokens[i] & 0xffff;
        if (tokens[i] & 0x80000000u) continue;          /* a parameter */
        if (op == OpEnd) break;
        if (op == OpComment) { i += (tokens[i] >> 16) & 0x7fff; continue; }
        if (op == OpDef) { i += 5; continue; }
        if (op == OpTexbem || op == OpTexbeml || op == OpBem) return 1;
    }
    return 0;
}

unsigned int rb_ps_translate(const unsigned int *tokens, unsigned int count, unsigned int fog, char *out,
                             unsigned int size, RB_PS_INFO *info)
{
    PS s;
    unsigned int imm = 3, k, log2e, base;
    char *z = (char *)&s;
    for (k = 0; k < sizeof(s); ++k) z[k] = 0;
    info->samplers = info->coords = info->reason = info->detail = info->constants = info->pointSize = 0;
    s.info = info;
    s.t.p = out; s.t.size = size;
    if (size) out[0] = 0;
    s.cube = (fog >> RB_PS_CUBE_SHIFT) & 255;
    s.volume = (fog >> RB_PS_VOLUME_SHIFT) & 255;
    fog &= (1u << RB_PS_CUBE_SHIFT) - 1;
    if (count && tokens[0] == 0xffff0200u) return translate2(&s, tokens, count, fog);
    /* 1.0, which Direct3D 8.0 programs use, has the instructions of 1.1; 1.4 has six registers r0..r5,
     * six coordinate sets and samplers, texld and texcrd, and a second phase. */
    if (!count || (tokens[0] & 0xffffff00u) != 0xffff0100u || (tokens[0] & 0xff) > 4) {
        fail(&s, RbPsVersion, count ? tokens[0] : 0);
        return 0;
    }
    s.minor = tokens[0] & 0xff ? tokens[0] & 0xff : 1;
    s.bump = uses_bump(tokens, count);
    base = s.bump ? RB_PS_BUMP_CONSTANTS : RB_PS_CONSTANTS;
    if (s.bump) info->constants = RB_PS_BUMP_CONSTANTS;
    put(&s.t, "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL IN[1], GENERIC[1], LINEAR\n");
    for (k = 0; k < (s.minor == 4 ? 6u : RB_PS_COORD_SETS); ++k) {
        put(&s.t, "DCL IN["); number(&s.t, 2 + k); put(&s.t, "], GENERIC["); number(&s.t, 2 + k);
        put(&s.t, "], PERSPECTIVE\n");
    }
    if ((fog & 3) == RB_PS_FOG_VERTEX) put(&s.t, "DCL IN[10], GENERIC[10], PERSPECTIVE\n");
    if ((fog & 3) == RB_PS_FOG_TABLE) put(&s.t, "DCL IN[10], POSITION, LINEAR\n");
    put(&s.t, "DCL OUT[0], COLOR\n");
    for (k = 0; k < (s.minor == 4 ? 6u : RB_PS_COORD_SETS); ++k) { put(&s.t, "DCL SAMP["); number(&s.t, k); put(&s.t, "]\n"); }
    put(&s.t, "DCL CONST[0.."); number(&s.t, base - 1 + (fog ? 2 : 0)); put(&s.t, "]\n");
    put(&s.t, "DCL TEMP[0..18]\n"
              "IMM[0] FLT32 {0x3f000000, 0x3f800000, 0x40000000, 0x40800000}\n"
              "IMM[1] FLT32 {0x41000000, 0x3e800000, 0x3e000000, 0xbf800000}\n"
              "IMM[2] FLT32 {0x00000000, 0x00000000, 0x00000000, 0x00000000}\n");
    if (!walk(&s, tokens, count, 0, &imm)) return 0;
    log2e = imm;
    if (fog) { put(&s.t, "IMM["); number(&s.t, log2e); put(&s.t, "] FLT32 {0x3fb8aa3b, 0x00000000, 0x00000000, 0x00000000}\n"); }
    /* Registers start at zero, so a shader never reads undefined values. */
    line(&s, "MOV TEMP[0], IMM[2]", 0, 0, 0, 0, 0, 0);
    line(&s, "MOV TEMP[1], IMM[2]", 0, 0, 0, 0, 0, 0);
    if (s.minor == 4)
        for (k = 2; k < 6; ++k) { char r[16]; r[0] = 0; cat(r, sizeof(r), "TEMP["); cat_number(r, sizeof(r), k); cat(r, sizeof(r), "]"); line(&s, "MOV ", r, ", IMM[2]", 0, 0, 0, 0); }
    if (!walk(&s, tokens, count, 1, &imm)) return 0;
    flush(&s);
    if (fog) fog_lines(&s, fog, log2e, "TEMP[0]", base);
    line(&s, "MOV OUT[0], TEMP[0]", 0, 0, 0, 0, 0, 0);
    line(&s, "END", 0, 0, 0, 0, 0, 0);
    if (s.t.overflow) return fail(&s, RbPsOverflow, s.t.n);
    return s.t.n + 1;
}

/* ---- vertex shaders 1.1 and 2.0 ----
 * The vertex shader of a draw fetches its inputs itself: the draw's
 * vertices are constants from CONST[0].w on, CONST[0].z constants apart
 * (an integer), each holding the vertex's words as the declaration lays
 * them out. Registers: r0..r11 are TEMP[0..11], v0..v15 TEMP[33..48],
 * c# CONST[19+#] unless defined, a0 TEMP[55] (whole numbers; loaded into
 * ADDR[0] before each relative read), oPos TEMP[20], oFog TEMP[21], oPts
 * TEMP[22], oD0 and oD1 TEMP[23] and TEMP[24], oT0..oT7 TEMP[25..32].
 * TEMP[17..19] hold intermediate values of expanded instructions, as in
 * pixel shaders, and TEMP[49..54] those of the fetch and the position.
 *
 * CONST[0] holds the target's width and height (IEEE) and the two integers
 * above, CONST[1] the viewport's x, y, width and height and CONST[2] its
 * minimum and maximum z (all IEEE). oPos goes through the viewport to the
 * target's clip space, keeping w, so the host clips as Direct3D does. */
enum { VsPos = 20, VsFog = 21, VsColors = 23, VsTexOut = 25, VsInputs = 33, VsAddress = 55 };
enum { OpVsSlt = 12, OpVsSge = 13, OpVsLit = 16, OpVsDst = 17, OpVsSgn = 34, OpVsMova = 46,
       OpVsDefb = 47, OpVsDefi = 48, OpVsExpp = 78, OpVsLogp = 79, OpVsCall = 25, OpVsCallnz = 26, OpVsLoop = 27,
       OpVsRet = 28, OpVsEndLoop = 29, OpVsLabel = 30, OpVsRep = 38, OpVsEndRep = 39, OpVsIf = 40, OpVsElse = 42,
       OpVsEndif = 43 };
enum { VsCounters = 56, VsLoopRegs = 60, VsMaxLoops = 4, VsMaxCalls = 4 };

/* The IEEE single bits of a whole number (exact below 2^24 in magnitude). */
unsigned int rb_float_bits(int v)
{
    unsigned int m = v < 0 ? 0u - (unsigned int)v : (unsigned int)v, e = 150, sign = v < 0 ? 0x80000000u : 0;
    if (!m) return 0;
    while (m >= 0x1000000u) { m >>= 1; ++e; }
    while (!(m & 0x800000u)) { m <<= 1; --e; }
    return sign | e << 23 | (m & 0x7fffffu);
}


static void reg_text(char *out, unsigned int size, const char *file, unsigned int n)
{
    cat(out, size, file); cat(out, size, "["); cat_number(out, size, n); cat(out, size, "]");
}

static int vreg_name(PS *s, unsigned int token, unsigned int add, char *out, unsigned int size)
{
    unsigned int type = reg2_type(token), num = reg_num(token) + add;
    switch (type) {
    case Reg2Temp:
        if (num > 11) return fail(s, RbPsRegister, token);
        reg_text(out, size, "TEMP", num);
        return 1;
    case Reg2Input:
        if (num > 15) return fail(s, RbPsRegister, token);
        reg_text(out, size, "TEMP", VsInputs + num);
        return 1;
    case Reg2Const:
        if (num >= RB_VS_CONSTANTS) return fail(s, RbPsRegister, token);
        if (token & 0x2000) {
            /* c[a0.? + n]: the component is in bits 14-15 (see vertex_walk). */
            static const char *const load[4] = { "ARL ADDR[0].x, TEMP[55].xxxx", "ARL ADDR[0].x, TEMP[55].yyyy",
                                                 "ARL ADDR[0].x, TEMP[55].zzzz", "ARL ADDR[0].x, TEMP[55].wwww" };
            if (s->relLoop) {   /* c[aL + n]: the innermost loop's register */
                char r[16];
                r[0] = 0;
                reg_text(r, sizeof(r), "TEMP", VsLoopRegs + s->loops - 1);
                line(s, "ARL ADDR[0].x, ", r, ".xxxx", 0, 0, 0, 0);
            } else line(s, load[(token >> 14) & 3], 0, 0, 0, 0, 0, 0);
            cat(out, size, "CONST[ADDR[0].x+"); cat_number(out, size, RB_VS_CONST_BASE + num); cat(out, size, "]");
            s->info->constants = RB_VS_CONSTANTS;
            return 1;
        }
        if (defined(s, num)) { reg_text(out, size, "IMM", s->defImm[num]); return 1; }
        reg_text(out, size, "CONST", RB_VS_CONST_BASE + num);
        if (num + 1 > s->info->constants) s->info->constants = num + 1;
        return 1;
    case RegVsRastOut:
        if (num > 2) return fail(s, RbPsRegister, token);
        reg_text(out, size, "TEMP", VsPos + num);
        return 1;
    case RegVsAttrOut:
        if (num > 1) return fail(s, RbPsRegister, token);
        reg_text(out, size, "TEMP", VsColors + num);
        return 1;
    case RegVsTexOut:
        if (num > 7) return fail(s, RbPsRegister, token);
        reg_text(out, size, "TEMP", VsTexOut + num);
        return 1;
    }
    return fail(s, RbPsRegister, token);
}

/* An integer constant i# (rep, loop) or a boolean b# (if, callnz) as a TGSI source. defi and defb
 * values are immediates; the others are CONST[3 + n], whose x, y and z hold i#'s count, start and
 * step and whose w holds b# (1 or 0), when the driver gives them (RB_VS_FLOW). */
static int flow_const(PS *s, unsigned int token, int boolean, char *out, unsigned int size)
{
    unsigned int n = reg_num(token);
    out[0] = 0;
    if (reg2_type(token) != (unsigned int)(boolean ? RegVsConstBool : RegVsConstInt) || n > 15) return fail(s, RbPsRegister, token);
    if (boolean ? (s->defBool >> n) & 1 : (s->defInt >> n) & 1)
        reg_text(out, size, "IMM", boolean ? s->defBoolImm[n] : s->defIntImm[n]);
    else if (s->flow) reg_text(out, size, "CONST", 3 + n);
    else return fail(s, RbPsOpcode, token);
    if (boolean) cat(out, size, ".wwww");
    return 1;
}

/* The instructions only vertex shaders have; -1 for the others. */
static int vertex_instruction(PS *s, unsigned int op, const unsigned int *p, unsigned int n)
{
    char d[40], a[48], b[48], mask[6];
    const char *sat = " ";
    if ((op == OpMov || op == OpVsMova) && n >= 2 && reg2_type(p[0]) == RegVsAddr) {
        /* a0: mov rounds toward minus infinity, mova to the nearest. */
        if (reg_num(p[0]) || !source2(s, p[1], 0, 0, a, sizeof(a))) return fail(s, RbPsRegister, p[0]);
        mask_text((p[0] >> 16) & 15 ? (p[0] >> 16) & 15 : 15, mask, sizeof(mask));
        if (op == OpVsMova) {
            line(s, "ADD TEMP[50], ", a, ", IMM[0].xxxx", 0, 0, 0, 0);
            line(s, "FLR TEMP[55]", mask, ", TEMP[50]", 0, 0, 0, 0);
        } else line(s, "FLR TEMP[55]", mask, ", ", a, 0, 0, 0);
        return 1;
    }
    switch (op) {
    case OpVsSlt: case OpVsSge: case OpVsDst:
        if (n < 3 || !dest2(s, p[0], d, sizeof(d), &sat) || !source2(s, p[1], 0, 0, a, sizeof(a)) ||
            !source2(s, p[2], 0, 0, b, sizeof(b))) return 0;
        line(s, op == OpVsSlt ? "SLT" : op == OpVsSge ? "SGE" : "DST", sat, d, ", ", a, ", ", b);
        return 1;
    case OpVsLit: case OpVsSgn:
        if (n < 2 || !dest2(s, p[0], d, sizeof(d), &sat) || !source2(s, p[1], 0, 0, a, sizeof(a))) return 0;
        line(s, op == OpVsLit ? "LIT" : "SSG", sat, d, ", ", a, 0, 0);
        return 1;
    case OpVsExpp: case OpVsLogp:
        /* Version 1: the four results of the partial-precision forms; 2: one value. */
        if (n < 2 || !dest2(s, p[0], d, sizeof(d), &sat) || !source2(s, p[1], 0, 1, a, sizeof(a))) return 0;
        if (op == OpVsLogp) absolute(a, b, sizeof(b));
        line(s, op == OpVsExpp ? (s->version < 2 ? "EXP" : "EX2") : (s->version < 2 ? "LOG" : "LG2"), sat, d, ", ",
             op == OpVsLogp ? b : a, 0, 0);
        return 1;
    case OpVsRep: case OpVsLoop: {
        /* Static flow control of vs_2_0. Through the Direct3D 8 interface the Direct3D 9 runtime
         * removes defi and defb and passes no integer or boolean constants, so counts and conditions
         * are known only through the Direct3D 9 interface (RB_VS_FLOW). A loop counts down in
         * TEMP[56+n]; loop also steps aL in TEMP[60+n]. */
        unsigned int depth = s->loops;
        char t[16], l[16];
        if (depth >= VsMaxLoops || n < (op == OpVsLoop ? 2u : 1u)) return fail(s, RbPsOpcode, op);
        if (op == OpVsLoop && reg2_type(p[0]) != RegVsLoop) return fail(s, RbPsRegister, p[0]);
        if (!flow_const(s, p[op == OpVsLoop ? 1 : 0], 0, s->loopInt[depth], sizeof(s->loopInt[depth]))) return 0;
        t[0] = l[0] = 0;
        reg_text(t, sizeof(t), "TEMP", VsCounters + depth);
        reg_text(l, sizeof(l), "TEMP", VsLoopRegs + depth);
        line(s, "MOV ", t, ".x, ", s->loopInt[depth], ".xxxx", 0, 0);
        if (op == OpVsLoop) line(s, "MOV ", l, ".x, ", s->loopInt[depth], ".yyyy", 0, 0);
        line(s, "BGNLOOP", 0, 0, 0, 0, 0, 0);
        line(s, "SGE ", t, ".y, IMM[2].xxxx, ", t, ".xxxx", 0, 0);
        line(s, "IF ", t, ".yyyy", 0, 0, 0, 0);
        line(s, "BRK", 0, 0, 0, 0, 0, 0);
        line(s, "ENDIF", 0, 0, 0, 0, 0, 0);
        s->loopIsLoop[depth] = op == OpVsLoop;
        s->loops = depth + 1;
        return 1;
    }
    case OpVsEndRep: case OpVsEndLoop: {
        unsigned int depth;
        char t[16], l[16];
        if (!s->loops || s->loopIsLoop[s->loops - 1] != (unsigned int)(op == OpVsEndLoop)) return fail(s, RbPsOpcode, op);
        depth = --s->loops;
        t[0] = l[0] = 0;
        reg_text(t, sizeof(t), "TEMP", VsCounters + depth);
        reg_text(l, sizeof(l), "TEMP", VsLoopRegs + depth);
        if (op == OpVsEndLoop) line(s, "ADD ", l, ".x, ", l, ".xxxx, ", s->loopInt[depth], ".zzzz");
        line(s, "ADD ", t, ".x, ", t, ".xxxx, IMM[1].wwww", 0, 0);
        line(s, "ENDLOOP", 0, 0, 0, 0, 0, 0);
        return 1;
    }
    case OpVsIf:
        if (n < 1 || !flow_const(s, p[0], 1, a, sizeof(a))) return 0;
        line(s, "IF ", a, 0, 0, 0, 0, 0);
        ++s->ifs;
        return 1;
    case OpVsElse: case OpVsEndif:
        if (!s->ifs) return fail(s, RbPsOpcode, op);
        if (op == OpVsEndif) --s->ifs;
        line(s, op == OpVsElse ? "ELSE" : "ENDIF", 0, 0, 0, 0, 0, 0);
        return 1;
    case OpVsCall: case OpVsCallnz: case OpVsRet: case OpVsLabel:   /* vertex_walk inlines subroutines */
    case 41: case 44: case 45:   /* if_comp, break, breakc: vs_2_x */
        return fail(s, RbPsOpcode, op);
    }
    return -1;
}

/* Operand count of a version 1 instruction, whose token carries none. */
static int vertex1_length(unsigned int op)
{
    switch (op) {
    case OpNop: return 0;
    case OpMov: case Op2Rcp: case Op2Rsq: case Op2Exp: case Op2Log: case OpVsLit: case Op2Frc:
    case OpVsExpp: case OpVsLogp: case Op2Dcl: return 2;
    case OpAdd: case OpSub: case OpMul: case OpDp3: case OpDp4: case Op2Min: case Op2Max: case OpVsSlt:
    case OpVsSge: case OpVsDst: case Op2M4x4: case Op2M4x3: case Op2M3x4: case Op2M3x3: case Op2M3x2: return 3;
    case OpMad: return 4;
    case OpDef: return 5;
    }
    return -1;
}

/* Pass 0: def, defi and defb constants, and where each label's code starts. Pass 1: the
 * instructions from start to the end, or to a ret or label (the end of the main program), or for a
 * subroutine (sub) to its ret; calls are inlined. A shader model 2 relative source is followed by
 * its address token, whose component is kept in bits 14-15 of the source (aL sets relLoop). */
static int vertex_walk(PS *s, const unsigned int *tok, unsigned int count, int pass, unsigned int *imm,
                       unsigned int start, int sub)
{
    unsigned int i = start, ifs = s->ifs, loops = s->loops;
    while (i < count) {
        unsigned int t = tok[i], op = t & 0xffff, n, k, m, ops[8];
        if (t == OpEnd) return pass == 0 || (!sub && !s->ifs && !s->loops) ? 1 : fail(s, RbPsOpcode, t);
        if (op == OpComment) { i += 1 + ((t >> 16) & 0x7fff); continue; }
        if (t & 0x90000000u) return fail(s, RbPsOpcode, t);
        if (s->version >= 2) n = (t >> 24) & 15;
        else {
            int len = vertex1_length(op);
            if (len < 0) return fail(s, RbPsOpcode, t);
            n = (unsigned int)len;
        }
        if (i + 1 + n > count) return fail(s, RbPsTruncated, i);
        /* oPts as a destination: the program sets the point size. */
        if (pass == 0 && n && reg2_type(tok[i + 1]) == RegVsRastOut && reg_num(tok[i + 1]) == 2 &&
            op != OpDef && op != OpVsDefb && op != OpVsDefi && op != Op2Dcl && op != OpVsLabel && op != OpVsRet)
            s->info->pointSize = 1;
        if (pass == 1 && s->version >= 2 && (op == OpVsRet || op == OpVsLabel)) {
            /* The end of the main program or of a subroutine, with all it opened closed. */
            if (s->ifs != ifs || s->loops != loops || (sub && op == OpVsLabel)) return fail(s, RbPsOpcode, t);
            return 1;
        }
        if (op == OpDef || op == OpVsDefb || op == OpVsDefi || op == Op2Dcl || op == OpVsLabel || op == OpVsRet) {
            if (pass == 0 && op == OpDef) {
                unsigned int c = reg_num(tok[i + 1]);
                if (n != 5 || reg2_type(tok[i + 1]) != Reg2Const || c >= RB_VS_CONSTANTS || *imm > 255)
                    return fail(s, RbPsRegister, tok[i + 1]);
                define(s, c);
                s->defImm[c] = (unsigned char)(*imm)++;
                put(&s->t, "IMM["); number(&s->t, s->defImm[c]); put(&s->t, "] FLT32 {");
                hex(&s->t, tok[i + 2]); put(&s->t, ", "); hex(&s->t, tok[i + 3]); put(&s->t, ", ");
                hex(&s->t, tok[i + 4]); put(&s->t, ", "); hex(&s->t, tok[i + 5]); put(&s->t, "}\n");
            } else if (pass == 0 && (op == OpVsDefi || op == OpVsDefb) && s->version >= 2) {
                /* i# as floats (count, start, step); b# in w as 1 or 0. */
                unsigned int c = reg_num(tok[i + 1]);
                if (n != (op == OpVsDefi ? 5u : 2u) || c > 15 || *imm > 255 ||
                    reg2_type(tok[i + 1]) != (unsigned int)(op == OpVsDefi ? RegVsConstInt : RegVsConstBool))
                    return fail(s, RbPsRegister, tok[i + 1]);
                put(&s->t, "IMM["); number(&s->t, *imm); put(&s->t, "] FLT32 {");
                if (op == OpVsDefi) {
                    s->defInt |= 1u << c;
                    s->defIntImm[c] = (unsigned char)*imm;
                    hex(&s->t, rb_float_bits((int)tok[i + 2])); put(&s->t, ", ");
                    hex(&s->t, rb_float_bits((int)tok[i + 3])); put(&s->t, ", ");
                    hex(&s->t, rb_float_bits((int)tok[i + 4])); put(&s->t, ", 0x00000000}\n");
                } else {
                    s->defBool |= 1u << c;
                    s->defBoolImm[c] = (unsigned char)*imm;
                    put(&s->t, "0x00000000, 0x00000000, 0x00000000, ");
                    hex(&s->t, tok[i + 2] ? 0x3f800000u : 0); put(&s->t, "}\n");
                }
                ++*imm;
            } else if (pass == 0 && op == OpVsLabel && s->version >= 2) {
                unsigned int l = reg_num(tok[i + 1]);
                if (n != 1 || l > 15 || reg2_type(tok[i + 1]) != RegVsLabel) return fail(s, RbPsRegister, tok[i + 1]);
                s->labelAt[l] = i + 1 + n;
            }
        } else if (pass == 1) {
            int a0 = 0;
            s->relLoop = 0;
            for (k = 0, m = 0; k < n && m < 8; ++k) {
                unsigned int o = tok[i + 1 + k] & ~0xc000u;
                if (s->version >= 2 && m > 0 && (o & 0x2000)) {
                    unsigned int at = k + 1 < n ? reg2_type(tok[i + 2 + k]) : ~0u;
                    if (at == RegVsAddr) a0 = 1;
                    else if (at == RegVsLoop && s->loops) s->relLoop = 1;
                    else return fail(s, RbPsRegister, o);
                    if (a0 && s->relLoop) return fail(s, RbPsRegister, o);   /* a0 and aL in one instruction */
                    o |= ((tok[i + 2 + k] >> 16) & 3) << 14;
                    ++k;
                }
                ops[m++] = o;
            }
            if (op == OpVsCall || op == OpVsCallnz) {
                /* Inlined: vs_2_0 has no recursion, and its calls nest at most VsMaxCalls deep. */
                unsigned int l = m ? reg_num(ops[0]) : 16;
                char b[48];
                int ok;
                if (s->version < 2 || l > 15 || reg2_type(ops[0]) != RegVsLabel || !s->labelAt[l] || s->calls >= VsMaxCalls ||
                    (op == OpVsCallnz && (m < 2 || !flow_const(s, ops[1], 1, b, sizeof(b))))) return fail(s, RbPsOpcode, t);
                if (op == OpVsCallnz) line(s, "IF ", b, 0, 0, 0, 0, 0);
                ++s->calls;
                ok = vertex_walk(s, tok, count, 1, imm, s->labelAt[l], 1);
                --s->calls;
                if (!ok) return 0;
                if (op == OpVsCallnz) line(s, "ENDIF", 0, 0, 0, 0, 0, 0);
            } else if (!instruction2(s, op, 0, ops, m)) return 0;
        }
        i += 1 + n;
    }
    return fail(s, RbPsTruncated, count);
}

/* Fetch input register reg, of declaration type kind, from word offset k
 * of the vertex. */
static int fetch_one(PS *s, unsigned int reg, unsigned int kind, unsigned int k)
{
    static const char letters[] = "xyzw";
    char dst[16], src[48], src2[48];
    dst[0] = 0;
    reg_text(dst, sizeof(dst), "TEMP", VsInputs + reg);
    if (kind <= 3) {               /* FLOAT1..FLOAT4 */
        unsigned int count = kind + 1, j = 0;
        while (j < count) {
            unsigned int q = (k + j) / 4, at;
            char mask[6], sw[6];
            mask[0] = sw[0] = '.';
            for (at = 1; at < 5; ++at) sw[at] = letters[(k + j) % 4];
            for (at = 1; j < count && (k + j) / 4 == q; ++j) {
                mask[at++] = letters[j];
                sw[1 + j] = letters[(k + j) % 4];
            }
            mask[at] = 0; sw[5] = 0;
            src[0] = 0;
            cat(src, sizeof(src), "CONST[ADDR[0].x+"); cat_number(src, sizeof(src), q); cat(src, sizeof(src), "]");
            line(s, "MOV ", dst, mask, ", ", src, sw, 0);
        }
        return 1;
    }
    src[0] = src2[0] = 0;
    {
        static const char *const rep[4] = { "].xxxx", "].yyyy", "].zzzz", "].wwww" };
        cat(src, sizeof(src), "CONST[ADDR[0].x+"); cat_number(src, sizeof(src), k / 4); cat(src, sizeof(src), rep[k % 4]);
        cat(src2, sizeof(src2), "CONST[ADDR[0].x+"); cat_number(src2, sizeof(src2), (k + 1) / 4);
        cat(src2, sizeof(src2), rep[(k + 1) % 4]);
    }
    switch (kind) {
    case 4:                        /* D3DCOLOR: ARGB bytes to (r, g, b, a) in 0..1 */
    case 5:                        /* UBYTE4: bytes 0..3 as they are */
        line(s, "USHR TEMP[50], ", src, kind == 4 ? ", IMM[4]" : ", IMM[5]", 0, 0, 0, 0);
        line(s, "AND TEMP[50], TEMP[50], IMM[6].xxxx", 0, 0, 0, 0, 0, 0);
        if (kind == 4) {
            line(s, "U2F TEMP[50], TEMP[50]", 0, 0, 0, 0, 0, 0);
            line(s, "MUL ", dst, ", TEMP[50], IMM[7].xxxx", 0, 0, 0, 0);
        } else line(s, "U2F ", dst, ", TEMP[50]", 0, 0, 0, 0);
        return 1;
    case 6: case 7:                /* SHORT2, SHORT4: signed halves */
        line(s, "SHL TEMP[50].x, ", src, ", IMM[6].yyyy", 0, 0, 0, 0);
        line(s, "ISHR TEMP[50].x, TEMP[50].xxxx, IMM[6].yyyy", 0, 0, 0, 0, 0, 0);
        line(s, "ISHR TEMP[50].y, ", src, ", IMM[6].yyyy", 0, 0, 0, 0);
        if (kind == 7) {
            line(s, "SHL TEMP[50].z, ", src2, ", IMM[6].yyyy", 0, 0, 0, 0);
            line(s, "ISHR TEMP[50].z, TEMP[50].zzzz, IMM[6].yyyy", 0, 0, 0, 0, 0, 0);
            line(s, "ISHR TEMP[50].w, ", src2, ", IMM[6].yyyy", 0, 0, 0, 0);
            line(s, "I2F ", dst, ", TEMP[50]", 0, 0, 0, 0);
        } else line(s, "I2F ", dst, ".xy, TEMP[50].xyyy", 0, 0, 0, 0);
        return 1;
    }
    return fail(s, RbPsRegister, kind);
}

/* With RB_VS_ATTRIBUTES: input register reg, of declaration type kind at word k of its stream's
 * vertex, is the next vertex attribute. The host's vertex fetch converts it (and fills what a
 * short element leaves out with 0, 0, 0, 1, as Direct3D does); a D3DCOLOR comes as its bytes. */
static int attribute_one(PS *s, unsigned int reg, unsigned int kind, unsigned int k)
{
    char dst[16], src[16];
    unsigned int n = s->info->elementCount;
    if (n >= RB_VS_ELEMENTS) return fail(s, RbPsRegister, reg);
    dst[0] = src[0] = 0;
    reg_text(dst, sizeof(dst), "TEMP", VsInputs + reg);
    reg_text(src, sizeof(src), "IN", n);
    s->info->elements[n] = RB_VS_ELEMENT(kind, s->stream, k * 4);
    s->info->elementCount = n + 1;
    line(s, "MOV ", dst, ", ", src, kind == 4 ? ".zyxw" : 0, 0, 0);
    return 1;
}

/* The registers a declaration feeds (its STREAMDATA tokens that are not skips). */
static unsigned int decl_registers(const unsigned int *decl, unsigned int count)
{
    unsigned int i, n = 0;
    for (i = 0; i < count && decl[i] != 0xffffffffu; ++i)
        if (decl[i] >> 29 == 2 && !(decl[i] & 0x10000000u)) ++n;
    return n;
}

/* The Direct3D 8 declaration tokens: streams, registers and skips. */
static int fetch(PS *s, const unsigned int *decl, unsigned int count)
{
    static const unsigned char words[8] = { 1, 2, 3, 4, 1, 1, 1, 2 };
    unsigned int i, offset = 0;
    for (i = 0; i < count; ++i) {
        unsigned int t = decl[i];
        if (t == 0xffffffffu) return 1;
        switch (t >> 29) {
        case 0: break;                                         /* NOP */
        case 1: {                                              /* STREAM n (not the tessellator) */
            unsigned int stream = t & 0x1fffffffu;
            if (stream >= RB_VS_STREAMS || (stream && !s->streamBases && !s->attributes)) return fail(s, RbPsRegister, t);
            s->stream = stream;
            offset = s->attributes ? 0 : s->streamBases ? s->streamBases[stream] : 0;
            break;
        }
        case 2:                                                /* STREAMDATA */
            if (t & 0x10000000u) { offset += (t >> 16) & 0xf; break; }      /* SKIP */
            if ((t & 0x1f) > 15 || ((t >> 16) & 0xf) > 7) return fail(s, RbPsRegister, t);
            if (s->attributes ? !attribute_one(s, t & 0x1f, (t >> 16) & 0xf, offset)
                              : !fetch_one(s, t & 0x1f, (t >> 16) & 0xf, offset)) return 0;
            offset += words[(t >> 16) & 7];
            if (offset > s->info->coords) s->info->coords = offset;   /* words of the vertex read */
            break;
        default:                                               /* constants, tessellator */
            return fail(s, RbPsRegister, t);
        }
        if (offset > 4 * RB_VS_VERTEX_CONSTANTS) return fail(s, RbPsRegister, t);
    }
    return fail(s, RbPsTruncated, count);
}

unsigned int rb_vs_translate(const unsigned int *tokens, unsigned int count, const unsigned int *decl,
                             unsigned int declCount, const unsigned int *streamBases, unsigned int flags, char *out,
                             unsigned int size, RB_PS_INFO *info)
{
    static const char *const prologue[] = {
        "UMAD TEMP[49].x, SV[0].xxxx, CONST[0].zzzz, CONST[0].wwww", "UARL ADDR[0].x, TEMP[49].xxxx",
        "MOV TEMP[20], IMM[3]", "MOV TEMP[21], IMM[3].wwww", "MOV TEMP[22], IMM[2]", "MOV TEMP[23], IMM[3].wwww",
        "MOV TEMP[24], IMM[2]", "MOV TEMP[55], IMM[2]"
    };
    static const char *const epilogue[] = {
        "RCP TEMP[51].x, CONST[0].xxxx", "RCP TEMP[51].y, CONST[0].yyyy",
        "MUL TEMP[52].xy, CONST[1].zwww, TEMP[51].xyyy",
        "MAD TEMP[53].xy, CONST[1].xyyy, IMM[0].zzzz, CONST[1].zwww",
        "ADD TEMP[53].xy, TEMP[53].xyyy, IMM[0].yyyy",
        "MAD TEMP[53].xy, TEMP[53].xyyy, TEMP[51].xyyy, -IMM[0].yyyy",
        "ADD TEMP[52].z, CONST[2].yyyy, -CONST[2].xxxx", "MUL TEMP[52].z, TEMP[52].zzzz, IMM[0].zzzz",
        "MAD TEMP[53].z, CONST[2].xxxx, IMM[0].zzzz, -IMM[0].yyyy",
        "MUL TEMP[54].xyz, TEMP[20].xyzz, TEMP[52].xyzz", "MOV TEMP[54].y, -TEMP[54].yyyy",
        "MAD OUT[0].xyz, TEMP[20].wwww, TEMP[53].xyzz, TEMP[54].xyzz", "MOV OUT[0].w, TEMP[20].wwww",
        "MOV_SAT OUT[1], TEMP[23]", "MOV_SAT OUT[2], TEMP[24]"
    };
    PS s;
    unsigned int imm = 8, k;
    char *z = (char *)&s, r[16];
    for (k = 0; k < sizeof(s); ++k) z[k] = 0;
    info->samplers = info->coords = info->reason = info->detail = info->constants = info->pointSize = 0;
    info->elementCount = 0;
    s.info = info;
    s.vertex = 1;
    s.streamBases = streamBases;
    s.attributes = (flags & RB_VS_ATTRIBUTES) != 0;
    s.flow = (flags & RB_VS_FLOW) != 0;
    s.t.p = out; s.t.size = size;
    if (size) out[0] = 0;
    if (!count || (tokens[0] & 0xffff0000u) != 0xfffe0000u ||
        (tokens[0] != 0xfffe0100u && tokens[0] != 0xfffe0101u && tokens[0] != 0xfffe0200u)) {
        fail(&s, RbPsVersion, count ? tokens[0] : 0);
        return 0;
    }
    s.version = (tokens[0] >> 8) & 0xff;
    if (s.attributes) {
        /* The attributes, then the constants: the shader's and the six clip planes after them. */
        unsigned int inputs = decl_registers(decl, declCount);
        if (inputs > RB_VS_ELEMENTS) { fail(&s, RbPsRegister, inputs); return 0; }
        put(&s.t, "VERT\n");
        for (k = 0; k < inputs; ++k) { put(&s.t, "DCL IN["); number(&s.t, k); put(&s.t, "]\n"); }
        put(&s.t, "DCL CONST[0..");
        number(&s.t, RB_VS_CONST_BASE + RB_VS_CONSTANTS + 6 - 1);
    } else {
        put(&s.t, "VERT\nDCL SV[0], VERTEXID\nDCL CONST[0..");
        number(&s.t, RB_VS_CONST_BASE + RB_VS_CONSTANTS + RB_VS_VERTEX_CONSTANTS * RB_VS_VERTICES - 1);
    }
    put(&s.t, "]\nDCL ADDR[0]\nDCL TEMP[0..63]\nDCL OUT[0], POSITION\n");
    for (k = 0; k < 11; ++k) {
        put(&s.t, "DCL OUT["); number(&s.t, k + 1); put(&s.t, "], GENERIC["); number(&s.t, k); put(&s.t, "]\n");
    }
    if (flags & RB_VS_CLIP) put(&s.t, "DCL OUT[12], CLIPDIST[0]\nDCL OUT[13], CLIPDIST[1]\n");
    put(&s.t, "IMM[0] FLT32 {0x3f000000, 0x3f800000, 0x40000000, 0x40800000}\n"
              "IMM[1] FLT32 {0x41000000, 0x3e800000, 0x3e000000, 0xbf800000}\n"
              "IMM[2] FLT32 {0x00000000, 0x00000000, 0x00000000, 0x00000000}\n"
              "IMM[3] FLT32 {0x00000000, 0x00000000, 0x00000000, 0x3f800000}\n"
              "IMM[4] UINT32 {16, 8, 0, 24}\n"
              "IMM[5] UINT32 {0, 8, 16, 24}\n"
              "IMM[6] UINT32 {255, 16, 0, 0}\n"
              "IMM[7] FLT32 {0x3b808081, 0x00000000, 0x00000000, 0x00000000}\n");
    if (!vertex_walk(&s, tokens, count, 0, &imm, 1, 0)) return 0;
    if (info->pointSize) put(&s.t, "DCL OUT[14], PSIZE\n");
    /* The first two lines find the vertex in the constants; attributes need neither. */
    for (k = s.attributes ? 2 : 0; k < sizeof(prologue) / sizeof(*prologue); ++k) line(&s, prologue[k], 0, 0, 0, 0, 0, 0);
    for (k = 0; k < 12; ++k) { r[0] = 0; reg_text(r, sizeof(r), "TEMP", k); line(&s, "MOV ", r, ", IMM[2]", 0, 0, 0, 0); }
    for (k = 0; k < 8; ++k) { r[0] = 0; reg_text(r, sizeof(r), "TEMP", VsTexOut + k); line(&s, "MOV ", r, ", IMM[3]", 0, 0, 0, 0); }
    for (k = 0; k < 16; ++k) { r[0] = 0; reg_text(r, sizeof(r), "TEMP", VsInputs + k); line(&s, "MOV ", r, ", IMM[3]", 0, 0, 0, 0); }
    if (!fetch(&s, decl, declCount)) return 0;
    if (!vertex_walk(&s, tokens, count, 1, &imm, 1, 0)) return 0;
    for (k = 0; k < sizeof(epilogue) / sizeof(*epilogue); ++k) line(&s, epilogue[k], 0, 0, 0, 0, 0, 0);
    if (flags & RB_VS_FOG_ALPHA) line(&s, "MOV_SAT OUT[2].w, TEMP[21].xxxx", 0, 0, 0, 0, 0, 0);
    for (k = 0; k < 8; ++k) {
        char o[16];
        o[0] = r[0] = 0;
        reg_text(o, sizeof(o), "OUT", 3 + k);
        reg_text(r, sizeof(r), "TEMP", VsTexOut + k);
        line(&s, "MOV ", o, ", ", r, 0, 0, 0);
    }
    line(&s, "MOV OUT[11], IMM[2]", 0, 0, 0, 0, 0, 0);
    if (flags & RB_VS_CLIP) {
        /* dot(plane, oPos) for each plane, after the shader's constants (the miniport puts them there). */
        static const char *const dist[6] = { "OUT[12].x", "OUT[12].y", "OUT[12].z", "OUT[12].w", "OUT[13].x", "OUT[13].y" };
        char cst[16];
        for (k = 0; k < 6; ++k) {
            cst[0] = 0;
            reg_text(cst, sizeof(cst), "CONST", RB_VS_CONST_BASE + info->constants + k);
            line(&s, "DP4 ", dist[k], ", TEMP[20], ", cst, 0, 0, 0);
        }
        line(&s, "MOV OUT[13].zw, IMM[2]", 0, 0, 0, 0, 0, 0);
    }
    if (info->pointSize) line(&s, "MOV OUT[14].x, TEMP[22].xxxx", 0, 0, 0, 0, 0, 0);
    line(&s, "END", 0, 0, 0, 0, 0, 0);
    if (s.t.overflow) return fail(&s, RbPsOverflow, s.t.n);
    return s.t.n + 1;
}

/* ---- fixed-function texture stages as a pixel program ----
 * For draws whose vertex shader feeds the fixed-function pixel stages. The
 * inputs are those of translated pixel shaders. TEMP[0] is the current
 * colour, TEMP[1] the stage's texel, TEMP[2] and TEMP[3] the two
 * arguments, TEMP[4] the stage result, TEMP[5] the TEMP register of
 * D3DTA_TEMP, TEMP[6] and TEMP[7] scratch, TEMP[9] the texture factor
 * (unpacked from the ARGB word in CONST[0].x). Results are saturated. */
enum { TaDiffuse, TaCurrent, TaTexture, TaFactor, TaSpecular, TaTemp, TaSelect = 15, TaComplement = 16,
       TaReplicate = 32 };
enum { TopDisable = 1, TopArg1, TopArg2, TopMod, TopMod2, TopMod4, TopAdd, TopAddSigned, TopAddSigned2,
       TopSub, TopAddSmooth, TopBlendDiffuse, TopBlendTexture, TopBlendFactor, TopBlendTexturePm,
       TopBlendCurrent, TopPremodulate, TopModAlphaAddColor, TopModColorAddAlpha, TopModInvAlphaAddColor,
       TopModInvColorAddAlpha, TopBump, TopBumpLum, TopDot3 };

/* Argument a into TEMP[slot]. */
static int stage_arg(PS *s, unsigned int a, unsigned int stage, const char *slot)
{
    static const char *const src[6] = { "IN[0]", "TEMP[0]", "TEMP[1]", "TEMP[9]", "IN[1]", "TEMP[5]" };
    const char *from = (a & TaSelect) == TaCurrent && !stage ? "IN[0]" : (a & TaSelect) < 6 ? src[a & TaSelect] : 0;
    if (!from || (a & ~(TaSelect | TaComplement | TaReplicate))) return fail(s, RbPsRegister, a);
    if (a & TaComplement) line(s, "ADD ", slot, ", IMM[0].yyyy, -", from, a & TaReplicate ? ".wwww" : "", 0, 0);
    else line(s, "MOV ", slot, ", ", from, a & TaReplicate ? ".wwww" : "", 0, 0);
    return 1;
}

static int uses_texture(unsigned int op, unsigned int a1, unsigned int a2)
{
    return op != TopDisable && ((a1 & TaSelect) == TaTexture || (op != TopArg1 && (a2 & TaSelect) == TaTexture) ||
                                op == TopBlendTexture || op == TopBlendTexturePm);
}

/* One operation into TEMP[4] with the write mask m (".xyz" or ".w"). */
static int stage_op(PS *s, unsigned int op, unsigned int stage, const char *m)
{
    switch (op) {
    case TopArg1: line(s, "MOV_SAT TEMP[4]", m, ", TEMP[2]", 0, 0, 0, 0); return 1;
    case TopArg2: line(s, "MOV_SAT TEMP[4]", m, ", TEMP[3]", 0, 0, 0, 0); return 1;
    case TopMod: line(s, "MUL_SAT TEMP[4]", m, ", TEMP[2], TEMP[3]", 0, 0, 0, 0); return 1;
    case TopMod2: case TopMod4:
        line(s, "MUL TEMP[6], TEMP[2], TEMP[3]", 0, 0, 0, 0, 0, 0);
        line(s, "MUL_SAT TEMP[4]", m, op == TopMod2 ? ", TEMP[6], IMM[0].zzzz" : ", TEMP[6], IMM[0].wwww", 0, 0, 0, 0);
        return 1;
    case TopAdd: line(s, "ADD_SAT TEMP[4]", m, ", TEMP[2], TEMP[3]", 0, 0, 0, 0); return 1;
    case TopAddSigned: case TopAddSigned2:
        line(s, "ADD TEMP[6], TEMP[2], TEMP[3]", 0, 0, 0, 0, 0, 0);
        if (op == TopAddSigned) { line(s, "ADD_SAT TEMP[4]", m, ", TEMP[6], -IMM[0].xxxx", 0, 0, 0, 0); return 1; }
        line(s, "ADD TEMP[6], TEMP[6], -IMM[0].xxxx", 0, 0, 0, 0, 0, 0);
        line(s, "MUL_SAT TEMP[4]", m, ", TEMP[6], IMM[0].zzzz", 0, 0, 0, 0);
        return 1;
    case TopSub: line(s, "ADD_SAT TEMP[4]", m, ", TEMP[2], -TEMP[3]", 0, 0, 0, 0); return 1;
    case TopAddSmooth:
        line(s, "MAD TEMP[6], -TEMP[2], TEMP[3], TEMP[2]", 0, 0, 0, 0, 0, 0);
        line(s, "ADD_SAT TEMP[4]", m, ", TEMP[6], TEMP[3]", 0, 0, 0, 0);
        return 1;
    case TopBlendDiffuse: case TopBlendTexture: case TopBlendFactor: case TopBlendCurrent: {
        const char *w = op == TopBlendDiffuse ? ", IN[0].wwww, TEMP[2], TEMP[3]" :
                        op == TopBlendTexture ? ", TEMP[1].wwww, TEMP[2], TEMP[3]" :
                        op == TopBlendFactor ? ", TEMP[9].wwww, TEMP[2], TEMP[3]" :
                        stage ? ", TEMP[0].wwww, TEMP[2], TEMP[3]" : ", IN[0].wwww, TEMP[2], TEMP[3]";
        line(s, "LRP_SAT TEMP[4]", m, w, 0, 0, 0, 0);
        return 1;
    }
    case TopBlendTexturePm:
        line(s, "ADD TEMP[6], IMM[0].yyyy, -TEMP[1].wwww", 0, 0, 0, 0, 0, 0);
        line(s, "MAD_SAT TEMP[4]", m, ", TEMP[3], TEMP[6], TEMP[2]", 0, 0, 0, 0);
        return 1;
    case TopModAlphaAddColor: line(s, "MAD_SAT TEMP[4]", m, ", TEMP[2].wwww, TEMP[3], TEMP[2]", 0, 0, 0, 0); return 1;
    case TopModColorAddAlpha: line(s, "MAD_SAT TEMP[4]", m, ", TEMP[2], TEMP[3], TEMP[2].wwww", 0, 0, 0, 0); return 1;
    case TopModInvAlphaAddColor:
        line(s, "ADD TEMP[6], IMM[0].yyyy, -TEMP[2].wwww", 0, 0, 0, 0, 0, 0);
        line(s, "MAD_SAT TEMP[4]", m, ", TEMP[6], TEMP[3], TEMP[2]", 0, 0, 0, 0);
        return 1;
    case TopModInvColorAddAlpha:
        line(s, "ADD TEMP[6], IMM[0].yyyy, -TEMP[2]", 0, 0, 0, 0, 0, 0);
        line(s, "MAD_SAT TEMP[4]", m, ", TEMP[6], TEMP[3], TEMP[2].wwww", 0, 0, 0, 0);
        return 1;
    case TopDot3:
        line(s, "MAD TEMP[6], TEMP[2], IMM[0].zzzz, -IMM[0].yyyy", 0, 0, 0, 0, 0, 0);
        line(s, "MAD TEMP[7], TEMP[3], IMM[0].zzzz, -IMM[0].yyyy", 0, 0, 0, 0, 0, 0);
        line(s, "DP3_SAT TEMP[4]", m, ", TEMP[6], TEMP[7]", 0, 0, 0, 0);
        return 1;
    }
    return fail(s, RbPsOpcode, op);
}

unsigned int rb_stage_translate(const RB_STAGE_STATE *stages, unsigned int count, unsigned int specular,
                                unsigned int fog, char *out, unsigned int size, RB_PS_INFO *info)
{
    PS s;
    unsigned int i, k, log2e = 4, factor = 0, transforms = 0, base, bump = ~0u, bumpLum = 0;
    char *z = (char *)&s, tex[64];
    for (k = 0; k < sizeof(s); ++k) z[k] = 0;
    info->samplers = info->coords = info->reason = info->detail = info->constants = info->pointSize = 0;
    s.info = info;
    s.t.p = out; s.t.size = size;
    if (size) out[0] = 0;
    if (count > RB_PS2_COORD_SETS) count = RB_PS2_COORD_SETS;   /* a stage has a sampler and a coordinate set */
    for (i = 0; i < count && stages[i].colorOp != TopDisable; ++i) {
        const RB_STAGE_STATE *st = &stages[i];
        if ((st->colorArg1 & TaSelect) == TaFactor || (st->colorArg2 & TaSelect) == TaFactor ||
            (st->alphaArg1 & TaSelect) == TaFactor || (st->alphaArg2 & TaSelect) == TaFactor ||
            st->colorOp == TopBlendFactor || st->alphaOp == TopBlendFactor) factor = 1;
        if (st->transform & 0xff) {
            if (i >= RB_STAGE_TRANSFORMS || (st->transform & 0xff) > 4 || (st->transform & ~(0xffu | RB_TTFF_PROJECTED)))
                return fail(&s, RbPsRegister, st->transform);
            transforms = 1;
        }
        /* A bump stage keeps its matrix where its texture matrix would be. */
        if (st->colorOp == TopBump || st->colorOp == TopBumpLum) {
            if (i >= RB_STAGE_TRANSFORMS || (st->transform & 0xff)) return fail(&s, RbPsOpcode, st->colorOp);
            transforms = 1;
        }
    }
    count = i;
    base = transforms ? RB_PS2_CONSTANTS : RB_PS_CONSTANTS;
    if (transforms) info->constants = RB_PS2_CONSTANTS;
    put(&s.t, "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL IN[1], GENERIC[1], LINEAR\n");
    for (k = 0; k < RB_PS2_COORD_SETS; ++k) {
        put(&s.t, "DCL IN["); number(&s.t, 2 + k); put(&s.t, "], GENERIC["); number(&s.t, 2 + k);
        put(&s.t, "], PERSPECTIVE\n");
    }
    if ((fog & 3) == RB_PS_FOG_VERTEX) put(&s.t, "DCL IN[10], GENERIC[10], PERSPECTIVE\n");
    if ((fog & 3) == RB_PS_FOG_TABLE) put(&s.t, "DCL IN[10], POSITION, LINEAR\n");
    put(&s.t, "DCL OUT[0], COLOR\n");
    for (k = 0; k < RB_PS2_COORD_SETS; ++k) { put(&s.t, "DCL SAMP["); number(&s.t, k); put(&s.t, "]\n"); }
    put(&s.t, "DCL CONST[0.."); number(&s.t, base - 1 + (fog ? 2 : 0)); put(&s.t, "]\n");
    put(&s.t, "DCL TEMP[0..11]\n"
              "IMM[0] FLT32 {0x3f000000, 0x3f800000, 0x40000000, 0x40800000}\n"
              "IMM[1] UINT32 {16, 8, 0, 24}\n"
              "IMM[2] FLT32 {0x00000000, 0x00000000, 0x00000000, 0x3b808081}\n"
              "IMM[3] UINT32 {255, 0, 0, 0}\n");
    if (fog) put(&s.t, "IMM[4] FLT32 {0x3fb8aa3b, 0x00000000, 0x00000000, 0x00000000}\n");
    line(&s, "MOV TEMP[0], IN[0]", 0, 0, 0, 0, 0, 0);
    line(&s, "MOV TEMP[5], IMM[2].xxxx", 0, 0, 0, 0, 0, 0);
    if (factor) {
        line(&s, "USHR TEMP[9], CONST[0].xxxx, IMM[1]", 0, 0, 0, 0, 0, 0);
        line(&s, "AND TEMP[9], TEMP[9], IMM[3].xxxx", 0, 0, 0, 0, 0, 0);
        line(&s, "U2F TEMP[9], TEMP[9]", 0, 0, 0, 0, 0, 0);
        line(&s, "MUL TEMP[9], TEMP[9], IMM[2].wwww", 0, 0, 0, 0, 0, 0);
    }
    for (i = 0; i < count; ++i) {
        const RB_STAGE_STATE *st = &stages[i];
        unsigned int set = st->coordIndex & 0xffff, alphaOp = st->alphaOp, a1 = st->alphaArg1, a2 = st->alphaArg2;
        if (st->coordIndex >> 16 || set >= RB_PS2_COORD_SETS) return fail(&s, RbPsRegister, st->coordIndex);
        if (alphaOp == TopDisable) { alphaOp = TopArg1; a1 = TaCurrent; a2 = TaCurrent; }
        if (st->colorOp == TopBump || st->colorOp == TopBumpLum) {
            /* The bump map: du, dv and a luminance for the next stage's lookup;
             * the current colour goes on unchanged. */
            tex[0] = 0;
            cat(tex, sizeof(tex), "TEX TEMP[8], IN["); cat_number(tex, sizeof(tex), 2 + set);
            cat(tex, sizeof(tex), "], SAMP["); cat_number(tex, sizeof(tex), i); cat(tex, sizeof(tex), "], 2D");
            line(&s, tex, 0, 0, 0, 0, 0, 0);
            info->samplers |= 1u << i;
            info->coords |= 1u << set;
            bump = i;
            bumpLum = st->colorOp == TopBumpLum;
            continue;
        }
        if (uses_texture(st->colorOp, st->colorArg1, st->colorArg2) || uses_texture(alphaOp, a1, a2)) {
            char in[16];
            in[0] = 0;
            cat(in, sizeof(in), "IN["); cat_number(in, sizeof(in), 2 + set); cat(in, sizeof(in), "]");
            if (st->transform & 0xff) {
                /* The stage's texture matrix (columns in CONST[8+4i]), then the
                 * division by the last coordinate it gives. */
                static const char *const comp[4] = { ".x", ".y", ".z", ".w" };
                static const char *const last[4] = { ".xxxx", ".yyyy", ".zzzz", ".wwww" };
                unsigned int n = st->transform & 0xff, j;
                char c[16];
                line(&s, "MOV TEMP[10], IMM[2].xxxx", 0, 0, 0, 0, 0, 0);
                for (j = 0; j < n; ++j) {
                    c[0] = 0;
                    cat(c, sizeof(c), "CONST["); cat_number(c, sizeof(c), 8 + 4 * i + j); cat(c, sizeof(c), "]");
                    line(&s, "DP4 TEMP[10]", comp[j], ", ", in, ", ", c, 0);
                }
                if (st->transform & RB_TTFF_PROJECTED) {
                    line(&s, "RCP TEMP[11].x, TEMP[10]", last[n - 1], 0, 0, 0, 0, 0);
                    line(&s, "MUL TEMP[10], TEMP[10], TEMP[11].xxxx", 0, 0, 0, 0, 0, 0);
                }
                in[0] = 0;
                cat(in, sizeof(in), "TEMP[10]");
            }
            if (bump != ~0u && bump + 1 == i) {
                /* Moved by the bump stage's du and dv through its matrix (00, 01, 10, 11). */
                char m[16];
                m[0] = 0;
                cat(m, sizeof(m), "CONST["); cat_number(m, sizeof(m), 8 + 4 * bump); cat(m, sizeof(m), "]");
                if (!(st->transform & 0xff)) line(&s, "MOV TEMP[10], ", in, 0, 0, 0, 0, 0);
                line(&s, "MAD TEMP[10].xy, TEMP[8].xxxx, ", m, ".xyxy, TEMP[10]", 0, 0, 0, 0);
                line(&s, "MAD TEMP[10].xy, TEMP[8].yyyy, ", m, ".zwzw, TEMP[10]", 0, 0, 0, 0);
                in[0] = 0;
                cat(in, sizeof(in), "TEMP[10]");
            }
            tex[0] = 0;
            cat(tex, sizeof(tex), "TEX TEMP[1], "); cat(tex, sizeof(tex), in);
            cat(tex, sizeof(tex), ", SAMP["); cat_number(tex, sizeof(tex), i); cat(tex, sizeof(tex), "], 2D");
            line(&s, tex, 0, 0, 0, 0, 0, 0);
            if (bump != ~0u && bump + 1 == i && bumpLum) {
                /* BUMPENVMAPLUMINANCE: times the luminance times the scale plus the offset. */
                char l[16];
                l[0] = 0;
                cat(l, sizeof(l), "CONST["); cat_number(l, sizeof(l), 9 + 4 * bump); cat(l, sizeof(l), "]");
                line(&s, "MAD_SAT TEMP[11].x, TEMP[8].zzzz, ", l, ".xxxx, ", l, ".yyyy", 0, 0);
                line(&s, "MUL TEMP[1].xyz, TEMP[1], TEMP[11].xxxx", 0, 0, 0, 0, 0, 0);
            }
            info->samplers |= 1u << i;
            info->coords |= 1u << set;
        }
        if (!stage_arg(&s, st->colorArg1, i, "TEMP[2]") || !stage_arg(&s, st->colorArg2, i, "TEMP[3]") ||
            !stage_op(&s, st->colorOp, i, ".xyz")) return 0;
        if (st->colorOp == TopDot3) line(&s, "MOV TEMP[4].w, TEMP[4].xxxx", 0, 0, 0, 0, 0, 0);
        else if (!stage_arg(&s, a1, i, "TEMP[2]") || !stage_arg(&s, a2, i, "TEMP[3]") ||
                 !stage_op(&s, alphaOp, i, ".w")) return 0;
        line(&s, "MOV TEMP[0], TEMP[4]", 0, 0, 0, 0, 0, 0);
    }
    if (specular) line(&s, "ADD_SAT TEMP[0].xyz, TEMP[0], IN[1]", 0, 0, 0, 0, 0, 0);
    if (fog) fog_lines(&s, fog, log2e, "TEMP[0]", base);
    line(&s, "MOV OUT[0], TEMP[0]", 0, 0, 0, 0, 0, 0);
    line(&s, "END", 0, 0, 0, 0, 0, 0);
    if (s.t.overflow) return fail(&s, RbPsOverflow, s.t.n);
    return s.t.n + 1;
}
