/*
 * setmode: list the main display's modes, or switch to one (CoreGraphics display
 * configuration, the API System Preferences' Displays pane uses).
 *   setmode              list modes; * marks the current one
 *   setmode 1920x1200    switch permanently to that size
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    CGDirectDisplayID d = CGMainDisplayID();
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, NULL);
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
    if (!modes || !cur) {
        fprintf(stderr, "no display modes\n");
        return 1;
    }
    size_t want_w = 0, want_h = 0;
    if (argc > 1 && sscanf(argv[1], "%zux%zu", &want_w, &want_h) != 2) {
        fprintf(stderr, "usage: setmode [WIDTHxHEIGHT]\n");
        return 2;
    }
    CGDisplayModeRef pick = NULL;
    for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        size_t w = CGDisplayModeGetWidth(m), h = CGDisplayModeGetHeight(m);
        if (!want_w)
            printf("%c %zux%zu @ %.0f Hz (id %d)\n",
                   CGDisplayModeGetIODisplayModeID(m) == CGDisplayModeGetIODisplayModeID(cur) ? '*' : ' ',
                   w, h, CGDisplayModeGetRefreshRate(m), CGDisplayModeGetIODisplayModeID(m));
        else if (w == want_w && h == want_h && !pick)
            pick = m;
    }
    if (!want_w)
        return 0;
    if (!pick) {
        fprintf(stderr, "no %zux%zu mode\n", want_w, want_h);
        return 3;
    }
    CGDisplayConfigRef cfg;
    CGError e = CGBeginDisplayConfiguration(&cfg);
    if (!e) e = CGConfigureDisplayWithDisplayMode(cfg, d, pick, NULL);
    if (!e) e = CGCompleteDisplayConfiguration(cfg, kCGConfigurePermanently);
    CGRect b = CGDisplayBounds(d);
    printf("switch to %zux%zu: %s; display now %.0fx%.0f\n", want_w, want_h,
           e ? "failed" : "ok", b.size.width, b.size.height);
    return e ? 4 : 0;
}
