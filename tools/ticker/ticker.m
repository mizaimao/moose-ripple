/*
 * ticker: keeps the screen changing by one pixel, so QEMU's VNC server keeps sending frames
 * every 30 ms (scripts/vnc-record.sh: after a still spell it sends at most every 3 s and speeds
 * up only step by step, so an animation's first second came out as two or three frames). A 1x1
 * window at the top-left corner, above everything (Dashboard included), black, with its red
 * channel flipping between 0 and 1 every 20 ms: invisible, and the corner is black anyway (the
 * menu bar's rounded corner). No Dock icon, no menu bar, doesn't take the focus.
 *   ticker [SECONDS]      default 60
 */
#import <Cocoa/Cocoa.h>
#include <stdlib.h>

@interface Ticker : NSObject {
@public
    NSWindow *win;
    int phase;
}
- (void)tick:(NSTimer *)t;
@end

@implementation Ticker
- (void)tick:(NSTimer *)t
{
    phase ^= 1;
    [win setBackgroundColor:[NSColor colorWithDeviceRed:phase / 255.0 green:0 blue:0 alpha:1]];
    [win display];
}
@end

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    double seconds = argc > 1 ? atof(argv[1]) : 60;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    NSRect screen = [[NSScreen mainScreen] frame];
    Ticker *t = [[Ticker alloc] init];
    t->win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, NSMaxY(screen) - 1, 1, 1)
                                         styleMask:NSBorderlessWindowMask
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [t->win setLevel:NSScreenSaverWindowLevel];
    [t->win setOpaque:YES];
    [t->win setHasShadow:NO];
    [t->win setIgnoresMouseEvents:YES];
    [t->win setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces];
    [t->win setBackgroundColor:[NSColor blackColor]];
    [t->win orderFrontRegardless];
    [NSTimer scheduledTimerWithTimeInterval:0.02 target:t selector:@selector(tick:) userInfo:nil
                                    repeats:YES];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    [pool release];
    return 0;
}
