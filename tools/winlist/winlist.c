/*
 * winlist: the windows on screen, front to back, with their owner, title, layer and bounds in
 * screen points (top-left origin), so scripts can find and click an app's window on VM 210
 * without GUI scripting (that needs "access for assistive devices", off in the guest). M6.
 *   winlist [OWNER]      only windows whose owner name contains OWNER
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>

static void cstr(CFStringRef s, char *buf, size_t n)
{
    buf[0] = 0;
    if (s)
        CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8);
}

int main(int argc, char **argv)
{
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly |
                                                 kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    if (!list)
        return 1;
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
        CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
        char owner[128], name[256];
        cstr(CFDictionaryGetValue(w, kCGWindowOwnerName), owner, sizeof(owner));
        cstr(CFDictionaryGetValue(w, kCGWindowName), name, sizeof(name));
        if (argc > 1 && !strstr(owner, argv[1]))
            continue;
        int id = 0, layer = 0;
        CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowNumber), kCFNumberIntType, &id);
        CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowLayer), kCFNumberIntType, &layer);
        CGRect r = CGRectZero;
        CFDictionaryRef b = CFDictionaryGetValue(w, kCGWindowBounds);
        if (b)
            CGRectMakeWithDictionaryRepresentation(b, &r);
        printf("%6d  layer %4d  %5.0f,%-5.0f %5.0fx%-5.0f  %s: %s\n", id, layer, r.origin.x,
               r.origin.y, r.size.width, r.size.height, owner, name);
    }
    CFRelease(list);
    return 0;
}
