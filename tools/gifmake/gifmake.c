/*
 * gifmake: an animated GIF from a real-time recording of the VM's display
 * (tools/vncgrab/vncgrab.py --record), playing at the speed it was recorded: each frame stays
 * for the time until the next one arrived. For the README's animations. Plain C, runs where it
 * is built (dev.lan; scripts/vnc-record.sh builds and runs it):
 *   cc -O2 -o gifmake gifmake.c -lm
 *   gifmake [-d DITHER] DIR OUT.gif WIDTH [FROM TO]
 * DIR holds frames.bin (BGRX frames, one after another) and times.txt ("W H", then the arrival
 * time of each frame in seconds). WIDTH: output width (the height follows), scaled with an area
 * filter. FROM and TO: the part of the recording to keep, in seconds; the last frame stays until
 * TO. DITHER: ordered dithering strength in colour levels (default 6, 0 for none).
 *
 * One palette for the whole animation (median cut over every frame, 255 colours; index 255 is
 * transparent), ordered dithering (it gives the same pixels for the same picture, so still parts
 * stay still; error diffusion would not), and each frame stores only the rectangle that changed,
 * with unchanged pixels transparent. Frames closer than 20 ms to the one before are left out:
 * browsers show delays under 20 ms as 100 ms.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int SW, SH, OW, OH;          /* source and output size */
static int BPP = 32;                /* 32: BGRX, 16: RGB565 (little-endian) */
static uint8_t *raw;
static int nframes;
static double *times;
static FILE *frames;

/* ---- Scaling: area-weighted, separable ---------------------------------------------------- */
#define MAXTAP 8
struct taps { int n, idx[MAXTAP]; float w[MAXTAP]; };
static struct taps *xt, *yt;

static void make_taps(struct taps *t, int out, int in)
{
    double s = (double)in / out;
    for (int o = 0; o < out; o++) {
        double a = o * s, b = (o + 1) * s;
        t[o].n = 0;
        for (int i = (int)floor(a); i < (int)ceil(b) && i < in && t[o].n < MAXTAP; i++) {
            double lo = i > a ? i : a, hi = i + 1 < b ? i + 1 : b;
            if (hi > lo) {
                t[o].idx[t[o].n] = i;
                t[o].w[t[o].n++] = (float)((hi - lo) / s);
            }
        }
    }
}

static uint8_t *src;                /* one source frame, BGRX */
static float *tmp;                  /* rows scaled horizontally: OW x SH x 3 */
static uint8_t *rgb;                /* scaled frame: OW x OH x 3 */

static int read_frame(int k)
{
    size_t px = (size_t)SW * SH, n = px * (BPP / 8);
    if (fseeko(frames, (off_t)k * (off_t)n, SEEK_SET) || fread(BPP == 32 ? src : raw, 1, n, frames) != n)
        return 0;
    if (BPP == 16)
        for (size_t i = 0; i < px; i++) {
            unsigned v = raw[2 * i] | raw[2 * i + 1] << 8;
            unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            src[4 * i] = (uint8_t)(b << 3 | b >> 2);
            src[4 * i + 1] = (uint8_t)(g << 2 | g >> 4);
            src[4 * i + 2] = (uint8_t)(r << 3 | r >> 2);
        }
    return 1;
}

static void scale_frame(void)
{
    for (int y = 0; y < SH; y++)
        for (int ox = 0; ox < OW; ox++) {
            float r = 0, g = 0, b = 0;
            for (int k = 0; k < xt[ox].n; k++) {
                const uint8_t *p = src + ((size_t)y * SW + xt[ox].idx[k]) * 4;
                b += p[0] * xt[ox].w[k];
                g += p[1] * xt[ox].w[k];
                r += p[2] * xt[ox].w[k];
            }
            float *t = tmp + ((size_t)y * OW + ox) * 3;
            t[0] = r; t[1] = g; t[2] = b;
        }
    for (int oy = 0; oy < OH; oy++)
        for (int ox = 0; ox < OW; ox++) {
            float c[3] = { 0, 0, 0 };
            for (int k = 0; k < yt[oy].n; k++) {
                const float *t = tmp + ((size_t)yt[oy].idx[k] * OW + ox) * 3;
                for (int j = 0; j < 3; j++)
                    c[j] += t[j] * yt[oy].w[k];
            }
            for (int j = 0; j < 3; j++) {
                int v = (int)(c[j] + 0.5f);
                rgb[((size_t)oy * OW + ox) * 3 + j] = v < 0 ? 0 : v > 255 ? 255 : v;
            }
        }
}

/* ---- Palette: median cut over a 5-bit histogram ------------------------------------------- */
#define HB 32
static uint32_t hcount[HB * HB * HB];
static uint64_t hsum[HB * HB * HB][3];
static uint8_t pal[256][3];
static int npal;

struct box { int lo[3], hi[3]; uint64_t count; };

static int bin(int r, int g, int b) { return (r * HB + g) * HB + b; }

static void shrink(struct box *bx)
{
    int lo[3] = { HB, HB, HB }, hi[3] = { -1, -1, -1 };
    bx->count = 0;
    for (int r = bx->lo[0]; r <= bx->hi[0]; r++)
        for (int g = bx->lo[1]; g <= bx->hi[1]; g++)
            for (int b = bx->lo[2]; b <= bx->hi[2]; b++) {
                uint32_t c = hcount[bin(r, g, b)];
                if (!c)
                    continue;
                bx->count += c;
                int v[3] = { r, g, b };
                for (int j = 0; j < 3; j++) {
                    if (v[j] < lo[j]) lo[j] = v[j];
                    if (v[j] > hi[j]) hi[j] = v[j];
                }
            }
    if (bx->count)
        for (int j = 0; j < 3; j++) {
            bx->lo[j] = lo[j];
            bx->hi[j] = hi[j];
        }
}

static void median_cut(void)
{
    static struct box boxes[255];
    int n = 1;
    boxes[0] = (struct box){ { 0, 0, 0 }, { HB - 1, HB - 1, HB - 1 }, 0 };
    shrink(&boxes[0]);
    while (n < 255) {
        int best = -1, axis = 0;
        double score = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < 3; j++) {
                int range = boxes[i].hi[j] - boxes[i].lo[j];
                /* Weighted by pixel count, with a floor so rare colours (a widget's orange) get
                 * entries too. */
                double s = range * sqrt((double)boxes[i].count);
                if (range > 0 && s > score) {
                    score = s;
                    best = i;
                    axis = j;
                }
            }
        }
        if (best < 0)
            break;
        struct box *bx = &boxes[best];
        uint64_t plane[HB] = { 0 };
        for (int r = bx->lo[0]; r <= bx->hi[0]; r++)
            for (int g = bx->lo[1]; g <= bx->hi[1]; g++)
                for (int b = bx->lo[2]; b <= bx->hi[2]; b++) {
                    int v[3] = { r, g, b };
                    plane[v[axis]] += hcount[bin(r, g, b)];
                }
        uint64_t half = bx->count / 2, acc = 0;
        int cut = bx->lo[axis];
        for (int v = bx->lo[axis]; v < bx->hi[axis]; v++) {
            acc += plane[v];
            cut = v;
            if (acc >= half)
                break;
        }
        struct box b2 = *bx;
        bx->hi[axis] = cut;
        b2.lo[axis] = cut + 1;
        shrink(bx);
        shrink(&b2);
        boxes[n++] = b2;
    }
    npal = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s[3] = { 0, 0, 0 }, c = 0;
        for (int r = boxes[i].lo[0]; r <= boxes[i].hi[0]; r++)
            for (int g = boxes[i].lo[1]; g <= boxes[i].hi[1]; g++)
                for (int b = boxes[i].lo[2]; b <= boxes[i].hi[2]; b++) {
                    int k = bin(r, g, b);
                    c += hcount[k];
                    for (int j = 0; j < 3; j++)
                        s[j] += hsum[k][j];
                }
        if (!c)
            continue;
        for (int j = 0; j < 3; j++)
            pal[npal][j] = (uint8_t)((s[j] + c / 2) / c);
        npal++;
    }
}

/* Nearest palette entry, cached per 6-bit colour. */
static int16_t nearest_cache[64 * 64 * 64];

static int nearest(int r, int g, int b)
{
    int key = ((r >> 2) * 64 + (g >> 2)) * 64 + (b >> 2);
    if (nearest_cache[key] >= 0)
        return nearest_cache[key];
    int cr = (r & ~3) | 2, cg = (g & ~3) | 2, cb = (b & ~3) | 2, best = 0;
    long bd = -1;
    for (int i = 0; i < npal; i++) {
        long dr = cr - pal[i][0], dg = cg - pal[i][1], db = cb - pal[i][2];
        long d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (bd < 0 || d < bd) {
            bd = d;
            best = i;
        }
    }
    return nearest_cache[key] = (int16_t)best;
}

static const int bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
static double dither = 6;

static void quantize(uint8_t *out)
{
    for (int y = 0; y < OH; y++)
        for (int x = 0; x < OW; x++) {
            const uint8_t *p = rgb + ((size_t)y * OW + x) * 3;
            int off = (int)lround(((bayer[y & 3][x & 3] + 0.5) / 16.0 - 0.5) * dither);
            int c[3];
            for (int j = 0; j < 3; j++) {
                c[j] = p[j] + off;
                c[j] = c[j] < 0 ? 0 : c[j] > 255 ? 255 : c[j];
            }
            out[(size_t)y * OW + x] = (uint8_t)nearest(c[0], c[1], c[2]);
        }
}

/* ---- GIF writing --------------------------------------------------------------------------- */
static FILE *gif;

struct bits { uint8_t block[255]; int blen; uint32_t acc; int n; };

static void put_code(struct bits *z, int code, int size)
{
    z->acc |= (uint32_t)code << z->n;
    z->n += size;
    while (z->n >= 8) {
        z->block[z->blen++] = z->acc & 255;
        z->acc >>= 8;
        z->n -= 8;
        if (z->blen == 255) {
            fputc(255, gif);
            fwrite(z->block, 1, 255, gif);
            z->blen = 0;
        }
    }
}

#define HSIZE 8191
static int32_t hkey[HSIZE];
static int16_t hcode[HSIZE];

static void lzw(const uint8_t *px, size_t n)
{
    struct bits z = { .blen = 0 };
    int size = 9, next = 258;
    fputc(8, gif);                                  /* minimum code size */
    memset(hkey, 0xff, sizeof(hkey));
    put_code(&z, 256, size);                        /* clear */
    int prefix = px[0];
    for (size_t i = 1; i < n; i++) {
        int c = px[i];
        int32_t key = (prefix << 8) | c;
        uint32_t h = ((uint32_t)key * 2654435761u) % HSIZE;
        while (hkey[h] >= 0 && hkey[h] != key)
            h = (h + 1) % HSIZE;
        if (hkey[h] == key) {
            prefix = hcode[h];
            continue;
        }
        put_code(&z, prefix, size);
        /* The decoder adds its entry for this code when it reads the next one, so it is one
         * behind: it widens its codes once its count reaches 1 << size, which is when ours has
         * passed it. */
        hkey[h] = key;
        hcode[h] = (int16_t)next;
        next++;
        if (next - 1 == (1 << size) && size < 12)
            size++;
        if (next == 4096) {
            put_code(&z, 256, size);
            memset(hkey, 0xff, sizeof(hkey));
            size = 9;
            next = 258;
        }
        prefix = c;
    }
    put_code(&z, prefix, size);
    next++;
    if (next - 1 == (1 << size) && size < 12)
        size++;
    put_code(&z, 257, size);                        /* end */
    if (z.n)
        z.block[z.blen++] = z.acc & 255;
    if (z.blen) {
        fputc(z.blen, gif);
        fwrite(z.block, 1, z.blen, gif);
    }
    fputc(0, gif);
}

static void put16(int v) { fputc(v & 255, gif); fputc((v >> 8) & 255, gif); }

struct pending { int have, t, x, y, w, h; uint8_t *px; };

static void write_frame(struct pending *p, int delay)
{
    fputc(0x21, gif); fputc(0xf9, gif); fputc(4, gif);
    fputc((1 << 2) | 1, gif);                       /* leave in place; index 255 transparent */
    put16(delay);
    fputc(255, gif);
    fputc(0, gif);
    fputc(0x2c, gif);
    put16(p->x); put16(p->y); put16(p->w); put16(p->h);
    fputc(0, gif);
    lzw(p->px, (size_t)p->w * p->h);
}

int main(int argc, char **argv)
{
    int a = 1;
    if (a + 1 < argc && !strcmp(argv[a], "-d")) {
        dither = atof(argv[a + 1]);
        a += 2;
    }
    if (argc - a != 3 && argc - a != 5) {
        fprintf(stderr, "usage: gifmake [-d DITHER] DIR OUT.gif WIDTH [FROM TO]\n");
        return 2;
    }
    const char *dir = argv[a], *out = argv[a + 1];
    OW = atoi(argv[a + 2]);
    char path[1024];
    snprintf(path, sizeof(path), "%s/times.txt", dir);
    FILE *tf = fopen(path, "r");
    char line[128];
    if (!tf || !fgets(line, sizeof(line), tf) || sscanf(line, "%d %d %d", &SW, &SH, &BPP) < 2 ||
        (BPP != 32 && BPP != 16)) {
        fprintf(stderr, "gifmake: can't read %s\n", path);
        return 1;
    }
    double t;
    times = malloc(sizeof(double) * 100000);
    while (nframes < 100000 && fscanf(tf, "%lf", &t) == 1)
        times[nframes++] = t;
    fclose(tf);
    double from = 0, to = nframes ? times[nframes - 1] : 0;
    if (argc - a == 5) {
        from = atof(argv[a + 3]);
        to = atof(argv[a + 4]);
    }
    snprintf(path, sizeof(path), "%s/frames.bin", dir);
    frames = fopen(path, "rb");
    if (!frames || !nframes || OW < 16 || OW > SW || (double)SW / OW > MAXTAP - 2) {
        fprintf(stderr, "gifmake: no frames, or a width that doesn't fit\n");
        return 1;
    }
    OH = (int)lround((double)SH * OW / SW);
    src = malloc((size_t)SW * SH * 4);
    raw = malloc((size_t)SW * SH * 2);
    tmp = malloc(sizeof(float) * OW * SH * 3);
    rgb = malloc((size_t)OW * OH * 3);
    xt = calloc(OW, sizeof(*xt));
    yt = calloc(OH, sizeof(*yt));
    make_taps(xt, OW, SW);
    make_taps(yt, OH, SH);

    /* The frames in range: the last one shown at FROM, then every later one up to TO. */
    int first = 0;
    for (int k = 0; k < nframes; k++)
        if (times[k] <= from)
            first = k;
    int last = first;
    for (int k = first; k < nframes && times[k] <= to; k++)
        last = k;

    /* Pass 1: the palette, from every other pixel of every frame (scaled, as shown). */
    for (int k = first; k <= last; k++) {
        if (!read_frame(k))
            return 1;
        scale_frame();
        for (size_t i = (k & 1) * 3; i < (size_t)OW * OH * 3; i += 6) {
            int r = rgb[i], g = rgb[i + 1], b = rgb[i + 2];
            int bi = bin(r >> 3, g >> 3, b >> 3);
            hcount[bi]++;
            hsum[bi][0] += r; hsum[bi][1] += g; hsum[bi][2] += b;
        }
    }
    median_cut();
    memset(nearest_cache, 0xff, sizeof(nearest_cache));

    gif = fopen(out, "wb");
    if (!gif)
        return 1;
    fwrite("GIF89a", 1, 6, gif);
    put16(OW); put16(OH);
    fputc(0xf7, gif); fputc(0, gif); fputc(0, gif);  /* 256-entry global table */
    for (int i = 0; i < 256; i++)
        fwrite(i < npal ? pal[i] : (uint8_t[3]){ 0, 0, 0 }, 1, 3, gif);
    fwrite("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 1, 19, gif);   /* loop for ever */

    /* Pass 2: frames. */
    uint8_t *shown = malloc((size_t)OW * OH), *cur = malloc((size_t)OW * OH);
    struct pending p = { 0 };
    p.px = malloc((size_t)OW * OH);
    int emitted = 0, skipped = 0;
    for (int k = first; k <= last; k++) {
        int tk = (int)lround(((k == first ? from : times[k]) - from) * 100);
        if (p.have && tk - p.t < 2) {
            if (k != last) {
                skipped++;
                continue;
            }
            tk = p.t + 2;                           /* the final picture must be shown */
        }
        if (!read_frame(k))
            return 1;
        scale_frame();
        quantize(cur);
        int x0 = OW, x1 = -1, y0 = OH, y1 = -1;
        if (!p.have) {
            x0 = 0; x1 = OW - 1; y0 = 0; y1 = OH - 1;
        } else {
            for (int y = 0; y < OH; y++)
                for (int x = 0; x < OW; x++)
                    if (cur[(size_t)y * OW + x] != shown[(size_t)y * OW + x]) {
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
            if (x1 < 0)
                continue;                           /* nothing changed: the last frame stays */
        }
        if (p.have) {
            write_frame(&p, tk - p.t);
            emitted++;
        }
        int first_frame = !p.have;
        p.have = 1;
        p.t = tk;
        p.x = x0; p.y = y0; p.w = x1 - x0 + 1; p.h = y1 - y0 + 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                size_t i = (size_t)y * OW + x;
                p.px[(size_t)(y - y0) * p.w + (x - x0)] =
                    !first_frame && cur[i] == shown[i] ? 255 : cur[i];
            }
        memcpy(shown, cur, (size_t)OW * OH);
    }
    if (p.have) {
        int end = (int)lround((to - from) * 100);
        write_frame(&p, end - p.t > 2 ? end - p.t : 2);
        emitted++;
    }
    fputc(0x3b, gif);
    long size = ftell(gif);
    fclose(gif);
    fprintf(stderr, "gifmake: %s: %dx%d, %d frames (%d skipped as under 20 ms), %.2f s, %d colours, "
            "%.2f MB\n", out, OW, OH, emitted, skipped, to - from, npal, size / 1048576.0);
    return 0;
}
