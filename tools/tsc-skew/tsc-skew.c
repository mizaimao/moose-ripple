/*
 * tsc-skew: do the guest's CPUs agree on the time stamp counter?
 *
 * Mac OS X 10.6 takes its clock (mach_absolute_time, and with it every timer deadline) from
 * the TSC and assumes all CPUs count alike. On one CPU the TSC never goes backwards, so a
 * thread that reads it in a loop and sees it step back has just moved to a CPU whose counter
 * is behind: the size of the step is the difference between the two CPUs. A deadline set on
 * one CPU and waited for on another is then early or late by that much.
 *
 *   tsc-skew [SECONDS [THREADS]]     default 5 s, 8 threads (more than CPUs, so they move)
 *
 * Prints the largest backward step each thread saw, in TSC ticks and milliseconds, and the
 * same for mach_absolute_time.
 */
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

static volatile int stop;

struct result {
    uint64_t tsc_back, tsc_back_max, mat_back, mat_back_max, reads;
};

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void *reader(void *arg)
{
    struct result *r = arg;
    uint64_t prev_tsc = rdtsc(), prev_mat = mach_absolute_time();

    while (!stop) {
        uint64_t t = rdtsc();
        if (t < prev_tsc) {
            r->tsc_back++;
            if (prev_tsc - t > r->tsc_back_max)
                r->tsc_back_max = prev_tsc - t;
        }
        prev_tsc = t;
        uint64_t m = mach_absolute_time();
        if (m < prev_mat) {
            r->mat_back++;
            if (prev_mat - m > r->mat_back_max)
                r->mat_back_max = prev_mat - m;
        }
        prev_mat = m;
        if ((++r->reads & 0x3fff) == 0)
            sched_yield();              /* give the scheduler a reason to move us */
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 5, threads = argc > 2 ? atoi(argv[2]) : 8;
    uint64_t freq = 0;
    size_t len = sizeof(freq);
    mach_timebase_info_data_t tb;

    if (threads < 1 || threads > 64 || seconds < 1)
        return 2;
    sysctlbyname("machdep.tsc.frequency", &freq, &len, NULL, 0);
    mach_timebase_info(&tb);
    pthread_t th[64];
    struct result res[64] = { { 0 } };
    for (int i = 0; i < threads; i++)
        pthread_create(&th[i], NULL, reader, &res[i]);
    sleep(seconds);
    stop = 1;
    uint64_t tsc_max = 0, mat_max = 0, tsc_n = 0, mat_n = 0, reads = 0;
    for (int i = 0; i < threads; i++) {
        pthread_join(th[i], NULL);
        if (res[i].tsc_back_max > tsc_max) tsc_max = res[i].tsc_back_max;
        if (res[i].mat_back_max > mat_max) mat_max = res[i].mat_back_max;
        tsc_n += res[i].tsc_back;
        mat_n += res[i].mat_back;
        reads += res[i].reads;
    }
    printf("TSC frequency %llu Hz, %d threads, %d s, %llu reads\n", (unsigned long long)freq,
           threads, seconds, (unsigned long long)reads);
    printf("TSC stepped back %llu times, largest step %llu ticks = %.3f ms\n",
           (unsigned long long)tsc_n, (unsigned long long)tsc_max,
           freq ? tsc_max * 1000.0 / freq : 0.0);
    printf("mach_absolute_time stepped back %llu times, largest step %.3f ms\n",
           (unsigned long long)mat_n, mat_max * (double)tb.numer / tb.denom / 1e6);
    return 0;
}
