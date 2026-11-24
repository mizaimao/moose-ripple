/* Direct3D pixel shaders 1.1 to 1.3 and 2.0 as Gallium TGSI text.
 *
 * Registers: r0 and r1 are TEMP[0] and TEMP[1], texture registers t0..t3
 * TEMP[8..11], v0 and v1 IN[0] and IN[1], texture coordinate set n IN[2+n],
 * c0..c7 CONST[0..7] unless a def gives the value, which becomes an IMM.
 * TEMP[12..14] hold modified sources, TEMP[15] a comparison, TEMP[16] and
 * TEMP[17] the results of an instruction and of the one co-issued with it,
 * and TEMP[18] the dot products of the texm3x2 and texm3x3 sequences.
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
    unsigned int defined;           /* constants given by def */
    unsigned int defImm[RB_PS2_CONSTANTS];
    unsigned char samplerKind[16];  /* shader model 2: D3DSAMPLER_TEXTURE_TYPE of each sampler */
    unsigned int written;           /* r0, r1 and t0..t3 written so far: bits 0-1, 8-11 */
    unsigned int pads;              /* texm3x2/texm3x3 dot products so far */
    /* Results waiting to be written: register, write mask, result slot. */
    char pendDest[2][24];
    unsigned int pendMask[2], pending;
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

static unsigned int reg_type(unsigned int token) { return (token >> 28) & 7; }
static unsigned int reg_num(unsigned int token) { return token & 0x7ff; }

/* The TGSI register of a Direct3D register; 0 when it does not exist. */
static int reg_name(PS *s, unsigned int type, unsigned int num, char *out, unsigned int size)
{
    out[0] = 0;
    switch (type) {
    case RegTemp:
        if (num > 1) return 0;
        cat(out, size, "TEMP["); cat_number(out, size, num); cat(out, size, "]");
        return 1;
    case RegInput:
        if (num > 1) return 0;
        cat(out, size, "IN["); cat_number(out, size, num); cat(out, size, "]");
        return 1;
    case RegConst:
        if (num >= RB_PS_CONSTANTS) return 0;
        if (s->defined & (1u << num)) { cat(out, size, "IMM["); cat_number(out, size, s->defImm[num]); }
        else { cat(out, size, "CONST["); cat_number(out, size, num); }
        cat(out, size, "]");
        return 1;
    case RegTexture:
        if (num >= RB_PS_COORD_SETS) return 0;
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

static int texture_op(PS *s, unsigned int op, const unsigned int *p)
{
    unsigned int n = 0, m = 0;
    char tex[16], coord[16], samp[16], texm[16], coordm[16], sampm[16];
    if (!texture_dest(s, p[0], &n)) return 0;
    names(n, tex, coord, samp);
    switch (op) {
    case OpTex:
        s->info->samplers |= 1u << n; s->info->coords |= 1u << n;
        line(s, "TEX ", tex, ", ", coord, ", ", samp, ", 2D");
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
        line(s, "TEX ", tex, ", TEMP[18], ", samp, ", 2D", 0, 0);
        break;
    case OpTexm3x2pad:
    case OpTexm3x3pad:
        if (!texture_source(s, p[1], &m)) return 0;
        names(m, texm, coordm, sampm);
        s->info->coords |= 1u << n;
        if (s->pads > 1) return fail(s, RbPsOpcode, op);
        line(s, "DP3 TEMP[18].", s->pads ? "y" : "x", ", ", coord, ", ", texm, 0);
        ++s->pads;
        return 1;
    case OpTexm3x2tex:
    case OpTexdp3tex:
        if (!texture_source(s, p[1], &m)) return 0;
        if (op == OpTexdp3tex && s->minor < 2) return fail(s, RbPsOpcode, op);
        if (op == OpTexm3x2tex && s->pads != 1) return fail(s, RbPsOpcode, op);
        names(m, texm, coordm, sampm);
        s->info->coords |= 1u << n; s->info->samplers |= 1u << n;
        if (op == OpTexdp3tex) {
            line(s, "DP3 TEMP[18].x, ", coord, ", ", texm, 0, 0, 0);
            line(s, "MOV TEMP[18].y, IMM[2].xxxx", 0, 0, 0, 0, 0, 0);
        } else line(s, "DP3 TEMP[18].y, ", coord, ", ", texm, 0, 0, 0);
        line(s, "TEX ", tex, ", TEMP[18], ", samp, ", 2D", 0, 0);
        s->pads = 0;
        break;
    case OpTexm3x3:
        if (s->minor < 2 || s->pads != 2) return fail(s, RbPsOpcode, op);
        if (!texture_source(s, p[1], &m)) return 0;
        names(m, texm, coordm, sampm);
        s->info->coords |= 1u << n;
        line(s, "DP3 TEMP[18].z, ", coord, ", ", texm, 0, 0, 0);
        line(s, "MOV ", tex, ".xyz, TEMP[18]", 0, 0, 0, 0);
        line(s, "MOV ", tex, ".w, IMM[0].yyyy", 0, 0, 0, 0);
        s->pads = 0;
        break;
    case OpTexdp3:
        if (s->minor < 2) return fail(s, RbPsOpcode, op);
        if (!texture_source(s, p[1], &m)) return 0;
        names(m, texm, coordm, sampm);
        s->info->coords |= 1u << n;
        line(s, "DP3 ", tex, ", ", coord, ", ", texm, 0);
        break;
    default:
        /* Bump mapping, cube and depth lookups need texture kinds or
         * states this device does not offer. */
        return fail(s, RbPsOpcode, op);
    }
    s->written |= 1u << (8 + n);
    return 1;
}

static int parameters(unsigned int op)
{
    switch (op) {
    case OpNop: case OpPhase: return 0;
    case OpTexcoord: case OpTexkill: case OpTex: case OpTexdepth: return 1;
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
        n = parameters(op);
        if (n < 0 || (t & 0x80000000u) || ((t & COISSUE) && texture_family(op)))
            return fail(s, RbPsOpcode, t);
        if (i + 1 + (unsigned int)n > count) return fail(s, RbPsTruncated, i);
        if (pass == 0) {
            if (op == OpDef) {
                unsigned int c = reg_num(tok[i + 1]);
                if (reg_type(tok[i + 1]) != RegConst || c >= RB_PS_CONSTANTS) return fail(s, RbPsRegister, tok[i + 1]);
                s->defined |= 1u << c;
                s->defImm[c] = (*imm)++;
                put(&s->t, "IMM["); number(&s->t, s->defImm[c]); put(&s->t, "] FLT32 {");
                hex(&s->t, tok[i + 2]); put(&s->t, ", "); hex(&s->t, tok[i + 3]); put(&s->t, ", ");
                hex(&s->t, tok[i + 4]); put(&s->t, ", "); hex(&s->t, tok[i + 5]); put(&s->t, "}\n");
            }
        } else if (op != OpDef && op != OpNop) {
            const unsigned int *p = tok + i + 1;
            if (!(t & COISSUE)) flush(s);
            if (op == OpPhase || op == OpBem || op == OpTexdepth) return fail(s, RbPsOpcode, op);
            if (texture_family(op)) { if (!texture_op(s, op, p)) return 0; }
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
            fog_line(s, (fog & RB_PS_FOG_RANGE) ? "MOV TEMP[2].y, IN[8].yyyy" : "MOV TEMP[2].y, IN[8].xxxx", 0, 0, 0, 0);
        else fog_line(s, (fog & RB_PS_FOG_W) ? "RCP TEMP[2].y, IN[8].wwww" : "MOV TEMP[2].y, IN[8].zzzz", 0, 0, 0, 0);
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
enum { Mod2Abs = 11, Mod2AbsNeg = 12 };

static unsigned int reg2_type(unsigned int t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }

static int reg2_name(PS *s, unsigned int token, unsigned int add, char *out, unsigned int size)
{
    unsigned int type = reg2_type(token), num = reg_num(token) + add;
    out[0] = 0;
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
        if (s->defined & (1u << num)) { cat(out, size, "IMM["); cat_number(out, size, s->defImm[num]); }
        else { cat(out, size, "CONST["); cat_number(out, size, num); }
        cat(out, size, "]");
        return 1;
    case Reg2Texture:
        if (num >= RB_PS_COORD_SETS) return fail(s, RbPsRegister, token);
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
    if (!(token & 0x80000000u) || ((token >> 24) & 15) || ((token >> 20) & 8) ||
        (type != Reg2Temp && type != Reg2ColorOut)) return fail(s, RbPsRegister, token);
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

static int instruction2(PS *s, unsigned int op, unsigned int control, const unsigned int *p, unsigned int n)
{
    char d[40], a[48], b[48], c[48], name[24];
    const char *sat = " ";
    unsigned int k, rows;
    switch (op) {
    case OpNop: case OpDef: case Op2Dcl: return 1;
    case OpTexkill:
        if (n < 1 || !reg2_name(s, p[0], 0, name, sizeof(name))) return 0;
        line(s, "KILL_IF ", name, ".xyzz", 0, 0, 0, 0);
        return 1;
    case OpTex: {
        static const char *const ops[3] = { "TEX", "TXP", "TXB" };
        /* Only 2D textures exist on this device so far. */
        static const char *const targets[5] = { 0, 0, "2D", 0, 0 };
        unsigned int sampler;
        if (n < 3 || control > 2 || reg2_type(p[2]) != Reg2Sampler) return fail(s, RbPsOpcode, op);
        sampler = reg_num(p[2]);
        if (sampler >= RB_PS_COORD_SETS || !targets[s->samplerKind[sampler] < 5 ? s->samplerKind[sampler] : 0])
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
                s->defined |= 1u << c;
                s->defImm[c] = (*imm)++;
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
    for (k = 0; k < RB_PS_COORD_SETS; ++k) {
        put(&s->t, "DCL IN["); number(&s->t, 2 + k); put(&s->t, "], GENERIC["); number(&s->t, 2 + k);
        put(&s->t, "], PERSPECTIVE\n");
    }
    if ((fog & 3) == RB_PS_FOG_VERTEX) put(&s->t, "DCL IN[8], GENERIC[6], PERSPECTIVE\n");
    if ((fog & 3) == RB_PS_FOG_TABLE) put(&s->t, "DCL IN[8], POSITION, LINEAR\n");
    put(&s->t, "DCL OUT[0], COLOR\n");
    for (k = 0; k < RB_PS_COORD_SETS; ++k) { put(&s->t, "DCL SAMP["); number(&s->t, k); put(&s->t, "]\n"); }
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

unsigned int rb_ps_translate(const unsigned int *tokens, unsigned int count, unsigned int fog, char *out,
                             unsigned int size, RB_PS_INFO *info)
{
    PS s;
    unsigned int imm = 3, k, log2e;
    char *z = (char *)&s;
    for (k = 0; k < sizeof(s); ++k) z[k] = 0;
    info->samplers = info->coords = info->reason = info->detail = 0;
    s.info = info;
    s.t.p = out; s.t.size = size;
    if (size) out[0] = 0;
    if (count && tokens[0] == 0xffff0200u) return translate2(&s, tokens, count, fog);
    if (!count || (tokens[0] & 0xffffff00u) != 0xffff0100u || (tokens[0] & 0xff) < 1 || (tokens[0] & 0xff) > 3) {
        fail(&s, RbPsVersion, count ? tokens[0] : 0);
        return 0;
    }
    s.minor = tokens[0] & 0xff;
    put(&s.t, "FRAG\nDCL IN[0], GENERIC[0], LINEAR\nDCL IN[1], GENERIC[1], LINEAR\n");
    for (k = 0; k < RB_PS_COORD_SETS; ++k) {
        put(&s.t, "DCL IN["); number(&s.t, 2 + k); put(&s.t, "], GENERIC["); number(&s.t, 2 + k);
        put(&s.t, "], PERSPECTIVE\n");
    }
    if ((fog & 3) == RB_PS_FOG_VERTEX) put(&s.t, "DCL IN[8], GENERIC[6], PERSPECTIVE\n");
    if ((fog & 3) == RB_PS_FOG_TABLE) put(&s.t, "DCL IN[8], POSITION, LINEAR\n");
    put(&s.t, "DCL OUT[0], COLOR\n");
    for (k = 0; k < RB_PS_COORD_SETS; ++k) { put(&s.t, "DCL SAMP["); number(&s.t, k); put(&s.t, "]\n"); }
    put(&s.t, fog ? "DCL CONST[0..9]\n" : "DCL CONST[0..7]\n");
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
    if (!walk(&s, tokens, count, 1, &imm)) return 0;
    flush(&s);
    if (fog) fog_lines(&s, fog, log2e, "TEMP[0]", RB_PS_CONSTANTS);
    line(&s, "MOV OUT[0], TEMP[0]", 0, 0, 0, 0, 0, 0);
    line(&s, "END", 0, 0, 0, 0, 0, 0);
    if (s.t.overflow) return fail(&s, RbPsOverflow, s.t.n);
    return s.t.n + 1;
}
