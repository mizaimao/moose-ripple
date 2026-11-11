/*
 * window-check: Real Racing 2's intro order (see gl-feature-check.c, "sequence") with the
 * window context drawing into a real window, as Core Animation does (M6). Context A (shares
 * with the window's) draws a colour into a new rectangle pbuffer each frame and ties a texture
 * to it (or copies); the window context draws that texture through a 3D table into the window
 * and flushes. Prints the window's back-buffer centre per frame; the window stays on screen
 * for a capture.
 *   window-check [copy] [SECONDS]
 */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <string.h>

#define W 640
#define H 400

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    bool copy = argc > 1 && !strcmp(argv[1], "copy");
    bool zero_scissor = getenv("WC_ZERO_SCISSOR") != NULL;   /* Core Animation's glScissor(0,0,0,0), test off */
    double seconds = argc > 2 ? atof(argv[2]) : 6;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];

    NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFAAccelerated, NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAColorSize, 32, 0 };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, W, H)
        styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
    [win setTitle:copy ? @"window-check (copy)" : @"window-check (tie)"];
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    [win setContentView:view];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];

    NSOpenGLContext *bctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
    [bctx setView:view];
    GLint on = 1, rect[4] = { 0, 0, W, H };
    CGLContextObj b = [bctx CGLContextObj];
    CGLSetParameter(b, kCGLCPSwapRectangle, rect);        /* as Core Animation does */
    CGLEnable(b, kCGLCESwapRectangle);
    CGLSetParameter(b, kCGLCPSwapInterval, &on);
    CGLContextObj a;
    CGLCreateContext([pf CGLPixelFormatObj], b, &a);
    GLint screen = 0;
    CGLGetVirtualScreen(b, &screen);

    [bctx makeCurrentContext];
    printf("window-check (%s): %s\n", copy ? "copy" : "tie", glGetString(GL_RENDERER));
    static unsigned char lut[32 * 32 * 32 * 4];
    for (int z = 0; z < 32; z++)
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                unsigned char *p = &lut[((z * 32 + y) * 32 + x) * 4];
                p[0] = x * 255 / 31; p[1] = y * 255 / 31; p[2] = z * 255 / 31; p[3] = 255;
            }
    GLuint lt;
    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &lt);
    glBindTexture(GL_TEXTURE_3D, lt);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 32, 32, 32, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, lut);
    glActiveTexture(GL_TEXTURE0);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER,
        "uniform mat4 vertex_matrix;\nattribute vec4 vertex_position;\nattribute vec2 vertex_texcoord0;\n"
        "varying vec2 texcoord0;\nvoid main () { gl_Position = vertex_matrix * vertex_position; "
        "texcoord0 = vertex_texcoord0; }\n"));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER,
        "uniform sampler2DRect texture0;\nuniform sampler3D texture1;\nvarying vec2 texcoord0;\n"
        "void main (void) { vec3 c = texture2DRect (texture0, texcoord0).rgb * vec3 (0.96875) + "
        "vec3 (0.015625);\n  gl_FragColor = vec4 (texture3D (texture1, c).bgr, 1.); }\n"));
    glBindAttribLocation(p, 0, "vertex_position");
    glBindAttribLocation(p, 2, "vertex_texcoord0");
    glLinkProgram(p);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "texture0"), 0);
    glUniform1i(glGetUniformLocation(p, "texture1"), 1);
    GLfloat m[16] = { 2.0f / W, 0, 0, 0, 0, 2.0f / H, 0, 0, 0, 0, 0, 0, -1, -1, 0, 1 };
    glUniformMatrix4fv(glGetUniformLocation(p, "vertex_matrix"), 1, GL_FALSE, m);
    glUseProgram(0);
    glViewport(0, 0, W, H);

    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    for (int frame = 0; [end timeIntervalSinceNow] > 0; frame++) {
        CGLPBufferObj pb;
        CGLCreatePBuffer(W, H, GL_TEXTURE_RECTANGLE_ARB, GL_RGB, 0, &pb);
        CGLSetCurrentContext(a);
        CGLSetPBuffer(a, pb, 0, 0, screen);
        glViewport(0, 0, W, H);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glColor4f(1.0f, 0.5f, (frame % 4) * 0.25f, 1.0f);
        glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(W, 0); glVertex2f(W, H); glVertex2f(0, H); glEnd();
        glFlush();
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
        if (copy)
            glCopyTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGB, 0, 0, W, H, 0);
        else
            CGLTexImagePBuffer(a, pb, GL_FRONT);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
        glFlush();
        CGLClearDrawable(a);

        [bctx makeCurrentContext];
        glClearColor(0, 0, 1, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (zero_scissor) {
            glDisable(GL_SCISSOR_TEST);
            glScissor(0, 0, 0, 0);
            glDisable(GL_DITHER);
        }
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, lt);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
        struct { GLdouble pos[4]; GLfloat tc0[2], tc1[2], tc2[2]; GLubyte col[4]; } v[4];
        memset(v, 0, sizeof(v));
        const double xy[4][2] = { { 0, 0 }, { W, 0 }, { W, H }, { 0, H } };
        for (int i = 0; i < 4; i++) {
            v[i].pos[0] = xy[i][0]; v[i].pos[1] = xy[i][1]; v[i].pos[3] = 1;
            v[i].tc0[0] = xy[i][0]; v[i].tc0[1] = xy[i][1];
        }
        GLushort idx[4] = { 0, 1, 2, 3 };
        glVertexAttribPointer(0, 4, GL_DOUBLE, GL_FALSE, 64, v[0].pos);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 64, v[0].tc0);
        glEnableVertexAttribArray(2);
        glDrawRangeElements(GL_QUADS, 0, 4, 4, GL_UNSIGNED_SHORT, idx);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(2);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
        unsigned char px[4] = { 0 };
        glReadBuffer(GL_BACK);
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (frame < 4)
            printf("  frame %d: window back buffer centre RGBA %3d %3d %3d %3d (want 255 128 %d), GL "
                   "error 0x%x\n", frame, px[0], px[1], px[2], px[3], (frame % 4) * 64, glGetError());
        [bctx flushBuffer];
        glDeleteTextures(1, &tex);
        CGLDestroyPBuffer(pb);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.03]];
    }
    [pool release];
    return 0;
}
