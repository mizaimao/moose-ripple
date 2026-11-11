/*
 * qe-check: is Quartz Extreme on, per display? For each active display it prints what
 * WindowServer reports (CGDisplayUsesOpenGLAcceleration) and the inputs WindowServer's decision
 * reads (docs/re/qe-decision.md): the framebuffer's accelerator (IOAccelTypes, IOAccelIndex),
 * that accelerator's AccelCaps and IOGLBundleName, its first PCI ancestor, and the GL renderers
 * that drive the display's OpenGL display mask.
 *   qe-check          exit status 0 if every active display has QE on, 1 if not
 * Works over ssh (no window server connection beyond queries). M5 R3.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <OpenGL/OpenGL.h>
#include <stdio.h>
#include <string.h>

static void print_cfstring(const char *label, CFTypeRef v)
{
    char buf[512] = "-";
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
    printf(" %s %s", label, buf);
}

static long cfnumber(CFTypeRef v, long dflt)
{
    long n = dflt;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue(v, kCFNumberLongType, &n);
    return n;
}

/* The nearest ancestor (IOService plane) whose class is IOPCIDevice or IOAGPDevice:
 * WindowServer's GLCompositorRequiredClasses test (exact class names). */
static void print_pci_ancestor(io_registry_entry_t e)
{
    io_registry_entry_t cur = e, parent;
    IOObjectRetain(cur);
    while (IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent) == KERN_SUCCESS) {
        IOObjectRelease(cur);
        cur = parent;
        io_name_t cls, name;
        if (IOObjectGetClass(cur, cls) == KERN_SUCCESS &&
            (strcmp(cls, "IOPCIDevice") == 0 || strcmp(cls, "IOAGPDevice") == 0)) {
            IORegistryEntryGetName(cur, name);
            printf(" required-class %s (%s)", cls, name);
            IOObjectRelease(cur);
            return;
        }
    }
    IOObjectRelease(cur);
    printf(" required-class NONE");
}

static void print_accelerator(io_service_t fb)
{
    CFTypeRef path = IORegistryEntryCreateCFProperty(fb, CFSTR("IOAccelTypes"), NULL, 0);
    CFTypeRef index = IORegistryEntryCreateCFProperty(fb, CFSTR("IOAccelIndex"), NULL, 0);
    io_name_t fbclass = "?";
    IOObjectGetClass(fb, fbclass);
    printf("  framebuffer %s IOAccelIndex %ld", fbclass, cfnumber(index, -1));
    char p[512] = "";
    if (path && CFGetTypeID(path) == CFStringGetTypeID())
        CFStringGetCString(path, p, sizeof(p), kCFStringEncodingUTF8);
    io_registry_entry_t accel = p[0] ? IORegistryEntryFromPath(kIOMasterPortDefault, p) : 0;
    if (!accel) {
        printf(" accelerator NONE\n");
    } else {
        io_name_t cls = "?";
        IOObjectGetClass(accel, cls);
        CFTypeRef caps = IORegistryEntryCreateCFProperty(accel, CFSTR("AccelCaps"), NULL, 0);
        CFTypeRef gl = IORegistryEntryCreateCFProperty(accel, CFSTR("IOGLBundleName"), NULL, 0);
        printf("\n  accelerator %s AccelCaps %ld", cls, cfnumber(caps, -1));
        print_cfstring("IOGLBundleName", gl);
        print_pci_ancestor(accel);
        printf("\n");
        if (caps) CFRelease(caps);
        if (gl) CFRelease(gl);
        IOObjectRelease(accel);
    }
    if (path) CFRelease(path);
    if (index) CFRelease(index);
}

static void print_renderers(CGOpenGLDisplayMask mask)
{
    CGLRendererInfoObj info;
    GLint n = 0;
    if (CGLQueryRendererInfo(mask, &info, &n) != kCGLNoError) {
        printf("  renderers: query failed\n");
        return;
    }
    for (GLint i = 0; i < n; i++) {
        GLint id = 0, accel = 0, vram = 0, online = 0;
        CGLDescribeRenderer(info, i, kCGLRPRendererID, &id);
        CGLDescribeRenderer(info, i, kCGLRPAccelerated, &accel);
        CGLDescribeRenderer(info, i, kCGLRPVideoMemory, &vram);
        CGLDescribeRenderer(info, i, kCGLRPOnline, &online);
        printf("  renderer 0x%08x accelerated %d online %d vram %d MB\n", id, accel, online,
               vram >> 20);
    }
    CGLDestroyRendererInfo(info);
}

int main(void)
{
    CGDirectDisplayID ids[16];
    uint32_t n = 0;
    if (CGGetActiveDisplayList(16, ids, &n) != kCGErrorSuccess || n == 0) {
        fprintf(stderr, "qe-check: no active displays (no window server?)\n");
        return 2;
    }
    int all = 1;
    for (uint32_t i = 0; i < n; i++) {
        CGDirectDisplayID d = ids[i];
        int qe = CGDisplayUsesOpenGLAcceleration(d);
        all &= qe != 0;
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(d);
        char enc[64] = "?";
        if (mode) {
            CFStringRef e = CGDisplayModeCopyPixelEncoding(mode);
            if (e) {
                CFStringGetCString(e, enc, sizeof(enc), kCFStringEncodingUTF8);
                CFRelease(e);
            }
        }
        printf("display 0x%x%s %zux%zu %s: QE %s\n", d, CGDisplayIsMain(d) ? " (main)" : "",
               mode ? CGDisplayModeGetWidth(mode) : 0, mode ? CGDisplayModeGetHeight(mode) : 0,
               enc, qe ? "on" : "off");
        if (mode)
            CGDisplayModeRelease(mode);
        CGOpenGLDisplayMask mask = CGDisplayIDToOpenGLDisplayMask(d);
        printf("  GL display mask 0x%x\n", mask);
        print_accelerator(CGDisplayIOServicePort(d));
        print_renderers(mask);
    }
    return all ? 0 : 1;
}
