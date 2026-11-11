/*
 * frame-meter: WindowServer's own performance figures (CGSGetPerformanceData, the private call
 * behind Quartz Debug's frame meter), sampled while something runs, for any renderer: ours,
 * Apple's software compositor, the 9600 GT (performance stage, 2026-11-11). The four values are
 * rates over the last second; on VM 210 (1280x800): screens' worth of pixels updated per
 * second, the same in pixels, frames composited per second (Quartz Debug's frame meter),
 * update requests per second. Prints the peak and mean of each.
 *   frame-meter [SECONDS [INTERVAL [v]]]   default 6 s, 0.25 s; v: every sample
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef int CGSConnectionID;
extern CGSConnectionID CGSMainConnectionID(void);
extern CGError CGSGetPerformanceData(CGSConnectionID cid, float *a, float *b, float *c, float *d);

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 6.0, interval = argc > 2 ? atof(argv[2]) : 0.25;
    CGSConnectionID cid = CGSMainConnectionID();
    double sum[4] = { 0 }, peak[4] = { 0 };
    int n = 0;
    for (double t = 0; t < seconds; t += interval) {
        float v[4] = { 0 };
        CGError e = CGSGetPerformanceData(cid, &v[0], &v[1], &v[2], &v[3]);
        if (argc > 3)
            printf("%5.2f s: %8.2f %12.0f %8.2f %8.2f (error %d)\n", t, v[0], v[1], v[2], v[3], e);
        for (int i = 0; i < 4; i++) {
            sum[i] += v[i];
            if (v[i] > peak[i])
                peak[i] = v[i];
        }
        n++;
        usleep((useconds_t)(interval * 1e6));
    }
    if (!n)
        return 1;
    printf("frame-meter: %d samples; frames/s peak %.0f mean %.0f; screens/s peak %.1f mean %.1f; "
           "update requests/s peak %.0f mean %.0f\n", n, peak[2], sum[2] / n, peak[0], sum[0] / n,
           peak[3], sum[3] / n);
    return 0;
}
