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
 *   gl-feature-check stencil    a drawable with depth and stencil buffers, both used
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
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

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "stencil")) {
        run_stencil("ours", 0);
        run_stencil("software", (CGLPixelFormatAttribute)0x00020400);
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
