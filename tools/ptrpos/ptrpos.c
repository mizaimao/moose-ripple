/*
 * ptrpos: print where the guest thinks the mouse pointer is, and the main display's bounds.
 *   ptrpos           once
 *   ptrpos N         N times, every half second
 * Used to check tablet (absolute pointer) mapping against positions injected from QEMU.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    CGRect b = CGDisplayBounds(CGMainDisplayID());
    printf("main display %.0fx%.0f at %.0f,%.0f\n", b.size.width, b.size.height,
           b.origin.x, b.origin.y);
    for (int i = 0; i < n; i++) {
        CGEventRef e = CGEventCreate(NULL);
        CGPoint p = CGEventGetLocation(e);
        CFRelease(e);
        printf("pointer %.1f,%.1f\n", p.x, p.y);
        fflush(stdout);
        if (i + 1 < n)
            usleep(500000);
    }
    return 0;
}
