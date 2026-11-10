/*
 * cgl-info: list the OpenGL renderers CGL knows about, then make one context, clear an
 * offscreen framebuffer object and read a pixel back.
 *   cgl-info            accelerated renderer
 *   cgl-info -s         Apple's software renderer (GLRendererFloat), for comparison
 * M1 uses it as a trace target; M4 uses it to check our renderer.
 * RIPPLE_WAIT=N: list the renderers once (which makes OpenGL load the driver bundles), then
 * sleep N seconds so dtrace can attach with pid probes on the driver, then carry on. Loading
 * the driver with our own dlopen instead left dtrace unable to see any module (2026-11-10).
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLRenderers.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(call) do { CGLError e_ = (call); if (e_) { \
    fprintf(stderr, "%s failed: %d (%s)\n", #call, e_, CGLErrorString(e_)); exit(1); } } while (0)

static void list_renderers(void)
{
    CGLRendererInfoObj info;
    GLint count = 0;
    CHECK(CGLQueryRendererInfo(0xffffffff, &info, &count));
    printf("renderers: %d\n", count);
    for (GLint i = 0; i < count; i++) {
        GLint id = 0, accel = 0, vram = 0, tex = 0, online = 0, mask = 0, window = 0, offscreen = 0;
        CGLDescribeRenderer(info, i, kCGLRPRendererID, &id);
        CGLDescribeRenderer(info, i, kCGLRPAccelerated, &accel);
        CGLDescribeRenderer(info, i, kCGLRPVideoMemory, &vram);
        CGLDescribeRenderer(info, i, kCGLRPTextureMemory, &tex);
        CGLDescribeRenderer(info, i, kCGLRPOnline, &online);
        CGLDescribeRenderer(info, i, kCGLRPDisplayMask, &mask);
        CGLDescribeRenderer(info, i, kCGLRPWindow, &window);
        CGLDescribeRenderer(info, i, kCGLRPOffScreen, &offscreen);
        printf("  [%d] id 0x%08x accelerated %d online %d window %d offscreen %d "
               "displaymask 0x%x vram %d MB texmem %d MB\n", i, id, accel, online, window,
               offscreen, mask, vram >> 20, tex >> 20);
    }
    CGLDestroyRendererInfo(info);
}

int main(int argc, char **argv)
{
    int software = argc > 1 && strcmp(argv[1], "-s") == 0;
    const char *wait = getenv("RIPPLE_WAIT");
    list_renderers();
    if (wait) {
        printf("pid %d waiting %s s\n", getpid(), wait);
        fflush(stdout);
        sleep((unsigned)atoi(wait));
        list_renderers();
    }

    CGLPixelFormatAttribute hw[] = { kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0 };
    CGLPixelFormatAttribute sw[] = { kCGLPFARendererID, kCGLRendererGenericFloatID,
        kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0 };
    CGLPixelFormatObj pf;
    GLint npf = 0;
    CHECK(CGLChoosePixelFormat(software ? sw : hw, &pf, &npf));
    CGLContextObj ctx;
    CHECK(CGLCreateContext(pf, NULL, &ctx));
    CGLDestroyPixelFormat(pf);
    CHECK(CGLSetCurrentContext(ctx));

    printf("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\n", glGetString(GL_VENDOR),
           glGetString(GL_RENDERER), glGetString(GL_VERSION));
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("GLSL        %s\n", glsl ? glsl : "(none)");

    GLuint fbo, rb;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 64, 64);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, rb);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    glViewport(0, 0, 64, 64);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = { 0 };
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("fbo status 0x%x, pixel %u %u %u %u (expect about 64 128 191 255), glGetError 0x%x\n",
           status, px[0], px[1], px[2], px[3], glGetError());

    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    return 0;
}
