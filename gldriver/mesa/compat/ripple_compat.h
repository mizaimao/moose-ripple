/*
 * ripple_compat.h: POSIX functions Mesa uses that Mac OS X 10.6's libc lacks (they arrived in
 * 10.7 to 10.13). Force-included into every Mesa C and C++ file by gldriver/mesa/build.sh.
 * Everything is static inline, so nothing here is exported from the GL bundle.
 */
#ifndef RIPPLE_COMPAT_H
#define RIPPLE_COMPAT_H

#if defined(__APPLE__) && !defined(__ASSEMBLER__)

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <mach/mach_time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clock_gettime (10.12). Clock ids as in later SDKs. */
#ifndef CLOCK_REALTIME
typedef int clockid_t;
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_MONOTONIC          6
#define CLOCK_PROCESS_CPUTIME_ID 12
#define CLOCK_THREAD_CPUTIME_ID  16

static inline int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id == CLOCK_REALTIME) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts->tv_sec = tv.tv_sec;
        ts->tv_nsec = tv.tv_usec * 1000;
        return 0;
    }
    if (id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW) {
        static mach_timebase_info_data_t tb;
        if (!tb.denom)
            mach_timebase_info(&tb);
        uint64_t t = mach_absolute_time();
        uint64_t ns = tb.numer == tb.denom ? t : t / tb.denom * tb.numer;
        ts->tv_sec = (time_t)(ns / 1000000000u);
        ts->tv_nsec = (long)(ns % 1000000000u);
        return 0;
    }
    errno = EINVAL;
    return -1;
}
#endif

/* strnlen and strndup (10.7). */
static inline size_t ripple_strnlen(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len])
        len++;
    return len;
}
#define strnlen ripple_strnlen

static inline char *ripple_strndup(const char *s, size_t n)
{
    size_t len = ripple_strnlen(s, n);
    char *d = (char *)malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}
#define strndup ripple_strndup

/* memmem (10.7). */
static inline void *ripple_memmem(const void *h, size_t hlen, const void *n, size_t nlen)
{
    const char *p = (const char *)h;
    if (!nlen)
        return (void *)h;
    for (; hlen >= nlen; p++, hlen--)
        if (*p == *(const char *)n && !memcmp(p, n, nlen))
            return (void *)p;
    return NULL;
}
#define memmem ripple_memmem

/* open_memstream (10.13), on top of funopen. The buffer is NUL-terminated and *bufp / *sizep
 * are updated on every write and at fclose, as POSIX asks. */
struct ripple_memstream {
    char **bufp;
    size_t *sizep;
    size_t len, cap;
};

static inline int ripple_memstream_write(void *cookie, const char *data, int n)
{
    struct ripple_memstream *m = (struct ripple_memstream *)cookie;
    if (m->len + n + 1 > m->cap) {
        size_t cap = m->cap ? m->cap : 256;
        while (m->len + n + 1 > cap)
            cap *= 2;
        char *b = (char *)realloc(*m->bufp, cap);
        if (!b) {
            errno = ENOMEM;
            return -1;
        }
        *m->bufp = b;
        m->cap = cap;
    }
    memcpy(*m->bufp + m->len, data, n);
    m->len += n;
    (*m->bufp)[m->len] = 0;
    *m->sizep = m->len;
    return n;
}

static inline int ripple_memstream_close(void *cookie)
{
    free(cookie);
    return 0;
}

static inline FILE *ripple_open_memstream(char **bufp, size_t *sizep)
{
    struct ripple_memstream *m = (struct ripple_memstream *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->bufp = bufp;
    m->sizep = sizep;
    *bufp = (char *)calloc(1, 1);
    *sizep = 0;
    if (!*bufp) {
        free(m);
        return NULL;
    }
    FILE *f = funopen(m, NULL, ripple_memstream_write, NULL, ripple_memstream_close);
    if (!f) {
        free(*bufp);
        *bufp = NULL;
        free(m);
    }
    return f;
}
#define open_memstream ripple_open_memstream

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
#endif /* RIPPLE_COMPAT_H */
