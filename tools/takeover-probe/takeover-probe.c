/*
 * takeover-probe: checks at runtime, on Apple's software renderer, what the GL bundle's
 * dispatch takeover relies on (docs/re/gl-plugin-interface.md, "Where the takeover hooks in"):
 * the CGL object's magic, GLEngine's context (ctx->rend) with its magic and its pointer to
 * &ctx->disp, an all-zero back table after CGLCreateContext, and that a table installed with
 * CGLSelectDispatch receives libGL's calls and keeps them when GLEngine rebuilds its own
 * table (multithreaded engine on). Exit 0 on PASS. Build universal; run both architectures.
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLContext.h>
#include <OpenGL/CGLRenderers.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__)
#define CGL_MAGIC_OFFSET  0x18a0
#define GC_MAGIC_OFFSET   0x6588
#define GC_FRONT_DISPATCH 0x6550
#else
#define CGL_MAGIC_OFFSET  0xc60
#define GC_MAGIC_OFFSET   0x5844
#define GC_FRONT_DISPATCH 0x5820
#endif
#define ENTRIES (sizeof(GLIFunctionDispatch) / sizeof(void *))

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static void (*real_clear)(GLIContext, GLbitfield);
static GLIContext seen_rend;
static int clears;
static void hooked_clear(GLIContext rend, GLbitfield mask)
{
    clears++;
    seen_rend = rend;
    real_clear(rend, mask);
}

static unsigned nonzero(void **t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ENTRIES; i++)
        n += t[i] != 0;
    return n;
}

int main(void)
{
    typedef void (*select_fn)(CGLContextObj, GLIFunctionDispatch *);
    typedef void **(*table_fn)(CGLContextObj);
    select_fn select_dispatch = (select_fn)dlsym(RTLD_DEFAULT, "CGLSelectDispatch");
    table_fn front_dispatch = (table_fn)dlsym(RTLD_DEFAULT, "CGLFrontDispatch");
    table_fn back_dispatch = (table_fn)dlsym(RTLD_DEFAULT, "CGLBackDispatch");
    char msg[160];

    check(select_dispatch && front_dispatch && back_dispatch,
          "OpenGL exports CGLSelectDispatch, CGLFrontDispatch, CGLBackDispatch");
    if (failures)
        return 1;

    CGLPixelFormatAttribute attrs[] = { kCGLPFARendererID, kCGLRendererGenericFloatID,
        kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0 };
    CGLPixelFormatObj pf;
    GLint npf;
    CGLContextObj ctx;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &ctx)) {
        printf("FAIL: no software context\n");
        return 1;
    }
    CGLDestroyPixelFormat(pf);

    uint8_t *obj = (uint8_t *)ctx, *gc = (uint8_t *)ctx->rend;
    snprintf(msg, sizeof(msg), "CGL magic at +0x%x: 0x%x", CGL_MAGIC_OFFSET,
             *(uint32_t *)(obj + CGL_MAGIC_OFFSET));
    check(*(uint32_t *)(obj + CGL_MAGIC_OFFSET) == 0x1b7f1843, msg);
    snprintf(msg, sizeof(msg), "GLEngine context (rend) magic at +0x%x: 0x%x", GC_MAGIC_OFFSET,
             *(uint32_t *)(gc + GC_MAGIC_OFFSET));
    check(*(uint32_t *)(gc + GC_MAGIC_OFFSET) == 0x3463a223, msg);
    check(*(void **)(gc + GC_FRONT_DISPATCH) == (void *)&ctx->disp,
          "GLEngine context points at &ctx->disp");
    check(front_dispatch(ctx) == (void **)&ctx->disp, "CGLFrontDispatch is &ctx->disp");
    void **back = back_dispatch(ctx);
    snprintf(msg, sizeof(msg), "back table empty after CGLCreateContext (%u of %u set)",
             nonzero(back), (unsigned)ENTRIES);
    check(back && nonzero(back) == 0, msg);

    /* Install a copy of the table with glClear hooked, as the bundle installs Mesa's. */
    static GLIFunctionDispatch table;
    memcpy(&table, &ctx->disp, sizeof(table));
    real_clear = table.clear;
    table.clear = hooked_clear;
    select_dispatch(ctx, &table);
    check(ctx->disp.clear == hooked_clear, "CGLSelectDispatch installed our table in front");
    snprintf(msg, sizeof(msg), "GLEngine's table moved to back (%u of %u set)", nonzero(back),
             (unsigned)ENTRIES);
    check(nonzero(back) > ENTRIES / 2, msg);

    CGLSetCurrentContext(ctx);
    GLuint fbo, tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, tex, 0);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLubyte px[4];
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    snprintf(msg, sizeof(msg), "glClear went through our table (%d call, rend %s), pixel %u %u %u %u",
             clears, seen_rend == ctx->rend ? "= ctx->rend" : "WRONG", px[0], px[1], px[2], px[3]);
    check(clears == 1 && seen_rend == ctx->rend && px[0] > 60 && px[0] < 68, msg);

    /* GLEngine rebuilds its whole table when the multithreaded engine is switched; the
     * rebuild must land in back and leave ours in front. */
    CGLError e = CGLEnable(ctx, kCGLCEMPEngine);
    check(ctx->disp.clear == hooked_clear,
          e ? "our table still in front (multithreaded engine refused here)"
            : "our table still in front after CGLEnable(kCGLCEMPEngine)");
    if (!e)
        CGLDisable(ctx, kCGLCEMPEngine);
    glClear(GL_COLOR_BUFFER_BIT);
    check(clears == 2, "glClear still comes to us");

    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    printf("%s (%d-bit process)\n", failures ? "FAIL" : "PASS", (int)(sizeof(void *) * 8));
    return failures ? 1 : 0;
}
