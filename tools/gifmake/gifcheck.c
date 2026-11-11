/*
 * gifcheck: decode an animated GIF with Apple's ImageIO (in the guest), as an independent check
 * of tools/gifmake: frame count, each frame's delay, the total, and optionally one frame as PNG.
 *   gifcheck IN.gif [FRAME OUT.png]
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "usage: gifcheck IN.gif [FRAME OUT.png]\n");
        return 2;
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1],
                                                           strlen(argv[1]), false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    if (!src) {
        fprintf(stderr, "gifcheck: can't open %s\n", argv[1]);
        return 1;
    }
    size_t n = CGImageSourceGetCount(src);
    double total = 0;
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        CGImageRef im = CGImageSourceCreateImageAtIndex(src, i, NULL);
        if (!im)
            bad++;
        CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, i, NULL);
        double delay = 0;
        if (props) {
            CFDictionaryRef g = CFDictionaryGetValue(props, kCGImagePropertyGIFDictionary);
            CFNumberRef d = g ? CFDictionaryGetValue(g, kCGImagePropertyGIFDelayTime) : NULL;
            if (d)
                CFNumberGetValue(d, kCFNumberDoubleType, &delay);
            CFRelease(props);
        }
        total += delay;
        if (argc == 4 && (size_t)atoi(argv[2]) == i && im) {
            CFURLRef o = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[3],
                                                                 strlen(argv[3]), false);
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL(o, kUTTypePNG, 1, NULL);
            CGImageDestinationAddImage(dst, im, NULL);
            CGImageDestinationFinalize(dst);
            CFRelease(dst);
            CFRelease(o);
            printf("frame %zu: %zux%zu written to %s\n", i, CGImageGetWidth(im), CGImageGetHeight(im),
                   argv[3]);
        }
        if (im)
            CGImageRelease(im);
    }
    printf("%s: %zu frames, %d failed to decode, delays add up to %.2f s\n", argv[1], n, bad, total);
    return bad ? 1 : 0;
}
