/* Direct3D's fixed-function vertex processing as an ARB vertex program (tnl_arb.h): the
   position through the full transform, Direct3D 7 lighting (directional, point and spot lights,
   material sources, local viewer), vertex and table fog, and texture coordinates (a vertex's
   sets, or made in view space, through the texture matrices). Self-contained C without a
   run-time library. MIT licence.

   Lighting follows Direct3D's formulas: a light adds attenuation x spot x (ambient + diffuse x
   max(N.L, 0)) to the colour, and attenuation x spot x specular x (N.H)^power to the specular
   colour where N.L > 0 (LIT); the colour starts at emissive + global ambient x ambient. */
#include "tnl_arb.h"

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

/* "program.env[n]", with a swizzle or component after it if s isn't empty */
static void env(OUT *o, unsigned int n, const char *s)
{
    emit(o, "program.env[");
    emit_u(o, n);
    emit(o, "]");
    emit(o, s);
}

/* one instruction: op, then its operands, each a string, the last followed by ";\n" */
static void ins(OUT *o, const char *op, const char *a, const char *b, const char *c, const char *d)
{
    emit(o, op);
    emit(o, " ");
    emit(o, a);
    if (b) { emit(o, ", "); emit(o, b); }
    if (c) { emit(o, ", "); emit(o, c); }
    if (d) { emit(o, ", "); emit(o, d); }
    emit(o, ";\n");
}

/* The same with program.env[n] (and a suffix) as one operand: written by hand, operand by
   operand, through these. */
static void op_begin(OUT *o, const char *op, const char *dst)
{
    emit(o, op);
    emit(o, " ");
    emit(o, dst);
}

static void op_s(OUT *o, const char *s)
{
    emit(o, ", ");
    emit(o, s);
}

static void op_env(OUT *o, unsigned int n, const char *s)
{
    emit(o, ", ");
    env(o, n, s);
}

static void op_end(OUT *o)
{
    emit(o, ";\n");
}

/* A material colour: the material's own (program.env) or one of the vertex's colours. */
static void op_material(OUT *o, const TNL_KEY *k, int which)
{
    if (k->mcs[which] == TNL_COLOR1)
        op_s(o, "vertex.color");
    else if (k->mcs[which] == TNL_COLOR2)
        op_s(o, "vertex.color.secondary");
    else
        op_env(o, TNL_ENV_MAT + which, "");
}

static void lights(OUT *o, const TNL_KEY *k)
{
    int i, any = 0;
    for (i = 0; i < TNL_LIGHTS; i++)
        if (k->light[i])
            any = 1;
    /* col = emissive + ambient light x ambient */
    op_begin(o, "MOV", "col");
    op_material(o, k, 3);
    op_end(o);
    op_begin(o, "MAD", "col");
    op_env(o, TNL_ENV_AMBIENT, "");
    op_material(o, k, 1);
    op_s(o, "col");
    op_end(o);
    ins(o, "MOV", "spc", "K.xxxx", 0, 0);
    if (!any)
        return;
    /* e: towards the eye, for the half vector */
    if (k->local_viewer) {
        ins(o, "DP3", "e.w", "p", "p", 0);
        ins(o, "RSQ", "e.w", "e.w", 0, 0);
        ins(o, "MUL", "e.xyz", "-p", "e.w", 0);
    } else {
        ins(o, "MOV", "e", "-K.xxyx", 0, 0);   /* (0, 0, -1, 0) */
    }
    for (i = 0; i < TNL_LIGHTS; i++) {
        unsigned int b = TNL_ENV_LIGHT + (unsigned int)i * TNL_LIGHT_SIZE;
        int kind = k->light[i];
        if (!kind)
            continue;
        emit(o, "# light ");
        emit_u(o, (unsigned int)i);
        emit(o, "\n");
        if (kind == TNL_DIRECTIONAL) {
            op_begin(o, "MOV", "l");
            op_env(o, b + 4, "");
            op_end(o);
            ins(o, "MOV", "a.x", "K.y", 0, 0);
        } else {
            op_begin(o, "SUB", "l");
            op_env(o, b + 3, "");
            op_s(o, "p");
            op_end(o);
            ins(o, "DP3", "t.x", "l", "l", 0);          /* d squared */
            ins(o, "RSQ", "t.y", "t.x", 0, 0);
            ins(o, "MUL", "l.xyz", "l", "t.y", 0);
            ins(o, "MUL", "t.z", "t.x", "t.y", 0);      /* d */
            /* 1 / (a0 + a1 d + a2 d^2), 0 beyond the range */
            op_begin(o, "MUL", "t.w");
            op_env(o, b + 5, ".z");
            op_s(o, "t.x");
            op_end(o);
            op_begin(o, "MAD", "t.w");
            op_env(o, b + 5, ".y");
            op_s(o, "t.z");
            op_s(o, "t.w");
            op_end(o);
            op_begin(o, "ADD", "t.w");
            op_s(o, "t.w");
            op_env(o, b + 5, ".x");
            op_end(o);
            ins(o, "RCP", "a.x", "t.w", 0, 0);
            op_begin(o, "SGE", "t.w");
            op_env(o, b + 3, ".w");
            op_s(o, "t.z");
            op_end(o);
            ins(o, "MUL", "a.x", "a.x", "t.w", 0);
            if (kind == TNL_SPOT) {
                /* ((rho - cos phi/2) / (cos theta/2 - cos phi/2)) clamped, to the falloff */
                op_begin(o, "DP3", "t.w");
                op_s(o, "-l");
                op_env(o, b + 4, "");
                op_end(o);
                op_begin(o, "SUB", "t.w");
                op_s(o, "t.w");
                op_env(o, b + 6, ".y");
                op_end(o);
                op_begin(o, "MUL", "t.w");
                op_s(o, "t.w");
                op_env(o, b + 6, ".z");
                op_end(o);
                ins(o, "MAX", "t.w", "t.w", "K.x", 0);
                ins(o, "MIN", "t.w", "t.w", "K.y", 0);
                op_begin(o, "POW", "t.w");
                op_s(o, "t.w");
                op_env(o, b + 5, ".w");
                op_end(o);
                ins(o, "MUL", "a.x", "a.x", "t.w", 0);
            }
        }
        /* r = (1, max(N.L, 0), N.L > 0 ? max(N.H, 0)^power : 0, 1) */
        ins(o, "DP3", "t.x", "n", "l", 0);
        ins(o, "ADD", "h", "l", "e", 0);
        ins(o, "DP3", "h.w", "h", "h", 0);
        ins(o, "RSQ", "h.w", "h.w", 0, 0);
        ins(o, "MUL", "h.xyz", "h", "h.w", 0);
        ins(o, "DP3", "t.y", "n", "h", 0);
        op_begin(o, "MOV", "t.w");
        op_env(o, TNL_ENV_POWER, ".x");
        op_end(o);
        ins(o, "LIT", "r", "t", 0, 0);
        ins(o, "MUL", "r", "r", "a.x", 0);
        op_begin(o, "MUL", "t");
        op_env(o, b + 2, "");
        op_material(o, k, 1);
        op_end(o);
        ins(o, "MAD", "col", "t", "r.x", "col");
        op_begin(o, "MUL", "t");
        op_env(o, b + 0, "");
        op_material(o, k, 0);
        op_end(o);
        ins(o, "MAD", "col", "t", "r.y", "col");
        op_begin(o, "MUL", "t");
        op_env(o, b + 1, "");
        op_material(o, k, 2);
        op_end(o);
        ins(o, "MAD", "spc", "t", "r.z", "spc");
    }
}

/* The fog factor into f.x (clamped), and 0.25 + f / 2 (what the fog unit and the fragment
   programs read) into f.y. */
static void fog(OUT *o, const TNL_KEY *k)
{
    if (k->fog == TNL_FOG_ALPHA) {
        ins(o, "MOV", "f.x", k->specular ? "vertex.color.secondary.w" : "K.y", 0, 0);
    } else {
        if (k->fog_depth == TNL_DEPTH_RANGE) {
            ins(o, "DP3", "f.x", "p", "p", 0);
            ins(o, "RSQ", "f.y", "f.x", 0, 0);
            ins(o, "MUL", "f.x", "f.x", "f.y", 0);
        } else if (k->fog_depth == TNL_DEPTH_SCREEN) {
            /* Direct3D's z / w: OpenGL's clip z is 2 z - w */
            op_begin(o, "DP4", "f.x");
            op_env(o, TNL_ENV_MVP + 2, "");
            op_s(o, "vertex.position");
            op_end(o);
            op_begin(o, "DP4", "f.y");
            op_env(o, TNL_ENV_MVP + 3, "");
            op_s(o, "vertex.position");
            op_end(o);
            ins(o, "ADD", "f.x", "f.x", "f.y", 0);
            ins(o, "MUL", "f.x", "f.x", "K.z", 0);
            ins(o, "RCP", "f.y", "f.y", 0, 0);
            ins(o, "MUL", "f.x", "f.x", "f.y", 0);
        } else {
            ins(o, "MOV", "f.x", "p.z", 0, 0);
        }
        if (k->fog == TNL_FOG_LINEAR) {
            op_begin(o, "MAD", "f.x");
            op_s(o, "-f.x");
            op_env(o, TNL_ENV_FOG, ".y");
            op_env(o, TNL_ENV_FOG, ".x");
            op_end(o);
        } else {
            op_begin(o, "MUL", "f.x");
            op_s(o, "f.x");
            op_env(o, TNL_ENV_FOG, ".z");
            op_end(o);
            if (k->fog == TNL_FOG_EXP2)
                ins(o, "MUL", "f.x", "f.x", "f.x", 0);
            ins(o, "MUL", "f.x", "f.x", "L.x", 0);   /* -log2(e): e^-x is 2^(-x log2 e) */
            ins(o, "EX2", "f.x", "f.x", 0, 0);
        }
    }
    ins(o, "MAX", "f.x", "f.x", "K.x", 0);
    ins(o, "MIN", "f.x", "f.x", "K.y", 0);
    ins(o, "MAD", "f.y", "f.x", "K.z", "K.w");
}

/* Texture coordinates of output s. */
static void texcoord(OUT *o, const TNL_KEY *k, unsigned int s)
{
    char dst[24], src[24];
    const char *d = "result.texcoord[";
    unsigned int i, src_set = 0, count = k->xform[s] & 7;
    int generated = k->tex[s] >= TNL_TEX_NORMAL;

    for (i = 0; d[i]; i++)
        dst[i] = d[i];
    dst[i++] = (char)('0' + s);
    dst[i++] = ']';
    dst[i] = 0;
    if (!k->tex[s]) {
        ins(o, "MOV", dst, "K.xxxy", 0, 0);
        return;
    }
    if (!generated) {
        const char *v = "vertex.texcoord[";
        src_set = k->tex[s] - 1;
        for (i = 0; v[i]; i++)
            src[i] = v[i];
        src[i++] = (char)('0' + src_set);
        src[i++] = ']';
        src[i] = 0;
        if (!count) {
            ins(o, "MOV", dst, src, 0, 0);
            return;
        }
        /* Direct3D's input to the matrix: the first missing component is 1, the rest 0 */
        ins(o, "MOV", "u", src, 0, 0);
        switch (k->tex_size[s]) {
        case 1: ins(o, "MOV", "u.yzw", "K.xyxx", 0, 0); break;
        case 2: ins(o, "MOV", "u.zw", "K.xxyx", 0, 0); break;
        case 3: ins(o, "MOV", "u.w", "K.y", 0, 0); break;
        default: break;
        }
    } else if (k->tex[s] == TNL_TEX_NORMAL) {
        ins(o, "MOV", "u", "n", 0, 0);
        ins(o, "MOV", "u.w", "K.y", 0, 0);
    } else if (k->tex[s] == TNL_TEX_POSITION) {
        ins(o, "MOV", "u", "p", 0, 0);
        ins(o, "MOV", "u.w", "K.y", 0, 0);
    } else {
        /* the eye's ray to the vertex, reflected about the normal: I - 2 (N.I) N */
        if (k->local_viewer) {
            ins(o, "DP3", "u.w", "p", "p", 0);
            ins(o, "RSQ", "u.w", "u.w", 0, 0);
            ins(o, "MUL", "u.xyz", "p", "u.w", 0);
        } else {
            ins(o, "MOV", "u", "K.xxyx", 0, 0);
        }
        ins(o, "DP3", "u.w", "n", "u", 0);
        ins(o, "ADD", "u.w", "u.w", "u.w", 0);
        ins(o, "MAD", "u.xyz", "-n", "u.w", "u");
        ins(o, "MOV", "u.w", "K.y", 0, 0);
    }
    if (!count) {
        ins(o, "MOV", dst, "u", 0, 0);
        return;
    }
    for (i = 0; i < 4; i++) {
        static const char *const comp[4] = { "v.x", "v.y", "v.z", "v.w" };
        op_begin(o, "DP4", comp[i]);
        op_env(o, TNL_ENV_TEX + 4 * s + i, "");
        op_s(o, "u");
        op_end(o);
    }
    /* count components out; projected: the last of them divides (OpenGL's q) */
    ins(o, "MOV", "w", "K.xxxy", 0, 0);
    if (k->xform[s] & TNL_PROJECTED) {
        static const char *const last[5] = { "v.w", "v.x", "v.y", "v.z", "v.w" };
        if (count >= 2) ins(o, "MOV", "w.x", "v.x", 0, 0);
        if (count >= 3) ins(o, "MOV", "w.y", "v.y", 0, 0);
        if (count >= 4) ins(o, "MOV", "w.z", "v.z", 0, 0);
        ins(o, "MOV", "w.w", last[count], 0, 0);
    } else {
        if (count >= 1) ins(o, "MOV", "w.x", "v.x", 0, 0);
        if (count >= 2) ins(o, "MOV", "w.y", "v.y", 0, 0);
        if (count >= 3) ins(o, "MOV", "w.z", "v.z", 0, 0);
        if (count >= 4) ins(o, "MOV", "w.w", "v.w", 0, 0);
    }
    ins(o, "MOV", dst, "w", 0, 0);
}

unsigned int tnl_text(const TNL_KEY *k, char *out, unsigned int size)
{
    OUT o;
    unsigned int s, i;
    int lit_lights = 0, need_p = 0, need_n = 0;

    o.p = out;
    o.n = 0;
    o.size = size;
    o.overflow = 0;
    for (i = 0; i < TNL_LIGHTS; i++)
        if (k->light[i] && k->light[i] != TNL_DIRECTIONAL)
            lit_lights = 1;
    if (k->lighting) {
        need_n = 1;
        if (lit_lights || k->local_viewer)
            need_p = 1;
    }
    if (k->fog && k->fog != TNL_FOG_ALPHA && k->fog_depth != TNL_DEPTH_SCREEN)
        need_p = 1;
    for (s = 0; s < TNL_STAGES; s++) {
        if (k->tex[s] == TNL_TEX_POSITION || k->tex[s] == TNL_TEX_REFLECTION)
            need_p = 1;
        if (k->tex[s] == TNL_TEX_NORMAL || k->tex[s] == TNL_TEX_REFLECTION)
            need_n = 1;
    }

    emit(&o, "!!ARBvp1.0\n");
    emit(&o, "PARAM K = { 0, 1, 0.5, 0.25 };\n");
    emit(&o, "PARAM L = { -1.4426950408889634, 0, 0, 0 };\n");
    emit(&o, "TEMP p, n, e, l, h, t, a, r, col, spc, f, u, v, w;\n");
    for (i = 0; i < 4; i++) {
        static const char *const comp[4] = { "result.position.x", "result.position.y",
                                             "result.position.z", "result.position.w" };
        op_begin(&o, "DP4", comp[i]);
        op_env(&o, TNL_ENV_MVP + i, "");
        op_s(&o, "vertex.position");
        op_end(&o);
    }
    if (need_p) {
        static const char *const comp[3] = { "p.x", "p.y", "p.z" };
        for (i = 0; i < 3; i++) {
            op_begin(&o, "DP4", comp[i]);
            op_env(&o, TNL_ENV_MV + i, "");
            op_s(&o, "vertex.position");
            op_end(&o);
        }
        ins(&o, "MOV", "p.w", "K.y", 0, 0);
    }
    if (need_n) {
        if (k->normal) {
            static const char *const comp[3] = { "n.x", "n.y", "n.z" };
            for (i = 0; i < 3; i++) {
                op_begin(&o, "DP3", comp[i]);
                op_env(&o, TNL_ENV_NORMAL + i, "");
                op_s(&o, "vertex.normal");
                op_end(&o);
            }
            if (k->normalize) {
                ins(&o, "DP3", "n.w", "n", "n", 0);
                ins(&o, "RSQ", "n.w", "n.w", 0, 0);
                ins(&o, "MUL", "n.xyz", "n", "n.w", 0);
            }
            ins(&o, "MOV", "n.w", "K.x", 0, 0);
        } else {
            ins(&o, "MOV", "n", "K.xxxx", 0, 0);
        }
    }
    if (k->lighting) {
        lights(&o, k);
        /* the alpha is the diffuse material's */
        op_begin(&o, "MOV", "col.w");
        if (k->mcs[0] == TNL_COLOR1)
            op_s(&o, "vertex.color.w");
        else if (k->mcs[0] == TNL_COLOR2)
            op_s(&o, "vertex.color.secondary.w");
        else
            op_env(&o, TNL_ENV_MAT, ".w");
        op_end(&o);
    } else {
        ins(&o, "MOV", "col", k->diffuse ? "vertex.color" : "K.yyyy", 0, 0);
        ins(&o, "MOV", "spc", k->specular ? "vertex.color.secondary" : "K.xxxx", 0, 0);
    }
    ins(&o, "MOV", "result.color", "col", 0, 0);
    if (k->fog) {
        fog(&o, k);
        if (k->fog_coord) {
            ins(&o, "MOV", "result.fogcoord.x", "f.y", 0, 0);
        } else {
            char dst[24];
            const char *d = "result.texcoord[";
            for (i = 0; d[i]; i++)
                dst[i] = d[i];
            dst[i++] = (char)('0' + k->fog_unit);
            dst[i++] = ']';
            dst[i] = 0;
            ins(&o, "MOV", "w", "K.xxxy", 0, 0);
            ins(&o, "MOV", "w.x", "f.y", 0, 0);
            ins(&o, "MOV", dst, "w", 0, 0);
        }
        if (k->fog_specular)
            ins(&o, "MUL", "spc.xyz", "spc", "f.x", 0);
    }
    ins(&o, "MOV", "result.color.secondary", "spc", 0, 0);
    for (s = 0; s < TNL_STAGES; s++)
        if (!(k->fog && !k->fog_coord && s == k->fog_unit))
            texcoord(&o, k, s);
    emit(&o, "END\n");
    if (o.overflow)
        return 0;
    o.p[o.n] = 0;
    return o.n + 1;
}
