/*
 * swaprect-check: which part of a window a swap rectangle updates (kCGLCPSwapRectangle with
 * kCGLCESwapRectangle on). Core Animation presents only the changed part of a view that way.
 * Three frames: all red with the rectangle off; all green with the rectangle {0, 0, W, H/4};
 * all blue with {3W/4, H/2, W/4, H/2}. In GL window coordinates (y from the bottom) that
 * leaves the bottom quarter green, the right quarter of the top half blue and the rest red.
 * The window then stays for a capture; compare with Apple's software renderer (-s).
 *   swaprect-check [-s] [SECONDS]
 */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <string.h>

#define W 400
#define H 300

static void frame(NSOpenGLContext *ctx, float r, float g, float b)
{
    [ctx makeCurrentContext];
    glViewport(0, 0, W, H);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    [ctx flushBuffer];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    bool software = false;
    double seconds = 6;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s"))
            software = true;
        else
            seconds = atof(argv[i]);
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];

    NSOpenGLPixelFormatAttribute hw[] = { NSOpenGLPFAAccelerated, NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAColorSize, 32, 0 };
    NSOpenGLPixelFormatAttribute sw[] = { NSOpenGLPFARendererID, 0x00020400 /* generic float */,
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 32, 0 };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:software ? sw : hw];
    if (!pf) {
        fprintf(stderr, "swaprect-check: no pixel format\n");
        return 1;
    }
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 360, W, H)
        styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
    [win setTitle:software ? @"swaprect-check (software)" : @"swaprect-check"];
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    [win setContentView:view];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];

    NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
    [ctx setView:view];
    CGLContextObj c = [ctx CGLContextObj];
    [ctx makeCurrentContext];
    printf("swaprect-check: %s\n", glGetString(GL_RENDERER));

    frame(ctx, 1, 0, 0);
    GLint bottom[4] = { 0, 0, W, H / 4 }, corner[4] = { 3 * W / 4, H / 2, W / 4, H / 2 };
    CGLSetParameter(c, kCGLCPSwapRectangle, bottom);
    CGLEnable(c, kCGLCESwapRectangle);
    frame(ctx, 0, 1, 0);
    CGLSetParameter(c, kCGLCPSwapRectangle, corner);
    frame(ctx, 0, 0, 1);
    printf("swaprect-check: expect the bottom quarter green, the right quarter of the top half "
           "blue, the rest red\n");
    fflush(stdout);
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    [pool release];
    return 0;
}
