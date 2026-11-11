/*
 * ci-effects: Core Image filters and Photo Booth's effect compositions, on our renderer and on
 * Apple's software renderers, compared pixel by pixel and timed (M6: "Core Image filters run on
 * the GPU (Quartz Composer, Photo Booth effects)"; VM 210 has no camera, so the effects get a
 * still picture as their camera image). Writes a contact sheet (ours | reference) per mode.
 *   ci-effects filters IMAGE OUT.png      Core Image filters: our GPU context vs CI's
 *                                         software renderer
 *   ci-effects booth IMAGE OUT.png        /System/Library/Compositions effects through
 *                                         QCRenderer: our renderer vs Apple's software renderer
 * A line per effect: max channel difference, share of pixels off by more than 16, ms per frame.
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Quartz/Quartz.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <mach/mach_time.h>

enum { W = 480, H = 300, TW = 240, TH = 150 };

static double ms_since(uint64_t t0)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

static CGLContextObj make_context(CGLPixelFormatAttribute renderer, CGLPixelFormatObj *pfo)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, kCGLPFAPBuffer,
                                        kCGLPFANoRecovery,
                                        renderer ? kCGLPFARendererID : kCGLPFAAccelerated,
                                        renderer, 0 };
    if (!renderer)
        attrs[7] = 0;
    CGLPixelFormatObj pf = NULL;
    GLint n = 0;
    CGLContextObj c = NULL;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &c))
        return NULL;
    *pfo = pf;
    return c;
}

/* Compare two RGBA8 buffers: max channel difference and pixels off by more than 16. */
static void compare(const uint8_t *a, const uint8_t *b, int *maxd, double *off)
{
    int m = 0, bad = 0;
    for (int i = 0; i < W * H; i++) {
        int worst = 0;
        for (int c = 0; c < 4; c++) {
            int d = abs((int)a[i * 4 + c] - (int)b[i * 4 + c]);
            if (d > worst)
                worst = d;
        }
        if (worst > m)
            m = worst;
        if (worst > 16)
            bad++;
    }
    *maxd = m;
    *off = 100.0 * bad / (W * H);
}

/* The contact sheet: row r holds ours (left) and the reference (right), each TW x TH. */
static CGContextRef sheet;
static CGColorSpaceRef rgb;
static void sheet_put(int row, int col, const uint8_t *px, const char *label)
{
    CGContextRef bc = CGBitmapContextCreate((void *)px, W, H, 8, W * 4, rgb,
                                            kCGImageAlphaPremultipliedLast);
    CGImageRef img = CGBitmapContextCreateImage(bc);
    int rows = (int)CGBitmapContextGetHeight(sheet) / (TH + 16);
    CGRect r = CGRectMake(col * (TW + 8), (rows - 1 - row) * (TH + 16), TW, TH);
    CGContextDrawImage(sheet, r, img);
    CGImageRelease(img);
    CGContextRelease(bc);
    CGContextSetRGBFillColor(sheet, 1, 1, 1, 1);
    CGContextSelectFont(sheet, "Helvetica", 11, kCGEncodingMacRoman);
    CGContextShowTextAtPoint(sheet, r.origin.x + 2, r.origin.y + TH + 3, label, strlen(label));
}

static void png_write(CGImageRef img, const char *path)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    CGImageDestinationFinalize(d);
    CFRelease(d);
    CFRelease(url);
}

/* The sheet as OUT.png, and in pages of 8 rows as OUT-1.png, OUT-2.png, ... (easier to view). */
static void sheet_write(const char *path)
{
    CGImageRef img = CGBitmapContextCreateImage(sheet);
    png_write(img, path);
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img), page = 8 * (TH + 16);
    char base[512];
    snprintf(base, sizeof(base), "%s", path);
    char *dot = strrchr(base, '.');
    if (dot)
        *dot = 0;
    for (size_t y = 0, n = 1; y < h; y += page, n++) {
        CGImageRef part = CGImageCreateWithImageInRect(img, CGRectMake(0, y, w, MIN(page, h - y)));
        char p[600];
        snprintf(p, sizeof(p), "%s-%zu.png", base, n);
        png_write(part, p);
        CGImageRelease(part);
    }
    CGImageRelease(img);
}

/* Core Image on 10.6 renders 8-bit as ARGB only: to RGBA in place. */
static void argb_to_rgba(uint8_t *p)
{
    for (int i = 0; i < W * H; i++) {
        uint8_t a = p[i * 4];
        p[i * 4] = p[i * 4 + 1];
        p[i * 4 + 1] = p[i * 4 + 2];
        p[i * 4 + 2] = p[i * 4 + 3];
        p[i * 4 + 3] = a;
    }
}

static CIImage *filtered(CIImage *in, NSString *name)
{
    CIFilter *f = [CIFilter filterWithName:name];
    if (!f)
        return nil;
    [f setDefaults];
    [f setValue:in forKey:kCIInputImageKey];
    NSDictionary *attrs = [f attributes];
    CIVector *centre = [CIVector vectorWithX:W / 2 Y:H / 2];
    if ([attrs objectForKey:@"inputCenter"])
        [f setValue:centre forKey:@"inputCenter"];
    if ([name isEqualToString:@"CISepiaTone"])
        [f setValue:[NSNumber numberWithFloat:0.9f] forKey:@"inputIntensity"];
    CIImage *out = [f valueForKey:kCIOutputImageKey];
    return [out imageByCroppingToRect:CGRectMake(0, 0, W, H)];
}

static int run_filters(CIImage *src, const char *out)
{
    NSArray *names = [NSArray arrayWithObjects:@"CIColorControls", @"CISepiaTone",
        @"CIColorMonochrome", @"CIColorInvert", @"CIFalseColor", @"CIExposureAdjust",
        @"CIGaussianBlur", @"CIZoomBlur", @"CIMotionBlur", @"CIBloom", @"CIGloom",
        @"CIPixellate", @"CICrystallize", @"CIPointillize", @"CIEdges", @"CIEdgeWork",
        @"CIComicEffect", @"CITwirlDistortion", @"CIBumpDistortion", @"CIPinchDistortion",
        @"CIHoleDistortion", @"CIKaleidoscope", @"CIColorPosterize", @"CIUnsharpMask", nil];
    CGLPixelFormatObj pf = NULL;
    CGLContextObj gl = make_context(0, &pf);
    if (!gl) {
        fprintf(stderr, "ci-effects: no accelerated context\n");
        return 1;
    }
    if (getenv("CI_EFFECTS_WAIT"))        /* time to attach dtrace to the loaded GL bundle */
        sleep(atoi(getenv("CI_EFFECTS_WAIT")));
    CGLSetCurrentContext(gl);
    printf("Core Image filters, %dx%d: ours = %s; reference = Core Image's software renderer\n",
           W, H, glGetString(GL_RENDERER));
    CIContext *gpu = [CIContext contextWithCGLContext:gl pixelFormat:pf options:nil];
    NSDictionary *swo = [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:YES]
                                                    forKey:kCIContextUseSoftwareRenderer];
    CIContext *sw = [CIContext contextWithCGLContext:gl pixelFormat:pf options:swo];
    sheet = CGBitmapContextCreate(NULL, 2 * TW + 8, (TH + 16) * [names count], 8, 0, rgb,
                                  kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(sheet, 0.1, 0.1, 0.1, 1);
    CGContextFillRect(sheet, CGRectMake(0, 0, 2 * TW + 8, (TH + 16) * [names count]));
    static uint8_t a[W * H * 4], b[W * H * 4];
    int row = 0, worst_bad = 0;
    for (NSString *name in names) {
        CIImage *img = filtered(src, name);
        if (!img) {
            printf("  %-20s not available\n", [name UTF8String]);
            continue;
        }
        double tg = 0, ts = 0;
        for (int pass = 0; pass < 4; pass++) {          /* the first pass compiles kernels */
            uint64_t t0 = mach_absolute_time();
            /* a slightly different image each pass, so neither context answers from its cache */
            CIImage *pi = [img imageByApplyingTransform:CGAffineTransformMakeTranslation(0, pass * 0.001)];
            [gpu render:pi toBitmap:a rowBytes:W * 4 bounds:CGRectMake(0, 0, W, H)
                 format:kCIFormatARGB8 colorSpace:NULL];
            if (pass)
                tg += ms_since(t0);
            t0 = mach_absolute_time();
            [sw render:pi toBitmap:b rowBytes:W * 4 bounds:CGRectMake(0, 0, W, H)
                format:kCIFormatARGB8 colorSpace:NULL];
            if (pass)
                ts += ms_since(t0);
        }
        argb_to_rgba(a);
        argb_to_rgba(b);
        int maxd;
        double off;
        compare(a, b, &maxd, &off);
        printf("  %-20s max diff %3d, %5.1f%% of pixels off by >16; ms per frame: ours %6.1f, "
               "software %6.1f\n", [name UTF8String], maxd, off, tg / 3, ts / 3);
        if (off > 1.0)
            worst_bad++;
        char label[96];
        snprintf(label, sizeof(label), "%s: ours", [name UTF8String]);
        sheet_put(row, 0, a, label);
        snprintf(label, sizeof(label), "%s: software", [name UTF8String]);
        sheet_put(row, 1, b, label);
        row++;
    }
    sheet_write(out);
    printf("%d of %d filters differ on more than 1%% of pixels; contact sheet %s\n", worst_bad,
           row, out);
    return 0;
}

/* One composition rendered with `renderer` into a W x H pbuffer; RGBA8 into px (top row first). */
static bool render_composition(QCComposition *comp, CGLPixelFormatAttribute renderer, CIImage *src,
                               uint8_t *px, double *ms)
{
    CGLPixelFormatObj pf = NULL;
    CGLContextObj gl = make_context(renderer, &pf);
    if (!gl)
        return false;
    CGLPBufferObj pb = NULL;
    if (CGLCreatePBuffer(W, H, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pb))
        return false;
    GLint screen = 0;
    CGLGetVirtualScreen(gl, &screen);
    CGLSetPBuffer(gl, pb, 0, 0, screen);
    CGLSetCurrentContext(gl);
    glViewport(0, 0, W, H);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
    QCRenderer *r = [[QCRenderer alloc] initWithCGLContext:gl pixelFormat:pf colorSpace:cs
                                               composition:comp];
    CGColorSpaceRelease(cs);
    if (!r)
        return false;
    [r setValue:src forInputKey:QCCompositionInputImageKey];
    double total = 0;
    for (int i = 0; i < 6; i++) {
        uint64_t t0 = mach_absolute_time();
        [r renderAtTime:1.0 + i / 30.0 arguments:nil];
        glFinish();
        if (i >= 2)
            total += ms_since(t0);
    }
    [r renderAtTime:2.0 arguments:nil];
    glFinish();
    *ms = total / 4;
    static uint8_t tmp[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < H; y++)
        memcpy(px + (size_t)y * W * 4, tmp + (size_t)(H - 1 - y) * W * 4, W * 4);
    [r release];
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(gl);
    CGLDestroyPBuffer(pb);
    CGLDestroyPixelFormat(pf);
    return true;
}

static int run_booth(CIImage *src, const char *out)
{
    NSString *dir = @"/System/Library/Compositions";
    NSMutableArray *effects = [NSMutableArray array];
    for (NSString *f in [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:NULL]
                         sortedArrayUsingSelector:@selector(compare:)]) {
        if (![[f pathExtension] isEqualToString:@"qtz"])
            continue;
        QCComposition *c = [QCComposition compositionWithFile:[dir stringByAppendingPathComponent:f]];
        if (c && [[c inputKeys] containsObject:QCCompositionInputImageKey] &&
            [[c protocols] containsObject:QCCompositionProtocolImageFilter])
            [effects addObject:f];
    }
    printf("Photo Booth / image-filter compositions (%d in %s), %dx%d: ours vs Apple's software "
           "renderer\n", (int)[effects count], [dir UTF8String], W, H);
    sheet = CGBitmapContextCreate(NULL, 2 * TW + 8, (TH + 16) * [effects count], 8, 0, rgb,
                                  kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(sheet, 0.1, 0.1, 0.1, 1);
    CGContextFillRect(sheet, CGRectMake(0, 0, 2 * TW + 8, (TH + 16) * [effects count]));
    static uint8_t a[W * H * 4], b[W * H * 4];
    int row = 0, differ = 0;
    for (NSString *f in effects) {
        NSAutoreleasePool *p = [[NSAutoreleasePool alloc] init];
        QCComposition *c = [QCComposition compositionWithFile:[dir stringByAppendingPathComponent:f]];
        double tg = -1, ts = -1;
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        bool okg = render_composition(c, 0, src, a, &tg);
        bool oks = render_composition(c, (CGLPixelFormatAttribute)0x00020400, src, b, &ts);
        int maxd = 0;
        double off = 0;
        compare(a, b, &maxd, &off);
        printf("  %-22s %s%s max diff %3d, %5.1f%% off by >16; ms per frame: ours %6.1f, software "
               "%6.1f\n", [[f stringByDeletingPathExtension] UTF8String], okg ? "" : "[ours failed] ",
               oks ? "" : "[software failed] ", maxd, off, tg, ts);
        if (off > 1.0) {
            differ++;
            /* full-size pair next to the sheet: OUT-NAME-ours.png, OUT-NAME-software.png */
            for (int k = 0; k < 2; k++) {
                CGContextRef bc = CGBitmapContextCreate(k ? b : a, W, H, 8, W * 4, rgb,
                                                        kCGImageAlphaPremultipliedLast);
                CGImageRef im = CGBitmapContextCreateImage(bc);
                char p[700];
                snprintf(p, sizeof(p), "%.*s-%s-%s.png", (int)(strrchr(out, '.') - out), out,
                         [[f stringByDeletingPathExtension] UTF8String], k ? "software" : "ours");
                png_write(im, p);
                CGImageRelease(im);
                CGContextRelease(bc);
            }
        }
        char label[96];
        snprintf(label, sizeof(label), "%s: ours", [[f stringByDeletingPathExtension] UTF8String]);
        sheet_put(row, 0, a, label);
        snprintf(label, sizeof(label), "%s: software", [[f stringByDeletingPathExtension] UTF8String]);
        sheet_put(row, 1, b, label);
        row++;
        [p release];
    }
    sheet_write(out);
    printf("%d of %d differ on more than 1%% of pixels; contact sheet %s\n", differ, row, out);
    return 0;
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    if (argc < 4) {
        fprintf(stderr, "usage: ci-effects filters|booth IMAGE OUT.png\n");
        return 2;
    }
    rgb = CGColorSpaceCreateDeviceRGB();
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]];
    CIImage *src = [CIImage imageWithContentsOfURL:url];
    if (!src) {
        fprintf(stderr, "ci-effects: can't read %s\n", argv[2]);
        return 1;
    }
    /* Scale the picture to W x H. */
    CGRect e = [src extent];
    CGAffineTransform t = CGAffineTransformMakeScale(W / e.size.width, H / e.size.height);
    src = [[src imageByApplyingTransform:t] imageByCroppingToRect:CGRectMake(0, 0, W, H)];
    int r = !strcmp(argv[1], "booth") ? run_booth(src, argv[3]) : run_filters(src, argv[3]);
    [pool release];
    return r;
}
