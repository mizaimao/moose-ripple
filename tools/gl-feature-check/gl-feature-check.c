/*
 * gl-feature-check: Core Animation's way of showing a video frame, step by step, on our
 * renderer and on Apple's software renderer in the same run (M6, Real Racing 2's intro).
 * Context A fills a rectangle pbuffer; context B (shares with A) ties a texture to it with
 * CGLTexImagePBuffer and draws a quad into its own framebuffer object with a GLSL program,
 * positions as floats or doubles, optionally through a 3D lookup texture. Prints the centre
 * pixel of each case; the two renderers should agree.
 *   gl-feature-check            the cases one by one
 *   gl-feature-check sequence   Real Racing 2's order frame by frame (tie or copy)
 *   gl-feature-check dxt        a DXT1 (S3TC) texture drawn 1:1
 *   gl-feature-check vbo        GL_SHORT attributes from a buffer object
 *   gl-feature-check generic    RGBA uploaded as GL_COMPRESSED_RGBA (driver picks the format)
 *   gl-feature-check tex3d      a 32x32x32 3D texture with client storage, every slice checked
 *   gl-feature-check yuv        Core Video's 2vuy conversion program, whole and in steps
 *   gl-feature-check ycbcr      YCbCr 4:2:2 textures (GL_APPLE_ycbcr_422), as Photo Booth's
 *                               camera frames
 *   gl-feature-check stencil    a drawable with depth and stencil buffers, both used
 *   gl-feature-check iosurface  a texture that is an IOSurface follows the surface's contents
 *                               and one IOSurface drawn into in one context reads in another
 *   gl-feature-check iosurface-xproc   one IOSurface drawn into in this process, read in another
 *   gl-feature-check stale [N]  N contexts in turn, each drawn with on one thread and destroyed
 *                               from another (software first: ours crashed here, issue 240)
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/CGLIOSurface.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define W 64
#define H 64

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("  compile failed: %s\n", log);
    }
    return s;
}

static const char *vs_src =
    "attribute vec4 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 tc;\n"
    "uniform mat4 proj;\n"
    "void main() { tc = texcoord; gl_Position = proj * position; }\n";

static const char *fs_plain =
    "uniform sampler2DRect texture0;\n"
    "varying vec2 tc;\n"
    "void main() { gl_FragColor = vec4(texture2DRect(texture0, tc).rgb, 1.0); }\n";

static const char *fs_lut =
    "uniform sampler2DRect texture0;\n"
    "uniform sampler3D texture1;\n"
    "varying vec2 tc;\n"
    "void main() {\n"
    "  vec3 c = texture2DRect(texture0, tc).rgb * vec3(0.96875) + vec3(0.015625);\n"
    "  gl_FragColor = vec4(texture3D(texture1, c).bgr, 1.0);\n"
    "}\n";

static void run(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFADoubleBuffer, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : 0, renderer, 0 };
    if (!renderer)
        attrs[4] = kCGLPFAAccelerated, attrs[5] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf) {
        printf("%s: no pixel format\n", name);
        return;
    }
    CGLContextObj a, b;
    CGLCreateContext(pf, NULL, &a);
    CGLCreateContext(pf, a, &b);
    CGLPBufferObj pb;
    if (CGLCreatePBuffer(W, H, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pb)) {
        printf("%s: no pbuffer\n", name);
        return;
    }
    CGLSetCurrentContext(a);
    GLint screen = 0;
    CGLGetVirtualScreen(a, &screen);
    CGLSetPBuffer(a, pb, 0, 0, screen);
    printf("%s: %s\n", name, glGetString(GL_RENDERER));
    glViewport(0, 0, W, H);
    glClearColor(1, 0.5, 0, 1);                 /* orange */
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();

    CGLSetCurrentContext(b);
    GLuint fbo, rb;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, W, H);

    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    CGLError e = CGLTexImagePBuffer(b, pb, GL_FRONT);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* An identity-ish 3D lookup table, 32^3, BGRA: entry (r,g,b) holds (b,g,r). */
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

    const char *fss[2] = { fs_plain, fs_lut };
    for (int f = 0; f < 2; f++) {
        GLuint p = glCreateProgram();
        glAttachShader(p, shader(GL_VERTEX_SHADER, vs_src));
        glAttachShader(p, shader(GL_FRAGMENT_SHADER, fss[f]));
        glBindAttribLocation(p, 0, "position");
        glBindAttribLocation(p, 3, "texcoord");
        glLinkProgram(p);
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "texture0"), 0);
        if (f)
            glUniform1i(glGetUniformLocation(p, "texture1"), 1);
        GLfloat proj[16] = { 2.0f / W, 0, 0, 0, 0, 2.0f / H, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1 };
        glUniformMatrix4fv(glGetUniformLocation(p, "proj"), 1, GL_FALSE, proj);
        for (int dbl = 0; dbl < 2; dbl++) {
            GLdouble pd[16] = { 0, 0, 0, 1, W, 0, 0, 1, W, H, 0, 1, 0, H, 0, 1 };
            GLfloat pfl[16] = { 0, 0, 0, 1, W, 0, 0, 1, W, H, 0, 1, 0, H, 0, 1 };
            GLfloat tc[8] = { 0, 0, W, 0, W, H, 0, H };
            GLushort idx[4] = { 0, 1, 2, 3 };
            glClearColor(0, 0, 1, 0);           /* transparent blue: nothing drawn */
            glClear(GL_COLOR_BUFFER_BIT);
            if (dbl)
                glVertexAttribPointer(0, 4, GL_DOUBLE, GL_FALSE, 0, pd);
            else
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pfl);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, tc);
            glEnableVertexAttribArray(3);
            glDrawRangeElements(GL_QUADS, 0, 3, 4, GL_UNSIGNED_SHORT, idx);
            unsigned char px[4] = { 0 };
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("  %s, positions %s: centre RGBA %3d %3d %3d %3d, GL error 0x%x%s\n",
                   f ? "through the 3D LUT" : "plain", dbl ? "double" : "float ",
                   px[0], px[1], px[2], px[3], glGetError(),
                   e ? " (CGLTexImagePBuffer failed)" : "");
        }
    }

    /* Core Animation's own layer program and vertex layout (traced in Real Racing 2): one
     * interleaved 64-byte vertex, double4 position at 0, float2 texcoords at 32/40/48, ubyte4
     * colour at 56; attributes bound 0-4 by name; texcoords scaled by a uniform; GL_QUADS. */
    {
        static const char *ca_vs =
            "#define lowp\n#define mediump\n#define highp\n"
            "uniform highp mat4 vertex_matrix;\n"
            "uniform mediump vec2 texscale0, texscale1, texscale2;\n"
            "attribute highp vec4 vertex_position;\n"
            "attribute lowp vec4 vertex_color;\n"
            "attribute highp vec2 vertex_texcoord0, vertex_texcoord1, vertex_texcoord2;\n"
            "varying lowp vec4 color;\n"
            "varying highp vec2 texcoord0, texcoord1, texcoord2;\n"
            "void main () {\n"
            "  gl_Position = vertex_matrix * vertex_position;\n"
            "  color = vertex_color;\n"
            "  texcoord0 = vertex_texcoord0 * texscale0;\n"
            "  texcoord1 = vertex_texcoord1 * texscale1;\n"
            "  texcoord2 = vertex_texcoord2 * texscale2;\n"
            "}\n";
        static const char *ca_fs =
            "#define lowp\n#define mediump\n#define highp\n"
            "uniform lowp sampler2DRect texture0;\n"
            "uniform lowp sampler3D texture1;\n"
            "varying highp vec2 texcoord0;\n"
            "void main (void) {\n"
            "  lowp vec4 s0 = texture2DRect (texture0, texcoord0);\n"
            "  {\n"
            "    lowp vec3 c = s0.rgb, t;\n"
            "    c = c * vec3 (0.96875) + vec3 (0.015625);\n"
            "    c = texture3D (texture1, c).bgr;\n"
            "    s0 = vec4 (c, 1.);\n"
            "  }\n"
            "  gl_FragColor = s0;\n"
            "}\n";
        GLuint p = glCreateProgram();
        glAttachShader(p, shader(GL_VERTEX_SHADER, ca_vs));
        glAttachShader(p, shader(GL_FRAGMENT_SHADER, ca_fs));
        glBindAttribLocation(p, 0, "vertex_position");
        glBindAttribLocation(p, 1, "vertex_color");
        glBindAttribLocation(p, 2, "vertex_texcoord0");
        glBindAttribLocation(p, 3, "vertex_texcoord1");
        glBindAttribLocation(p, 4, "vertex_texcoord2");
        glLinkProgram(p);
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "texture0"), 0);
        glUniform1i(glGetUniformLocation(p, "texture1"), 1);
        GLfloat one[2] = { 1, 1 };
        glUniform2fv(glGetUniformLocation(p, "texscale0"), 1, one);
        GLfloat m[16] = { 2.0f / W, 0, 0, 0, 0, 2.0f / H, 0, 0, 0, 0, 0, 0, -1, -1, 0, 1 };
        glUniformMatrix4fv(glGetUniformLocation(p, "vertex_matrix"), 1, GL_FALSE, m);
        printf("  CA layout: vertex_matrix at location %d, texscale0 at %d\n",
               glGetUniformLocation(p, "vertex_matrix"), glGetUniformLocation(p, "texscale0"));
        struct { GLdouble pos[4]; GLfloat tc0[2], tc1[2], tc2[2]; GLubyte col[4]; } v[4];
        memset(v, 0, sizeof(v));
        const double xy[4][2] = { { 0, 0 }, { W, 0 }, { W, H }, { 0, H } };
        for (int i = 0; i < 4; i++) {
            v[i].pos[0] = xy[i][0]; v[i].pos[1] = xy[i][1]; v[i].pos[3] = 1;
            v[i].tc0[0] = xy[i][0]; v[i].tc0[1] = xy[i][1];
            memset(v[i].col, 255, 4);
        }
        GLushort idx[4] = { 0, 1, 2, 3 };
        glClearColor(0, 0, 1, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisableVertexAttribArray(3);
        glVertexAttribPointer(0, 4, GL_DOUBLE, GL_FALSE, 64, v[0].pos);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 64, v[0].col);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 64, v[0].tc0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 64, v[0].tc1);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, 64, v[0].tc2);
        glEnableVertexAttribArray(4);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, lt);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
        glDrawRangeElements(GL_QUADS, 0, 4, 4, GL_UNSIGNED_SHORT, idx);
        unsigned char px[4] = { 0 };
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("  CA layout and program: centre RGBA %3d %3d %3d %3d, GL error 0x%x\n",
               px[0], px[1], px[2], px[3], glGetError());

        /* The same into B's own drawable (a 2D pbuffer attached to B, double-buffered pixel
         * format: drawing goes to its back buffer), as Core Animation draws into its window. */
        CGLPBufferObj q;
        if (!CGLCreatePBuffer(W, H, GL_TEXTURE_2D, GL_RGBA, 0, &q)) {
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
            CGLSetPBuffer(b, q, 0, 0, screen);
            glViewport(0, 0, W, H);
            glClearColor(0, 0, 1, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawRangeElements(GL_QUADS, 0, 4, 4, GL_UNSIGNED_SHORT, idx);
            glReadBuffer(GL_BACK);
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("  CA program into B's own drawable (back buffer): centre RGBA %3d %3d %3d %3d, "
                   "GL error 0x%x\n", px[0], px[1], px[2], px[3], glGetError());
            CGLFlushDrawable(b);
            glReadBuffer(GL_FRONT);
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("  ... after CGLFlushDrawable, front buffer: centre RGBA %3d %3d %3d %3d\n",
                   px[0], px[1], px[2], px[3]);
            CGLClearDrawable(b);
            CGLDestroyPBuffer(q);
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);   /* back to the framebuffer object */
            glViewport(0, 0, W, H);
        }
        for (int i = 0; i < 5; i++)
            glDisableVertexAttribArray(i);
    }

    /* Real Racing 2's 2D quads: 12-byte vertices of GL_SHORT, position 3 shorts at 0, texcoord
     * 2 shorts at 8, not normalized; texcoords scaled by a matrix (1/16384); a 2D texture with
     * a left half red, right half green. Sampled at the quarter and three-quarter points. */
    {
        static const char *vs =
            "attribute vec3 a_Position;\nattribute vec2 a_TexCoord0;\n"
            "uniform mat4 u_ModelToProj;\nuniform mat4 u_TexMatrix0;\nvarying vec2 v_TexCoord0;\n"
            "void main() {\n"
            "  v_TexCoord0 = (u_TexMatrix0 * vec4(a_TexCoord0, 0.0, 1.0)).xy;\n"
            "  gl_Position = u_ModelToProj * vec4(a_Position, 1.0);\n}\n";
        static const char *fs =
            "uniform sampler2D s_Texture0;\nvarying vec2 v_TexCoord0;\n"
            "void main() { gl_FragColor = texture2D(s_Texture0, v_TexCoord0); }\n";
        GLuint p = glCreateProgram();
        glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
        glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
        glBindAttribLocation(p, 0, "a_Position");
        glBindAttribLocation(p, 3, "a_TexCoord0");
        glLinkProgram(p);
        glUseProgram(p);
        GLfloat proj[16] = { 2.0f / W, 0, 0, 0, 0, 2.0f / H, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1 };
        GLfloat texm[16] = { 1.0f / 16384, 0, 0, 0, 0, 1.0f / 16384, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        glUniformMatrix4fv(glGetUniformLocation(p, "u_ModelToProj"), 1, GL_FALSE, proj);
        glUniformMatrix4fv(glGetUniformLocation(p, "u_TexMatrix0"), 1, GL_FALSE, texm);
        glUniform1i(glGetUniformLocation(p, "s_Texture0"), 0);
        static unsigned char img[8 * 8 * 4];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                unsigned char *q = &img[(y * 8 + x) * 4];
                q[0] = x < 4 ? 255 : 0; q[1] = x < 4 ? 0 : 255; q[2] = 0; q[3] = 255;
            }
        GLuint t2;
        glGenTextures(1, &t2);
        glBindTexture(GL_TEXTURE_2D, t2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        struct { GLshort pos[3], pad, tc[2]; } v[4] = {
            { { 0, 0, 0 }, 0, { 0, 0 } }, { { W, 0, 0 }, 0, { 16384, 0 } },
            { { W, H, 0 }, 0, { 16384, 16384 } }, { { 0, H, 0 }, 0, { 0, 16384 } } };
        glClearColor(0, 0, 1, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glVertexAttribPointer(0, 3, GL_SHORT, GL_FALSE, 12, v[0].pos);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(3, 2, GL_SHORT, GL_FALSE, 12, v[0].tc);
        glEnableVertexAttribArray(3);
        glDrawArrays(GL_QUADS, 0, 4);
        unsigned char l[4] = { 0 }, r[4] = { 0 };
        glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l);
        glReadPixels(3 * W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
        printf("  GL_SHORT vertices, texture matrix: left RGBA %d %d %d %d (want red), right "
               "RGBA %d %d %d %d (want green), GL error 0x%x\n", l[0], l[1], l[2], l[3],
               r[0], r[1], r[2], r[3], glGetError());

        /* The same quad as vertices 32-35 of a longer array, drawn as a triangle strip with
         * first = 32 (Real Racing 2's 2D UI: draw_arrays(GL_TRIANGLE_STRIP, 32, 4)). */
        static struct { GLshort pos[3], pad, tc[2]; } many[40];
        memset(many, 0, sizeof(many));
        const GLshort strip[4][4] = { { 0, 0, 0, 0 }, { W, 0, 16384, 0 }, { 0, H, 0, 16384 },
                                      { W, H, 16384, 16384 } };
        for (int i = 0; i < 4; i++) {
            many[32 + i].pos[0] = strip[i][0]; many[32 + i].pos[1] = strip[i][1];
            many[32 + i].tc[0] = strip[i][2]; many[32 + i].tc[1] = strip[i][3];
        }
        glClear(GL_COLOR_BUFFER_BIT);
        glVertexAttribPointer(0, 3, GL_SHORT, GL_FALSE, 12, many[0].pos);
        glVertexAttribPointer(3, 2, GL_SHORT, GL_FALSE, 12, many[0].tc);
        glDrawArrays(GL_TRIANGLE_STRIP, 32, 4);
        glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l);
        glReadPixels(3 * W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
        printf("  ... strip from vertex 32: left RGBA %d %d %d %d (want red), right RGBA %d %d %d "
               "%d (want green), GL error 0x%x\n", l[0], l[1], l[2], l[3], r[0], r[1], r[2],
               r[3], glGetError());
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(b);
    CGLDestroyContext(a);
    CGLDestroyPBuffer(pb);
    CGLDestroyPixelFormat(pf);
}


/* Real Racing 2's exact order (M6): window context B with its own drawable and a 3D table made
 * with client storage; per frame, video context A attaches a new rectangle RGB pbuffer, draws
 * into it, flushes, ties a new texture to it while still attached (CGLTexImagePBuffer on A),
 * detaches; B binds that texture and draws Core Animation's program, read from B's back buffer.
 * Variants: tie (as the game) or copy (glCopyTexImage2D in A instead). */
static void run_sequence(const char *name, CGLPixelFormatAttribute renderer, bool copy)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFADoubleBuffer, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[5] = 0;
    CGLPixelFormatObj pf;
    GLint n, screen = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf)
        return;
    CGLContextObj a, b;
    CGLCreateContext(pf, NULL, &b);
    CGLCreateContext(pf, b, &a);
    CGLPBufferObj q;
    CGLCreatePBuffer(W, H, GL_TEXTURE_2D, GL_RGBA, 0, &q);
    CGLSetCurrentContext(b);
    CGLGetVirtualScreen(b, &screen);
    CGLSetPBuffer(b, q, 0, 0, screen);

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
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, 1);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 32);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 32, 32, 32, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, lut);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glActiveTexture(GL_TEXTURE0);

    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER,
        "uniform mat4 vertex_matrix;\nuniform vec2 texscale0;\nattribute vec4 vertex_position;\n"
        "attribute vec2 vertex_texcoord0;\nvarying vec2 texcoord0;\n"
        "void main () { gl_Position = vertex_matrix * vertex_position; texcoord0 = vertex_texcoord0 * texscale0; }\n"));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER,
        "uniform sampler2DRect texture0;\nuniform sampler3D texture1;\nvarying vec2 texcoord0;\n"
        "void main (void) { vec4 s0 = texture2DRect (texture0, texcoord0);\n"
        "  vec3 c = s0.rgb * vec3 (0.96875) + vec3 (0.015625);\n"
        "  gl_FragColor = vec4 (texture3D (texture1, c).bgr, 1.); }\n"));
    glBindAttribLocation(p, 0, "vertex_position");
    glBindAttribLocation(p, 2, "vertex_texcoord0");
    glLinkProgram(p);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "texture0"), 0);
    glUniform1i(glGetUniformLocation(p, "texture1"), 1);
    GLfloat one[2] = { 1, 1 };
    glUniform2fv(glGetUniformLocation(p, "texscale0"), 1, one);
    GLfloat m[16] = { 2.0f / W, 0, 0, 0, 0, 2.0f / H, 0, 0, 0, 0, 0, 0, -1, -1, 0, 1 };
    glUniformMatrix4fv(glGetUniformLocation(p, "vertex_matrix"), 1, GL_FALSE, m);
    glUseProgram(0);

    for (int frame = 0; frame < 3; frame++) {
        /* A: new pbuffer, draw a colour into it, tie or copy, detach. */
        CGLPBufferObj pb;
        CGLCreatePBuffer(W, H, GL_TEXTURE_RECTANGLE_ARB, GL_RGB, 0, &pb);
        CGLSetCurrentContext(a);
        CGLSetPBuffer(a, pb, 0, 0, screen);
        glViewport(0, 0, W, H);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glColor4f(1.0f, 0.5f, 0.25f * frame, 1.0f);
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

        /* B: draw it through the table into its own drawable. */
        CGLSetCurrentContext(b);
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 1, 0);
        glClear(GL_COLOR_BUFFER_BIT);
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
        printf("  %s, frame %d, %s: B's back buffer centre RGBA %3d %3d %3d %3d (want 255 128 %d), "
               "GL error 0x%x\n", name, frame, copy ? "copy" : "tie ", px[0], px[1], px[2], px[3],
               64 * frame > 255 ? 255 : 64 * frame - (frame ? 1 : 0), glGetError());
        CGLFlushDrawable(b);
        glDeleteTextures(1, &tex);
        CGLDestroyPBuffer(pb);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(a);
    CGLDestroyContext(b);
    CGLDestroyPBuffer(q);
    CGLDestroyPixelFormat(pf);
}


/* DXT1 (GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) textures, as Real Racing 2 uploads them (M6): a
 * 64x32 texture, each 4x4 block one colour: red in the left half, green in the right half,
 * blue in the bottom half's right quarter. Drawn to a 64x32 framebuffer object 1:1; four points
 * read back. */
static void run_dxt(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    enum { TW = 64, TH = 32 };
    static unsigned char blocks[(TW / 4) * (TH / 4) * 8];
    for (int by = 0; by < TH / 4; by++)
        for (int bx = 0; bx < TW / 4; bx++) {
            unsigned short col = bx < TW / 8 ? 0xF800 : 0x07E0;        /* red, green (565) */
            if (by < TH / 8 && bx >= 3 * TW / 16)
                col = 0x001F;                                          /* blue */
            unsigned char *b = &blocks[(by * (TW / 4) + bx) * 8];
            b[0] = col & 0xff; b[1] = col >> 8; b[2] = col & 0xff; b[3] = col >> 8;
            b[4] = b[5] = b[6] = b[7] = 0;                             /* all texels colour 0 */
        }
    GLuint tex, fbo, rb;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, TW, TH, 0,
                           sizeof(blocks), blocks);
    GLenum err = glGetError();
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, TW, TH);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, TW, TH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, TW, 0, TH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(TW, 0);
    glTexCoord2f(1, 1); glVertex2f(TW, TH);
    glTexCoord2f(0, 1); glVertex2f(0, TH);
    glEnd();
    const int pts[4][2] = { { 8, 24 }, { 40, 24 }, { 56, 4 }, { 40, 4 } };
    const char *want[4] = { "red", "green", "blue", "green" };
    printf("%s: DXT1 64x32 (upload error 0x%x, %s):\n", name, err,
           strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_texture_compression_s3tc")
               ? "s3tc listed" : "s3tc NOT listed");
    for (int i = 0; i < 4; i++) {
        unsigned char px[4] = { 0 };
        glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("  at %2d,%2d: RGBA %3d %3d %3d %3d (want %s)\n", pts[i][0], pts[i][1], px[0], px[1],
               px[2], px[3], want[i]);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}


/* Real Racing 2's menu meshes (M6): GL_SHORT positions (3) and texcoords (2) read from a
 * buffer object, stride 24, texcoords at 16; texture coordinates scaled by a matrix (1/16384);
 * a 2D texture with a red left half and a green right half. */
static void run_vbo(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    GLuint fbo, rb;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, W, H);
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER,
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord0;\n"
        "uniform mat4 u_ModelToProj;\nuniform mat4 u_TexMatrix0;\nvarying vec2 v_TexCoord0;\n"
        "void main() { v_TexCoord0 = (u_TexMatrix0 * vec4(a_TexCoord0, 0.0, 1.0)).xy;\n"
        "  gl_Position = u_ModelToProj * vec4(a_Position, 1.0); }\n"));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER,
        "uniform sampler2D s_Texture0;\nvarying vec2 v_TexCoord0;\n"
        "void main() { gl_FragColor = texture2D(s_Texture0, v_TexCoord0); }\n"));
    glBindAttribLocation(p, 0, "a_Position");
    glBindAttribLocation(p, 3, "a_TexCoord0");
    glLinkProgram(p);
    glUseProgram(p);
    GLfloat proj[16] = { 2.0f / W, 0, 0, 0, 0, 2.0f / H, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1 };
    GLfloat texm[16] = { 1.0f / 16384, 0, 0, 0, 0, 1.0f / 16384, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    glUniformMatrix4fv(glGetUniformLocation(p, "u_ModelToProj"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_TexMatrix0"), 1, GL_FALSE, texm);
    glUniform1i(glGetUniformLocation(p, "s_Texture0"), 0);
    static unsigned char img[8 * 8 * 4];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            unsigned char *q = &img[(y * 8 + x) * 4];
            q[0] = x < 4 ? 255 : 0; q[1] = x < 4 ? 0 : 255; q[2] = 0; q[3] = 255;
        }
    GLuint t2;
    glGenTextures(1, &t2);
    glBindTexture(GL_TEXTURE_2D, t2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    struct { GLshort pos[3], pad, nrm[3], pad2, tc[2], tc1[2]; } v[4] = {
        { { 0, 0, 0 }, 0, { 0 }, 0, { 0, 0 }, { 0 } },
        { { W, 0, 0 }, 0, { 0 }, 0, { 16384, 0 }, { 0 } },
        { { W, H, 0 }, 0, { 0 }, 0, { 16384, 16384 }, { 0 } },
        { { 0, H, 0 }, 0, { 0 }, 0, { 0, 16384 }, { 0 } } };
    GLushort idx[6] = { 0, 1, 2, 0, 2, 3 };
    GLuint vbo, ibo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glClearColor(0, 0, 1, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(0, 3, GL_SHORT, GL_FALSE, 24, (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(3, 2, GL_SHORT, GL_FALSE, 24, (void *)16);
    glEnableVertexAttribArray(3);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0);
    unsigned char l[4] = { 0 }, r[4] = { 0 };
    glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l);
    glReadPixels(3 * W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
    printf("%s: GL_SHORT from a VBO, stride 24: left RGBA %d %d %d %d (want red), right RGBA %d %d "
           "%d %d (want green), GL error 0x%x\n", name, l[0], l[1], l[2], l[3], r[0], r[1], r[2],
           r[3], glGetError());
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}


/* Three sampler2D uniforms on units 0, 1 and 2 (a red, a green and a blue texture), declared
 * in the order Cinebench R11.5's facade shader has them (the unit 2 one first, the unit 0 one
 * last). Each case prints what the lookups through the unit 1 and unit 2 samplers give:
 *   all used   every sampler contributes
 *   0 unused   the unit 0 lookup is there but its result is multiplied by a constant zero, as
 *              a light without highlights makes it in Cinebench
 *   0 absent   the unit 0 sampler is declared and never used
 * Right is "0 255 0" for unit 1 and "0 0 255" for unit 2 in every case. */
static void run_samplers(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    static const unsigned char colours[3][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 },
                                                 { 0, 0, 255, 255 } };
    GLuint tex[3], col, fbo;
    glGenTextures(3, tex);
    for (int i = 0; i < 3; i++) {
        unsigned char img[8 * 8 * 4];
        for (int p = 0; p < 64; p++)
            memcpy(img + p * 4, colours[i], 4);
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glActiveTexture(GL_TEXTURE3);
    glGenTextures(1, &col);
    glBindTexture(GL_TEXTURE_2D, col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, col, 0);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    static const char *vs_src =
        "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n";
    static const char *head =
        "uniform sampler2D bmptex6F;\n"         /* unit 2 */
        "uniform sampler2D bmptex4F;\n"         /* unit 1 */
        "uniform float which;\n"
        "uniform sampler2D matspecfn_0F;\n"     /* unit 0 */
        "void main() {\n"
        "  vec2 uvw = fract(gl_TexCoord[0].xy);\n"
        "  vec4 ocolor = vec4(0.0, 0.0, 0.0, 1.0);\n";
    static const char *body[3] = {
        "  ocolor.rgb += texture2D(matspecfn_0F, uvw).rgb * 0.001;\n",
        "  vec3 lightcolorS = vec3(0.0);\n"
        "  vec3 diffspec = vec3(0.3, 0.4, 0.5);\n"
        "  diffspec.y = diffspec.z > 0.0 ? texture2D(matspecfn_0F, diffspec.yz).r : 0.0;\n"
        "  ocolor.rgb += diffspec.yyy * max(vec3(0.0), lightcolorS.rgb);\n",
        "" };
    static const char *tail =
        "  vec4 color_1 = texture2D(bmptex6F, uvw.xy);\n"
        "  vec4 color_5 = texture2D(bmptex4F, uvw.xy);\n"
        "  vec4 color_0 = texture2D(matspecfn_0F, uvw.xy);\n"
        "  ocolor.rgb += which > 1.5 ? color_0.rgb : which > 0.5 ? color_1.rgb : color_5.rgb;\n"
        "  gl_FragColor = ocolor;\n"
        "}\n";
    static const char *label[3] = { "all used", "0 unused", "0 absent" };
    printf("%s (%s):\n", name, glGetString(GL_RENDERER));
    for (int v = 0; v < 3; v++) {
        char fs_src[2048];
        snprintf(fs_src, sizeof(fs_src), "%s%s%s", head, body[v], tail);
        const char *fsp = fs_src;
        GLhandleARB vs = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
        GLhandleARB fs = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
        GLhandleARB prog = glCreateProgramObjectARB();
        glShaderSourceARB(vs, 1, &vs_src, NULL);
        glShaderSourceARB(fs, 1, &fsp, NULL);
        glCompileShaderARB(vs);
        glCompileShaderARB(fs);
        glAttachObjectARB(prog, vs);
        glAttachObjectARB(prog, fs);
        glLinkProgramARB(prog);
        glUseProgramObjectARB(prog);
        GLint l0 = glGetUniformLocationARB(prog, "matspecfn_0F");
        GLint l1 = glGetUniformLocationARB(prog, "bmptex4F");
        GLint l2 = glGetUniformLocationARB(prog, "bmptex6F");
        /* Cinebench's order: unit 0, then 1, then 2, binding the texture just before */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        glUniform1iARB(l0, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex[1]);
        glUniform1iARB(l1, 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex[2]);
        glUniform1iARB(l2, 2);
        glActiveTexture(GL_TEXTURE0);
        unsigned char got[3][4];
        for (int w = 0; w < 3; w++) {
            glUniform1fARB(glGetUniformLocationARB(prog, "which"), (float)w);
            glClearColor(0.5f, 0.5f, 0.5f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(-1, -1);
            glTexCoord2f(1, 0); glVertex2f(1, -1);
            glTexCoord2f(1, 1); glVertex2f(1, 1);
            glTexCoord2f(0, 1); glVertex2f(-1, 1);
            glEnd();
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got[w]);
        }
        glUseProgramObjectARB(0);
        printf("  %s (locations %d %d %d): unit 0 sampler gives %d %d %d, unit 1 sampler gives %d %d %d, unit 2 sampler gives "
               "%d %d %d; GL error 0x%x\n", label[v], l0, l1, l2, got[2][0], got[2][1], got[2][2], got[0][0], got[0][1],
               got[0][2], got[1][0], got[1][1], got[1][2], glGetError());
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* The same surface drawn by two GLSL programs must land on the same depth. Cinebench R11.5
 * draws layers of one object with different programs whose vertex shaders all compute the
 * position with the same words ("oposition = (transmatrix * iposition);") next to different
 * other work. Program A does only that; program B also transforms the vertex with two more
 * matrices, as Cinebench's material shaders do. A draws a tilted quad with GL_LESS, B draws it
 * again with GL_EQUAL: every pixel must pass (and with GL_LESS none). */
static void run_samepos(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    static const char *vs_a =
        "uniform mat4 transmatrix;\n"
        "void main() {\n"
        "  vec4 oposition;\n"
        "  vec4 iposition = gl_Vertex;\n"
        "  iposition.w = 1.0;\n"
        "  oposition = (transmatrix * iposition);\n"
        "  gl_Position = oposition;\n"
        "}\n";
    static const char *fs_a = "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    static const char *vs_b =
        "uniform mat4 transmatrix;\n"
        "uniform mat4 objectmatrix;\n"
        "uniform mat3 objectmatrix_n;\n"
        "uniform mat3 texmatrixR;\n"
        "uniform vec3 texmatrixP;\n"
        "void main() {\n"
        "  vec4 worldcoord;\n"
        "  vec4 oposition;\n"
        "  vec3 ouvw;\n"
        "  vec4 iposition = gl_Vertex;\n"
        "  iposition.w = 1.0;\n"
        "  worldcoord = objectmatrix * iposition;\n"
        "  ouvw.xyz = (texmatrixR * iposition.xyz) + texmatrixP;\n"
        "  oposition = (transmatrix * iposition);\n"
        "  gl_TexCoord[0] = worldcoord;\n"
        "  gl_TexCoord[1].xyz = objectmatrix_n * gl_Normal;\n"
        "  gl_TexCoord[2].xyz = ouvw;\n"
        "  gl_Position = oposition;\n"
        "}\n";
    static const char *fs_b =
        "void main() {\n"
        "  vec3 v = gl_TexCoord[0].xyz + gl_TexCoord[1].xyz + fract(gl_TexCoord[2].xyz);\n"
        "  gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0) + vec4(v * 0.0000001, 0.0);\n"
        "}\n";
    const char *src[4] = { vs_a, fs_a, vs_b, fs_b };
    GLhandleARB prog[2];
    for (int p = 0; p < 2; p++) {
        GLhandleARB vs = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
        GLhandleARB fs = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
        prog[p] = glCreateProgramObjectARB();
        glShaderSourceARB(vs, 1, &src[p * 2], NULL);
        glShaderSourceARB(fs, 1, &src[p * 2 + 1], NULL);
        glCompileShaderARB(vs);
        glCompileShaderARB(fs);
        glAttachObjectARB(prog[p], vs);
        glAttachObjectARB(prog[p], fs);
        glLinkProgramARB(prog[p]);
    }
    /* A projection with awkward numbers, column major: a quad from a real scene lands here. */
    static const GLfloat trans[16] = { 0.000731f, 0.000113f, -0.000207f, -0.000199f,
                                       -0.000091f, 0.001057f, 0.000311f, 0.000301f,
                                       0.000277f, -0.000189f, 0.000403f, 0.000397f,
                                       -0.137f, 0.229f, 0.371f, 1.613f };
    static const GLfloat obj[16] = { 0.83f, 0.11f, -0.54f, 0, -0.07f, 0.99f, 0.09f, 0,
                                     0.55f, -0.04f, 0.83f, 0, 22297.1f, 59.53f, 4124.03f, 1 };
    static const GLfloat m3[9] = { 0.83f, 0.11f, -0.54f, -0.07f, 0.99f, 0.09f, 0.55f, -0.04f, 0.83f };
    static const GLfloat quad[4][3] = { { -2811.37f, -2603.11f, 913.77f },
                                        { 2907.53f, -2499.83f, -1207.19f },
                                        { 3011.29f, 2713.61f, 1511.03f },
                                        { -2707.91f, 2809.07f, -1103.47f } };
    GLuint col, dep, fbo;
    glGenTextures(1, &col);
    glBindTexture(GL_TEXTURE_2D, col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenTextures(1, &dep);
    glBindTexture(GL_TEXTURE_2D, dep);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, W, H, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, col, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, dep, 0);
    glViewport(0, 0, W, H);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    printf("%s (%s):\n", name, glGetString(GL_RENDERER));
    static const GLenum funcs[2] = { GL_EQUAL, GL_LESS };
    for (int t = 0; t < 2; t++) {
        unsigned char px[W * H * 4];
        int red = 0, blue = 0;
        glClearColor(0, 0, 0, 1);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int p = 0; p < 2; p++) {
            glUseProgramObjectARB(prog[p]);
            glUniformMatrix4fvARB(glGetUniformLocationARB(prog[p], "transmatrix"), 1, GL_FALSE, trans);
            if (p) {
                glUniformMatrix4fvARB(glGetUniformLocationARB(prog[p], "objectmatrix"), 1, GL_FALSE, obj);
                glUniformMatrix3fvARB(glGetUniformLocationARB(prog[p], "objectmatrix_n"), 1, GL_FALSE, m3);
                glUniformMatrix3fvARB(glGetUniformLocationARB(prog[p], "texmatrixR"), 1, GL_FALSE, m3);
                glUniform3fARB(glGetUniformLocationARB(prog[p], "texmatrixP"), 0.31f, 0.47f, 0.59f);
            }
            glDepthFunc(p ? funcs[t] : GL_LESS);
            glBegin(GL_QUADS);
            for (int v = 0; v < 4; v++) {
                glNormal3f(0.2f, 0.3f, 0.93f);
                glVertex3fv(quad[v]);
            }
            glEnd();
        }
        glUseProgramObjectARB(0);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int i = 0; i < W * H; i++) {
            red += px[i * 4] > 128;
            blue += px[i * 4 + 2] > 128;
        }
        printf("  second program with %s: its pixels %4d, the first program's left %4d; GL error "
               "0x%x\n", t ? "GL_LESS " : "GL_EQUAL", blue, red, glGetError());
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* Depth peeling as Cinebench R11.5 draws its glass: a slanted surface is drawn into a depth
 * texture, then drawn again into a second framebuffer with a program that samples the first
 * depth texture (GL_DEPTH_TEXTURE_MODE GL_ALPHA) at its own pixel and discards the fragment
 * unless it lies behind what is stored there, by more than 1.2e-7:
 *     if (texture2D(peel, xy).a >= z - 1.2e-7) discard;
 * The same surface must vanish entirely (0 pixels drawn); a surface 0.00001 farther must be
 * drawn everywhere. Also prints the stored depth as the program sees it at one pixel. */
static void run_peel(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    static const GLenum formats[] = { GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT24,
                                      GL_DEPTH_COMPONENT16 };
    static const int bits[] = { 32, 24, 16 };
    static const char *vs_src =
        "void main() { gl_Position = ftransform(); }\n";
    static const char *fs_src =
        "uniform sampler2D peel;\n"
        "uniform vec4 fragmenttransform;\n"
        "uniform float show;\n"
        "void main() {\n"
        "  vec3 fc;\n"
        "  fc.xy = (gl_FragCoord.xy - fragmenttransform.xy) * fragmenttransform.zw;\n"
        "  fc.z = gl_FragCoord.z;\n"
        "  float d = texture2D(peel, fc.xy).a;\n"
        "  if (show > 0.5) { gl_FragColor = vec4(d, fc.z, abs(d - fc.z) * 1000000.0, 1.0); return; }\n"
        "  if (d >= fc.z - 1.2e-7) discard;\n"
        "  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "}\n";
    GLhandleARB vs = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
    GLhandleARB fs = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
    GLhandleARB prog = glCreateProgramObjectARB();
    glShaderSourceARB(vs, 1, &vs_src, NULL);
    glShaderSourceARB(fs, 1, &fs_src, NULL);
    glCompileShaderARB(vs);
    glCompileShaderARB(fs);
    glAttachObjectARB(prog, vs);
    glAttachObjectARB(prog, fs);
    glLinkProgramARB(prog);
    printf("%s (%s):\n", name, glGetString(GL_RENDERER));
    for (int f = 0; f < 3; f++) {
        GLuint col[2], dep[2], fbo[2];
        glGenTextures(2, col);
        glGenTextures(2, dep);
        glGenFramebuffersEXT(2, fbo);
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, col[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, dep[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, formats[f], W, H, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[i]);
            glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
                                      col[i], 0);
            glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D,
                                      dep[i], 0);
        }
        glViewport(0, 0, W, H);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        /* the first layer: a surface slanted in both directions */
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[0]);
        glClearColor(0, 0, 0, 1);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor4f(0, 1, 0, 1);
        glBegin(GL_QUADS);
        glVertex3f(-1, -1, -0.31f);
        glVertex3f(1, -1, 0.17f);
        glVertex3f(1, 1, 0.43f);
        glVertex3f(-1, 1, -0.05f);
        glEnd();
        int drawn[3] = { 0, 0, 0 };
        unsigned char px[W * H * 4], seen[4] = { 0, 0, 0, 0 };
        for (int pass = 0; pass < 3; pass++) {      /* same surface; one farther; what it sees */
            float dz = pass == 1 ? 0.00002f : 0;
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[1]);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, dep[0]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_ALPHA);
            glUseProgramObjectARB(prog);
            glUniform1iARB(glGetUniformLocationARB(prog, "peel"), 0);
            glUniform4fARB(glGetUniformLocationARB(prog, "fragmenttransform"), -0.1f, -0.1f,
                           1.0f / W, 1.0f / H);
            glUniform1fARB(glGetUniformLocationARB(prog, "show"), pass == 2 ? 1.0f : 0.0f);
            glBegin(GL_QUADS);
            glVertex3f(-1, -1, -0.31f + dz);
            glVertex3f(1, -1, 0.17f + dz);
            glVertex3f(1, 1, 0.43f + dz);
            glVertex3f(-1, 1, -0.05f + dz);
            glEnd();
            glUseProgramObjectARB(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (pass == 2)
                memcpy(seen, px + (20 * W + 40) * 4, 4);
            else
                for (int i = 0; i < W * H; i++)
                    drawn[pass] += px[i * 4] > 128;
        }
        GLint got = 0;
        glBindTexture(GL_TEXTURE_2D, dep[0]);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_DEPTH_SIZE, &got);
        glBindTexture(GL_TEXTURE_2D, 0);
        printf("  %2d bits asked, %2d given: the same surface drawn again in %4d of %d pixels, one "
               "0.00001 farther in %4d; at pixel 40,20 stored %d, own %d, apart %d millionths; "
               "GL error 0x%x\n", bits[f], got, drawn[0], W * H, drawn[1], seen[0], seen[1],
               seen[2], glGetError());
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glDeleteFramebuffersEXT(2, fbo);
        glDeleteTextures(2, col);
        glDeleteTextures(2, dep);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* Cinebench R11.5's highlight tables: a 128x128 texture with internal format 1 (one component,
 * that is luminance) filled from GL_RED bytes, linear, clamped to the edge, and looked up in a
 * GLSL program as texture2D(t, st).r with s running below 0 and above 1. Texel (x, y) is 2 * x.
 * Prints the texture as it reads back (as luminance and as RGBA) and five pixels of a row drawn
 * with s from -0.5 to 1.5: with the clamp, 0 at the left and 254 at the right. */
static void run_redlum(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    static unsigned char data[128 * 128], lum[128 * 128], rgba[128 * 128 * 4];
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++)
            data[y * 128 + x] = 2 * x;
    GLuint t, col, fbo;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, 1, 128, 128, 0, GL_RED, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint ifmt = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    printf("%s (%s): internal format 0x%x\n  read back as luminance: texel 0 %d, 64 %d, 127 %d; "
           "as RGBA: texel 64 %d %d %d %d\n", name, glGetString(GL_RENDERER), ifmt, lum[0],
           lum[64], lum[127], rgba[256], rgba[257], rgba[258], rgba[259]);
    glGenTextures(1, &col);
    glBindTexture(GL_TEXTURE_2D, col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, col, 0);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    static const char *fs_src =
        "uniform sampler2D t;\n"
        "void main() { float v = texture2D(t, gl_TexCoord[0].xy).r; gl_FragColor = vec4(v, v, v, 1.0); }\n";
    static const char *vs_src =
        "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n";
    GLhandleARB vs = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
    GLhandleARB fs = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
    GLhandleARB prog = glCreateProgramObjectARB();
    glShaderSourceARB(vs, 1, &vs_src, NULL);
    glShaderSourceARB(fs, 1, &fs_src, NULL);
    glCompileShaderARB(vs);
    glCompileShaderARB(fs);
    glAttachObjectARB(prog, vs);
    glAttachObjectARB(prog, fs);
    glLinkProgramARB(prog);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glUseProgramObjectARB(prog);
    glUniform1iARB(glGetUniformLocationARB(prog, "t"), 0);
    glBegin(GL_QUADS);
    glTexCoord2f(-0.5f, 0.5f); glVertex2f(-1, -1);
    glTexCoord2f(1.5f, 0.5f);  glVertex2f(1, -1);
    glTexCoord2f(1.5f, 0.5f);  glVertex2f(1, 1);
    glTexCoord2f(-0.5f, 0.5f); glVertex2f(-1, 1);
    glEnd();
    glUseProgramObjectARB(0);
    unsigned char px[W * 4];
    glReadPixels(0, H / 2, W, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("  drawn, s from -0.5 to 1.5: x 0: %d %d %d, x 8: %d, x 16: %d, x 32: %d, x 48: %d, "
           "x 56: %d, x 63: %d; GL error 0x%x\n", px[0], px[1], px[2], px[32], px[64], px[128],
           px[192], px[224], px[252], glGetError());
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* A 1D texture as a lookup table (Cinebench R11.5 keeps 17 of them, 256 texels of RGBA,
 * linear, clamped to the edge): texel i is (i, 255 - i, i / 2, 255). Read back with
 * glGetTexImage, then drawn across a 64-pixel row twice, by the fixed pipeline and by a GLSL
 * program with a sampler1D; prints five pixels of each row. */
static void run_tex1d(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    unsigned char lut[256 * 4], back[256 * 4];
    for (int i = 0; i < 256; i++) {
        lut[i * 4] = i; lut[i * 4 + 1] = 255 - i; lut[i * 4 + 2] = i / 2; lut[i * 4 + 3] = 255;
    }
    GLuint t1, col, fbo;
    glGenTextures(1, &t1);
    glBindTexture(GL_TEXTURE_1D, t1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    memset(back, 0, sizeof(back));
    glGetTexImage(GL_TEXTURE_1D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
    printf("%s (%s):\n  read back: texel 0 %d %d %d %d, texel 128 %d %d %d %d, texel 255 "
           "%d %d %d %d\n", name, glGetString(GL_RENDERER), back[0], back[1], back[2], back[3],
           back[512], back[513], back[514], back[515], back[1020], back[1021], back[1022],
           back[1023]);
    glGenTextures(1, &col);
    glBindTexture(GL_TEXTURE_2D, col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, col, 0);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    static const char *fs_src =
        "uniform sampler1D lut;\n"
        "void main() { gl_FragColor = texture1D(lut, gl_TexCoord[0].x); }\n";
    static const char *vs_src =
        "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n";
    GLhandleARB vs = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
    GLhandleARB fs = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
    GLhandleARB prog = glCreateProgramObjectARB();
    glShaderSourceARB(vs, 1, &vs_src, NULL);
    glShaderSourceARB(fs, 1, &fs_src, NULL);
    glCompileShaderARB(vs);
    glCompileShaderARB(fs);
    glAttachObjectARB(prog, vs);
    glAttachObjectARB(prog, fs);
    glLinkProgramARB(prog);
    for (int pass = 0; pass < 2; pass++) {
        unsigned char px[W * 4];
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_1D, t1);
        if (pass) {
            glUseProgramObjectARB(prog);
            glUniform1iARB(glGetUniformLocationARB(prog, "lut"), 0);
        } else {
            glEnable(GL_TEXTURE_1D);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        }
        glBegin(GL_QUADS);
        glTexCoord1f(0); glVertex2f(-1, -1);
        glTexCoord1f(1); glVertex2f(1, -1);
        glTexCoord1f(1); glVertex2f(1, 1);
        glTexCoord1f(0); glVertex2f(-1, 1);
        glEnd();
        if (pass)
            glUseProgramObjectARB(0);
        else
            glDisable(GL_TEXTURE_1D);
        glReadPixels(0, H / 2, W, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("  %s: x 0: %d %d %d, x 16: %d %d %d, x 32: %d %d %d, x 48: %d %d %d, x 63: "
               "%d %d %d; GL error 0x%x\n", pass ? "sampler1D     " : "fixed pipeline",
               px[0], px[1], px[2], px[64], px[65], px[66], px[128], px[129], px[130], px[192],
               px[193], px[194], px[252], px[253], px[254], glGetError());
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* Depth textures read back (Cinebench R11.5's probe before its OpenGL test): a 64x64 depth
 * texture of 32, 24 and 16 bits on a framebuffer object, cleared to 0.75, then a quad whose
 * depth runs from 0.5 at the left to 0.5001 at the right with the depth test at GL_ALWAYS,
 * read back as floats with glGetTexImage. A format with enough bits shows the slope: many
 * different values in a row; Cinebench takes the first format that does, and with 16 bits its
 * scene's near surfaces fight. Prints the middle row's ends, extremes and how many different
 * values it has. */
static void run_depth(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    static const GLenum formats[] = { GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT24,
                                      GL_DEPTH_COMPONENT16 };
    static const int bits[] = { 32, 24, 16 };
    printf("%s (%s):\n", name, glGetString(GL_RENDERER));
    for (int f = 0; f < 3; f++) {
        GLuint col, dep, fbo;
        glGenTextures(1, &col);
        glBindTexture(GL_TEXTURE_2D, col);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, W, H, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &dep);
        glBindTexture(GL_TEXTURE_2D, dep);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, formats[f], W, H, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLint got = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_DEPTH_SIZE, &got);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffersEXT(1, &fbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, col, 0);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, dep, 0);
        GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        glViewport(0, 0, W, H);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glClearColor(0, 1, 0, 1);
        glClearDepth(0.75);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glColor4f(1, 0.5f, 0, 1);
        glBegin(GL_QUADS);
        glVertex3f(-1, -1, 0);
        glVertex3f(1, -1, 0.0002f);
        glVertex3f(1, 1, 0.0002f);
        glVertex3f(-1, 1, 0);
        glEnd();
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        static float z[W * H];
        static unsigned char rgb[W * H * 3];
        memset(z, 0, sizeof(z));
        glBindTexture(GL_TEXTURE_2D, dep);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, z);
        glBindTexture(GL_TEXTURE_2D, col);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        const float *row = z + (H / 2) * W;
        int different = 1;
        float lo = row[0], hi = row[0];
        for (int x = 1; x < W; x++) {
            if (row[x] != row[x - 1])
                different++;
            if (row[x] < lo) lo = row[x];
            if (row[x] > hi) hi = row[x];
        }
        printf("  %2d bits asked, %2d given, framebuffer 0x%x: left %.7f right %.7f, lowest %.7f "
               "highest %.7f, %2d different values in the row; colour %d %d %d; GL error 0x%x\n",
               bits[f], got, status, row[0], row[W - 1], lo, hi, different,
               rgb[(H / 2 * W + W / 2) * 3], rgb[(H / 2 * W + W / 2) * 3 + 1],
               rgb[(H / 2 * W + W / 2) * 3 + 2], glGetError());

        /* The same row by the other ways of reading depth: glReadPixels as floats, and
         * glGetTexImage as 32-bit integers (one value in hex shows which bits are filled). */
        {
            static float rp[W];
            static GLuint ui[W * H];
            int rp_different = 1, ui_different = 1;
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
            glReadPixels(0, H / 2, W, 1, GL_DEPTH_COMPONENT, GL_FLOAT, rp);
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
            glBindTexture(GL_TEXTURE_2D, dep);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, ui);
            for (int x = 1; x < W; x++) {
                if (rp[x] != rp[x - 1])
                    rp_different++;
                if (ui[H / 2 * W + x] != ui[H / 2 * W + x - 1])
                    ui_different++;
            }
            printf("          glReadPixels floats: %2d different; glGetTexImage integers: %2d "
                   "different, pixel 10 is 0x%08x\n", rp_different, ui_different,
                   ui[H / 2 * W + 10]);
        }

        /* What the buffer itself keeps, without reading depth back: a red surface, then a blue
         * one 0.000003 nearer with GL_LESS. 24 bits tell them apart (blue shows), 16 do not. */
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDepthFunc(GL_LESS);
        for (int pass = 0; pass < 2; pass++) {
            float zz = pass ? 0.00002f - 0.000006f : 0.00002f;
            glColor4f(pass ? 0 : 1, 0, pass ? 1 : 0, 1);
            glBegin(GL_QUADS);
            glVertex3f(-1, -1, zz);
            glVertex3f(1, -1, zz);
            glVertex3f(1, 1, zz);
            glVertex3f(-1, 1, zz);
            glEnd();
        }
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glBindTexture(GL_TEXTURE_2D, col);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        printf("          a surface 0.000003 nearer: %s (colour %d %d %d)\n",
               rgb[(H / 2 * W + W / 2) * 3 + 2] > 128 ? "shows" : "hidden",
               rgb[(H / 2 * W + W / 2) * 3], rgb[(H / 2 * W + W / 2) * 3 + 1],
               rgb[(H / 2 * W + W / 2) * 3 + 2]);
        glDeleteFramebuffersEXT(1, &fbo);
        glDeleteTextures(1, &col);
        glDeleteTextures(1, &dep);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* Generic compression (M6): Real Racing 2 uploads RGBA bytes with internal format
 * GL_COMPRESSED_RGBA, 2048x2048 with mipmaps, and leaves the format to the driver. A 2048x2048
 * picture, red left half, green right half, blue bottom-right quarter; drawn scaled to 64x64. */
static void run_generic(const char *name, CGLPixelFormatAttribute renderer, int size)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    unsigned char *img = malloc((size_t)size * size * 4);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            unsigned char *q = &img[((size_t)y * size + x) * 4];
            bool blue = y < size / 2 && x >= 3 * size / 4;
            q[0] = !blue && x < size / 2 ? 255 : 0;
            q[1] = !blue && x >= size / 2 ? 255 : 0;
            q[2] = blue ? 255 : 0;
            q[3] = 255;
        }
    GLuint tex, fbo, rb;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int level = 0, s = size; s >= 1; level++, s /= 2) {
        /* each level from the same buffer, like the game: a box-filtered copy of the picture */
        if (level) {
            unsigned char *src = img;
            for (int y = 0; y < s; y++)
                for (int x = 0; x < s; x++)
                    for (int k = 0; k < 4; k++) {
                        int a = src[((size_t)(2 * y) * (2 * s) + 2 * x) * 4 + k];
                        int b = src[((size_t)(2 * y) * (2 * s) + 2 * x + 1) * 4 + k];
                        int cc = src[((size_t)(2 * y + 1) * (2 * s) + 2 * x) * 4 + k];
                        int d = src[((size_t)(2 * y + 1) * (2 * s) + 2 * x + 1) * 4 + k];
                        img[((size_t)y * s + x) * 4 + k] = (a + b + cc + d) / 4;
                    }
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_COMPRESSED_RGBA, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    }
    free(img);
    GLint compressed = 0, ifmt = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(W, 0);
    glTexCoord2f(1, 1); glVertex2f(W, H);
    glTexCoord2f(0, 1); glVertex2f(0, H);
    glEnd();
    const int pts[4][2] = { { W / 8, 3 * H / 4 }, { 5 * W / 8, 3 * H / 4 }, { 7 * W / 8, H / 8 }, { 5 * W / 8, H / 8 } };
    const char *want[4] = { "red", "green", "blue", "green" };
    printf("%s: GL_COMPRESSED_RGBA %dx%d with mipmaps: compressed %d, internal format 0x%x, error 0x%x\n",
           name, size, size, compressed, ifmt, glGetError());
    for (int i = 0; i < 4; i++) {
        unsigned char px[4] = { 0 };
        glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("  at %2d,%2d: RGBA %3d %3d %3d %3d (want %s)\n", pts[i][0], pts[i][1], px[0], px[1],
               px[2], px[3], want[i]);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* A 32x32x32 BGRA 3D texture made with client storage, as Core Animation's colour table for
 * video (M6, Real Racing 2's intro). The program fills the memory again after glTexImage3D,
 * with no GL call, and expects the new contents at the next draw. The texel at x, y, z holds
 * (8x, 8y, 8z) at first and (255-8x, 255-8y, 255-8z) after the refill. A quad samples one
 * slice at a time (nearest) into a 32x32 framebuffer object and texel 5,9 is read back. */
#ifndef GL_UNPACK_CLIENT_STORAGE_APPLE
#define GL_UNPACK_CLIENT_STORAGE_APPLE 0x85B2
#endif
static void tex3d_fill(uint32_t *t, int n, int second)
{
    for (int z = 0; z < n; z++)
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                unsigned r = x * 8, g = y * 8, b = z * 8;
                if (second) { r = 255 - r; g = 255 - g; b = 255 - b; }
                t[(z * n + y) * n + x] = 0xFF000000u | r << 16 | g << 8 | b;   /* BGRA, 8888 REV */
            }
}

static void tex3d_read(const char *what, int n, int second)
{
    static const int slices[] = { 0, 1, 15, 16, 31 };
    printf("  %s:", what);
    for (unsigned i = 0; i < sizeof(slices) / sizeof(slices[0]); i++) {
        int z = slices[i];
        float r = (z + 0.5f) / n;
        unsigned char px[4] = { 0 };
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord3f(0, 0, r); glVertex2f(0, 0);
        glTexCoord3f(1, 0, r); glVertex2f(n, 0);
        glTexCoord3f(1, 1, r); glVertex2f(n, n);
        glTexCoord3f(0, 1, r); glVertex2f(0, n);
        glEnd();
        glReadPixels(5, 9, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int wr = second ? 255 - 40 : 40, wg = second ? 255 - 72 : 72, wb = second ? 255 - z * 8 : z * 8;
        printf(" slice %d: %d %d %d %d%s", z, px[0], px[1], px[2], px[3],
               px[0] == wr && px[1] == wg && px[2] == wb && px[3] == 255 ? "" : " WRONG");
    }
    printf("\n");
}

static void run_tex3d(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint npf;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    enum { N = 32 };
    static uint32_t table[N * N * N];
    GLuint tex, fbo, rb;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, N, N);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, N, N);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, N, 0, N, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    printf("%s: 3D texture 32x32x32, texel 5,9 of five slices (want 40 72 8z 255, then 215 183 255-8z 255)\n", name);
    for (int client = 0; client < 2; client++) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_3D, tex);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, client);
        tex3d_fill(table, N, 0);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, N, N, N, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, table);
        GLenum err = glGetError();
        glEnable(GL_TEXTURE_3D);
        tex3d_read(client ? "client storage, as made (error ignored)" : "plain upload", N, 0);
        if (client) {
            /* The memory changes behind GL's back; a new frame, the texture bound again. */
            tex3d_fill(table, N, 1);
            glFlush();
            glBindTexture(GL_TEXTURE_3D, 0);
            glBindTexture(GL_TEXTURE_3D, tex);
            tex3d_read("client storage, memory refilled", N, 1);
        } else {
            tex3d_fill(table, N, 1);
            glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, N, N, N, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, table);
            tex3d_read("glTexSubImage3D, whole", N, 1);
        }
        (void)err;
        glDisable(GL_TEXTURE_3D);
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, 0);
        glDeleteTextures(1, &tex);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* Core Video's ARB fragment program that turns a '2vuy' video frame (a BGRA rectangle texture
 * holding U Y0 V Y1 per texel) into RGB, as Real Racing 2's intro runs it every frame (M6):
 * the real program, and the same program cut down step by step, to find a step our renderer
 * gets wrong. Every texel is U 126, Y 219/220, V 129, which the real matrix (BT.709, as the game
 * sets it) turns into about 238 236 232. Fixed-function vertex stage, texture coordinates on
 * units 0-4 from arrays and 5-7 as current values, as Core Video does. */
static const char *const yuv_programs[][2] = {
    { "constant",
      "!!ARBfp1.0\nMOV result.color, {0.25, 0.5, 0.75, 1.0};\nEND\n" },
    { "texel, alpha 1",
      "!!ARBfp1.0\nTEMP s;\nTEX s, fragment.texcoord[0], texture[0], RECT;\n"
      "MOV result.color, s;\nMOV result.color.a, 1.0;\nEND\n" },
    { "matrix only",
      "!!ARBfp1.0\nTEMP s;\nTEX s, fragment.texcoord[0], texture[0], RECT;\n"
      "MOV s, s.yzxy;\n"
      "DPH result.color.r, s, program.local[0];\nDPH result.color.g, s, program.local[1];\n"
      "DPH result.color.b, s, program.local[2];\nMOV result.color.a, 1.0;\nEND\n" },
    { "side select",
      "!!ARBfp1.0\nTEMP s, side;\nMUL side, fragment.texcoord[0], {1.0, 0.5, 0.0, 0.0};\n"
      "FRC side, side;\nSLT side.zw, side.xyxy, 0.5;\nSGE side.xy, side.xyxy, 0.5;\n"
      "TEX s, fragment.texcoord[0], texture[0], RECT;\n"
      "LRP s, side.xxxx, s.wwww, s.yyyy;\nMOV result.color, s;\nMOV result.color.a, 1.0;\nEND\n" },
    { "the real program",
      "!!ARBfp1.0\n"
      "TEMP sampleY0, sample00, sample10, sample01, sample11, midSampleT, midSampleB, side, weightLR, weightTB;\n"
      "ALIAS weights = fragment.texcoord[5];\n"
      "MUL side, fragment.texcoord[0], {1.0, 0.5, 0.0, 0.0};\n"
      "FRC side, side;\n"
      "SLT side.zw, side.xyxy, 0.5;\n"
      "SGE side.xy, side.xyxy, 0.5;\n"
      "MUL weightLR, side.z, weights.x;\n"
      "MUL weightTB, side.w, weights.z;\n"
      "MAD weightLR, side.x, weights.y, weightLR;\n"
      "MAD weightTB, side.y, weights.w, weightTB;\n"
      "TEX sample00, fragment.texcoord[1], texture[0], RECT;\n"
      "TEX sample10, fragment.texcoord[2], texture[0], RECT;\n"
      "TEX sampleY0, fragment.texcoord[0], texture[0], RECT;\n"
      "TEX sample01, fragment.texcoord[3], texture[0], RECT;\n"
      "TEX sample11, fragment.texcoord[4], texture[0], RECT;\n"
      "LRP sampleY0, side.xxxx, sampleY0.wwww, sampleY0.yyyy;\n"
      "LRP midSampleT, weightLR, sample00.zzxx, sample10.zzxx;\n"
      "LRP midSampleB, weightLR, sample01.zzxx, sample11.zzxx;\n"
      "LRP sampleY0.yz, weightTB, midSampleT, midSampleB;\n"
      "DPH result.color.r, sampleY0, program.local[0];\n"
      "DPH result.color.g, sampleY0, program.local[1];\n"
      "DPH result.color.b, sampleY0, program.local[2];\n"
      "MOV result.color.a, 1.0;\n"
      "END\n" },
};

static void run_yuv(const char *name, CGLPixelFormatAttribute renderer, bool pbuffer)
{
    /* Into a framebuffer object, or as Core Video does into a double-buffered context's
     * rectangle RGB pbuffer. */
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFADoubleBuffer, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[5] = 0;
    CGLPixelFormatObj pf;
    GLint npf, screen = 0;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    enum { TW = 4, TH = 4, FW = 8, FH = 4 };
    CGLPBufferObj pb = NULL;
    if (pbuffer) {
        if (CGLCreatePBuffer(FW, FH, GL_TEXTURE_RECTANGLE_ARB, GL_RGB, 0, &pb)) {
            printf("%s: no pbuffer\n", name);
            return;
        }
        CGLGetVirtualScreen(c, &screen);
        CGLSetPBuffer(c, pb, 0, 0, screen);
    }
    static uint32_t frame[TW * TH];
    for (int i = 0; i < TW * TH; i++)
        frame[i] = 220u << 24 | 129u << 16 | 219u << 8 | 126u;     /* A=Y1 R=V G=Y0 B=U */
    GLuint tex, fbo, rb;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA8, TW, TH, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, frame);
    if (!pbuffer) {
        glGenFramebuffersEXT(1, &fbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glGenRenderbuffersEXT(1, &rb);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, FW, FH);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                     GL_RENDERBUFFER_EXT, rb);
    }
    glViewport(0, 0, FW, FH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, FW, 0, FH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    static const GLfloat pos[8] = { 0, 0, FW, 0, FW, FH, 0, FH };
    static const GLfloat tc[8] = { 0, 0, TW, 0, TW, TH, 0, TH };
    static const GLfloat w5[4] = { 0.5f, 0.5f, 0.5f, 0.5f }, w6[4] = { 0, 0, 0, 0 };
    static const GLdouble locals[3][4] = { { 1.16438, 0, 1.79274, -0.972945 },
                                           { 1.16438, -0.213249, -0.532909, 0.301483 },
                                           { 1.16438, 2.1124, 0, -1.1334 } };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glMultiTexCoord4fv(GL_TEXTURE5, w5);
    glMultiTexCoord4fv(GL_TEXTURE6, w6);
    glMultiTexCoord2f(GL_TEXTURE7, TW - 0.5f, TH);
    for (int u = 0; u < 5; u++) {
        glClientActiveTexture(GL_TEXTURE0 + u);
        glTexCoordPointer(2, GL_FLOAT, 0, tc);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    printf("%s, into %s: Core Video's 2vuy program (want about 238 236 232 255 from the last three)\n",
           name, pbuffer ? "a rectangle pbuffer" : "a framebuffer object");
    unsigned n = sizeof(yuv_programs) / sizeof(yuv_programs[0]);
    GLuint progs[8];
    glGenProgramsARB(n, progs);
    for (unsigned i = 0; i < n; i++) {
        unsigned char px[2][4] = { { 0 } };
        GLint errpos = -1;
        glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, progs[i]);
        glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB,
                           (GLsizei)strlen(yuv_programs[i][1]), yuv_programs[i][1]);
        GLenum err = glGetError();
        glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &errpos);
        glEnable(GL_FRAGMENT_PROGRAM_ARB);
        for (int k = 0; k < 3; k++)
            glProgramLocalParameter4dvARB(GL_FRAGMENT_PROGRAM_ARB, k, locals[k]);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
        glReadPixels(2, 1, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("  %-18s pixels 2 and 3: %3d %3d %3d %3d / %3d %3d %3d %3d  (string error 0x%x at %d, draw error 0x%x)\n",
               yuv_programs[i][0], px[0][0], px[0][1], px[0][2], px[0][3], px[1][0], px[1][1],
               px[1][2], px[1][3], err, errpos, glGetError());
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    if (pb)
        CGLDestroyPBuffer(pb);
    CGLDestroyPixelFormat(pf);
}

/* Photo Booth's camera frames: 4:2:2 YCbCr uploaded with GL_YCBCR_422_APPLE into a GL_RGB
 * rectangle texture, rows given in bytes (issue 11), drawn 1:1. Both byte orders, an update
 * through glTexSubImage2D, and a 2D texture. Prints every texel drawn; the two renderers should
 * agree to a step or two. (Odd widths are left out: Apple's software renderer gives other values
 * than ours from the fifth texel of a 7-wide row on.) */
#ifndef GL_YCBCR_422_APPLE
#define GL_YCBCR_422_APPLE 0x85B9
#define GL_UNSIGNED_SHORT_8_8_APPLE 0x85BA
#define GL_UNSIGNED_SHORT_8_8_REV_APPLE 0x85BB
#endif
#ifndef GL_UNPACK_ROW_BYTES_APPLE
#define GL_UNPACK_ROW_BYTES_APPLE 0x8A16
#endif
static void run_ycbcr(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[4] = 0;
    CGLPixelFormatObj pf;
    GLint npf;
    CGLContextObj c;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &c))
        return;
    CGLSetCurrentContext(c);
    enum { TW = 8, TH = 2, ROW = TW * 2 + 6 };
    /* Pairs as Cb Y0 Cr Y1: black and white, then red, green, blue, grey and two skin-ish
     * tones from a camera, and a pair at the ends of the range. */
    static const uint8_t pairs[TH][TW / 2][4] = {
        { { 128, 16, 128, 235 }, { 90, 81, 240, 82 }, { 54, 145, 34, 146 }, { 240, 41, 110, 42 } },
        { { 128, 126, 128, 127 }, { 110, 150, 160, 152 }, { 120, 90, 145, 95 }, { 0, 255, 255, 0 } },
    };
    static uint8_t vuy[TH * ROW], yuvs[TH * ROW];
    for (int y = 0; y < TH; y++)
        for (int p = 0; p < TW / 2; p++) {
            const uint8_t *q = pairs[y][p];
            uint8_t *a = vuy + y * ROW + p * 4, *b = yuvs + y * ROW + p * 4;
            a[0] = q[0]; a[1] = q[1]; a[2] = q[2]; a[3] = q[3];
            b[0] = q[1]; b[1] = q[0]; b[2] = q[3]; b[3] = q[2];
        }
    GLuint fbo, rb, tex;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, TW, TH);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, TW, TH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, TW, 0, TH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    static const struct { const char *what; GLenum target, type; bool sub; int w; } cases[] = {
        { "rectangle, Cb Y0 Cr Y1 (2vuy)", GL_TEXTURE_RECTANGLE_ARB, GL_UNSIGNED_SHORT_8_8_APPLE, false, TW },
        { "rectangle, Y0 Cb Y1 Cr (yuvs)", GL_TEXTURE_RECTANGLE_ARB, GL_UNSIGNED_SHORT_8_8_REV_APPLE, false, TW },
        { "rectangle, 2vuy by glTexSubImage2D", GL_TEXTURE_RECTANGLE_ARB, GL_UNSIGNED_SHORT_8_8_APPLE, true, TW },
        { "2D, 2vuy", GL_TEXTURE_2D, GL_UNSIGNED_SHORT_8_8_APPLE, false, TW },
    };
    printf("%s: YCbCr 4:2:2 textures (GL_APPLE_ycbcr_422), every texel R G B\n", name);
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        GLenum t = cases[k].target;
        int w = cases[k].w;
        const uint8_t *src = cases[k].type == GL_UNSIGNED_SHORT_8_8_APPLE ? vuy : yuvs;
        glGenTextures(1, &tex);
        glBindTexture(t, tex);
        glTexParameteri(t, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(t, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ROW_BYTES_APPLE, ROW);
        if (cases[k].sub) {
            glTexImage2D(t, 0, GL_RGB, w, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexSubImage2D(t, 0, 0, 0, w, TH, GL_YCBCR_422_APPLE, cases[k].type, src);
        } else {
            glTexImage2D(t, 0, GL_RGB, w, TH, 0, GL_YCBCR_422_APPLE, cases[k].type, src);
        }
        GLenum err = glGetError();
        glPixelStorei(GL_UNPACK_ROW_BYTES_APPLE, 0);
        glClearColor(1, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(t);
        float sw = t == GL_TEXTURE_2D ? 1 : w, sh = t == GL_TEXTURE_2D ? 1 : TH;
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(sw, 0); glVertex2f(w, 0);
        glTexCoord2f(sw, sh); glVertex2f(w, TH);
        glTexCoord2f(0, sh); glVertex2f(0, TH);
        glEnd();
        glDisable(t);
        unsigned char px[TH][TW][4];
        memset(px, 0, sizeof(px));
        glReadPixels(0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("  %-36s upload error 0x%x\n", cases[k].what, err);
        for (int y = 0; y < TH; y++) {
            printf("    row %d:", y);
            for (int x = 0; x < w; x++)
                printf(" %3d %3d %3d |", px[y][x][0], px[y][x][1], px[y][x][2]);
            printf("\n");
        }
        glDeleteTextures(1, &tex);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPixelFormat(pf);
}

/* A drawable with a stencil buffer (24-bit depth, 8-bit stencil), as Real Racing 2 and Chess
 * ask for. Our frontend once gave such drawables a depth buffer format the host could not
 * make, and everything the context drew after its first use was lost (2026-11-12). The left
 * half is marked in the stencil buffer, a green quad is drawn where the mark is, and a blue
 * quad behind a nearer one must lose the depth test: left green, right red (the clear
 * colour), and the nearer quad's yellow in the bottom quarter. */
static void run_stencil(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFADepthSize, 24,
                                        kCGLPFAStencilSize, 8, kCGLPFADoubleBuffer, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[9] = 0;
    CGLPixelFormatObj pf;
    GLint npf, screen = 0, sbits = -1, dbits = -1;
    CGLContextObj c;
    CGLPBufferObj pb;
    enum { SW = 64, SH = 64 };
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &c)) {
        printf("%s: no pixel format with stencil\n", name);
        return;
    }
    if (CGLCreatePBuffer(SW, SH, GL_TEXTURE_2D, GL_RGBA, 0, &pb))
        return;
    CGLSetCurrentContext(c);
    CGLGetVirtualScreen(c, &screen);
    CGLSetPBuffer(c, pb, 0, 0, screen);
    glGetIntegerv(GL_STENCIL_BITS, &sbits);
    glGetIntegerv(GL_DEPTH_BITS, &dbits);
    glViewport(0, 0, SW, SH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, SW, 0, SH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(1, 0, 0, 1);
    glClearStencil(0);
    glClearDepth(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    /* Mark the left half: stencil 1, no colour. */
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
    glColorMask(0, 0, 0, 0);
    glRectf(0, 0, SW / 2, SH);
    glColorMask(1, 1, 1, 1);
    /* Green where the mark is. */
    glStencilFunc(GL_EQUAL, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(0, 1, 0);
    glRectf(0, 0, SW, SH);
    glDisable(GL_STENCIL_TEST);
    /* Depth: a near yellow quad over the bottom quarter, then a far blue one that must lose. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glColor3f(1, 1, 0);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0.5f); glVertex3f(SW, 0, 0.5f); glVertex3f(SW, SH / 4, 0.5f); glVertex3f(0, SH / 4, 0.5f);
    glEnd();
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, -0.5f); glVertex3f(SW, 0, -0.5f); glVertex3f(SW, SH / 4, -0.5f); glVertex3f(0, SH / 4, -0.5f);
    glEnd();
    glDisable(GL_DEPTH_TEST);
    const int pts[3][2] = { { SW / 4, SH / 2 }, { 3 * SW / 4, SH / 2 }, { SW / 2, SH / 8 } };
    const char *want[3] = { "green", "red", "yellow" };
    printf("%s: drawable with %d depth and %d stencil bits (error 0x%x):\n", name, dbits, sbits,
           glGetError());
    for (int i = 0; i < 3; i++) {
        unsigned char px[4] = { 0 };
        glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("  at %2d,%2d: RGBA %3d %3d %3d %3d (want %s)\n", pts[i][0], pts[i][1], px[0], px[1],
               px[2], px[3], want[i]);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPBuffer(pb);
    CGLDestroyPixelFormat(pf);
}

/* A texture made from an IOSurface is the surface: what is written to the surface afterwards
 * must show the next time the texture is bound and drawn (Core Video's pool of video frames
 * works that way; with a single copy a movie repeated its first frames). Two textures on two
 * surfaces, as a pool would have, each filled, shown, filled again and shown again. */
static void iosurface_fill(IOSurfaceRef s, uint32_t bgra)
{
    IOSurfaceLock(s, 0, NULL);
    uint32_t *p = IOSurfaceGetBaseAddress(s);
    size_t rb = IOSurfaceGetBytesPerRow(s), h = IOSurfaceGetHeight(s), w = IOSurfaceGetWidth(s);
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++)
            ((uint32_t *)((uint8_t *)p + y * rb))[x] = bgra;
    IOSurfaceUnlock(s, 0, NULL);
}

static void run_iosurface(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[4] = 0;
    CGLPixelFormatObj pf;
    GLint npf, screen = 0;
    CGLContextObj c;
    CGLPBufferObj pb;
    enum { SW = 64, SH = 64 };
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &c)) {
        printf("%s: no pixel format\n", name);
        return;
    }
    if (CGLCreatePBuffer(SW, SH, GL_TEXTURE_2D, GL_RGBA, 0, &pb))
        return;
    CGLSetCurrentContext(c);
    CGLGetVirtualScreen(c, &screen);
    CGLSetPBuffer(c, pb, 0, 0, screen);
    glViewport(0, 0, SW, SH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, SW, 0, SH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    int w = SW, h = SH, bpe = 4;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h),
                nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe);
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement };
    const void *vals[] = { nw, nh, nb };
    CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef surf[2] = { IOSurfaceCreate(props), IOSurfaceCreate(props) };
    GLuint tex[2];
    glGenTextures(2, tex);
    /* BGRA words: red, green, then blue, yellow for the second round. */
    const uint32_t first[2] = { 0xffff0000, 0xff00ff00 }, second[2] = { 0xff0000ff, 0xffffff00 };
    const char *want[2][2] = { { "red", "green" }, { "blue", "yellow" } };
    CGLError err[2] = { 0, 0 };
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
    for (int i = 0; i < 2 && surf[0] && surf[1]; i++) {
        iosurface_fill(surf[i], first[i]);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[i]);
        err[i] = CGLTexImageIOSurface2D(c, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, SW, SH, GL_BGRA,
                                        GL_UNSIGNED_INT_8_8_8_8_REV, surf[i], 0);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    printf("%s: two textures on two IOSurfaces (CGLTexImageIOSurface2D %d %d):\n", name, err[0], err[1]);
    for (int round = 0; round < 2; round++) {
        if (round)
            for (int i = 0; i < 2; i++)
                iosurface_fill(surf[i], second[i]);     /* only the surfaces change */
        for (int i = 0; i < 2; i++) {
            glClearColor(0.5f, 0.5f, 0.5f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[i]);
            glColor3f(1, 1, 1);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, 0);
            glTexCoord2f(SW, 0); glVertex2f(SW, 0);
            glTexCoord2f(SW, SH); glVertex2f(SW, SH);
            glTexCoord2f(0, SH); glVertex2f(0, SH);
            glEnd();
            unsigned char px[4] = { 0 };
            glReadPixels(SW / 2, SH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("  texture %d, %s: RGBA %3d %3d %3d %3d (want %s)\n", i + 1,
                   round ? "after the surface changed" : "as made", px[0], px[1], px[2], px[3],
                   want[round][i]);
        }
    }
    printf("  GL error 0x%x\n", glGetError());
    glDeleteTextures(2, tex);
    for (int i = 0; i < 2; i++)
        if (surf[i])
            CFRelease(surf[i]);
    CFRelease(props); CFRelease(nw); CFRelease(nh); CFRelease(nb);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    CGLDestroyPBuffer(pb);
    CGLDestroyPixelFormat(pf);
}

/* One IOSurface in two contexts that share nothing, as Core Animation uses a layer's surface
 * (issue 11): context A draws into it through a framebuffer object (a red quad over half of a
 * blue clear), context B draws it; then the surface's memory is written (green) and B binds it
 * again. B should see what A drew, then the memory. */
static void ios_draw_quad(float x0, float y0, float x1, float y1, float s1, float t1)
{
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(s1, 0); glVertex2f(x1, y0);
    glTexCoord2f(s1, t1); glVertex2f(x1, y1);
    glTexCoord2f(0, t1); glVertex2f(x0, y1);
    glEnd();
}

static void run_iosurface_share(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[4] = 0;
    CGLPixelFormatObj pf;
    GLint npf;
    CGLContextObj a, b;
    enum { SW = 64, SH = 64 };
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &a) ||
        CGLCreateContext(pf, NULL, &b)) {
        printf("%s: no contexts\n", name);
        return;
    }
    int w = SW, h = SH, bpe = 4;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h),
                nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe);
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement };
    const void *vals[] = { nw, nh, nb };
    CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef surf = IOSurfaceCreate(props);
    iosurface_fill(surf, 0xff808080);                   /* grey in memory to start with */

    /* A: the surface as the colour buffer of a framebuffer object. */
    CGLSetCurrentContext(a);
    GLuint ta, fbo;
    glGenTextures(1, &ta);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, ta);
    CGLError ea = CGLTexImageIOSurface2D(a, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA8, SW, SH, GL_BGRA,
                                         GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_RECTANGLE_ARB, ta, 0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    glViewport(0, 0, SW, SH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, SW, 0, SH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 0, 0);
    ios_draw_quad(0, 0, SW / 2, SH, 1, 1);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glFlush();

    /* B: the surface as a texture, drawn into B's own framebuffer object. */
    CGLSetCurrentContext(b);
    GLuint tb, fbb, rb;
    glGenFramebuffersEXT(1, &fbb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbb);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, SW, SH);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, SW, SH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, SW, 0, SH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glGenTextures(1, &tb);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tb);
    CGLError eb = CGLTexImageIOSurface2D(b, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, SW, SH, GL_BGRA,
                                         GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    printf("%s: one IOSurface in two contexts (framebuffer status 0x%x, CGLTexImageIOSurface2D %d %d):\n",
           name, status, ea, eb);
    for (int round = 0; round < 2; round++) {
        if (round) {
            iosurface_fill(surf, 0xff00ff00);           /* green in memory */
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tb);
        }
        glClearColor(0.5f, 0.5f, 0.5f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_RECTANGLE_ARB);
        ios_draw_quad(0, 0, SW, SH, SW, SH);
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
        unsigned char px[2][4] = { { 0 } };
        glReadPixels(SW / 4, SH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[0]);
        glReadPixels(3 * SW / 4, SH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[1]);
        printf("  in B, %s: left %3d %3d %3d %3d, right %3d %3d %3d %3d (want %s)\n",
               round ? "after the memory changed" : "after A drew", px[0][0], px[0][1], px[0][2],
               px[0][3], px[1][0], px[1][1], px[1][2], px[1][3],
               round ? "green, green" : "red, blue");
    }
    printf("  GL error 0x%x\n", glGetError());
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(b);
    CGLDestroyContext(a);
    CFRelease(surf);
    CFRelease(props); CFRelease(nw); CFRelease(nh); CFRelease(nb);
    CGLDestroyPixelFormat(pf);
}

/* One IOSurface in two processes, as browsers share a plugin's or a GPU process's picture
 * (queue item 2, 2026-11-27): this process draws into a global surface through a framebuffer
 * object (a red half over a blue clear) and a second process, started from here, looks the
 * surface up by its ID and draws it. Apple's software renderer is the reference. */
static CGLContextObj xproc_context(CGLPixelFormatAttribute renderer)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32, kCGLPFAPBuffer,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[4] = 0;
    CGLPixelFormatObj pf;
    GLint npf;
    CGLContextObj c = NULL;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &c))
        return NULL;
    CGLDestroyPixelFormat(pf);
    return c;
}

/* The second process: draw surface ID into its own framebuffer object, print two pixels. */
static int run_iosurface_xproc_child(const char *name, CGLPixelFormatAttribute renderer, uint32_t id)
{
    enum { SW = 64, SH = 64 };
    CGLContextObj b = xproc_context(renderer);
    IOSurfaceRef surf = IOSurfaceLookup(id);
    if (!b || !surf) {
        printf("  %s, second process: %s\n", name, b ? "surface not found" : "no context");
        return 1;
    }
    CGLSetCurrentContext(b);
    GLuint fbo, rb, tex;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, SW, SH);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, SW, SH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, SW, 0, SH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    CGLError e = CGLTexImageIOSurface2D(b, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, SW, SH, GL_BGRA,
                                        GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClearColor(0.5f, 0.5f, 0.5f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
    ios_draw_quad(0, 0, SW, SH, SW, SH);
    glDisable(GL_TEXTURE_RECTANGLE_ARB);
    unsigned char px[2][4] = { { 0 } };
    glReadPixels(SW / 4, SH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[0]);
    glReadPixels(3 * SW / 4, SH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[1]);
    printf("  %s, second process (surface %u, CGLTexImageIOSurface2D %d): left %3d %3d %3d %3d, "
           "right %3d %3d %3d %3d (want red, blue)\n", name, id, e, px[0][0], px[0][1], px[0][2],
           px[0][3], px[1][0], px[1][1], px[1][2], px[1][3]);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(b);
    CFRelease(surf);
    return 0;
}

#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

static void run_iosurface_xproc(const char *self, const char *name, CGLPixelFormatAttribute renderer)
{
    enum { SW = 64, SH = 64 };
    CGLContextObj a = xproc_context(renderer);
    int w = SW, h = SH, bpe = 4;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h),
                nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe);
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceIsGlobal };
    const void *vals[] = { nw, nh, nb, kCFBooleanTrue };
    CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef surf = IOSurfaceCreate(props);
    if (!a || !surf) {
        printf("%s: no context or surface\n", name);
        return;
    }
    iosurface_fill(surf, 0xff808080);                   /* grey in memory to start with */
    CGLSetCurrentContext(a);
    GLuint ta, fbo;
    glGenTextures(1, &ta);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, ta);
    CGLError ea = CGLTexImageIOSurface2D(a, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA8, SW, SH, GL_BGRA,
                                         GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_RECTANGLE_ARB, ta, 0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    glViewport(0, 0, SW, SH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, SW, 0, SH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 0, 0);
    ios_draw_quad(0, 0, SW / 2, SH, 1, 1);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glFinish();
    printf("%s: one IOSurface in two processes (framebuffer status 0x%x, CGLTexImageIOSurface2D %d):\n",
           name, status, ea);
    fflush(stdout);
    char ids[16], rid[16];
    snprintf(ids, sizeof(ids), "%u", IOSurfaceGetID(surf));
    snprintf(rid, sizeof(rid), "%u", (unsigned)renderer);
    char *args[] = { (char *)self, "iosurface-xproc-child", ids, rid, (char *)name, NULL };
    pid_t pid;
    int st = 0;
    if (posix_spawn(&pid, self, NULL, NULL, args, environ) == 0)
        waitpid(pid, &st, 0);
    else
        printf("  could not start the second process\n");
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(a);
    CFRelease(surf);
    CFRelease(props); CFRelease(nw); CFRelease(nh); CFRelease(nb);
}

/* The iTunes visualizer's first check (breadth pass, 2026-11-27): a proxy rectangle texture of
 * the view's size, RGBA with 8888_REV, then its width read back; the visualizer draws nothing
 * when the answer is 0. A few sizes and formats, ours against Apple's software renderer. */
static void run_proxy(const char *name, CGLPixelFormatAttribute renderer)
{
    CGLContextObj c = xproc_context(renderer);
    if (!c) {
        printf("%s: no context\n", name);
        return;
    }
    CGLSetCurrentContext(c);
    static const struct { GLenum target, ifmt, format, type; int w, h; } cases[] = {
        { GL_PROXY_TEXTURE_RECTANGLE_ARB, GL_RGBA, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, 1098, 546 },
        { GL_PROXY_TEXTURE_RECTANGLE_ARB, GL_RGBA, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 1098, 546 },
        { GL_PROXY_TEXTURE_RECTANGLE_ARB, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1098, 546 },
        { GL_PROXY_TEXTURE_2D, GL_RGBA, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, 1024, 512 },
        { GL_PROXY_TEXTURE_RECTANGLE_ARB, GL_RGBA, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, 8192, 8192 },
    };
    GLint maxrect = 0;
    glGetIntegerv(GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB, &maxrect);
    printf("%s: proxy textures (max rectangle size %d)\n", name, maxrect);
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        GLint w = -1, h = -1, f = -1;
        glTexImage2D(cases[k].target, 0, cases[k].ifmt, cases[k].w, cases[k].h, 0, cases[k].format,
                     cases[k].type, NULL);
        GLenum err = glGetError();
        glGetTexLevelParameteriv(cases[k].target, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(cases[k].target, 0, GL_TEXTURE_HEIGHT, &h);
        glGetTexLevelParameteriv(cases[k].target, 0, GL_TEXTURE_INTERNAL_FORMAT, &f);
        printf("  target 0x%x %dx%d ifmt 0x%x format 0x%x type 0x%x: width %d height %d internal 0x%x, "
               "errors 0x%x 0x%x\n", cases[k].target, cases[k].w, cases[k].h, cases[k].ifmt,
               cases[k].format, cases[k].type, w, h, f, err, glGetError());
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
}

/* ---- stale: a context destroyed from another thread ----------------------------------------- *
 * A thread draws with a context and lets go of it; the main thread destroys that context and
 * makes the next one, which the thread then draws with. A garbage-collected program does this
 * all the time (the collector's thread destroys what the main thread used; ScreenSaverEngine).
 * A driver that remembers "the context current on this thread" by its address goes wrong when
 * the new context lands where the old one was. Each round clears a framebuffer object to a
 * colour of its own and reads it back. */
#include <pthread.h>

static pthread_mutex_t stale_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stale_cond = PTHREAD_COND_INITIALIZER;
static int stale_step;                  /* odd: the drawing thread's turn; even: main's */
static CGLContextObj stale_ctx;
static unsigned stale_value, stale_read;
static bool stale_quit;

static void stale_wait(int parity)
{
    pthread_mutex_lock(&stale_lock);
    while ((stale_step & 1) != parity)
        pthread_cond_wait(&stale_cond, &stale_lock);
    pthread_mutex_unlock(&stale_lock);
}

static void stale_next(void)
{
    pthread_mutex_lock(&stale_lock);
    stale_step++;
    pthread_cond_broadcast(&stale_cond);
    pthread_mutex_unlock(&stale_lock);
}

static void *stale_thread(void *arg)
{
    (void)arg;
    for (;;) {
        stale_wait(1);
        if (stale_quit)
            break;
        GLuint fbo = 0, tex = 0;
        GLubyte px[4] = { 0, 0, 0, 0 };
        CGLSetCurrentContext(stale_ctx);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffersEXT(1, &fbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
                                  tex, 0);
        glViewport(0, 0, 16, 16);
        glClearColor(stale_value / 255.0f, 0.5f, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glFinish();
        stale_read = px[0];
        CGLSetCurrentContext(NULL);
        stale_next();
    }
    return NULL;
}

static void run_stale(const char *name, CGLPixelFormatAttribute renderer, int rounds)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 32,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[3] = 0;
    CGLPixelFormatObj pf;
    GLint npf;
    pthread_t thread;
    int wrong = 0, done = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf) {
        printf("%s: no pixel format\n", name);
        return;
    }
    stale_quit = false;
    stale_step = 0;                     /* main's turn: the thread waits for its first context */
    if (pthread_create(&thread, NULL, stale_thread, NULL)) {
        printf("%s: no thread\n", name);
        return;
    }
    for (int r = 0; r < rounds; r++) {
        CGLContextObj c;
        if (CGLCreateContext(pf, NULL, &c)) {
            printf("%s: no context in round %d\n", name, r);
            break;
        }
        stale_ctx = c;
        stale_value = 16 + (unsigned)(r * 37) % 224;
        stale_read = 0;
        stale_next();                   /* the thread draws with it and lets go */
        stale_wait(0);
        if (abs((int)stale_read - (int)stale_value) > 1)
            wrong++;
        CGLDestroyContext(c);           /* from here: the other thread used it last */
        done++;
    }
    stale_quit = true;
    stale_next();
    pthread_join(thread, NULL);
    CGLDestroyPixelFormat(pf);
    printf("%s: %d rounds of a context drawn with on one thread and destroyed from another, "
           "%d read back wrong\n", name, done, wrong);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc > 4 && !strcmp(argv[1], "iosurface-xproc-child"))
        return run_iosurface_xproc_child(argv[4], (CGLPixelFormatAttribute)strtoul(argv[3], NULL, 0),
                                         (uint32_t)strtoul(argv[2], NULL, 0));
    if (argc > 1 && !strcmp(argv[1], "proxy")) {
        run_proxy("ours", 0);
        run_proxy("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "iosurface-xproc")) {
        run_iosurface_xproc(argv[0], "ours", 0);
        run_iosurface_xproc(argv[0], "software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "stale")) {
        int rounds = argc > 2 ? atoi(argv[2]) : 40;
        run_stale("software", (CGLPixelFormatAttribute)0x00020400, rounds);
        run_stale("ours", 0, rounds);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "iosurface")) {
        run_iosurface("ours", 0);
        run_iosurface("software", (CGLPixelFormatAttribute)0x00020400);
        run_iosurface_share("ours", 0);
        run_iosurface_share("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "stencil")) {
        run_stencil("ours", 0);
        run_stencil("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "ycbcr")) {
        run_ycbcr("ours", 0);
        run_ycbcr("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "yuv")) {

        run_yuv("ours", 0, false);
        run_yuv("ours", 0, true);
        run_yuv("software", (CGLPixelFormatAttribute)0x00020400, false);
        run_yuv("software", (CGLPixelFormatAttribute)0x00020400, true);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "tex3d")) {

        run_tex3d("ours", 0);
        run_tex3d("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "samplers")) {
        run_samplers("ours", 0);
        run_samplers("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "samepos")) {
        run_samepos("ours", 0);
        run_samepos("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "peel")) {
        run_peel("ours", 0);
        run_peel("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "redlum")) {
        run_redlum("ours", 0);
        run_redlum("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "tex1d")) {
        run_tex1d("ours", 0);
        run_tex1d("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "depth")) {
        run_depth("ours", 0);
        run_depth("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "generic")) {
        run_generic("ours", 0, 2048);
        run_generic("software", (CGLPixelFormatAttribute)0x00020400, 2048);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "vbo")) {
        run_vbo("ours", 0);
        run_vbo("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "dxt")) {
        run_dxt("ours", 0);
        run_dxt("software", (CGLPixelFormatAttribute)0x00020400);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "sequence")) {
        run_sequence("ours", 0, false);
        run_sequence("ours", 0, true);
        run_sequence("software", (CGLPixelFormatAttribute)0x00020400, false);
        run_sequence("software", (CGLPixelFormatAttribute)0x00020400, true);
        return 0;
    }
    run("ours (accelerated)", 0);
    run("Apple software", (CGLPixelFormatAttribute)0x00020400);   /* kCGLRendererGenericFloatID */
    return 0;
}
