/*
 * gamma-test: drive the display's gamma table, to see whether the driver shows it.
 *   gamma-test hold PERCENT SECONDS   every channel's top at PERCENT for SECONDS, then restored
 *   gamma-test fade SECONDS           CGDisplayFade to black over SECONDS, a second of black,
 *                                     and back (what Mac OS X does around logins, sleep and
 *                                     full-screen switches)
 * A screenshot taken in the guest never contains the table: look at the host's display
 * (scripts/screen-vnc.sh).
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    CGDirectDisplayID display = CGMainDisplayID();
    if (argc == 4 && !strcmp(argv[1], "hold")) {
        float top = (float)atof(argv[2]) / 100.0f;
        CGError e = CGSetDisplayTransferByFormula(display, 0, top, 1, 0, top, 1, 0, top, 1);
        printf("set to %.2f: error %d\n", top, (int)e);
        fflush(stdout);
        usleep((useconds_t)(atof(argv[3]) * 1e6));
        CGDisplayRestoreColorSyncSettings();
        printf("restored\n");
        return e != kCGErrorSuccess;
    }
    if (argc == 3 && !strcmp(argv[1], "fade")) {
        CGDisplayFadeInterval seconds = (CGDisplayFadeInterval)atof(argv[2]);
        CGDisplayFadeReservationToken token;
        CGError e = CGAcquireDisplayFadeReservation(kCGMaxDisplayReservationInterval, &token);
        if (e != kCGErrorSuccess) {
            fprintf(stderr, "no fade reservation: error %d\n", (int)e);
            return 1;
        }
        e = CGDisplayFade(token, seconds, kCGDisplayBlendNormal, kCGDisplayBlendSolidColor, 0, 0, 0, true);
        printf("faded out: error %d\n", (int)e);
        fflush(stdout);
        sleep(1);
        e = CGDisplayFade(token, seconds, kCGDisplayBlendSolidColor, kCGDisplayBlendNormal, 0, 0, 0, true);
        printf("faded in: error %d\n", (int)e);
        CGReleaseDisplayFadeReservation(token);
        return e != kCGErrorSuccess;
    }
    fprintf(stderr, "usage: gamma-test hold PERCENT SECONDS | gamma-test fade SECONDS\n");
    return 2;
}
