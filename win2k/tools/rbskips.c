/* rbskips: what the driver has skipped so far, by kind, for the end of a benchmark run, so that
 * the run fails on its own. Prints one line,
 *   RBSKIPS run=NAME draws=0 clears=0 ... total=0 result=PASS
 * and exits with 0. Any count above zero: result=FAIL and exit code 1. No counts from the running
 * driver (a release build, or another driver): result=UNAVAILABLE and exit code 2.
 * "rbskips NAME" puts the run's name in the line. Development only (see drvcheck.h). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "drvcheck.h"

int main(int argc, char **argv)
{
    static const char *const names[RB2K_SKIP_KINDS] = RB2K_SKIP_KIND_NAMES;
    unsigned long counts[SkipKindsMax], total = 0;
    int kinds, k;
    memset(counts, 0, sizeof(counts));
    kinds = driver_skips(counts);
    printf("RBSKIPS");
    if (argc > 1) printf(" run=%s", argv[1]);
    if (!kinds) { printf(" result=UNAVAILABLE\n"); return 2; }
    for (k = 0; k < kinds; ++k) {
        if (skipsNamed && k < RB2K_SKIP_KINDS) printf(" %s=%lu", names[k], counts[k]);
        else printf(" kind%d=%lu", k, counts[k]);
        total += counts[k];
    }
    printf(" total=%lu result=%s\n", total, total ? "FAIL" : "PASS");
    return total ? 1 : 0;
}
