/*
 * gamma-test: drive the display's gamma table, to see whether the driver shows it.
 *   gamma-test hold PERCENT SECONDS   every channel's top at PERCENT for SECONDS, then restored
 *   gamma-test fade SECONDS           CGDisplayFade to black over SECONDS, a second of black,
 *                                     and back (what Mac OS X does around logins, sleep and
 *                                     full-screen switches)
 *   sudo gamma-test probe ROW [COUNT [MS]]   ROW of the image the driver shows (counted from the
 *                                     top), read back from the host: each byte's mean over the
 *                                     row and eight texels; COUNT times, MS apart (default once)
 * A screenshot taken in the guest never contains the table: look at the host's display
 * (scripts/screen-vnc.sh), or, where a script can't see it (UTM), use probe.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <IOKit/IOKitLib.h>

static int probe(int row, int count, int ms)
{
    io_service_t fb = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                  IOServiceMatching("RippleFramebuffer"));
    if (!fb) {
        fprintf(stderr, "no RippleFramebuffer\n");
        return 1;
    }
    struct timeval t0, t;
    gettimeofday(&t0, NULL);
    for (int i = 0; i < count; i++) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &row);
        kern_return_t kr = IORegistryEntrySetCFProperty(fb, CFSTR("RippleGammaProbe"), n);
        CFRelease(n);
        CFTypeRef r = kr ? NULL : IORegistryEntryCreateCFProperty(fb, CFSTR("RippleGammaProbeResult"),
                                                                  NULL, 0);
        char buf[600] = "";
        if (r && CFGetTypeID(r) == CFStringGetTypeID())
            CFStringGetCString(r, buf, sizeof(buf), kCFStringEncodingUTF8);
        else if (r && CFGetTypeID(r) == CFDataGetTypeID())
            snprintf(buf, sizeof(buf), "%.*s", (int)CFDataGetLength(r), CFDataGetBytePtr(r));
        gettimeofday(&t, NULL);
        printf("%.2f s: %s\n", (t.tv_sec - t0.tv_sec) + (t.tv_usec - t0.tv_usec) / 1e6,
               kr ? "refused (run it as root)" : buf);
        fflush(stdout);
        if (r)
            CFRelease(r);
        if (kr)
            break;
        if (i + 1 < count)
            usleep((useconds_t)ms * 1000);
    }
    IOObjectRelease(fb);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "probe"))
        return probe(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 1, argc > 4 ? atoi(argv[4]) : 100);
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
    fprintf(stderr, "usage: gamma-test hold PERCENT SECONDS | gamma-test fade SECONDS | gamma-test probe ROW [COUNT [MS]]\n");
    return 2;
}
