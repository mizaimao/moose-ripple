/*
 * timer-late: how late do short sleeps end?
 *
 * Each thread sleeps to an absolute deadline PERIOD ms ahead (mach_wait_until), over and over,
 * and notes how long after the deadline it runs again. On a healthy system that is well under
 * a millisecond; the tool counts the wake-ups that are more than 20 ms late and reports the
 * worst, with the time since the start so they can be matched to other logs.
 *
 *   timer-late [SECONDS [THREADS [PERIOD_MS]]]     default 20 s, 1 thread, 1 ms
 */
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static mach_timebase_info_data_t tb;
static uint64_t t_start, t_end, period;
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

struct result {
    int id;
    uint64_t wakes, late, worst;
    double sum;
};

static double ms(uint64_t abs)
{
    return abs * (double)tb.numer / tb.denom / 1e6;
}

static void *sleeper(void *arg)
{
    struct result *r = arg;

    for (;;) {
        uint64_t deadline = mach_absolute_time() + period;
        if (deadline >= t_end)
            break;
        mach_wait_until(deadline);
        uint64_t now = mach_absolute_time(), over = now > deadline ? now - deadline : 0;
        r->wakes++;
        r->sum += ms(over);
        if (over > r->worst)
            r->worst = over;
        if (ms(over) > 20.0) {
            r->late++;
            pthread_mutex_lock(&print_lock);
            printf("  thread %d: %.1f ms late at %.2f s\n", r->id, ms(over), ms(now - t_start) / 1e3);
            fflush(stdout);
            pthread_mutex_unlock(&print_lock);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20, threads = argc > 2 ? atoi(argv[2]) : 1;
    double period_ms = argc > 3 ? atof(argv[3]) : 1.0;

    if (threads < 1 || threads > 64 || seconds < 1 || period_ms <= 0)
        return 2;
    mach_timebase_info(&tb);
    period = (uint64_t)(period_ms * 1e6 * tb.denom / tb.numer);
    t_start = mach_absolute_time();
    t_end = t_start + (uint64_t)(seconds * 1e9 * tb.denom / tb.numer);
    pthread_t th[64];
    struct result res[64] = { { 0 } };
    for (int i = 0; i < threads; i++) {
        res[i].id = i;
        pthread_create(&th[i], NULL, sleeper, &res[i]);
    }
    uint64_t wakes = 0, late = 0, worst = 0;
    double sum = 0;
    for (int i = 0; i < threads; i++) {
        pthread_join(th[i], NULL);
        wakes += res[i].wakes;
        late += res[i].late;
        sum += res[i].sum;
        if (res[i].worst > worst)
            worst = res[i].worst;
    }
    printf("%d thread(s), %d s, sleeps of %.1f ms: %llu wake-ups, mean %.3f ms late, worst "
           "%.1f ms, %llu over 20 ms\n", threads, seconds, period_ms, (unsigned long long)wakes,
           wakes ? sum / wakes : 0.0, ms(worst), (unsigned long long)late);
    return 0;
}
