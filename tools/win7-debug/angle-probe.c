/* What the ANGLE build inside UTM offers for compressed textures on this Mac: load its EGL and
 * GLES libraries, make an off-screen ES 3 context on the Metal backend (as UTM's QEMU does with
 * ANGLE_DEFAULT_PLATFORM=metal), print the extensions that name a compression, and try a DXT
 * upload of each kind. No window is opened. Run on the Mac that has UTM (ANGLE's EGL library
 * looks for its GLES library through the run path, hence the linker option):
 *   cc angle-probe.c -o angle-probe -Wl,-rpath,/Applications/UTM.app/Contents/Frameworks
 *   ./angle-probe [metal|opengl]
 * (docs/windows/mesa-win7.md, "UTM's host could take DXT textures").
 * MIT licence (LICENSE at the repository root).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FW "/Applications/UTM.app/Contents/Frameworks/"

typedef void *(*get_display_fn)(unsigned, void *, const intptr_t *);
typedef unsigned (*initialize_fn)(void *, int *, int *);
typedef unsigned (*choose_fn)(void *, const int *, void **, int, int *);
typedef void *(*pbuffer_fn)(void *, void *, const int *);
typedef unsigned (*bind_api_fn)(unsigned);
typedef void *(*context_fn)(void *, void *, void *, const int *);
typedef unsigned (*current_fn)(void *, void *, void *, void *);
typedef int (*egl_error_fn)(void);
typedef const unsigned char *(*string_fn)(unsigned);
typedef const unsigned char *(*stringi_fn)(unsigned, unsigned);
typedef void (*integer_fn)(unsigned, int *);
typedef void (*gen_fn)(int, unsigned *);
typedef void (*bind_fn)(unsigned, unsigned);
typedef void (*compressed_fn)(unsigned, int, unsigned, int, int, int, int, const void *);
typedef unsigned (*error_fn)(void);

int main(int argc, char **argv)
{
    const int metal = !(argc > 1 && !strcmp(argv[1], "opengl"));
    void *gles = dlopen(FW "GLESv2.framework/Versions/A/GLESv2", RTLD_NOW | RTLD_GLOBAL);
    void *egl = dlopen(FW "EGL.framework/Versions/A/EGL", RTLD_NOW | RTLD_GLOBAL);
    if (!gles || !egl) { printf("cannot load ANGLE: %s\n", dlerror()); return 2; }
#define E(type, name) type name = (type)dlsym(egl, #name)
#define G(type, name) type name = (type)dlsym(gles, #name)
    E(get_display_fn, eglGetPlatformDisplay); E(initialize_fn, eglInitialize); E(choose_fn, eglChooseConfig);
    E(pbuffer_fn, eglCreatePbufferSurface); E(bind_api_fn, eglBindAPI); E(context_fn, eglCreateContext);
    E(current_fn, eglMakeCurrent); E(egl_error_fn, eglGetError);
    G(string_fn, glGetString); G(stringi_fn, glGetStringi); G(integer_fn, glGetIntegerv); G(gen_fn, glGenTextures);
    G(bind_fn, glBindTexture); G(compressed_fn, glCompressedTexImage2D); G(error_fn, glGetError);
    if (!eglGetPlatformDisplay || !glGetStringi || !glCompressedTexImage2D) { puts("missing entry points"); return 2; }

    const intptr_t display_attribs[] = {0x3203 /* platform type */, metal ? 0x3489 : 0x320D, 0x3038};
    void *display = eglGetPlatformDisplay(0x3202 /* ANGLE */, NULL, display_attribs);
    int major = 0, minor = 0, count = 0;
    if (!display || !eglInitialize(display, &major, &minor)) { printf("no display: EGL error 0x%x\n", eglGetError()); return 1; }
    const int config_attribs[] = {0x3033 /* surface type */, 0x0001 /* pbuffer */, 0x3040 /* renderable */, 0x0040 /* ES3 */,
                                  0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038};
    void *config = NULL;
    if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count < 1) { printf("no config: 0x%x\n", eglGetError()); return 1; }
    const int surface_attribs[] = {0x3057, 16, 0x3056, 16, 0x3038};
    void *surface = eglCreatePbufferSurface(display, config, surface_attribs);
    eglBindAPI(0x30A0);
    const int context_attribs[] = {0x3098, 3, 0x3038};
    void *context = eglCreateContext(display, config, NULL, context_attribs);
    if (!surface || !context || !eglMakeCurrent(display, surface, surface, context)) { printf("no context: 0x%x\n", eglGetError()); return 1; }

    printf("EGL %d.%d, backend asked for: %s\n", major, minor, metal ? "Metal" : "OpenGL");
    printf("version:  %s\nrenderer: %s\n", glGetString(0x1F02), glGetString(0x1F01));
    int extensions = 0;
    glGetIntegerv(0x821D, &extensions);
    printf("%d extensions; those that name a compression:\n", extensions);
    for (int i = 0; i < extensions; ++i) {
        const char *name = (const char *)glGetStringi(0x1F03, (unsigned)i);
        if (name && (strstr(name, "compress") || strstr(name, "s3tc") || strstr(name, "S3")))
            printf("  %s\n", name);
    }
    static const struct { const char *name; unsigned format; int size; } kinds[] = {
        {"DXT1 RGB", 0x83F0, 8 * 16}, {"DXT1 RGBA", 0x83F1, 8 * 16}, {"DXT3", 0x83F2, 16 * 16}, {"DXT5", 0x83F3, 16 * 16},
        {"sRGB DXT1", 0x8C4C, 8 * 16}, {"sRGB DXT5", 0x8C4F, 16 * 16},
    };
    unsigned char blocks[16 * 16];
    for (unsigned i = 0; i < sizeof(blocks); ++i) blocks[i] = (unsigned char)(i * 37 + 11);
    while (glGetError()) {}
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
        unsigned texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(0x0DE1, texture);
        glCompressedTexImage2D(0x0DE1, 0, kinds[k].format, 16, 16, 0, kinds[k].size, blocks);
        unsigned error = glGetError();
        printf("upload of a 16x16 %s texture: %s", kinds[k].name, error ? "refused" : "accepted");
        if (error) printf(" (GL error 0x%x)", error);
        putchar('\n');
    }
    return 0;
}
