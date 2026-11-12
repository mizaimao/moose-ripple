/*
 * vncrec: record QEMU's VNC display in real time, on the VM host itself (no network between
 * QEMU and the recorder, so whole-screen updates arrive at QEMU's own 30 ms pace instead of
 * every 50-250 ms through the ssh relay). Frames are written as they come, stamped with their
 * arrival time; tools/gifmake makes the GIF. Speaks RFB 3.8 with VNC authentication on stdin and
 * stdout, joined by scripts/vnc-record.sh to `qm vncproxy` on the host. Built static on dev.lan,
 * copied to the host's /tmp for one recording and removed afterwards.
 *   VNC_PASSWORD=<pw> vncrec SECONDS OUTDIR     writes OUTDIR/frames.bin and OUTDIR/times.txt
 * With VNCREC_TIMES set it keeps no pictures: times.txt gets one line per screen update, "wall
 * clock time (s), rectangles, pixels changed", for measuring how evenly frames reach a viewer
 * (scripts/pace-run.sh). There VNCREC_ENCODING=tight asks for Tight encoding, as web consoles
 * (noVNC) do, and skips the data unread: what the server can deliver when it has to compress.
 * A fourth number on each line is the bytes the update took on the wire.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const uint8_t PC1[] = { 57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const uint8_t PC2[] = { 14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const uint8_t SHIFTS[] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const uint8_t IP[] = { 58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const uint8_t FP[] = { 40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const uint8_t E[] = { 32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const uint8_t P[] = { 16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const uint8_t S[8][64] = {
        {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
    4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
    0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
    13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
    10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
    4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
    9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
    1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
    7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11},
};

static uint64_t permute(uint64_t v, const uint8_t *table, int n, int nbits)
{
    uint64_t out = 0;
    for (int i = 0; i < n; i++)
        out = (out << 1) | ((v >> (nbits - table[i])) & 1);
    return out;
}

static uint64_t des_encrypt(uint64_t key, uint64_t block)
{
    uint64_t k = permute(key, PC1, 56, 64), sub[16];
    uint32_t c = (uint32_t)(k >> 28), d = (uint32_t)(k & 0xFFFFFFF);
    for (int i = 0; i < 16; i++) {
        int s = SHIFTS[i];
        c = ((c << s) | (c >> (28 - s))) & 0xFFFFFFF;
        d = ((d << s) | (d >> (28 - s))) & 0xFFFFFFF;
        sub[i] = permute(((uint64_t)c << 28) | d, PC2, 48, 56);
    }
    uint64_t v = permute(block, IP, 64, 64);
    uint32_t l = (uint32_t)(v >> 32), r = (uint32_t)v;
    for (int i = 0; i < 16; i++) {
        uint64_t x = permute(r, E, 48, 32) ^ sub[i];
        uint32_t f = 0;
        for (int j = 0; j < 8; j++) {
            unsigned six = (x >> (42 - 6 * j)) & 0x3F;
            f = (f << 4) | S[j][(six & 0x20) | ((six & 1) << 4) | ((six >> 1) & 0xF)];
        }
        uint32_t t = r;
        r = l ^ (uint32_t)permute(f, P, 32, 32);
        l = t;
    }
    return permute(((uint64_t)r << 32) | l, FP, 64, 64);
}

/* ---- Buffered I/O on stdin/stdout ----------------------------------------------------------- */
static uint8_t ibuf[1 << 20];
static size_t ipos, ilen;
static unsigned long long rx_bytes;      /* bytes received so far */
static double deadline;          /* while waiting for a new message: give up after this */
static int in_fd = 0, out_fd = 1;  /* stdin/stdout, or QEMU's VNC socket (VNC_SOCKET) */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 0 at the deadline (only when `may_stop`), else 1; exits on errors. */
static int fill(int may_stop)
{
    for (;;) {
        if (may_stop && deadline > 0) {
            double left = deadline - now();
            if (left <= 0)
                return 0;
            struct pollfd p = { in_fd, POLLIN, 0 };
            if (poll(&p, 1, (int)(left * 1000) + 1) == 0)
                continue;
        }
        ssize_t n = read(in_fd, ibuf, sizeof(ibuf));
        if (n > 0) {
            ipos = 0;
            ilen = (size_t)n;
            rx_bytes += (unsigned long long)n;
            return 1;
        }
        if (n < 0 && errno == EINTR)
            continue;
        fprintf(stderr, "vncrec: connection closed\n");
        exit(1);
    }
}

static void get(void *dst, size_t n)
{
    uint8_t *o = dst;
    while (n) {
        if (ipos == ilen)
            fill(0);
        size_t k = ilen - ipos < n ? ilen - ipos : n;
        memcpy(o, ibuf + ipos, k);
        ipos += k;
        o += k;
        n -= k;
    }
}

static void put(const void *src, size_t n)
{
    const uint8_t *p = src;
    while (n) {
        ssize_t k = write(out_fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            fprintf(stderr, "vncrec: write failed\n");
            exit(1);
        }
        p += k;
        n -= (size_t)k;
    }
}

static unsigned be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void request(int incremental, unsigned w, unsigned h)
{
    uint8_t m[10] = { 3, (uint8_t)incremental, 0, 0, 0, 0, (uint8_t)(w >> 8), (uint8_t)w,
                      (uint8_t)(h >> 8), (uint8_t)h };
    put(m, sizeof(m));
}

static void skip(size_t n)
{
    static uint8_t junk[16384];
    while (n) {
        size_t k = n < sizeof(junk) ? n : sizeof(junk);
        get(junk, k);
        n -= k;
    }
}

static size_t tight_length(void)
{
    uint8_t b;
    size_t len;
    get(&b, 1);
    len = b & 0x7f;
    if (b & 0x80) {
        get(&b, 1);
        len |= (size_t)(b & 0x7f) << 7;
        if (b & 0x80) {
            get(&b, 1);
            len |= (size_t)b << 14;
        }
    }
    return len;
}

/* One Tight rectangle, unread (pixels are 3 bytes each with our pixel format). */
static void skip_tight(unsigned w, unsigned h)
{
    uint8_t ctl, b;
    get(&ctl, 1);
    unsigned type = ctl >> 4;
    if (type == 8) {                              /* fill */
        skip(3);
    } else if (type == 9) {                       /* JPEG */
        skip(tight_length());
    } else if (type > 9) {
        fprintf(stderr, "vncrec: unknown Tight rectangle type %u\n", type);
        exit(1);
    } else {                                      /* basic, zlib */
        size_t row = (size_t)w * 3;
        if (ctl & 0x40) {
            get(&b, 1);                           /* filter */
            if (b == 1) {                         /* palette */
                uint8_t n;
                get(&n, 1);
                skip(((size_t)n + 1) * 3);
                row = n + 1 <= 2 ? ((size_t)w + 7) / 8 : w;
            }
        }
        size_t size = row * h;
        skip(size < 12 ? size : tight_length());
    }
}

int main(int argc, char **argv)
{
    const char *pw = getenv("VNC_PASSWORD");
    if (argc != 3 || !pw) {
        fprintf(stderr, "usage: VNC_PASSWORD=<pw> vncrec SECONDS OUTDIR  (RFB on stdin/stdout)\n");
        return 2;
    }
    double seconds = atof(argv[1]);
    /* VNC_SOCKET=/var/run/qemu-server/ID.vnc: talk to QEMU directly, without qm vncproxy's relay
     * (the caller sets the password over QMP first). */
    const char *sock = getenv("VNC_SOCKET");
    if (sock) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            fprintf(stderr, "vncrec: can't connect to %s\n", sock);
            return 1;
        }
        in_fd = out_fd = fd;
    }
    uint8_t b[64];

    get(b, 12);                                   /* "RFB 003.008\n" */
    put("RFB 003.008\n", 12);
    get(b, 1);
    unsigned ntypes = b[0];
    if (!ntypes) {
        fprintf(stderr, "vncrec: server refused the connection\n");
        return 1;
    }
    get(b, ntypes);
    if (!memchr(b, 2, ntypes)) {
        fprintf(stderr, "vncrec: no VNC authentication offered\n");
        return 1;
    }
    put("\x02", 1);
    uint8_t ch[16], resp[16];
    get(ch, 16);
    uint64_t key = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = i < (int)strlen(pw) ? (uint8_t)pw[i] : 0, rev = 0;
        for (int bit = 0; bit < 8; bit++)
            rev |= ((c >> bit) & 1) << (7 - bit);
        key = key << 8 | rev;
    }
    for (int half = 0; half < 2; half++) {
        uint64_t blk = 0;
        for (int i = 0; i < 8; i++)
            blk = blk << 8 | ch[half * 8 + i];
        uint64_t e = des_encrypt(key, blk);
        for (int i = 0; i < 8; i++)
            resp[half * 8 + i] = (uint8_t)(e >> (56 - 8 * i));
    }
    put(resp, 16);
    get(b, 4);
    if (be32(b)) {
        fprintf(stderr, "vncrec: VNC authentication failed\n");
        return 1;
    }
    put("\x01", 1);                               /* ClientInit, shared */
    get(b, 24);
    unsigned w = be16(b), h = be16(b + 2);
    uint32_t nlen = be32(b + 20);
    while (nlen) {
        size_t k = nlen < sizeof(b) ? nlen : sizeof(b);
        get(b, k);
        nlen -= (uint32_t)k;
    }
    /* SetPixelFormat: 32 bpp, depth 24, little-endian, true colour, R/G/B at 16/8/0 (BGRX). */
    uint8_t pf[20] = { 0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0 };
    put(pf, sizeof(pf));
    uint8_t enc[12] = { 2, 0, 0, 2, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x21 };   /* Raw, DesktopSize */
    /* Tight, DesktopSize, JPEG quality 6 and compression level 2: noVNC's defaults. */
    uint8_t tight_enc[20] = { 2, 0, 0, 4, 0, 0, 0, 7, 0xff, 0xff, 0xff, 0x21,
                              0xff, 0xff, 0xff, 0xe6, 0xff, 0xff, 0xff, 0x02 };
    const char *want = getenv("VNCREC_ENCODING");
    int tight = want && !strcmp(want, "tight") && getenv("VNCREC_TIMES");
    if (tight)
        put(tight_enc, sizeof(tight_enc));
    else
        put(enc, sizeof(enc));

    char path[1024];
    snprintf(path, sizeof(path), "%s/frames.bin", argv[2]);
    int times_only = getenv("VNCREC_TIMES") != NULL;
    FILE *frames = times_only ? NULL : fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/times.txt", argv[2]);
    FILE *times = fopen(path, "w");
    if ((!frames && !times_only) || !times) {
        fprintf(stderr, "vncrec: can't write to %s\n", argv[2]);
        return 1;
    }
    if (frames)
        setvbuf(frames, NULL, _IOFBF, 8 << 20);
    uint8_t *fb = calloc((size_t)w * h, 4);
    double t0 = 0;
    unsigned count = 0;
    request(0, w, h);
    for (;;) {
        if (count)
            deadline = t0 + seconds + 0.3;
        if (ipos == ilen && !fill(1))
            break;                                /* still screen at the end */
        get(b, 1);
        if (b[0] == 0) {
            get(b, 3);
            unsigned nrect = be16(b + 1);
            unsigned long area = 0;
            unsigned long long rx0 = rx_bytes - (ilen - ipos) - 4;
            /* Ask for the next one at once: QEMU prepares it while this one arrives. */
            request(1, w, h);
            for (unsigned r = 0; r < nrect; r++) {
                get(b, 12);
                unsigned x = be16(b), y = be16(b + 2), rw = be16(b + 4), rh = be16(b + 6);
                int32_t e = (int32_t)be32(b + 8);
                if (e == -223) {
                    fprintf(stderr, "vncrec: the screen size changed; stopping\n");
                    goto done;
                }
                if (e == 7 && tight) {
                    area += (unsigned long)rw * rh;
                    skip_tight(rw, rh);
                    continue;
                }
                if (e != 0) {
                    fprintf(stderr, "vncrec: unexpected encoding %d\n", e);
                    return 1;
                }
                area += (unsigned long)rw * rh;
                for (unsigned row = 0; row < rh; row++) {
                    if (y + row < h && x + rw <= w)
                        get(fb + (((size_t)(y + row) * w) + x) * 4, (size_t)rw * 4);
                    else {
                        static uint8_t skip[16384];
                        for (size_t left = (size_t)rw * 4; left;) {
                            size_t k = left < sizeof(skip) ? left : sizeof(skip);
                            get(skip, k);
                            left -= k;
                        }
                    }
                }
            }
            double t = now();
            if (!count)
                t0 = t;
            if (times_only)
                fprintf(times, "%.6f %u %lu %llu\n", wall(), nrect, area,
                        rx_bytes - (ilen - ipos) - rx0);
            else {
                fwrite(fb, 4, (size_t)w * h, frames);
                fprintf(times, "%.4f\n", t - t0);
            }
            count++;
            if (t - t0 >= seconds)
                break;
        } else if (b[0] == 1) {
            get(b, 5);
            unsigned n = be16(b + 3);
            while (n--)
                get(b, 6);
        } else if (b[0] == 2) {
            /* bell */
        } else if (b[0] == 3) {
            get(b, 7);
            uint32_t n = be32(b + 3);
            while (n) {
                size_t k = n < sizeof(b) ? n : sizeof(b);
                get(b, k);
                n -= (uint32_t)k;
            }
        } else {
            fprintf(stderr, "vncrec: unknown server message %u\n", b[0]);
            return 1;
        }
    }
done:
    if (frames)
        fclose(frames);
    fclose(times);
    /* times.txt gets its header last: the size line first, then the times. */
    snprintf(path, sizeof(path), "%s/size.txt", argv[2]);
    FILE *sz = fopen(path, "w");
    if (sz) {
        fprintf(sz, "%u %u 32\n", w, h);
        fclose(sz);
    }
    fprintf(stderr, "vncrec: %u frames of %ux%u in %.2f s\n", count, w, h, count ? now() - t0 : 0.0);
    return 0;
}
