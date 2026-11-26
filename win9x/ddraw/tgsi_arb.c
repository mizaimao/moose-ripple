/* The TGSI text that win2k/d3d8_ps.c makes of a Direct3D pixel shader, as an OpenGL
   ARB_fragment_program (tgsi_arb.h). Line by line: declarations become ATTRIB, OUTPUT, PARAM and
   TEMP statements, instructions keep their names with ARB register names (TEMP[n] is Tn, IN[n]
   INn, CONST[n] C[n], IMM[n] In, OUT[n] OUTn, SAMP[n] texture[n]). What ARB lacks is spelled
   out: |x| through ABS into X0..X2, SSG through two SLT, KILL_IF is KIL. Scalar instructions
   take one component. Self-contained C without a run-time library. MIT licence. */
#include "tgsi_arb.h"

typedef struct {
    char *p;
    unsigned int n, size;
    int overflow;
} OUT;

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

static void emit_c(OUT *o, char c)
{
    char b[2];
    b[0] = c;
    b[1] = 0;
    emit(o, b);
}

static int starts(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

static unsigned int number(const char **s)
{
    unsigned int v = 0;
    while (**s >= '0' && **s <= '9')
        v = v * 10 + (unsigned int)(*(*s)++ - '0');
    return v;
}

static int hex(const char **s, unsigned int *v)
{
    unsigned int x = 0, digits = 0;
    while (**s == ' ')
        (*s)++;
    if (!starts(*s, "0x"))
        return 0;
    *s += 2;
    for (;; (*s)++, digits++) {
        char c = **s;
        if (c >= '0' && c <= '9') x = x * 16 + (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') x = x * 16 + (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x = x * 16 + (unsigned int)(c - 'A' + 10);
        else break;
    }
    *v = x;
    return digits > 0;
}

/* A register "TYPE[n]" at *s, written to o under its ARB name. */
static int reg(OUT *o, const char **s)
{
    if (starts(*s, "TEMP[")) { *s += 5; emit(o, "T"); emit_u(o, number(s)); }
    else if (starts(*s, "IN[")) { *s += 3; emit(o, "IN"); emit_u(o, number(s)); }
    else if (starts(*s, "CONST[")) { *s += 6; emit(o, "C["); emit_u(o, number(s)); emit(o, "]"); }
    else if (starts(*s, "IMM[")) { *s += 4; emit(o, "I"); emit_u(o, number(s)); }
    else if (starts(*s, "OUT[")) { *s += 4; emit(o, "OUT"); emit_u(o, number(s)); }
    else if (starts(*s, "SAMP[")) { *s += 5; emit(o, "texture["); emit_u(o, number(s)); emit(o, "]"); }
    else return 0;
    if (**s != ']')
        return 0;
    (*s)++;
    return 1;
}

/* A swizzle or write mask ".xyzw" at *s, copied; scalar: only its first component. */
static void swizzle(OUT *o, const char **s, int scalar)
{
    int n = 0;
    if (**s != '.')
        return;
    (*s)++;
    emit(o, ".");
    while ((**s == 'x' || **s == 'y' || **s == 'z' || **s == 'w') && n < 4) {
        if (!scalar || n == 0)
            emit_c(o, **s);
        (*s)++;
        n++;
    }
}

/* A source operand from *s to the next comma or the end: -x, |x|, -|x|, each register with a
   swizzle. |x| first goes through ABS into the scratch register X<scratch>. */
static int source(OUT *pre, OUT *o, const char **s, int scalar, unsigned int scratch)
{
    int neg = 0;
    while (**s == ' ')
        (*s)++;
    if (**s == '-') {
        neg = 1;
        (*s)++;
    }
    if (**s == '|') {
        (*s)++;
        emit(pre, "ABS X");
        emit_u(pre, scratch);
        emit(pre, ", ");
        if (!reg(pre, s))
            return 0;
        swizzle(pre, s, 0);
        emit(pre, ";\n");
        if (**s != '|')
            return 0;
        (*s)++;
        if (neg)
            emit(o, "-");
        emit(o, "X");
        emit_u(o, scratch);
        if (**s == '.')
            swizzle(o, s, scalar);
        else if (scalar)
            emit(o, ".x");
        return 1;
    }
    if (neg)
        emit(o, "-");
    if (!reg(o, s))
        return 0;
    if (**s == '.')
        swizzle(o, s, scalar);
    else if (scalar)
        emit(o, ".x");
    return 1;
}

static int next_operand(const char **s)
{
    while (**s == ' ')
        (*s)++;
    if (**s != ',')
        return 0;
    (*s)++;
    while (**s == ' ')
        (*s)++;
    return 1;
}

static int end_of_line(const char *s)
{
    while (*s == ' ' || *s == '\r')
        s++;
    return *s == '\n' || *s == 0;
}

static const char *const scalar_ops[] = { "RCP", "RSQ", "LG2", "EX2", "POW", "SIN", "COS", 0 };
static const char *const vector_ops[] = {
    "MOV", "ADD", "SUB", "MUL", "MAD", "LRP", "MIN", "MAX", "DP3", "DP4", "CMP", "FRC", "FLR",
    "SLT", "SGE", "ABS", "XPD", 0
};

static int known(const char *const *list, const char *op, unsigned int len)
{
    for (; *list; list++) {
        unsigned int i;
        for (i = 0; i < len && (*list)[i] == op[i]; i++)
            ;
        if (i == len && !(*list)[len])
            return 1;
    }
    return 0;
}

/* One instruction (the text after "N: "). */
static int instruction(OUT *o, const char *s)
{
    char prebuf[256], mainbuf[256];
    OUT pre, m;
    const char *op = s;
    unsigned int len = 0, sources, i;
    int sat = 0, scalar = 0;

    pre.p = prebuf; pre.n = 0; pre.size = sizeof prebuf; pre.overflow = 0;
    m.p = mainbuf; m.n = 0; m.size = sizeof mainbuf; m.overflow = 0;
    while ((op[len] >= 'A' && op[len] <= 'Z') || (op[len] >= '0' && op[len] <= '9') || op[len] == '_')
        len++;
    s = op + len;
    if (len > 4 && starts(op + len - 4, "_SAT")) {
        sat = 1;
        len -= 4;
    }
    if (len == 3 && starts(op, "END")) {
        emit(o, "END\n");
        return 1;
    }
    if (len == 7 && starts(op, "KILL_IF")) {
        emit(&m, "KIL ");
        if (!source(&pre, &m, &s, 0, 0) || !end_of_line(s))
            return 0;
    } else if (len == 3 && (starts(op, "TEX") || starts(op, "TXP") || starts(op, "TXB"))) {
        /* TEX dst, coordinates, SAMP[n], target */
        emit_c(&m, op[0]); emit_c(&m, op[1]); emit_c(&m, op[2]);
        emit(&m, sat ? "_SAT " : " ");
        while (*s == ' ')
            s++;
        if (!reg(&m, &s))
            return 0;
        swizzle(&m, &s, 0);
        emit(&m, ", ");
        if (!next_operand(&s) || !source(&pre, &m, &s, 0, 0))
            return 0;
        emit(&m, ", ");
        if (!next_operand(&s) || !reg(&m, &s))
            return 0;
        emit(&m, ", ");
        if (!next_operand(&s))
            return 0;
        if (starts(s, "2D")) { emit(&m, "2D"); s += 2; }
        else if (starts(s, "CUBE")) { emit(&m, "CUBE"); s += 4; }
        else if (starts(s, "3D")) { emit(&m, "3D"); s += 2; }
        else return 0;
        if (!end_of_line(s))
            return 0;
    } else if (len == 3 && starts(op, "SSG")) {
        /* sign(x) = (0 < x) - (x < 0) */
        char d[64], a[64];
        OUT dst, src;
        dst.p = d; dst.n = 0; dst.size = sizeof d; dst.overflow = 0;
        src.p = a; src.n = 0; src.size = sizeof a; src.overflow = 0;
        while (*s == ' ')
            s++;
        if (!reg(&dst, &s))
            return 0;
        swizzle(&dst, &s, 0);
        if (!next_operand(&s) || !source(&pre, &src, &s, 0, 2) || !end_of_line(s) ||
            dst.overflow || src.overflow)
            return 0;
        d[dst.n] = 0;
        a[src.n] = 0;
        emit(&m, "SLT X0, Z, "); emit(&m, a); emit(&m, ";\nSLT X1, "); emit(&m, a);
        emit(&m, ", Z;\nSUB"); emit(&m, sat ? "_SAT " : " "); emit(&m, d); emit(&m, ", X0, X1");
    } else {
        scalar = known(scalar_ops, op, len);
        if (!scalar && !known(vector_ops, op, len))
            return 0;
        for (i = 0; i < len; i++)
            emit_c(&m, op[i]);
        emit(&m, sat ? "_SAT " : " ");
        while (*s == ' ')
            s++;
        if (!reg(&m, &s))
            return 0;
        swizzle(&m, &s, 0);
        for (sources = 0; next_operand(&s); sources++) {
            emit(&m, ", ");
            if (sources > 2 || !source(&pre, &m, &s, scalar, sources))
                return 0;
        }
        if (!end_of_line(s))
            return 0;
    }
    if (pre.overflow || m.overflow)
        return 0;
    prebuf[pre.n] = 0;
    mainbuf[m.n] = 0;
    emit(o, prebuf);
    emit(o, mainbuf);
    emit(o, ";\n");
    return 1;
}

unsigned int tgsi_to_arb(const char *t, char *out, unsigned int size, TGSI_ARB *info)
{
    OUT o;
    int body = 0;
    unsigned int i;

    o.p = out; o.n = 0; o.size = size; o.overflow = 0;
    info->immediates = info->constants = 0;
    info->line = 0;
    while (*t) {
        const char *s = t;
        while (*t && *t != '\n')
            t++;
        if (*t)
            t++;
        info->line++;
        while (*s == ' ')
            s++;
        if (*s == '\n' || *s == '\r' || !*s)
            continue;
        if (starts(s, "FRAG")) {
            emit(&o, "!!ARBfp1.0\n");
        } else if (starts(s, "DCL IN[")) {
            unsigned int k, g;
            s += 7;
            k = number(&s);
            if (!starts(s, "], GENERIC["))
                return 0;
            s += 11;
            g = number(&s);
            emit(&o, "ATTRIB IN");
            emit_u(&o, k);
            if (g == 0) emit(&o, " = fragment.color.primary;\n");
            else if (g == 1) emit(&o, " = fragment.color.secondary;\n");
            else if (g >= 2 && g <= 9) {
                emit(&o, " = fragment.texcoord[");
                emit_u(&o, g - 2);
                emit(&o, "];\n");
            } else if (g == 10) {
                emit(&o, " = fragment.texcoord[");
                emit_u(&o, info->fog_unit);
                emit(&o, "];\n");
            }
            else return 0;
        } else if (starts(s, "DCL OUT[")) {
            unsigned int k;
            s += 8;
            k = number(&s);
            emit(&o, "OUTPUT OUT");
            emit_u(&o, k);
            if (starts(s, "], COLOR")) emit(&o, " = result.color;\n");
            else if (starts(s, "], POSITION")) emit(&o, " = result.depth;\n");
            else return 0;
        } else if (starts(s, "DCL CONST[")) {
            unsigned int a, b;
            s += 10;
            a = b = number(&s);
            if (starts(s, "..")) {
                s += 2;
                b = number(&s);
            }
            if (a != 0 || *s != ']')
                return 0;
            info->constants = b + 1;
            emit(&o, "PARAM C[");
            emit_u(&o, b + 1);
            emit(&o, "] = { program.env[0..");
            emit_u(&o, b);
            emit(&o, "] };\n");
        } else if (starts(s, "DCL TEMP[")) {
            unsigned int a, b;
            s += 9;
            a = b = number(&s);
            if (starts(s, "..")) {
                s += 2;
                b = number(&s);
            }
            if (a != 0)
                return 0;
            emit(&o, "TEMP T0");
            for (i = 1; i <= b; i++) {
                emit(&o, ", T");
                emit_u(&o, i);
            }
            emit(&o, ";\n");
        } else if (starts(s, "DCL SAMP[") || starts(s, "DCL SVIEW[") || starts(s, "PROPERTY ")) {
            /* samplers are texture units; nothing to declare */
        } else if (starts(s, "IMM[")) {
            unsigned int k;
            s += 4;
            k = number(&s);
            if (k >= TGSI_ARB_IMMEDIATES || k != info->immediates || !starts(s, "] FLT32 {"))
                return 0;
            s += 9;
            for (i = 0; i < 4; i++) {
                if (!hex(&s, &info->imm[k][i]))
                    return 0;
                if (i < 3) {
                    if (*s != ',')
                        return 0;
                    s++;
                }
            }
            info->immediates = k + 1;
            emit(&o, "PARAM I");
            emit_u(&o, k);
            emit(&o, " = program.local[");
            emit_u(&o, k);
            emit(&o, "];\n");
        } else if (*s >= '0' && *s <= '9') {
            number(&s);
            if (*s != ':')
                return 0;
            s++;
            while (*s == ' ')
                s++;
            if (!body) {
                emit(&o, "TEMP X0, X1, X2;\nPARAM Z = { 0, 0, 0, 0 };\n");
                body = 1;
            }
            if (!instruction(&o, s))
                return 0;
        } else {
            return 0;
        }
        if (o.overflow)
            return 0;
    }
    if (o.n >= size)
        return 0;
    out[o.n] = 0;
    info->line = 0;
    return o.n + 1;
}
