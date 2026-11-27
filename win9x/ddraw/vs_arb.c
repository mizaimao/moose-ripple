/* Direct3D vertex shaders as ARB vertex programs (vs_arb.h): vs_1_0 (Direct3D 8.0's, a subset
   of vs_1_1), vs_1_1 and vs_2_0 without flow control. ARB_vertex_program was made after
   Direct3D 8's vertex shaders, so most instructions keep their names; what it lacks is spelled
   out: |x| through ABS into a scratch register, _sat through MAX and MIN, lrp, nrm, sgn, mova
   (rounding to nearest), and relative reads through ARL from a copy of a0 (vs_2_0's a0 has four
   components, an ADDRESS register one). Self-contained C without a run-time library. MIT
   licence. */
#include "vs_arb.h"

typedef struct {
    char *p;
    unsigned int n, size;
    int overflow;
} OUT;

typedef struct {
    OUT o;
    VS_ARB *info;
    unsigned int version;               /* 1 or 2 */
    unsigned int scratch;               /* the next of s0..s2 for a source modifier */
    /* static flow control (walk): the caller's b# and i#, the shader's own (defb, defi), its
       labels, and the loops, repeats and calls it is inside */
    const VS_ARB_FLOW *flow;
    unsigned int defb_mask, defb_val, defi_mask;
    int defi[16][3];
    unsigned int label[16];
    struct { unsigned int kind, at, left; int al, step; } frame[8];
    unsigned int frames;
} VS;

static void emit(OUT *o, const char *s)
{
    while (*s) {
        if (o->n + 1 < o->size)
            o->p[o->n++] = *s;
        else
            o->overflow = 1;
        s++;
    }
}

static void emit_u(OUT *o, unsigned int v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    emit(o, b + i);
}

/* a small string buffer */
typedef struct { char s[48]; unsigned int n; } STR;

static void put(STR *b, const char *s)
{
    while (*s && b->n + 1 < sizeof b->s)
        b->s[b->n++] = *s++;
    b->s[b->n] = 0;
}

static void put_u(STR *b, unsigned int v)
{
    char t[12];
    int i = 11;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(b, t + i);
}

static int fail(VS *s, unsigned int reason, unsigned int detail)
{
    s->info->reason = reason;
    s->info->detail = detail;
    return 0;
}

static unsigned int reg_type(unsigned int t)
{
    return ((t >> 28) & 7) | ((t >> 8) & 0x18);
}

enum { RegTemp = 0, RegInput = 1, RegConst = 2, RegAddr = 3, RegRastOut = 4, RegAttrOut = 5,
       RegTexOut = 6, RegConstInt = 7, RegConstBool = 14, RegLoop = 15, RegLabel = 18 };
enum { OpNop = 0, OpMov, OpAdd, OpSub, OpMad, OpMul, OpRcp, OpRsq, OpDp3, OpDp4, OpMin, OpMax,
       OpSlt, OpSge, OpExp, OpLog, OpLit, OpDst, OpLrp, OpFrc, OpM4x4, OpM4x3, OpM3x4, OpM3x3,
       OpM3x2, OpCall = 25, OpCallnz = 26, OpLoop = 27, OpRet = 28, OpEndloop = 29, OpLabel = 30,
       OpDcl = 31, OpPow = 32, OpCrs = 33, OpSgn = 34, OpAbs = 35, OpNrm = 36,
       OpSincos = 37, OpRep = 38, OpEndrep = 39, OpIf = 40, OpIfc = 41, OpElse = 42, OpEndif = 43,
       OpMova = 46, OpDefb = 47, OpDefi = 48, OpExpp = 78, OpLogp = 79,
       OpDef = 81, OpComment = 0xFFFE, OpEnd = 0xFFFF };

static const char comps[] = "xyzw";

/* Operand count of a vs_1_1 instruction (its token carries none). */
static int length1(unsigned int op)
{
    switch (op) {
    case OpNop: return 0;
    case OpMov: case OpRcp: case OpRsq: case OpExp: case OpLog: case OpLit: case OpFrc:
    case OpExpp: case OpLogp: case OpDcl: return 2;
    case OpAdd: case OpSub: case OpMul: case OpDp3: case OpDp4: case OpMin: case OpMax:
    case OpSlt: case OpSge: case OpDst: case OpM4x4: case OpM4x3: case OpM3x4: case OpM3x3:
    case OpM3x2: return 3;
    case OpMad: return 4;
    case OpDef: return 5;
    }
    return -1;
}

/* A register's name (with k added to its number: the rows of a matrix instruction). A relative
   constant first loads the address register from the copy of a0 (component comp). */
static int reg_name(VS *s, unsigned int t, unsigned int k, unsigned int comp, STR *b)
{
    unsigned int type = reg_type(t), num = (t & 0x7FF) + k;
    switch (type) {
    case RegTemp:
        if (num > 11)
            return fail(s, VS_ARB_REGISTER, t);
        put(b, "r");
        put_u(b, num);
        return 1;
    case RegInput:
        if (num >= VS_ARB_INPUTS)
            return fail(s, VS_ARB_REGISTER, t);
        if (!(s->info->inputs & (1u << num))) {
            put(b, "K.xxxy");           /* not declared: (0, 0, 0, 1) */
            return 1;
        }
        put(b, "v");
        put_u(b, num);
        return 1;
    case RegConst:
        if (num >= VS_ARB_CONSTANTS)
            return fail(s, VS_ARB_REGISTER, t);
        if ((t & 0x2000) && (comp & 0x80000000u)) {     /* relative to aL, whose value is known */
            num += (unsigned int)(int)(short)(comp & 0xFFFF);
            if (num >= VS_ARB_CONSTANTS)
                return fail(s, VS_ARB_REGISTER, t);
        } else if (t & 0x2000) {
            emit(&s->o, "ARL A0.x, aT.");
            {
                char c[2];
                c[0] = comps[comp & 3];
                c[1] = 0;
                emit(&s->o, c);
            }
            emit(&s->o, ";\n");
            put(b, "c[A0.x+");
            put_u(b, num);
            put(b, "]");
            return 1;
        }
        put(b, "c[");
        put_u(b, num);
        put(b, "]");
        return 1;
    }
    return fail(s, VS_ARB_REGISTER, t);
}

/* A source operand: register, swizzle, negation; |x| through ABS into a scratch register.
   scalar: one component, the replicated one or else w's. */
static int source(VS *s, unsigned int t, unsigned int comp, int scalar, STR *b)
{
    STR r;
    unsigned int swz = (t >> 16) & 0xFF, mod = (t >> 24) & 15, i;
    char sw[6];

    r.n = 0;
    r.s[0] = 0;
    if (!reg_name(s, t, 0, comp, &r))
        return 0;
    sw[0] = '.';
    for (i = 0; i < 4; i++)
        sw[1 + i] = comps[(swz >> (2 * i)) & 3];
    sw[5] = 0;
    if (scalar) {
        sw[1] = sw[4];                  /* w's: the replicated one when they are all the same */
        sw[2] = 0;
    } else if (swz == 0xE4) {
        sw[0] = 0;
    }
    b->n = 0;
    b->s[0] = 0;
    if (mod == 0 || mod == 1) {
        if (mod == 1)
            put(b, "-");
        put(b, r.s);
        put(b, sw);
        return 1;
    }
    if (mod == 0xB || mod == 0xC) {     /* abs, -abs */
        char sname[4];
        if (s->scratch > 2)
            return fail(s, VS_ARB_REGISTER, t);
        sname[0] = 's';
        sname[1] = (char)('0' + s->scratch++);
        sname[2] = 0;
        emit(&s->o, "ABS ");
        emit(&s->o, sname);
        emit(&s->o, ", ");
        emit(&s->o, r.s);
        emit(&s->o, sw[0] && !scalar ? sw : "");
        emit(&s->o, ";\n");
        if (mod == 0xC)
            put(b, "-");
        put(b, sname);
        if (scalar)
            put(b, sw);
        return 1;
    }
    return fail(s, VS_ARB_REGISTER, t);
}

/* A destination: register and write mask; _sat goes through q (see finish). */
static int dest(VS *s, unsigned int t, STR *b, int *sat, unsigned int *mask)
{
    unsigned int type = reg_type(t), num = t & 0x7FF, m = (t >> 16) & 15, i;
    b->n = 0;
    b->s[0] = 0;
    *sat = ((t >> 20) & 1) != 0;
    *mask = m ? m : 15;
    switch (type) {
    case RegTemp:
        if (num > 11)
            return fail(s, VS_ARB_REGISTER, t);
        put(b, "r");
        put_u(b, num);
        break;
    case RegRastOut:
        if (num == 0) put(b, "oPos");
        else if (num == 1) put(b, "oFog");
        else if (num == 2) { put(b, "result.pointsize"); s->info->psize = 1; }
        else return fail(s, VS_ARB_REGISTER, t);
        break;
    case RegAttrOut:
        if (num > 1)
            return fail(s, VS_ARB_REGISTER, t);
        put(b, num ? "oD1" : "oD0");
        break;
    case RegTexOut:
        if (num > 7)
            return fail(s, VS_ARB_REGISTER, t);
        put(b, "oT");
        put_u(b, num);
        break;
    default:
        return fail(s, VS_ARB_REGISTER, t);
    }
    if (*mask != 15) {
        put(b, ".");
        for (i = 0; i < 4; i++)
            if (*mask & (1u << i)) {
                char c[2];
                c[0] = comps[i];
                c[1] = 0;
                put(b, c);
            }
    }
    return 1;
}

static void line(VS *s, const char *a, const char *b, const char *c, const char *d, const char *e)
{
    emit(&s->o, a);
    emit(&s->o, " ");
    emit(&s->o, b);
    if (c) { emit(&s->o, ", "); emit(&s->o, c); }
    if (d) { emit(&s->o, ", "); emit(&s->o, d); }
    if (e) { emit(&s->o, ", "); emit(&s->o, e); }
    emit(&s->o, ";\n");
}

/* The instruction's target: dst itself, or q with dst's mask when it saturates. */
static const char *target(STR *q, const STR *d, int sat, unsigned int mask)
{
    unsigned int i;
    if (!sat)
        return d->s;
    q->n = 0;
    q->s[0] = 0;
    put(q, "q");
    if (mask != 15) {
        put(q, ".");
        for (i = 0; i < 4; i++)
            if (mask & (1u << i)) {
                char c[2];
                c[0] = comps[i];
                c[1] = 0;
                put(q, c);
            }
    }
    return q->s;
}

static void finish(VS *s, const STR *d, int sat)
{
    if (!sat)
        return;
    line(s, "MAX", "q", "q", "K.xxxx", 0);
    line(s, "MIN", d->s, "q", "K.yyyy", 0);
}

/* One instruction: op with its n operand tokens (relative addresses already folded in:
   comp[k] is operand k's address component). */
static int instruction(VS *s, unsigned int op, const unsigned int *p, const unsigned int *comp,
                       unsigned int n)
{
    STR d, a, b, c, q;
    int sat;
    unsigned int mask, k;
    const char *t;

    s->scratch = 0;
    if ((op >= 25 && op <= 30) || (op >= OpSincos && op <= 45))
        return fail(s, VS_ARB_OPCODE, op);     /* flow control, sincos */
    switch (op) {
    case OpNop: case OpDcl: case OpDefb: case OpDefi:
        return 1;
    case OpMov: case OpMova:
        if (n < 2)
            return fail(s, VS_ARB_TRUNCATED, op);
        if (reg_type(p[0]) == RegAddr) {
            /* a0: kept in aT as whole numbers; mov rounds toward minus infinity, mova to the
               nearest */
            STR m;
            unsigned int i, wm = (p[0] >> 16) & 15;
            if (!source(s, p[1], comp[1], 0, &a))
                return 0;
            m.n = 0;
            m.s[0] = 0;
            put(&m, "aT");
            if (wm && wm != 15) {
                put(&m, ".");
                for (i = 0; i < 4; i++)
                    if (wm & (1u << i)) {
                        char ch[2];
                        ch[0] = comps[i];
                        ch[1] = 0;
                        put(&m, ch);
                    }
            }
            if (op == OpMova) {
                line(s, "ADD", "q", a.s, "K.zzzz", 0);
                line(s, "FLR", m.s, "q", 0, 0);
            } else {
                line(s, "FLR", m.s, a.s, 0, 0);
            }
            return 1;
        }
        break;
    }
    if (n < 1 || !dest(s, p[0], &d, &sat, &mask))
        return n < 1 ? fail(s, VS_ARB_TRUNCATED, op) : 0;
    t = target(&q, &d, sat, mask);
    switch (op) {
    case OpMov: case OpFrc: case OpLit: case OpAbs:
        if (n < 2 || !source(s, p[1], comp[1], 0, &a))
            return 0;
        line(s, op == OpMov ? "MOV" : op == OpFrc ? "FRC" : op == OpLit ? "LIT" : "ABS", t, a.s, 0, 0);
        break;
    case OpAdd: case OpSub: case OpMul: case OpDp3: case OpDp4: case OpMin: case OpMax:
    case OpSlt: case OpSge: case OpDst: case OpCrs: {
        static const char *const names[] = { "", "", "ADD", "SUB", "", "MUL", "", "", "DP3", "DP4",
                                              "MIN", "MAX", "SLT", "SGE", "", "", "", "DST" };
        if (n < 3 || !source(s, p[1], comp[1], 0, &a) || !source(s, p[2], comp[2], 0, &b))
            return 0;
        line(s, op == OpCrs ? "XPD" : names[op], t, a.s, b.s, 0);
        break;
    }
    case OpMad:
        if (n < 4 || !source(s, p[1], comp[1], 0, &a) || !source(s, p[2], comp[2], 0, &b) ||
            !source(s, p[3], comp[3], 0, &c))
            return 0;
        line(s, "MAD", t, a.s, b.s, c.s);
        break;
    case OpLrp:
        if (n < 4 || !source(s, p[1], comp[1], 0, &a) || !source(s, p[2], comp[2], 0, &b) ||
            !source(s, p[3], comp[3], 0, &c))
            return 0;
        line(s, "SUB", "q2", b.s, c.s, 0);
        line(s, "MAD", t, a.s, "q2", c.s);
        break;
    case OpRcp: case OpRsq: case OpExp: case OpLog: case OpExpp: case OpLogp: {
        unsigned int src = p[1];
        const char *name;
        if (n < 2)
            return fail(s, VS_ARB_TRUNCATED, op);
        /* log and logp read |x|; version 1's expp and logp are ARB's partial EXP and LOG */
        if ((op == OpLog || (op == OpLogp && s->version >= 2)) && ((src >> 24) & 15) == 0)
            src = (src & ~0x0F000000u) | 0x0B000000u;
        if (op == OpLog && ((p[1] >> 24) & 15) == 1)
            src = (src & ~0x0F000000u) | 0x0B000000u;   /* |-x| is |x| */
        if (op == OpExpp && s->version < 2)
            name = "EXP";
        else if (op == OpLogp && s->version < 2)
            name = "LOG";
        else
            name = op == OpRcp ? "RCP" : op == OpRsq ? "RSQ" : (op == OpExp || op == OpExpp) ? "EX2" : "LG2";
        if (!source(s, src, comp[1], 1, &a))
            return 0;
        line(s, name, t, a.s, 0, 0);
        break;
    }
    case OpPow:
        if (n < 3)
            return fail(s, VS_ARB_TRUNCATED, op);
        if (!source(s, (p[1] & ~0x0F000000u) | 0x0B000000u, comp[1], 1, &a) ||
            !source(s, p[2], comp[2], 1, &b))
            return 0;
        line(s, "POW", t, a.s, b.s, 0);
        break;
    case OpNrm:
        if (n < 2 || !source(s, p[1], comp[1], 0, &a))
            return 0;
        line(s, "DP3", "q2.w", a.s, a.s, 0);
        line(s, "RSQ", "q2.w", "q2.w", 0, 0);
        line(s, "MUL", t, a.s, "q2.w", 0);
        break;
    case OpSgn:
        if (n < 2 || !source(s, p[1], comp[1], 0, &a))
            return 0;
        line(s, "SLT", "q2", "K.xxxx", a.s, 0);
        line(s, "SLT", "s2", a.s, "K.xxxx", 0);
        line(s, "SUB", t, "q2", "s2", 0);
        break;
    case OpM4x4: case OpM4x3: case OpM3x4: case OpM3x3: case OpM3x2: {
        unsigned int rows = op == OpM4x4 || op == OpM3x4 ? 4 : op == OpM3x2 ? 2 : 3;
        const char *dp = op == OpM4x4 || op == OpM4x3 ? "DP4" : "DP3";
        STR base, row;
        unsigned int i;
        if (n < 3 || !source(s, p[1], comp[1], 0, &a))
            return 0;
        if (sat)
            return fail(s, VS_ARB_REGISTER, p[0]);
        for (k = 0; k < rows; k++) {
            base.n = 0;
            base.s[0] = 0;
            if (!reg_name(s, p[2], k, comp[2], &base))
                return 0;
            row.n = 0;
            row.s[0] = 0;
            for (i = 0; i < d.n && d.s[i] != '.'; i++) {
                char ch[2];
                ch[0] = d.s[i];
                ch[1] = 0;
                put(&row, ch);
            }
            put(&row, ".");
            {
                char ch[2];
                ch[0] = comps[k];
                ch[1] = 0;
                put(&row, ch);
            }
            line(s, dp, row.s, a.s, base.s, 0);
        }
        return 1;
    }
    default:
        return fail(s, VS_ARB_OPCODE, op);     /* flow control, sincos */
    }
    finish(s, &d, sat);
    return 1;
}

/* ---- Static flow control (vs_2_0: if, else, rep, loop, call, callnz on the boolean and integer
   constants). An ARB vertex program has no branches, and these only depend on constants, so
   the program is written out straight for the values they have now: a false branch is left
   out, a loop's body is written once a round with its counter aL folded into the constants'
   indices, a subroutine is written where it is called. info says which b# and i# were read,
   so that the caller keeps one program for each set of their values. ---- */
#define FLOW_DEPTH 8                    /* loops, repeats and calls inside one another */
#define FLOW_STEPS 4096                 /* instructions written at most */
#define AL_FLAG 0x80000000u             /* a relative constant's address is aL: its value follows */

/* The end of the block that i is inside: the index of its closing instruction, or of an else
   of its own when stop_else; blocks opened inside it by open or open2 are stepped over. */
static unsigned int block_end(const unsigned int *tok, unsigned int count, unsigned int i,
                              unsigned int open, unsigned int open2, unsigned int close, int stop_else)
{
    unsigned int depth = 0;
    while (i < count) {
        unsigned int t = tok[i], op = t & 0xFFFF;
        if (t == OpEnd)
            break;
        if (op == OpComment) {
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        if (op == open || op == open2) {
            depth++;
        } else if (op == close) {
            if (!depth)
                return i;
            depth--;
        } else if (stop_else && op == OpElse && !depth) {
            return i;
        }
        i += 1 + ((t >> 24) & 15);
    }
    return count;
}

/* The value of a boolean constant: the shader's own (defb), else the caller's. */
static int flow_bool(VS *s, unsigned int t, int *v)
{
    unsigned int n = t & 0x7FF;
    if (reg_type(t) != RegConstBool || n > 15)
        return fail(s, VS_ARB_REGISTER, t);
    if (s->defb_mask & (1u << n)) {
        *v = (s->defb_val >> n) & 1;
    } else {
        s->info->flow_bools |= 1u << n;
        *v = s->flow ? (int)((s->flow->bools >> n) & 1) : 0;
    }
    return 1;
}

/* An integer constant's three numbers (a count, aL's first value, its step). */
static int flow_int(VS *s, unsigned int t, const int **v)
{
    static const int none[3] = { 0, 0, 0 };
    unsigned int n = t & 0x7FF;
    if (reg_type(t) != RegConstInt || n > 15)
        return fail(s, VS_ARB_REGISTER, t);
    if (s->defi_mask & (1u << n)) {
        *v = s->defi[n];
    } else {
        s->info->flow_ints |= 1u << n;
        *v = s->flow ? s->flow->ints[n] : none;
    }
    return 1;
}

/* Pass 0: def, defb and defi constants and the labels; pass 1: the instructions, in the order
   they run. */
static int walk(VS *s, const unsigned int *tok, unsigned int count, int pass)
{
    unsigned int i = 1, steps = 0;
    s->frames = 0;
    while (i < count) {
        unsigned int t = tok[i], op = t & 0xFFFF, n, k, m, ops[8], comp[8];
        if (t == OpEnd)
            return 1;
        if (op == OpComment) {
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        if (s->version >= 2) {
            n = (t >> 24) & 15;
        } else {
            int len = length1(op);
            if (len < 0)
                return fail(s, VS_ARB_OPCODE, t);
            n = (unsigned int)len;
        }
        if (i + 1 + n > count)
            return fail(s, VS_ARB_TRUNCATED, i);
        if (pass == 0) {
            if (op == OpDef) {
                unsigned int c = tok[i + 1] & 0x7FF, j;
                VS_ARB *in = s->info;
                if (n != 5 || reg_type(tok[i + 1]) != RegConst || c >= VS_ARB_CONSTANTS ||
                    in->defs >= VS_ARB_DEFS)
                    return fail(s, VS_ARB_REGISTER, tok[i + 1]);
                in->def[in->defs].reg = (unsigned short)c;
                for (j = 0; j < 4; j++)
                    in->def[in->defs].v[j] = *(const float *)&tok[i + 2 + j];
                in->defs++;
            } else if (op == OpDefb && n == 2 && reg_type(tok[i + 1]) == RegConstBool &&
                       (tok[i + 1] & 0x7FF) < 16) {
                unsigned int b = tok[i + 1] & 0x7FF;
                s->defb_mask |= 1u << b;
                s->defb_val = tok[i + 2] ? s->defb_val | 1u << b : s->defb_val & ~(1u << b);
            } else if (op == OpDefi && n == 5 && reg_type(tok[i + 1]) == RegConstInt &&
                       (tok[i + 1] & 0x7FF) < 16) {
                unsigned int b = tok[i + 1] & 0x7FF, j;
                s->defi_mask |= 1u << b;
                for (j = 0; j < 3; j++)
                    s->defi[b][j] = (int)tok[i + 2 + j];
            } else if (op == OpLabel && n == 1 && (tok[i + 1] & 0x7FF) < 16) {
                s->label[tok[i + 1] & 0x7FF] = i + 2;   /* the subroutine's first instruction */
            }
            i += 1 + n;
            continue;
        }
        if (s->version >= 2 && ((op >= OpCall && op <= OpLabel) || (op >= OpRep && op <= OpEndif))) {
            int b = 0;
            const int *v;
            switch (op) {
            case OpIf:
                if (n != 1 || !flow_bool(s, tok[i + 1], &b))
                    return fail(s, VS_ARB_REGISTER, t);
                if (b) {
                    i += 1 + n;
                } else {                /* to its else, or past its endif */
                    i = block_end(tok, count, i + 1 + n, OpIf, OpIfc, OpEndif, 1);
                    if (i < count)
                        i += 1 + ((tok[i] >> 24) & 15);
                }
                continue;
            case OpElse:                /* met at the end of the branch taken: past the endif */
                i = block_end(tok, count, i + 1 + n, OpIf, OpIfc, OpEndif, 0);
                if (i < count)
                    i += 1 + ((tok[i] >> 24) & 15);
                continue;
            case OpEndif:
                i += 1 + n;
                continue;
            case OpRep:
            case OpLoop:
                if (n != (op == OpLoop ? 2u : 1u) || !flow_int(s, tok[i + n], &v))
                    return fail(s, VS_ARB_REGISTER, t);
                if (v[0] <= 0) {        /* no round: past its end */
                    i = op == OpLoop ? block_end(tok, count, i + 1 + n, OpLoop, OpLoop, OpEndloop, 0)
                                     : block_end(tok, count, i + 1 + n, OpRep, OpRep, OpEndrep, 0);
                    if (i < count)
                        i += 1 + ((tok[i] >> 24) & 15);
                    continue;
                }
                if (s->frames == FLOW_DEPTH)
                    return fail(s, VS_ARB_OVERFLOW, t);
                s->frame[s->frames].kind = op;
                s->frame[s->frames].at = i + 1 + n;
                s->frame[s->frames].left = (unsigned int)v[0];
                s->frame[s->frames].al = op == OpLoop ? v[1] : 0;
                s->frame[s->frames].step = op == OpLoop ? v[2] : 0;
                s->frames++;
                i += 1 + n;
                continue;
            case OpEndrep:
            case OpEndloop:
                if (!s->frames || s->frame[s->frames - 1].kind != (op == OpEndloop ? (unsigned int)OpLoop : (unsigned int)OpRep))
                    return fail(s, VS_ARB_OPCODE, t);
                s->frame[s->frames - 1].al += s->frame[s->frames - 1].step;
                if (--s->frame[s->frames - 1].left) {
                    i = s->frame[s->frames - 1].at;
                } else {
                    s->frames--;
                    i += 1 + n;
                }
                continue;
            case OpCallnz:
                if (n != 2 || !flow_bool(s, tok[i + 2], &b))
                    return fail(s, VS_ARB_REGISTER, t);
                if (!b) {
                    i += 1 + n;
                    continue;
                }
                /* fall through */
            case OpCall:
                if (n < 1 || reg_type(tok[i + 1]) != RegLabel || (tok[i + 1] & 0x7FF) > 15 ||
                    !s->label[tok[i + 1] & 0x7FF] || s->frames == FLOW_DEPTH)
                    return fail(s, VS_ARB_REGISTER, t);
                s->frame[s->frames].kind = OpCall;
                s->frame[s->frames].at = i + 1 + n;
                s->frame[s->frames].left = 0;
                s->frame[s->frames].al = s->frame[s->frames].step = 0;
                s->frames++;
                i = s->label[tok[i + 1] & 0x7FF];
                continue;
            case OpRet:
            case OpLabel:               /* the main program ends at its ret, or where the
                                           subroutines begin */
                while (s->frames && s->frame[s->frames - 1].kind != OpCall)
                    s->frames--;
                if (!s->frames)
                    return 1;
                i = s->frame[--s->frames].at;
                continue;
            default:                    /* ifc: dynamic flow control is not offered */
                return fail(s, VS_ARB_OPCODE, t);
            }
        }
        if (op != OpDef) {
            /* operands; a vs_2_0 relative source is followed by its address token */
            if (++steps > FLOW_STEPS)
                return fail(s, VS_ARB_OVERFLOW, steps);
            for (k = 0, m = 0; k < n && m < 8; k++) {
                unsigned int o = tok[i + 1 + k], a = 0;
                if (s->version >= 2 && m > 0 && (o & 0x2000)) {
                    unsigned int at;
                    if (k + 1 >= n)
                        return fail(s, VS_ARB_REGISTER, o);
                    at = tok[i + 2 + k];
                    if (reg_type(at) == RegLoop) {      /* aL: the innermost loop's counter */
                        unsigned int f = s->frames;
                        while (f && s->frame[f - 1].kind != OpLoop)
                            f--;
                        if (!f)
                            return fail(s, VS_ARB_REGISTER, at);
                        a = AL_FLAG | ((unsigned int)s->frame[f - 1].al & 0xFFFF);
                    } else if (reg_type(at) == RegAddr) {
                        a = (at >> 16) & 3;
                    } else {
                        return fail(s, VS_ARB_REGISTER, at);
                    }
                    k++;
                }
                comp[m] = a;
                ops[m++] = o;
            }
            if (!instruction(s, op, ops, comp, m))
                return 0;
        }
        i += 1 + n;
    }
    return fail(s, VS_ARB_TRUNCATED, count);
}

/* The Direct3D 8 declaration: each stream's registers and skips; constants (CONSTMEM) are the
   runtime's to load. */
static int declaration(VS *s, const unsigned int *decl, unsigned int count)
{
    static const unsigned char bytes[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    unsigned int i, offset = 0, stream = 0;
    for (i = 0; i < count; i++) {
        unsigned int t = decl[i];
        if (t == 0xFFFFFFFFu) {
            if (!stream)
                s->info->stride = offset;
            return 1;
        }
        switch (t >> 29) {
        case 0:                         /* NOP */
            break;
        case 1:                         /* STREAM: its number; not the tessellator's */
            if ((t & 0x10000000u) || (t & 0x0FFFFFFFu) >= VS_ARB_STREAMS)
                return fail(s, VS_ARB_DECLARATION, t);
            if (!stream)
                s->info->stride = offset;   /* what stream 0 laid out so far */
            stream = t & 0x0FFFFFFFu;
            offset = 0;
            break;
        case 2:                         /* STREAMDATA */
            if (t & 0x10000000u) {      /* SKIP: dwords */
                offset += 4 * ((t >> 16) & 15);
                break;
            } else {
                unsigned int reg = t & 0x1F, type = (t >> 16) & 15;
                if (reg >= VS_ARB_INPUTS || type > 7 || offset > 255)
                    return fail(s, VS_ARB_DECLARATION, t);
                s->info->inputs |= 1u << reg;
                s->info->type[reg] = (unsigned char)type;
                s->info->offset[reg] = (unsigned char)offset;
                s->info->stream[reg] = (unsigned char)stream;
                offset += bytes[type];
            }
            break;
        case 4:                         /* CONSTMEM: its values follow */
            i += 4 * ((t >> 25) & 15);
            break;
        default:                        /* tessellator, extensions */
            return fail(s, VS_ARB_DECLARATION, t);
        }
    }
    return fail(s, VS_ARB_DECLARATION, count);
}

/* A declaration that came without code: the fixed-function pipeline reads its vertices through
   it (Direct3D 8's way to more than one stream). Its layout into info; 0 if it can't be read. */
int vs_declared(const unsigned int *decl, unsigned int declWords, VS_ARB *info)
{
    VS s;
    unsigned int k;
    char *z = (char *)info;

    for (k = 0; k < sizeof *info; k++)
        z[k] = 0;
    s.o.p = 0;
    s.o.n = s.o.size = 0;
    s.o.overflow = 0;
    s.info = info;
    s.version = 0;
    s.scratch = 0;
    return declaration(&s, decl, declWords);
}

unsigned int vs_to_arb(const unsigned int *code, unsigned int codeWords, const unsigned int *decl,
                       unsigned int declWords, unsigned int key, char *out, unsigned int size,
                       VS_ARB *info, const VS_ARB_FLOW *flow)
{
    VS s;
    unsigned int k, fog = key & VS_ARB_FOG, table = (key >> VS_ARB_TABLE_SHIFT) & 3;
    unsigned int unit = (key >> VS_ARB_UNIT_SHIFT) & 15;
    char *z = (char *)info;

    for (k = 0; k < sizeof *info; k++)
        z[k] = 0;
    s.o.p = out;
    s.o.n = 0;
    s.o.size = size;
    s.o.overflow = 0;
    s.info = info;
    s.scratch = 0;
    s.flow = flow;
    s.defb_mask = s.defb_val = s.defi_mask = 0;
    s.frames = 0;
    for (k = 0; k < 16; k++)
        s.label[k] = 0;
    if (!codeWords || (code[0] != 0xFFFE0100u && code[0] != 0xFFFE0101u && code[0] != 0xFFFE0200u))
        return fail(&s, VS_ARB_VERSION, codeWords ? code[0] : 0);
    s.version = (code[0] >> 8) & 0xFF;
    if (!declaration(&s, decl, declWords) || !walk(&s, code, codeWords, 0))
        return 0;

    emit(&s.o, "!!ARBvp1.0\n");
    emit(&s.o, "PARAM c[256] = { program.env[0..255] };\n");
    emit(&s.o, "PARAM K = { 0, 1, 0.5, 2 };\n");
    emit(&s.o, "PARAM L = { -1.4426950408889634, 0.25, 0, 0 };\n");
    emit(&s.o, "PARAM H = program.local[0];\n");
    emit(&s.o, "PARAM F = program.local[1];\n");
    emit(&s.o, "ADDRESS A0;\n");
    emit(&s.o, "TEMP r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11;\n");
    emit(&s.o, "TEMP oPos, oFog, oD0, oD1, oT0, oT1, oT2, oT3, oT4, oT5, oT6, oT7;\n");
    emit(&s.o, "TEMP aT, q, q2, s0, s1, s2;\n");
    for (k = 0; k < VS_ARB_INPUTS; k++)
        if (info->inputs & (1u << k)) {
            emit(&s.o, "TEMP v");
            emit_u(&s.o, k);
            emit(&s.o, ";\n");
        }
    /* inputs: a D3DCOLOR's BGRA bytes swapped to RGBA */
    for (k = 0; k < VS_ARB_INPUTS; k++)
        if (info->inputs & (1u << k)) {
            emit(&s.o, "MOV v");
            emit_u(&s.o, k);
            emit(&s.o, ", vertex.attrib[");
            emit_u(&s.o, k);
            emit(&s.o, info->type[k] == VS_ARB_COLOR ? "].zyxw;\n" : "];\n");
        }
    /* what isn't written: position (0, 0, 0, 1), no fog, white, black, (0, 0, 0, 1) */
    emit(&s.o, "MOV oPos, K.xxxy;\nMOV oFog, K.yyyy;\nMOV oD0, K.yyyy;\nMOV oD1, K.xxxx;\n"
               "MOV aT, K.xxxx;\n");
    for (k = 0; k < 8; k++) {
        emit(&s.o, "MOV oT");
        emit_u(&s.o, k);
        emit(&s.o, ", K.xxxy;\n");
    }
    for (k = 0; k < 12; k++) {
        emit(&s.o, "MOV r");
        emit_u(&s.o, k);
        emit(&s.o, ", K.xxxx;\n");
    }
    if (!walk(&s, code, codeWords, 1))
        return 0;
    /* OpenGL's clip space, as for the fixed-function programs (tnl_load): y up, z from -w,
       pixel centres half a pixel in */
    emit(&s.o, "MAD result.position.x, oPos.w, H.x, oPos.x;\n"
               "MAD result.position.y, oPos.w, H.y, -oPos.y;\n"
               "MAD result.position.z, oPos.z, K.w, -oPos.w;\n"
               "MOV result.position.w, oPos.w;\n"
               "MOV result.color, oD0;\n");
    if (fog) {
        if (table) {                    /* from Direct3D's depth, z / w */
            emit(&s.o, "RCP q.y, oPos.w;\nMUL q.x, oPos.z, q.y;\n");
            if (table == 3) {
                emit(&s.o, "MAD q.x, -q.x, F.y, F.x;\n");
            } else {
                emit(&s.o, "MUL q.x, q.x, F.z;\n");
                if (table == 2)
                    emit(&s.o, "MUL q.x, q.x, q.x;\n");
                emit(&s.o, "MUL q.x, q.x, L.x;\nEX2 q.x, q.x;\n");
            }
        } else {
            emit(&s.o, "MOV q.x, oFog.x;\n");
        }
        emit(&s.o, "MAX q.x, q.x, K.x;\nMIN q.x, q.x, K.y;\nMAD q.y, q.x, K.z, L.y;\n");
        if (key & VS_ARB_FOG_COORD) {
            emit(&s.o, "MOV result.fogcoord.x, q.y;\n");
        } else {
            emit(&s.o, "MOV q2, K.xxxy;\nMOV q2.x, q.y;\nMOV result.texcoord[");
            emit_u(&s.o, unit);
            emit(&s.o, "], q2;\n");
        }
        if (key & VS_ARB_FOG_SPEC)
            emit(&s.o, "MUL oD1.xyz, oD1, q.x;\n");
    }
    emit(&s.o, "MOV result.color.secondary, oD1;\n");
    for (k = 0; k < 8; k++) {
        if (fog && !(key & VS_ARB_FOG_COORD) && k == unit)
            continue;
        emit(&s.o, "MOV result.texcoord[");
        emit_u(&s.o, k);
        emit(&s.o, "], oT");
        emit_u(&s.o, k);
        emit(&s.o, ";\n");
    }
    emit(&s.o, "END\n");
    if (s.o.overflow)
        return fail(&s, VS_ARB_OVERFLOW, s.o.n);
    out[s.o.n] = 0;
    return s.o.n + 1;
}
