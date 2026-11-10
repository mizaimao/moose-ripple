/*
 * RippleGLDriver (shim): M1 loader experiment. Exports the 80 gld* entry points and forwards
 * every call unchanged to Apple's software renderer, GLRendererFloat. If OpenGL loads this
 * bundle for our accelerator, the plugin path from IOGLBundleName to our code is proven.
 * x86_64 only (the trampolines are x86_64 assembly).
 */
#include <dlfcn.h>
#include <syslog.h>
#include <unistd.h>
#include "gld-names.h"

void *real_gld[GLD_COUNT];

static const char kFloat[] =
    "/System/Library/Frameworks/OpenGL.framework/Resources/GLRendererFloat.bundle/GLRendererFloat";

__attribute__((constructor))
static void ripple_shim_init(void)
{
    int missing = 0;
    void *h = dlopen(kFloat, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        syslog(LOG_ERR, "RippleGLDriver: dlopen GLRendererFloat failed: %s", dlerror());
        return;
    }
    for (int i = 0; i < GLD_COUNT; i++) {
        real_gld[i] = dlsym(h, gld_names[i]);
        if (!real_gld[i]) {
            syslog(LOG_ERR, "RippleGLDriver: GLRendererFloat lacks %s", gld_names[i]);
            missing++;
        }
    }
    syslog(LOG_NOTICE, "RippleGLDriver: loaded in pid %d, forwarding %d gld entry points "
           "to GLRendererFloat (%d missing)", getpid(), GLD_COUNT, missing);
}
