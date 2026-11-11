/*
 * ci-check: run Core Image on a CGL context of the default accelerated renderer and show what
 * it did (M5, M6). Under Quartz Extreme on VM 210 that renderer is ours, so this goes through
 * the same choice WindowServer's Core Image makes (GPU or its software renderer), in a process
 * that can be traced from its start (dtrace -c) and whose stderr is visible: Core Image prints
 * "falling back to software" there. Prints the renderer and samples of the result.
 *   ci-check [FILTER]        a generator, default CICheckerboardGenerator; the result also
 *                            goes through CIGaussianBlur so a kernel runs
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    const char *name = argc > 1 ? argv[1] : "CICheckerboardGenerator";
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFANoRecovery,
                                        kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0 };
    CGLPixelFormatObj pf = NULL;
    GLint npix = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &pf, &npix);
    if (err || !pf) {
        fprintf(stderr, "ci-check: no accelerated pixel format (%d)\n", err);
        return 1;
    }
    CGLContextObj gl = NULL;
    if ((err = CGLCreateContext(pf, NULL, &gl)) || !gl) {
        fprintf(stderr, "ci-check: CGLCreateContext %d\n", err);
        return 1;
    }
    CGLSetCurrentContext(gl);
    GLint rid = 0, frag = 0, accel = 0;
    CGLDescribePixelFormat(pf, 0, kCGLPFARendererID, &rid);
    printf("renderer 0x%08x \"%s\", GL %s\n", rid, (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));
    /* The limits Core Image reads before it decides on the GPU (fe_gl_update). */
    static const struct { GLenum pname; const char *name; } plim[] = {
        { 0x88ab, "native params" }, { 0x88a7, "native temps" }, { 0x88af, "native attribs" },
        { 0x88b4, "local params" }, { 0x88a3, "native instr" }, { 0x880e, "native ALU" },
        { 0x880f, "native TEX" }, { 0x8810, "native TEX indir" },
    };
    for (int t = 0; t < 2; t++) {
        GLenum target = t ? 0x8804 : 0x8620;
        printf("  %s program:", t ? "fragment" : "vertex");
        for (unsigned i = 0; i < sizeof(plim) / sizeof(plim[0]); i++) {
            GLint v = -1;
            glGetProgramivARB(target, plim[i].pname, &v);
            printf(" %s %d,", plim[i].name, v);
        }
        printf(" error 0x%x\n", glGetError());
    }
    GLint tc = -1, tu = -1, m3d = -1;
    GLfloat vp[2] = { -1, -1 }, rect = -1;
    glGetIntegerv(0x8871, &tc);
    glGetIntegerv(0x8872, &tu);
    glGetIntegerv(0x8073, &m3d);
    glGetFloatv(0x0d3a, vp);
    glGetFloatv(0x84f8, &rect);
    printf("  texture coords %d, image units %d, 3D max %d, viewport %gx%g, rectangle max %g\n",
           tc, tu, m3d, vp[0], vp[1], rect);
    CGLRendererInfoObj ri = NULL;
    GLint nr = 0;
    if (CGLQueryRendererInfo(0xffffffff, &ri, &nr) == kCGLNoError) {
        for (GLint i = 0; i < nr; i++) {
            GLint id = 0;
            CGLDescribeRenderer(ri, i, kCGLRPRendererID, &id);
            CGLDescribeRenderer(ri, i, kCGLRPGPUFragProcCapable, &frag);
            CGLDescribeRenderer(ri, i, kCGLRPAccelerated, &accel);
            printf("  renderer %d: 0x%08x accelerated %d, GPU fragment programs %d\n", i, id,
                   accel, frag);
        }
        CGLDestroyRendererInfo(ri);
    }

    CIContext *ci = [CIContext contextWithCGLContext:gl pixelFormat:pf options:nil];
    CIFilter *gen = [CIFilter filterWithName:[NSString stringWithUTF8String:name]];
    if (!ci || !gen) {
        fprintf(stderr, "ci-check: no CIContext (%p) or no filter %s\n", (void *)ci, name);
        return 1;
    }
    [gen setDefaults];
    CIImage *img = [gen valueForKey:kCIOutputImageKey];
    CIFilter *blur = [CIFilter filterWithName:@"CIGaussianBlur"];
    [blur setDefaults];
    [blur setValue:img forKey:kCIInputImageKey];
    [blur setValue:[NSNumber numberWithFloat:4.0f] forKey:@"inputRadius"];
    img = [blur valueForKey:kCIOutputImageKey];

    /* The same image through Core Image's software renderer, as the reference. Bounds across
     * a stripe/checker edge (generators centre at 150,150, 80 wide). */
    enum { W = 64, H = 64 };
    static uint32_t px[W * H], ref[W * H];
    CGRect bounds = CGRectMake(160, 160, W, H);
    NSDictionary *sw = [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:YES]
                                                   forKey:kCIContextUseSoftwareRenderer];
    CIContext *cisw = [CIContext contextWithCGLContext:gl pixelFormat:pf options:sw];
    [ci render:img toBitmap:px rowBytes:W * 4 bounds:bounds format:kCIFormatARGB8 colorSpace:NULL];
    [cisw render:img toBitmap:ref rowBytes:W * 4 bounds:bounds format:kCIFormatARGB8
      colorSpace:NULL];
    int maxdiff = 0, bad = 0;
    for (int i = 0; i < W * H; i++) {
        for (int c = 0; c < 32; c += 8) {
            int d = abs((int)((px[i] >> c) & 0xff) - (int)((ref[i] >> c) & 0xff));
            if (d > maxdiff)
                maxdiff = d;
            if (d > 8) {
                bad++;
                break;
            }
        }
    }
    printf("%s + CIGaussianBlur at 160,160 64x64, ARGB8 (bytes A R G B):\n  context  :", name);
    for (int i = 0; i < 8; i++)
        printf(" %08x", px[(i * 8 + 4) * W + i * 8 + 4]);
    printf("\n  software :");
    for (int i = 0; i < 8; i++)
        printf(" %08x", ref[(i * 8 + 4) * W + i * 8 + 4]);
    printf("\n  max difference %d, %d of %d pixels off by more than 8: %s\n", maxdiff, bad,
           W * H, bad ? "DIFFERENT" : "same");
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(gl);
    CGLDestroyPixelFormat(pf);
    [pool release];
    return 0;
}
