/*
 * fb-dump: save the framebuffer's memory (what the CPU sees as the screen, through our kext's
 * RIPPLE_GPU_MEMORY_FRAMEBUFFER mapping) as a PNG. Under Quartz Extreme the display shows a GPU
 * texture instead, so this tells whether the CPU's view of the screen is current (M5).
 *   fb-dump OUT.png
 */
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>

#include "ripple_gpu_uapi.h"

static uint32_t fb_property(io_service_t fb, const char *key)
{
    uint32_t v = 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingASCII);
    CFTypeRef p = IORegistryEntryCreateCFProperty(fb, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    if (p) {
        if (CFGetTypeID(p) == CFNumberGetTypeID())
            CFNumberGetValue(p, kCFNumberSInt32Type, &v);
        CFRelease(p);
    }
    return v;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: fb-dump OUT.png\n");
        return 2;
    }
    io_service_t fb = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                  IOServiceMatching("RippleFramebuffer"));
    io_service_t accel = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                     IOServiceMatching("RippleAccelerator"));
    if (!fb || !accel) {
        fprintf(stderr, "fb-dump: no RippleFramebuffer or RippleAccelerator\n");
        return 1;
    }
    uint32_t width = fb_property(fb, "RippleScreenWidth");
    uint32_t height = fb_property(fb, "RippleScreenHeight");
    uint32_t row_bytes = fb_property(fb, "RippleRowBytes");
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(accel, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &conn);
    mach_vm_address_t vram = 0;
    mach_vm_size_t size = 0;
    if (kr == KERN_SUCCESS)
        kr = IOConnectMapMemory64(conn, RIPPLE_GPU_MEMORY_FRAMEBUFFER, mach_task_self(), &vram,
                                  &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !width || !height || (uint64_t)row_bytes * height > size) {
        fprintf(stderr, "fb-dump: map 0x%x, %ux%u row bytes %u, %llu bytes\n", kr, width,
                height, row_bytes, (unsigned long long)size);
        return 1;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, (const void *)(uintptr_t)vram,
                                                        (size_t)row_bytes * height, NULL);
    CGImageRef img = CGImageCreate(width, height, 8, 32, row_bytes, cs,
                                   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp,
                                   NULL, false, kCGRenderingIntentDefault);
    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    int ok = img && dst;
    if (ok) {
        CGImageDestinationAddImage(dst, img, NULL);
        ok = CGImageDestinationFinalize(dst);
    }
    printf("%s: %ux%u (row bytes %u) %s\n", argv[1], width, height, row_bytes,
           ok ? "saved" : "NOT saved");
    return ok ? 0 : 1;
}
