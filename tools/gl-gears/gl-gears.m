/*
 * gl-gears: M4's window test (HANDOFF.md, M4 acceptance). An NSOpenGLView in a window draws
 * three spinning gears with plain OpenGL 1.x/2.1 calls, double-buffered, and prints the
 * renderer and the frame rate. It asks for an accelerated renderer without recovery, so it
 * runs on our driver (start it with RIPPLE_GL=1) or fails if there is none.
 *   gl-gears [OPTIONS] [SECONDS]   quit after SECONDS (default 10). Prints the frame rate and the
 *                                  longest frame every 2 s.
 *     -w WIDTHxHEIGHT   the size of the drawing area (default 400x400)
 *     -s                Apple's software renderer (a reference)
 *     -a                clear with alpha 0 (an opaque surface must still show the background)
 *     -o                a surface that is not opaque: the desktop shows through around the gears
 *     -f N              stop drawing after N frames: what stays on screen is up to the system
 *   Things the window does by itself, from 3 s on, the same on any driver:
 *     -r                resize to 640x480
 *     -m                move, 60 steps of 5,2 pixels, about 30 a second, then stay
 *     -c                a second, plain window crosses over it and leaves (covers, uncovers)
 *     -z                minimise, and come back at 6 s
 * Written for moose-ripple; geometry is our own (a disc of teeth, extruded).
 * Built without ARC (10.6): manual retain/release.
 */
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double run_seconds = 10;
static float clear_alpha = 1;   /* -a: clear with alpha 0 (an opaque surface must still show the background) */
static int resize_test;        /* -r: grow the window to 640x480 after 3 s */
static int move_test;          /* -m: the window moves by itself from 3 s to about 5 s, then stays */
static int cover_test;         /* -c: a plain window crosses over the GL window from 3 s on */
static int mini_test;          /* -z: minimise at 3 s, restore at 6 s */
static int not_opaque;        /* -o: surface opacity 0 */
static int max_frames;         /* -f N: stop drawing after N frames */
static int drawn;              /* frames drawn so far */
static NSWindow *cover;
static int win_w = 400, win_h = 400;   /* -w WIDTHxHEIGHT */
static int software;           /* -s: Apple's software renderer instead of an accelerated one */

/* One gear: `teeth` teeth between radius r0 (hub hole) and r1/r2 (root/tip), width w. */
static GLuint make_gear(float r0, float r1, float r2, float w, int teeth, const GLfloat *color)
{
    GLuint list = glGenLists(1);
    float da = 2.0f * (float)M_PI / teeth / 4.0f;

    glNewList(list, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, color);
    glShadeModel(GL_FLAT);
    for (int side = 0; side < 2; side++) {          /* front and back faces */
        float z = side ? -w / 2 : w / 2;
        glNormal3f(0, 0, side ? -1.0f : 1.0f);
        glBegin(GL_QUAD_STRIP);
        for (int i = 0; i <= teeth; i++) {
            float a = i * 2.0f * (float)M_PI / teeth;
            glVertex3f(r0 * cosf(a), r0 * sinf(a), z);
            glVertex3f(r1 * cosf(a), r1 * sinf(a), z);
        }
        glEnd();
        glBegin(GL_QUADS);
        for (int i = 0; i < teeth; i++) {
            float a = i * 2.0f * (float)M_PI / teeth;
            glVertex3f(r1 * cosf(a), r1 * sinf(a), z);
            glVertex3f(r2 * cosf(a + da), r2 * sinf(a + da), z);
            glVertex3f(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), z);
            glVertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), z);
        }
        glEnd();
    }
    glBegin(GL_QUAD_STRIP);                          /* outside, tooth by tooth */
    for (int i = 0; i <= teeth * 4; i++) {
        float a = i * da;
        int k = i % 4;
        float r = (k == 1 || k == 2) ? r2 : r1;
        glNormal3f(cosf(a), sinf(a), 0);
        glVertex3f(r * cosf(a), r * sinf(a), w / 2);
        glVertex3f(r * cosf(a), r * sinf(a), -w / 2);
    }
    glEnd();
    glShadeModel(GL_SMOOTH);
    glBegin(GL_QUAD_STRIP);                          /* hub hole */
    for (int i = 0; i <= teeth; i++) {
        float a = i * 2.0f * (float)M_PI / teeth;
        glNormal3f(-cosf(a), -sinf(a), 0);
        glVertex3f(r0 * cosf(a), r0 * sinf(a), -w / 2);
        glVertex3f(r0 * cosf(a), r0 * sinf(a), w / 2);
    }
    glEnd();
    glEndList();
    return list;
}

@interface GearsView : NSOpenGLView {
    GLuint gears[3];
    double start, last_report, last_frame, longest;
    int frames;
}
@end

@implementation GearsView
- (void)prepareOpenGL
{
    static const GLfloat light[4] = { 5, 5, 10, 0 };
    static const GLfloat red[4] = { 0.8f, 0.1f, 0.0f, 1 }, green[4] = { 0.0f, 0.8f, 0.2f, 1 },
                         blue[4] = { 0.2f, 0.2f, 1.0f, 1 };
    GLint swap = 0;                                   /* don't wait for vsync */
    [[self openGLContext] setValues:&swap forParameter:NSOpenGLCPSwapInterval];
    if (not_opaque) {
        GLint opacity = 0;
        [[self openGLContext] setValues:&opacity forParameter:NSOpenGLCPSurfaceOpacity];
    }
    glLightfv(GL_LIGHT0, GL_POSITION, light);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);
    /* Pitch radii 4 and 2 with teeth 0.7 deep, so the gears mesh at the positions and phases
     * used in drawRect (the classic glxgears layout). */
    gears[0] = make_gear(1.0f, 3.65f, 4.35f, 1.0f, 20, red);
    gears[1] = make_gear(0.5f, 1.65f, 2.35f, 2.0f, 10, green);
    gears[2] = make_gear(1.3f, 1.65f, 2.35f, 0.5f, 10, blue);
    printf("GL_RENDERER %s\nGL_VERSION  %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    fflush(stdout);
    start = last_report = [NSDate timeIntervalSinceReferenceDate];
    [NSTimer scheduledTimerWithTimeInterval:0.001 target:self selector:@selector(tick:)
                                   userInfo:nil repeats:YES];
}

/* -o: the window's own pixels under the view are clear, so the desktop shows through. */
- (BOOL)isOpaque
{
    return !not_opaque;
}

- (void)reshape
{
    NSRect r = [self bounds];
    float h = r.size.width > 0 ? (float)(r.size.height / r.size.width) : 1;
    [[self openGLContext] makeCurrentContext];
    glViewport(0, 0, (GLsizei)r.size.width, (GLsizei)r.size.height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -h, h, 5, 60);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -40);
}

- (void)tick:(NSTimer *)t
{
    double now = [NSDate timeIntervalSinceReferenceDate];
    if (resize_test == 1 && now - start > 3) {
        resize_test = 2;
        NSWindow *w = [self window];
        NSRect f = [w frameRectForContentRect:NSMakeRect(100, 100, 640, 480)];
        [w setFrame:f display:YES];
        printf("resized to 640x480\n");
        fflush(stdout);
    }
    if (move_test && now - start > 3 && move_test <= 60) {
        /* 60 steps of 5,2 pixels, about 30 a second: what WindowServer does with a GL
         * surface while its window moves, without a pointer. */
        static double last_step;
        if (now - last_step >= 0.033) {
            NSWindow *w = [self window];
            NSPoint o = [w frame].origin;
            [w setFrameOrigin:NSMakePoint(o.x + 5, o.y - 2)];
            last_step = now;
            if (move_test++ == 60) {
                printf("moved\n");
                fflush(stdout);
            }
        }
    }
    if (cover_test && now - start > 3 && cover_test <= 75) {
        /* 75 steps of 10 pixels: from the left of the GL window, across it, and clear of it. */
        static double last_cover;
        if (now - last_cover >= 0.033) {
            NSPoint o = [cover frame].origin;
            [cover setFrameOrigin:NSMakePoint(o.x + 10, o.y)];
            last_cover = now;
            if (cover_test++ == 75) {
                printf("uncovered\n");
                fflush(stdout);
            }
        }
    }
    if (mini_test == 1 && now - start > 3) {
        mini_test = 2;
        [[self window] miniaturize:nil];
        printf("minimised\n");
        fflush(stdout);
    }
    if (mini_test == 2 && now - start > 6) {
        mini_test = 3;
        [[self window] deminiaturize:nil];
        printf("restored\n");
        fflush(stdout);
    }
    if (now - start > run_seconds)
        [NSApp terminate:nil];
    if (!max_frames || drawn < max_frames)
        [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    double now = [NSDate timeIntervalSinceReferenceDate];
    float angle = (float)((now - start) * 70.0);

    if (max_frames && drawn >= max_frames) {
        /* Not from our timer: the system asked for the view to be drawn again. */
        printf("asked to draw again at %.1f s\n", now - start);
        fflush(stdout);
    }
    drawn++;
    if (not_opaque) {
        [[NSColor clearColor] set];
        NSRectFill(dirty);
    }
    [[self openGLContext] makeCurrentContext];
    glClearColor(0.1f, 0.1f, 0.12f, clear_alpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPushMatrix();
    glRotatef(20, 1, 0, 0);
    glRotatef(30, 0, 1, 0);
    glPushMatrix();
    glTranslatef(-3, -2, 0);
    glRotatef(angle, 0, 0, 1);
    glCallList(gears[0]);
    glPopMatrix();
    glPushMatrix();
    glTranslatef(3.1f, -2, 0);
    glRotatef(-2 * angle - 9, 0, 0, 1);
    glCallList(gears[1]);
    glPopMatrix();
    glPushMatrix();
    glTranslatef(-3.1f, 4.2f, 0);
    glRotatef(-2 * angle - 25, 0, 0, 1);
    glCallList(gears[2]);
    glPopMatrix();
    glPopMatrix();
    [[self openGLContext] flushBuffer];

    frames++;
    if (last_frame > 0 && now - last_frame > longest)
        longest = now - last_frame;
    last_frame = now;
    if (now - last_report >= 2.0) {
        /* The longest frame shows a stutter that the frame rate averages away. */
        printf("%d frames in %.1f s: %.1f fps, longest frame %.0f ms\n", frames, now - last_report,
               frames / (now - last_report), longest * 1000);
        fflush(stdout);
        frames = 0;
        longest = 0;
        last_report = now;
    }
}
@end

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) {
            clear_alpha = 0;
            continue;
        }
        if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &win_w, &win_h);
            continue;
        }
        if (!strcmp(argv[i], "-s")) {
            software = 1;
            continue;
        }
        if (!strcmp(argv[i], "-m")) {
            move_test = 1;
            continue;
        }
        if (!strcmp(argv[i], "-c")) {
            cover_test = 1;
            continue;
        }
        if (!strcmp(argv[i], "-z")) {
            mini_test = 1;
            continue;
        }
        if (!strcmp(argv[i], "-o")) {
            not_opaque = 1;
            clear_alpha = 0;
            continue;
        }
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "-r"))
            resize_test = 1;
        else
            run_seconds = atof(argv[i]);
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFAAccelerated, NSOpenGLPFANoRecovery,
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24, 0 };
    /* -s: Apple's software renderer (kCGLRendererGenericFloatID), as a reference. */
    NSOpenGLPixelFormatAttribute soft[] = { NSOpenGLPFARendererID, 0x00020400,
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24, 0 };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:software ? soft : attrs];
    if (!pf) {
        fprintf(stderr, "no accelerated pixel format (start with RIPPLE_GL=1?)\n");
        return 1;
    }
    NSRect frame = NSMakeRect(win_w > 800 ? 0 : 100, win_h > 500 ? 40 : 100, win_w, win_h);
    NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSTitledWindowMask | NSClosableWindowMask | NSResizableWindowMask
        backing:NSBackingStoreBuffered defer:NO];
    [win setTitle:@"gl-gears (moose-ripple)"];
    GearsView *view = [[GearsView alloc] initWithFrame:frame pixelFormat:pf];
    [win setContentView:view];
    if (not_opaque) {
        [win setOpaque:NO];
        [win setBackgroundColor:[NSColor clearColor]];
    }
    [win makeKeyAndOrderFront:nil];
    if (cover_test) {
        /* Starts left of the GL window, half way up it, and in front of it. */
        cover = [[NSWindow alloc] initWithContentRect:NSMakeRect(frame.origin.x - 250,
                                                                  frame.origin.y + frame.size.height / 4, 200, 200)
            styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
        [cover setTitle:@"cover"];
        [cover setLevel:NSFloatingWindowLevel];      /* stays in front of the key window */
        [cover orderFront:nil];
    }
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
    [view release];
    [win release];
    [pf release];
    [pool release];
    return 0;
}
