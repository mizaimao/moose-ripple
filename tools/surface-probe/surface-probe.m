/*
 * surface-probe: what WindowServer does when a window's surface is bound the way a hardware GL
 * driver binds it (docs/re/ioaccelerator-interface.md, trace 3). It makes a window, adds a
 * surface to it and calls CGSBindSurface(cid, wid, sid, KIND, MODE, DISPLAY), then asks our
 * kext whether an accelerator surface with that ID exists now and where it is
 * (RIPPLE_GPU_SURFACE_INFO): WindowServer creates it, the app does not.
 *   surface-probe [-k KIND] [-m MODE] [-d DISPLAY] [SECONDS]
 * Defaults: kind 2, mode 0x24, the main display's ID, 8 s. The window moves after 3 s and the
 * kext is asked again, to see the region follow.
 * Built without ARC (10.6).
 */
#import <Cocoa/Cocoa.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ripple_gpu_uapi.h"

extern int CGSMainConnectionID(void);
extern int CGSAddSurface(int cid, int wid, int *sid);
extern int CGSRemoveSurface(int cid, int wid, int sid);
extern int CGSSetSurfaceBounds(int cid, int wid, int sid, CGRect bounds);
extern int CGSOrderSurface(int cid, int wid, int sid, int place, int relative);
extern int CGSBindSurface(int cid, int wid, int sid, int kind, int mode, unsigned last);

static io_connect_t gpu;

static void ask(const char *when, int sid)
{
    struct ripple_gpu_surface_info si;
    uint64_t in = (uint32_t)sid;
    size_t size = sizeof(si);

    memset(&si, 0, sizeof(si));
    kern_return_t kr = gpu ? IOConnectCallMethod(gpu, RIPPLE_GPU_SURFACE_INFO, &in, 1, NULL, 0,
                                                 NULL, NULL, &si, &size) : KERN_FAILURE;
    if (kr != KERN_SUCCESS)
        printf("%s: the kext has no surface 0x%x (0x%x)\n", when, sid, kr);
    else
        printf("%s: kext surface 0x%x: flags 0x%x, %ux%u at %d,%d on a %ux%u screen, shape calls %u\n",
               when, si.id, si.flags, si.width, si.height, si.x, si.y, si.screen_width,
               si.screen_height, si.shape_seq);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int kind = 2, mode = 0x24;
    unsigned last = CGMainDisplayID();
    double seconds = 8;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc)
            kind = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            mode = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            last = (unsigned)strtoul(argv[++i], NULL, 0);
        else
            seconds = atof(argv[i]);
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                   IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
    if (!svc || IOServiceOpen(svc, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &gpu) != KERN_SUCCESS) {
        printf("no connection to the kext's GPU client; going on without asking it\n");
        gpu = 0;
    }

    NSRect content = NSMakeRect(300, 300, 400, 300);
    NSWindow *win = [[NSWindow alloc] initWithContentRect:content
                                                styleMask:NSTitledWindowMask
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [win setTitle:@"surface-probe"];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];

    int cid = CGSMainConnectionID(), wid = (int)[win windowNumber], sid = 0;
    int e = CGSAddSurface(cid, wid, &sid);
    printf("CGSAddSurface(cid %d, wid %d) = %d, surface %d (0x%x)\n", cid, wid, e, sid, sid);
    e = CGSSetSurfaceBounds(cid, wid, sid, CGRectMake(0, 0, 400, 300));
    printf("CGSSetSurfaceBounds = %d\n", e);
    e = CGSOrderSurface(cid, wid, sid, 1, 0);
    printf("CGSOrderSurface = %d\n", e);
    ask("before binding", sid);
    e = CGSBindSurface(cid, wid, sid, kind, mode, last);
    printf("CGSBindSurface(kind %d, mode 0x%x, 0x%x) = %d\n", kind, mode, last, e);
    fflush(stdout);
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    ask("after binding", sid);

    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.5]];
    [win setFrameOrigin:NSMakePoint(600, 150)];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
    ask("after moving the window", sid);

    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds > 4.5 ? seconds - 4.5 : 0.1]];
    e = CGSRemoveSurface(cid, wid, sid);
    printf("CGSRemoveSurface = %d\n", e);
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    ask("after removing the surface", sid);
    [pool drain];
    return 0;
}
