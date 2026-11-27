/* Direct3D 9 vertex declarations and vertex shader functions said in Direct3D 8's terms
 * (d3d9_decl.h). Self-contained C89 without a run-time library and without floating point.
 * MIT licence. */
#include "d3d9_decl.h"

enum {
    UsagePosition = 0, UsageBlendWeight = 1, UsageBlendIndices = 2, UsageNormal = 3, UsagePSize = 4,
    UsageTexCoord = 5, UsagePositionT = 9, UsageColor = 10, Usages = 14,
    TypeFloat3 = 2, TypeFloat4 = 3, TypeColor = 4, TypeShort4 = 7,
    OpDcl = 0x1f, OpDefB = 47, OpDefI = 48, OpDef = 81, OpComment = 0xfffe,
    MaxStreamElements = 64
};
#define TOKEN_STREAM(s)      ((1UL << 29) | (s))
#define TOKEN_DATA(type, r)  ((2UL << 29) | ((unsigned long)(type) << 16) | (r))
#define TOKEN_SKIP(words)    ((2UL << 29) | (1UL << 28) | ((unsigned long)(words) << 16))
#define TOKEN_END            0xffffffffUL

/* the 32-bit words of FLOAT1..4, D3DCOLOR, UBYTE4, SHORT2, SHORT4 */
static const unsigned char type_words[8] = { 1, 2, 3, 4, 1, 1, 1, 2 };

/* The elements of stream s in offset order, as indices into the declaration. */
static unsigned long stream_elements(const RB_D9_ELEMENT *elements, unsigned long count,
                                     unsigned long s, unsigned long *idx)
{
    unsigned long k, j, m = 0;
    for (k = 0; k < count && elements[k].stream != 0xff && m < MaxStreamElements; ++k) {
        if (elements[k].stream != s) continue;
        for (j = m; j > 0 && elements[idx[j - 1]].offset > elements[k].offset; --j) idx[j] = idx[j - 1];
        idx[j] = k;
        ++m;
    }
    return m;
}

/* The declaration tokens of all streams: reg gives an element's input register, or 0xff for
 * one to skip. Returns the words written with the end token, or 0. */
static unsigned long declaration_tokens(const RB_D9_ELEMENT *elements, unsigned long count,
                                        unsigned long streams, const unsigned char *regs,
                                        unsigned long *out, unsigned long room)
{
    unsigned long n = 0, s;
    for (s = 0; s < streams; ++s) {
        unsigned long idx[MaxStreamElements], m, cursor = 0, j;
        m = stream_elements(elements, count, s, idx);
        if (!m) continue;
        if (n + 1 >= room) return 0;
        out[n++] = TOKEN_STREAM(s);
        for (j = 0; j < m; ++j) {
            const RB_D9_ELEMENT *e = &elements[idx[j]];
            unsigned long gap, r;
            if (e->type > TypeShort4 || e->method || e->offset % 4 || e->offset < cursor) return 0;
            /* skips (at most 15 words each) over a gap */
            for (gap = (e->offset - cursor) / 4; gap; gap -= gap < 15 ? gap : 15) {
                if (n + 1 >= room) return 0;
                out[n++] = TOKEN_SKIP(gap < 15 ? gap : 15);
            }
            if (n + 1 >= room) return 0;
            r = regs[idx[j]];
            out[n++] = r != 0xff ? TOKEN_DATA(e->type, r) : TOKEN_SKIP(type_words[e->type]);
            cursor = e->offset + 4UL * type_words[e->type];
        }
    }
    if (n + 1 > room) return 0;
    out[n++] = TOKEN_END;
    return n;
}

int rb_d9_translate(const RB_D9_ELEMENT *elements, unsigned long count,
                    const unsigned long *tokens, unsigned long words, unsigned long streams,
                    unsigned long *out, unsigned long room,
                    unsigned long *declWords, unsigned long *codeWords)
{
    unsigned char reg[Usages][16], regs[256];
    unsigned long i = 1, n, k, start, u;

    if (words < 2 || (tokens[0] & 0xffff0000UL) != 0xfffe0000UL || (tokens[0] & 0xffff) > 0x0200)
        return 0;
    for (u = 0; u < Usages; ++u)
        for (k = 0; k < 16; ++k) reg[u][k] = 0xff;
    /* The input dcl statements. They stand before the first instruction, among comments and
     * the def, defi and defb statements: a compiled HLSL shader has its def statements first,
     * and stopping at them left every element of the declaration unread (3DMark05 then drew
     * most of its scenes from vertices at the origin). start: the first instruction. */
    while (i < words) {
        unsigned long t = tokens[i], op = t & 0xffff;
        if (op == OpComment) { i += 1 + ((t >> 16) & 0x7fff); continue; }
        if (op == OpDef || op == OpDefI) { i += 6; continue; }  /* a register and four values */
        if (op == OpDefB) { i += 3; continue; }                 /* a register and a value */
        if (op != OpDcl || i + 2 >= words) break;
        {
            unsigned long usage = tokens[i + 1] & 0x1f, index = (tokens[i + 1] >> 16) & 0xf;
            unsigned long dst = tokens[i + 2];
            unsigned long type = ((dst >> 28) & 7) | ((dst >> 8) & 0x18);
            if (type == 1 && usage < Usages) reg[usage][index] = (unsigned char)(dst & 0x7ff);  /* D3DSPR_INPUT */
        }
        i += 3;
    }
    start = i < words ? i : words;
    if (count > sizeof regs) return 0;
    for (k = 0; k < count; ++k)
        regs[k] = (unsigned char)(elements[k].usage < Usages ?
                                  reg[elements[k].usage][elements[k].usageIndex & 15] : 0xff);
    n = declaration_tokens(elements, count, streams, regs, out, room);
    if (!n || n + words > room) return 0;
    *declWords = n;
    out[n++] = tokens[0];
    /* The statements before the first instruction without the dcl ones, then the instructions. */
    for (i = 1; i < start; ) {
        unsigned long t = tokens[i], op = t & 0xffff;
        unsigned long w = op == OpComment ? 1 + ((t >> 16) & 0x7fff) : op == OpDefB || op == OpDcl ? 3 : 6;
        if (w > start - i) w = start - i;
        if (op != OpDcl) for (k = 0; k < w; ++k) out[n++] = tokens[i + k];
        i += w;
    }
    for (i = start; i < words; ++i) out[n++] = tokens[i];
    *codeWords = n - *declWords;
    return 1;
}

unsigned long rb_d9_decl_fvf(const RB_D9_ELEMENT *elements, unsigned long count)
{
    /* D3DFVF_XYZ 0x002, XYZRHW 0x004, NORMAL 0x010, DIFFUSE 0x040, SPECULAR 0x080, the count of
     * texture coordinate sets from bit 8, their sizes two bits a set from bit 16 */
    static const unsigned long size_code[4] = { 3, 0, 1, 2 };   /* FLOAT1..4 as D3DFVF_TEXCOORDSIZE codes */
    unsigned long fvf = 0, offset = 0, stage = 0, tex = 0, k;

    for (;;) {
        const RB_D9_ELEMENT *e = 0;
        for (k = 0; k < count && elements[k].stream != 0xff; ++k)
            if (elements[k].offset == offset) e = &elements[k];
        if (!e) break;
        if (e->stream || e->method) return 0;
        if (e->usage == UsagePosition && !e->usageIndex && e->type == TypeFloat3 && stage < 1) {
            fvf |= 0x002; stage = 1; offset += 12;
        } else if (e->usage == UsagePositionT && !e->usageIndex && e->type == TypeFloat4 && stage < 1) {
            fvf |= 0x004; stage = 1; offset += 16;
        } else if (e->usage == UsageNormal && !e->usageIndex && e->type == TypeFloat3 && stage == 1 && (fvf & 0x002)) {
            fvf |= 0x010; stage = 2; offset += 12;
        } else if (e->usage == UsageColor && e->usageIndex == 0 && e->type == TypeColor && stage >= 1 && stage < 3) {
            fvf |= 0x040; stage = 3; offset += 4;
        } else if (e->usage == UsageColor && e->usageIndex == 1 && e->type == TypeColor && stage >= 1 && stage < 4) {
            fvf |= 0x080; stage = 4; offset += 4;
        } else if (e->usage == UsageTexCoord && e->usageIndex == tex && e->type <= TypeFloat4 && stage >= 1 && tex < 8) {
            fvf |= size_code[e->type] << (16 + 2 * tex);
            ++tex;
            stage = 5;
            offset += 4UL * (e->type + 1);
        } else {
            return 0;
        }
    }
    for (k = 0; k < count && elements[k].stream != 0xff; ++k)
        if (elements[k].offset >= offset || elements[k].stream) return 0;   /* something left over */
    return stage ? fvf | (tex << 8) : 0;
}

unsigned long rb_d9_decl_fixed(const RB_D9_ELEMENT *elements, unsigned long count,
                               unsigned long streams, unsigned long *out, unsigned long room)
{
    unsigned char regs[256];
    unsigned long k;
    int position = 0;

    if (count > sizeof regs) return 0;
    for (k = 0; k < count && elements[k].stream != 0xff; ++k) {
        const RB_D9_ELEMENT *e = &elements[k];
        unsigned long r = 0xff;
        if (e->usage == UsagePositionT) return 0;
        if (e->usage == UsagePosition && !e->usageIndex) { r = 0; position = e->type == TypeFloat3; }
        else if (e->usage == UsageBlendWeight && !e->usageIndex) r = 1;
        else if (e->usage == UsageBlendIndices && !e->usageIndex) r = 2;
        else if (e->usage == UsageNormal && !e->usageIndex) r = 3;
        else if (e->usage == UsagePSize && !e->usageIndex) r = 4;
        else if (e->usage == UsageColor && e->usageIndex < 2) r = 5UL + e->usageIndex;
        else if (e->usage == UsageTexCoord && e->usageIndex < 8) r = 7UL + e->usageIndex;
        regs[k] = (unsigned char)r;
    }
    for (; k < count; ++k) regs[k] = 0xff;
    if (!position) return 0;
    return declaration_tokens(elements, count, streams, regs, out, room);
}
