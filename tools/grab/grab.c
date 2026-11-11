/*
 * grab: save what is on screen as a PNG, for checking GL output on VM 210. screencapture writes
 * nothing when run over ssh there, and QEMU can't screendump a virgl (GL) display.
 *   grab OUT.png
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: grab OUT.png\n");
        return 2;
    }
    fprintf(stderr, "grab: display 0x%x, captured %d, Quartz Extreme (CGDisplayUsesOpenGLAcceleration) %d\n",
            CGMainDisplayID(), (int)CGDisplayIsCaptured(CGMainDisplayID()),
            (int)CGDisplayUsesOpenGLAcceleration(CGMainDisplayID()));
    CGImageRef img = CGWindowListCreateImage(CGRectInfinite, kCGWindowListOptionOnScreenOnly,
                                             kCGNullWindowID, kCGWindowImageDefault);
    fprintf(stderr, "grab: image %p\n", (void *)img);
    if (!img) {
        fprintf(stderr, "grab: CGDisplayCreateImage failed\n");
        return 1;
    }
    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    int ok = dst != NULL;
    if (ok) {
        CGImageDestinationAddImage(dst, img, NULL);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    printf("%s: %zux%zu %s\n", argv[1], CGImageGetWidth(img), CGImageGetHeight(img),
           ok ? "saved" : "NOT saved");
    CFRelease(url);
    CFRelease(path);
    CGImageRelease(img);
    return ok ? 0 : 1;
}
