/*
 * gl-check: small rendering checks for the GL features WindowServer uses (M5), drawn into an
 * offscreen framebuffer object and read back, so a wrong result shows as numbers.
 *   RIPPLE_GL=1 gl-check     our renderer
 *   gl-check -s              Apple's software renderer (GLRendererFloat): the reference
 * Each check prints the read-back pixels at a few columns next to what Apple's renderer gives.
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLRenderers.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 160
#define H 160

#define CHECK(call) do { CGLError e_ = (call); if (e_) { \
    fprintf(stderr, "%s failed: %d (%s)\n", #call, e_, CGLErrorString(e_)); exit(1); } } while (0)

#ifndef GL_UNPACK_CLIENT_STORAGE_APPLE
#define GL_UNPACK_CLIENT_STORAGE_APPLE 0x85B2
#endif

static void clear_white(void)
{
    glDisable(GL_BLEND);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void print_row(const char *name, int y)
{
    static const int xs[] = { 0, 20, 40, 70, 100, 138 };
    unsigned char px[4];
    printf("%-28s", name);
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        glReadPixels(xs[i], y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf(" x%-3d %3u %3u %3u %3u |", xs[i], px[0], px[1], px[2], px[3]);
    }
    printf(" err 0x%x\n", glGetError());
}

/* A quad over (0,0)-(w,h) in window pixels, texture coordinates 0..tw, 0..th. */
static void quad(float w, float h, float tw, float th)
{
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);   glVertex2f(0, 0);
    glTexCoord2f(tw, 0);  glVertex2f(w, 0);
    glTexCoord2f(tw, th); glVertex2f(w, h);
    glTexCoord2f(0, th);  glVertex2f(0, h);
    glEnd();
}

/* The shadow: an alpha-only texture whose alpha rises with x (a = x * 255 / 138), drawn in
 * black with MODULATE and blended ONE, ONE_MINUS_SRC_ALPHA over white: expect 255 - a. */
static void check_alpha(GLenum target, GLenum internal, int client_storage, const char *name)
{
    const int tw = 139, th = 153, rowlen = 192;
    unsigned char *data = calloc(rowlen * th, 1);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++)
            data[y * rowlen + x] = (unsigned char)(x * 255 / (tw - 1));
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (client_storage)
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowlen);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(target, 0, internal, tw, th, 0, GL_ALPHA, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, 0);
    glGetError();

    clear_white();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4ub(0, 0, 0, 255);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(target);
    if (target == GL_TEXTURE_RECTANGLE_ARB)
        quad(tw, th, tw, th);
    else
        quad(tw, th, 1, 1);
    glDisable(target);
    glDisable(GL_BLEND);
    print_row(name, 70);
    glDeleteTextures(1, &tex);
    free(data);
}

/* The window texture: BGRA 8888_REV data into an RGB8 texture (alpha must read as 1), drawn
 * with REPLACE: expect the colour (r 200, g 100, b 50) everywhere. */
static void check_rgb8(void)
{
    const int tw = 139, th = 153;
    unsigned *data = malloc(tw * th * 4);
    for (int i = 0; i < tw * th; i++)
        data[i] = 0x00c86432;              /* A 0, R 200, G 100, B 50 */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGB8, tw, th, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, data);
    clear_white();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4ub(255, 255, 255, 255);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
    quad(tw, th, tw, th);
    glDisable(GL_TEXTURE_RECTANGLE_ARB);
    glDisable(GL_BLEND);
    print_row("rect RGB8 from BGRA, blended", 70);
    glDeleteTextures(1, &tex);
    free(data);
}

/* Sampling one format: a texture whose texels are (x, 255 - x, 128, x) in the format's own
 * terms, drawn with REPLACE and no blending, so the framebuffer holds what was sampled. Prints
 * column 70 (value 128) and the format Mesa or Apple picked. */
static void check_sample(GLenum target, GLenum internal, GLenum format, const char *name)
{
    const int tw = 139, th = 8;
    int comps = format == GL_LUMINANCE_ALPHA ? 2 : format == GL_RGBA ? 4 : 1;
    unsigned char *data = malloc(tw * th * 4);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            unsigned char v = (unsigned char)(x * 255 / (tw - 1));
            unsigned char *p = data + (y * tw + x) * comps;
            if (comps == 1)
                p[0] = v;
            else if (comps == 2)
                p[0] = 255 - v, p[1] = v;
            else
                p[0] = v, p[1] = 255 - v, p[2] = 128, p[3] = v;
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(target, 0, internal, tw, th, 0, format, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    GLint ifmt = 0, asize = 0, rsize = 0, lsize = 0;
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_ALPHA_SIZE, &asize);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_RED_SIZE, &rsize);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_LUMINANCE_SIZE, &lsize);

    glDisable(GL_BLEND);
    glClearColor(0.2f, 0.2f, 0.2f, 0.2f);
    glClear(GL_COLOR_BUFFER_BIT);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4ub(10, 20, 30, 40);
    glEnable(target);
    if (target == GL_TEXTURE_RECTANGLE_ARB)
        quad(tw, th, tw, th);
    else
        quad(tw, th, 1, 1);
    glDisable(target);
    unsigned char px[4];
    glReadPixels(70, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("%-26s internal 0x%04x r%d a%d l%d: x70 %3u %3u %3u %3u  err 0x%x\n", name, ifmt,
           rsize, asize, lsize, px[0], px[1], px[2], px[3], glGetError());
    glDeleteTextures(1, &tex);
    free(data);
}

static void check_formats(void)
{
    static const struct { GLenum internal, format; const char *name; } f[] = {
        { GL_ALPHA, GL_ALPHA, "ALPHA" },
        { GL_ALPHA8, GL_ALPHA, "ALPHA8" },
        { GL_LUMINANCE, GL_LUMINANCE, "LUMINANCE" },
        { GL_LUMINANCE16, GL_LUMINANCE, "LUMINANCE16" },
        { GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, "LUMINANCE_ALPHA" },
        { GL_INTENSITY, GL_LUMINANCE, "INTENSITY" },
        { GL_RGBA8, GL_RGBA, "RGBA8" },
    };
    char name[64];
    printf("sampled with REPLACE at x70 (texel value 128); expect ALPHA: c c c 128 (c = colour "
           "10 20 30), LUMINANCE: 128 128 128 255, L_A: 127 127 127 128, INTENSITY: all 128, "
           "RGBA8: 128 127 128 128\n");
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        snprintf(name, sizeof(name), "2D   %s", f[i].name);
        check_sample(GL_TEXTURE_2D, f[i].internal, f[i].format, name);
        snprintf(name, sizeof(name), "rect %s", f[i].name);
        check_sample(GL_TEXTURE_RECTANGLE_ARB, f[i].internal, f[i].format, name);
    }
}

int main(int argc, char **argv)
{
    int software = argc > 1 && strcmp(argv[1], "-s") == 0;
    CGLPixelFormatAttribute hw[] = { kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0 };
    CGLPixelFormatAttribute sw[] = { kCGLPFARendererID, kCGLRendererGenericFloatID,
        kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0 };
    CGLPixelFormatObj pf;
    GLint npf = 0;
    CHECK(CGLChoosePixelFormat(software ? sw : hw, &pf, &npf));
    if (!pf) {
        fprintf(stderr, "no pixel format\n");
        return 1;
    }
    CGLContextObj ctx;
    CHECK(CGLCreateContext(pf, NULL, &ctx));
    CGLDestroyPixelFormat(pf);
    CHECK(CGLSetCurrentContext(ctx));
    printf("GL_RENDERER %s\n", glGetString(GL_RENDERER));

    GLuint fbo, rb;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, rb);
    printf("fbo status 0x%x\n", glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT));
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    printf("expected for the alpha checks: 255 - x * 255 / 138 in r, g, b\n");

    check_alpha(GL_TEXTURE_RECTANGLE_ARB, GL_ALPHA, 1, "rect ALPHA (client storage)");
    check_alpha(GL_TEXTURE_RECTANGLE_ARB, GL_ALPHA, 0, "rect ALPHA");
    check_alpha(GL_TEXTURE_RECTANGLE_ARB, GL_ALPHA8, 0, "rect ALPHA8");
    check_alpha(GL_TEXTURE_2D, GL_ALPHA, 0, "2D ALPHA (npot)");
    check_rgb8();
    check_formats();

    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    return 0;
}
