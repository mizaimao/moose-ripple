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
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
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

int main(int argc, char **argv)
{
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
