/*
 * rippled3d.c: the Direct3D 7 HAL of rippledd.dll (moose-ripple, Windows 95/98/ME).
 *
 * Direct3D 7 hands a driver its work as command buffers (DrawPrimitives2, "DP2"): render
 * states, texture stage states, clears and primitives made of transformed and lit vertices
 * (screen x and y, depth z, 1/w, colours, texture coordinates). This file plays them on
 * OpenGL, calling our OpenGL driver (Mesa's virgl driver in ripplegl.dll, the host GPU) directly
 * rather than through Windows' opengl32.dll: each Direct3D context is an OpenGL context drawing
 * into a framebuffer object the size of the render target. At the end of a scene (and before
 * the render target is locked or shown) the picture is read back into the render target's
 * video memory, where DirectDraw flips it to the screen.
 *
 * Textures are DirectDraw surfaces: they are uploaded to OpenGL textures when first used and
 * again when DirectDraw says their contents changed.
 *
 * rippledd.dll is loaded once for all processes (rippledd.def): data here is shared. What
 * belongs to one process (OpenGL, contexts, textures) lives in that process's heap and is only
 * touched from that process.
 *
 * Built with Visual C++ 6 against the Windows Me DDK's DirectDraw and Direct3D headers (not in
 * this repository), without the C run-time.
 */
#include <windows.h>
#include <GL/gl.h>
#include <initguid.h>
#include "ddraw.h"
#include "ddrawi.h"
#include "d3dhal.h"

#include "rdd.h"
#include "d3d8ddi.h"     /* the Direct3D 8 interface, shared with Windows 2000 (win2k/) */
#include "d3d8_ps.h"      /* pixel shaders as TGSI text (win2k/), shared with Windows 2000 */
#include "tgsi_arb.h"     /* that text as an ARB fragment program */
#include "tnl_arb.h"      /* fixed-function vertex processing as an ARB vertex program */
#include "vs_arb.h"       /* vertex shaders as ARB vertex programs */
#include "rippled3d.h"

/* ---- No C run-time: what the compiler expects of it ------------------------------------------ */
int _fltused = 0x9875;

#pragma function(memcmp)
int memcmp(const void *a, const void *b, size_t n)
{
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p < *q ? -1 : 1;
    return 0;
}

#pragma function(memset, memcpy)
void *memset(void *d, int v, size_t n)
{
    BYTE *p = (BYTE *)d;
    while (n--)
        *p++ = (BYTE)v;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    BYTE *p = (BYTE *)d;
    const BYTE *q = (const BYTE *)s;
    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    BYTE *p = (BYTE *)d;
    const BYTE *q = (const BYTE *)s;
    if (p <= q || p >= q + n)
        return memcpy(d, s, n);
    while (n--)                         /* overlapping, the destination after the source */
        p[n] = q[n];
    return d;
}

static void dbg(const char *s)
{
    while (*s) {
        char c = *s++;
        __asm {
            mov al, c
            out 0E9h, al
        }
    }
}

static void dbg_hex(DWORD v)
{
    int i;
    char s[9];
    for (i = 7; i >= 0; i--) {
        s[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    s[8] = 0;
    dbg(s);
}

/* A trace of textures, texture blits, render targets and surface handles on QEMU's debug
   port, when the environment variable RIPPLE_D3D_TRACE is set (read at context creation). */
static int trace_on;

/* With RIPPLE_D3D_PROFILE set (read at context creation): every 100 read-backs, the time per
   frame (from one read-back to the next) and the share of it spent reading the picture back
   from the GPU, on QEMU's debug port. */
static int profile_on;
static LARGE_INTEGER prof_freq, prof_last;
static DWORD prof_read, prof_frame;   /* timer ticks (Windows 98: 1.19 MHz) */
static DWORD prof_n;
static DWORD flip_reads, flip_read_ticks, flip_gpu, flip_count;
static LARGE_INTEGER flip_start;

static void profile_read_back(LARGE_INTEGER t0, LARGE_INTEGER t1)
{
    char line[160];

    if (!profile_on)
        return;
    flip_reads++;
    flip_read_ticks += t1.LowPart - t0.LowPart;
    if (!prof_freq.QuadPart)
        QueryPerformanceFrequency(&prof_freq);
    prof_read += t1.LowPart - t0.LowPart;
    if (prof_last.QuadPart)
        prof_frame += t1.LowPart - prof_last.LowPart;
    prof_last = t1;
    if (++prof_n < 100 || !prof_freq.QuadPart)
        return;
    wsprintf(line, "rd3d: profile: %lu us a frame, read-back %lu us\r\n",
             (DWORD)MulDiv((int)(prof_frame / (prof_n - 1)), 1000000, (int)prof_freq.LowPart),
             (DWORD)MulDiv((int)(prof_read / prof_n), 1000000, (int)prof_freq.LowPart));
    dbg(line);
    prof_read = prof_frame = 0;
    prof_n = 0;
    prof_last.QuadPart = 0;
}

/* Measure presents independently: a GPU flip normally has no read-back to sample. */
void d3d_profile_flip(int gpu)
{
    LARGE_INTEGER t;
    char line[160];
    if (!profile_on || !QueryPerformanceCounter(&t))
        return;
    if (!flip_start.QuadPart) {
        flip_start = t;
        flip_count = flip_gpu = flip_reads = flip_read_ticks = 0;
        return;
    }
    flip_gpu += gpu;
    if (++flip_count < 100 || !QueryPerformanceFrequency(&prof_freq) || !prof_freq.LowPart)
        return;
    wsprintf(line, "rd3d: 100 flips: %lu GPU, %lu reads, %lu us/flip, %lu us/flip reading\r\n",
             flip_gpu, flip_reads, (DWORD)MulDiv((int)((t.LowPart - flip_start.LowPart) / flip_count),
             1000000, (int)prof_freq.LowPart),
             (DWORD)MulDiv((int)(flip_read_ticks / flip_count), 1000000, (int)prof_freq.LowPart));
    dbg(line);
    flip_start = t;
    flip_count = flip_gpu = flip_reads = flip_read_ticks = 0;
}

static void tracef(const char *fmt, ...)
{
    char b[300];
    va_list ap;

    if (!trace_on)
        return;
    va_start(ap, fmt);
    wvsprintf(b, fmt, ap);
    va_end(ap);
    dbg(b);
}

/* For rippledd.c's callbacks (Lock, Flip): a trace line with two numbers. */
void d3d_trace(const char *what, DWORD a, DWORD b)
{
    tracef("rd3d: %s %08lx %08lx\r\n", what, a, b);
}

/* A sum of a surface's bytes (for the trace: did the contents change between uploads?). */
static DWORD sum_rows(const BYTE *p, LONG pitch, DWORD row_bytes, DWORD rows)
{
    DWORD s = 0, x, y;

    for (y = 0; y < rows; y++, p += pitch)
        for (x = 0; x < row_bytes; x += 4)
            s = (s << 1 | s >> 31) ^ *(const DWORD *)(p + x);
    return s;
}

static void *alloc(DWORD n)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
}

static void release(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

static RDD_SHARED *shared;

/* ---- OpenGL constants beyond 1.1 ------------------------------------------------------------- */
#define GL_BGRA                         0x80E1
#define GL_BGR                          0x80E0
#define GL_UNSIGNED_SHORT_5_6_5         0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4_REV   0x8365
#define GL_UNSIGNED_SHORT_1_5_5_5_REV   0x8366
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_TEXTURE_CUBE_MAP             0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X  0x8515
#define GL_TEXTURE_3D                   0x806F
#define GL_TEXTURE_WRAP_R               0x8072
#define GL_UNPACK_IMAGE_HEIGHT          0x806E
#define GL_CLAMP_TO_BORDER              0x812D
#define GL_MIRRORED_REPEAT              0x8370
#define GL_TEXTURE0                     0x84C0
#define GL_ARRAY_BUFFER_ARB             0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ARB     0x8893
#define GL_STATIC_DRAW_ARB              0x88E4
#define GL_DYNAMIC_DRAW_ARB             0x88E8
#define GL_COMBINE                      0x8570
#define GL_COMBINE_RGB                  0x8571
#define GL_COMBINE_ALPHA                0x8572
#define GL_RGB_SCALE                    0x8573
#define GL_ADD_SIGNED                   0x8574
#define GL_INTERPOLATE                  0x8575
#define GL_CONSTANT                     0x8576
#define GL_PRIMARY_COLOR                0x8577
#define GL_PREVIOUS                     0x8578
#define GL_SUBTRACT                     0x84E7
#define GL_DOT3_RGBA                    0x86AF
#define GL_SOURCE0_RGB                  0x8580
#define GL_SOURCE0_ALPHA                0x8588
#define GL_OPERAND0_RGB                 0x8590
#define GL_OPERAND0_ALPHA               0x8598
#define GL_ALPHA_SCALE                  0x0D1C
#define GL_COLOR_SUM                    0x8458
#define GL_SECONDARY_COLOR_ARRAY        0x845E
#define GL_FRAGMENT_PROGRAM_ARB         0x8804
#define GL_PROGRAM_FORMAT_ASCII_ARB     0x8875
#define GL_PROGRAM_ERROR_STRING_ARB     0x8874
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_FOG_COORDINATE_SOURCE        0x8450
#define GL_FOG_COORDINATE               0x8451
#define GL_FRAGMENT_DEPTH               0x8452
#define GL_FOG_COORDINATE_ARRAY         0x8457
#define GL_VERTEX_PROGRAM_ARB           0x8620
#define GL_NORMAL_ARRAY                 0x8075
#define GL_RG                           0x8227
#define GL_RG8_SNORM                    0x8F95
#define GL_RGBA8_SNORM                  0x8F97
#define GL_TEXTURE_SWIZZLE_B            0x8E44
#define GL_FIRST_VERTEX_CONVENTION      0x8E4D
#define GL_POINT_SPRITE                  0x8861
#define GL_CLIP_PLANE0                   0x3000
#define CLIP_PLANES 6                   /* user clip planes offered (D3DCAPS8.MaxUserClipPlanes) */
#define GL_COORD_REPLACE                 0x8862
#define GL_POINT_SPRITE_COORD_ORIGIN     0x8CA0
#define GL_LOWER_LEFT                    0x8CA1
#define GL_VERTEX_PROGRAM_POINT_SIZE     0x8642
#define GL_FRAMEBUFFER                  0x8D40
#define GL_RENDERBUFFER                 0x8D41
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_DEPTH_ATTACHMENT             0x8D00
#define GL_STENCIL_ATTACHMENT           0x8D20
#define GL_DEPTH24_STENCIL8             0x88F0
#define GL_RENDERBUFFER_WIDTH           0x8D42
#define GL_RENDERBUFFER_HEIGHT          0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8D44
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_INCR_WRAP                    0x8507
#define GL_DECR_WRAP                    0x8508
#define GL_TEXTURE_BASE_LEVEL           0x813C
#define GL_TEXTURE_MAX_LEVEL            0x813D
#define GL_TEXTURE_LOD_BIAS             0x8501
#define GL_RGBA8                        0x8058
#define GL_RGB8                         0x8051
#define GL_RGB5                         0x8050
#define GL_RGB5_A1                      0x8057
#define GL_RGBA4                        0x8056
#define GL_RGB565                       0x8D62

/* ---- OpenGL entry points, per process ---------------------------------------------------------- */
typedef void (APIENTRY *GLVOIDENUM)(GLenum);
typedef struct {
    /* from Mesa's DrvGetProcAddress: keep the order of gl_names[] */
    void (APIENTRY *Enable)(GLenum);
    void (APIENTRY *Disable)(GLenum);
    void (APIENTRY *Clear)(GLbitfield);
    void (APIENTRY *ClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
    void (APIENTRY *ClearDepth)(GLclampd);
    void (APIENTRY *ClearStencil)(GLint);
    void (APIENTRY *Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (APIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (APIENTRY *MatrixMode)(GLenum);
    void (APIENTRY *LoadIdentity)(void);
    void (APIENTRY *Ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    void (APIENTRY *DepthFunc)(GLenum);
    void (APIENTRY *DepthMask)(GLboolean);
    void (APIENTRY *BlendFunc)(GLenum, GLenum);
    void (APIENTRY *AlphaFunc)(GLenum, GLclampf);
    void (APIENTRY *CullFace)(GLenum);
    void (APIENTRY *FrontFace)(GLenum);
    void (APIENTRY *ShadeModel)(GLenum);
    void (APIENTRY *PolygonMode)(GLenum, GLenum);
    void (APIENTRY *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (APIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
    void (APIENTRY *DrawPixels)(GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
    void (APIENTRY *PixelStorei)(GLenum, GLint);
    void (APIENTRY *Finish)(void);
    void (APIENTRY *GenTextures)(GLsizei, GLuint *);
    void (APIENTRY *DeleteTextures)(GLsizei, const GLuint *);
    void (APIENTRY *BindTexture)(GLenum, GLuint);
    void (APIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
    void (APIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
    void (APIENTRY *TexParameteri)(GLenum, GLenum, GLint);
    void (APIENTRY *TexParameterfv)(GLenum, GLenum, const GLfloat *);
    void (APIENTRY *TexEnvi)(GLenum, GLenum, GLint);
    void (APIENTRY *TexEnvfv)(GLenum, GLenum, const GLfloat *);
    void (APIENTRY *EnableClientState)(GLenum);
    void (APIENTRY *DisableClientState)(GLenum);
    void (APIENTRY *VertexPointer)(GLint, GLenum, GLsizei, const GLvoid *);
    void (APIENTRY *ColorPointer)(GLint, GLenum, GLsizei, const GLvoid *);
    void (APIENTRY *TexCoordPointer)(GLint, GLenum, GLsizei, const GLvoid *);
    void (APIENTRY *DrawArrays)(GLenum, GLint, GLsizei);
    void (APIENTRY *DrawElements)(GLenum, GLsizei, GLenum, const GLvoid *);
    void (APIENTRY *Color4ub)(GLubyte, GLubyte, GLubyte, GLubyte);
    void (APIENTRY *Fogi)(GLenum, GLint);
    void (APIENTRY *Fogf)(GLenum, GLfloat);
    void (APIENTRY *Fogfv)(GLenum, const GLfloat *);
    const GLubyte *(APIENTRY *GetString)(GLenum);
    GLenum (APIENTRY *GetError)(void);
    void (APIENTRY *StencilFunc)(GLenum, GLint, GLuint);
    void (APIENTRY *StencilOp)(GLenum, GLenum, GLenum);
    void (APIENTRY *StencilMask)(GLuint);
    /* Mesa's own entry points in ripplegl.dll; Windows' OPENGL32 isn't used for these */
    DWORD (WINAPI *DrvCreateContext)(HDC);
    BOOL (WINAPI *DrvDeleteContext)(DWORD);
    BOOL (WINAPI *RippleMakeCurrent)(DWORD);
    PROC (WINAPI *DrvGetProcAddress)(LPCSTR);
    /* extensions */
    void (APIENTRY *GenFramebuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindFramebuffer)(GLenum, GLuint);
    void (APIENTRY *FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (APIENTRY *GenRenderbuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindRenderbuffer)(GLenum, GLuint);
    void (APIENTRY *RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    GLenum (APIENTRY *CheckFramebufferStatus)(GLenum);
    void (APIENTRY *DeleteFramebuffers)(GLsizei, const GLuint *);
    void (APIENTRY *DeleteRenderbuffers)(GLsizei, const GLuint *);
    void (APIENTRY *ActiveTexture)(GLenum);
    void (APIENTRY *ClientActiveTexture)(GLenum);
    void (APIENTRY *SecondaryColorPointer)(GLint, GLenum, GLsizei, const GLvoid *);
    void (APIENTRY *FogCoordPointer)(GLenum, GLsizei, const GLvoid *);
    void (APIENTRY *ProvokingVertex)(GLenum);
    void (APIENTRY *WindowPos2i)(GLint, GLint);
    void (APIENTRY *GetRenderbufferParameteriv)(GLenum, GLenum, GLint *);
    /* ARB_fragment_program, for Direct3D 8 pixel shaders (optional) */
    void (APIENTRY *GenProgramsARB)(GLsizei, GLuint *);
    void (APIENTRY *DeleteProgramsARB)(GLsizei, const GLuint *);
    void (APIENTRY *BindProgramARB)(GLenum, GLuint);
    void (APIENTRY *ProgramStringARB)(GLenum, GLenum, GLsizei, const GLvoid *);
    void (APIENTRY *ProgramEnvParameter4fvARB)(GLenum, GLuint, const GLfloat *);
    void (APIENTRY *ProgramLocalParameter4fvARB)(GLenum, GLuint, const GLfloat *);
    /* compressed textures (DXT), for Direct3D 8 and 9 (optional) */
    void (APIENTRY *CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                                          const GLvoid *);
    void (APIENTRY *CompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                                             GLsizei, const GLvoid *);
    void (APIENTRY *TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                const GLvoid *);  /* volume textures (optional) */
    /* hardware vertex processing (optional) */
    void (APIENTRY *NormalPointer)(GLenum, GLsizei, const GLvoid *);
    void (APIENTRY *DepthRange)(GLclampd, GLclampd);
    /* vertex shaders: generic attributes (optional) */
    void (APIENTRY *VertexAttribPointerARB)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid *);
    void (APIENTRY *EnableVertexAttribArrayARB)(GLuint);
    void (APIENTRY *DisableVertexAttribArrayARB)(GLuint);
    /* render-to-texture on the GPU (optional) */
    void (APIENTRY *CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
    /* Direct3D 8 buffers as buffer objects (optional, buffer_object) */
    void (APIENTRY *GenBuffersARB)(GLsizei, GLuint *);
    void (APIENTRY *DeleteBuffersARB)(GLsizei, const GLuint *);
    void (APIENTRY *BindBufferARB)(GLenum, GLuint);
    void (APIENTRY *BufferDataARB)(GLenum, int, const GLvoid *, GLenum);
    /* points larger than a pixel and point sprites (optional, point_state) */
    void (APIENTRY *PointSize)(GLfloat);
    void (APIENTRY *PointParameterfv)(GLenum, const GLfloat *);
    /* user clip planes (optional, clip_state) */
    void (APIENTRY *ClipPlane)(GLenum, const GLdouble *);
} GLF;

#define GL_CORE_COUNT 53            /* entries up to DrvGetProcAddress */
static const char *const gl_names[] = {
    "glEnable", "glDisable", "glClear", "glClearColor", "glClearDepth", "glClearStencil",
    "glScissor", "glViewport", "glMatrixMode", "glLoadIdentity", "glOrtho", "glDepthFunc",
    "glDepthMask", "glBlendFunc", "glAlphaFunc", "glCullFace", "glFrontFace", "glShadeModel",
    "glPolygonMode", "glColorMask", "glReadPixels", "glDrawPixels", "glPixelStorei", "glFinish",
    "glGenTextures", "glDeleteTextures", "glBindTexture", "glTexImage2D", "glTexSubImage2D",
    "glTexParameteri",
    "glTexParameterfv", "glTexEnvi", "glTexEnvfv", "glEnableClientState",
    "glDisableClientState", "glVertexPointer", "glColorPointer", "glTexCoordPointer",
    "glDrawArrays", "glDrawElements", "glColor4ub", "glFogi", "glFogf", "glFogfv", "glGetString",
    "glGetError", "glStencilFunc", "glStencilOp", "glStencilMask",
    "DrvCreateContext", "DrvDeleteContext", "RippleMakeCurrent", "DrvGetProcAddress",
    /* extensions */
    "glGenFramebuffersEXT", "glBindFramebufferEXT", "glFramebufferRenderbufferEXT",
    "glGenRenderbuffersEXT", "glBindRenderbufferEXT", "glRenderbufferStorageEXT",
    "glCheckFramebufferStatusEXT", "glDeleteFramebuffersEXT", "glDeleteRenderbuffersEXT",
    "glActiveTextureARB", "glClientActiveTextureARB", "glSecondaryColorPointerEXT",
    "glFogCoordPointerEXT", "glProvokingVertexEXT", "glWindowPos2iARB",
    "glGetRenderbufferParameterivEXT",
    "glGenProgramsARB", "glDeleteProgramsARB", "glBindProgramARB", "glProgramStringARB",
    "glProgramEnvParameter4fvARB", "glProgramLocalParameter4fvARB",
    "glCompressedTexImage2DARB", "glCompressedTexSubImage2DARB",
    "glTexImage3D",
    "glNormalPointer", "glDepthRange",
    "glVertexAttribPointerARB", "glEnableVertexAttribArrayARB", "glDisableVertexAttribArrayARB",
    "glCopyTexSubImage2D",
    "glGenBuffersARB", "glDeleteBuffersARB", "glBindBufferARB", "glBufferDataARB",
    "glPointSize", "glPointParameterfv",
    "glClipPlane",
};
#define GL_ALL_COUNT (sizeof gl_names / sizeof gl_names[0])
typedef char c_gl_names[sizeof(GLF) == GL_ALL_COUNT * sizeof(PROC) ? 1 : -1];

/* What each process needs once: OpenGL's entry points and our window class. */
typedef struct {
    HMODULE opengl, mesa;
    int core_ok, ext_ok;
    GLF gl;
    BOOL (WINAPI *present_color)(void); /* optional in ripplegl.dll */
    ATOM wndclass;
    DWORD d3d_version;                  /* the highest the runtime announced in this process (RbGdi2DxVersion) */
} RPROC;

/* The tables below are in the DLL's shared data, the same in every process; the records they
   point to are in one process's heap, so a record is only followed in the process that made
   it. Windows 98 gives a new process the id of one that has just ended, so the id alone can't
   tell them apart: each process also gets a number of its own (its session), kept in its
   environment. A record belongs to the current process only if both match. */
typedef struct { DWORD pid, session; } OWNER;

static LONG sessions;                   /* the last session number given out */

#define MAX_PROCESSES 16
static struct { OWNER o; RPROC *p; } processes[MAX_PROCESSES];

/* Direct3D's parser for commands a driver doesn't know: a function of the Direct3D runtime,
   which is loaded at the same address in every process. */
static PFND3DPARSEUNKNOWNCOMMAND parse_unknown;

static int process_alive(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    DWORD code = 0;
    int alive;

    if (!h)
        return 0;
    alive = !GetExitCodeProcess(h, &code) || code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

static DWORD hex8(const char *s)
{
    DWORD v = 0;
    int i;

    for (i = 0; i < 8; i++)
        v = v << 4 | (DWORD)(s[i] >= 'a' ? s[i] - 'a' + 10 : s[i] - '0');
    return v;
}

static void owner_forget(DWORD pid);

static DWORD next_session(void)
{
    LONG s;

    __asm {
        mov     eax, 1
        lock xadd sessions, eax
        inc     eax
        mov     s, eax
    }
    return (DWORD)s;
}

/* Process id -> session, kept from DllMain's process attach to its detach. me() reads it
   without calling kernel32: most callers hold Windows' 16-bit lock (every DirectDraw call
   does), and kernel32's own lock, which the environment functions take, is held by threads in
   DllMain that may in turn wait for the 16-bit lock: the system would stop. */
#define MAX_KNOWN 32
static struct { DWORD pid, session; } known[MAX_KNOWN];

/* DllMain, process attach (begin) or detach (end). A new process may have the id of one that
   has just ended: what that one left goes. */
void d3d_process(int begin)
{
    DWORD pid = GetCurrentProcessId();
    int i, free = -1;

    for (i = 0; i < MAX_KNOWN; i++) {
        if (known[i].pid == pid)
            known[i].pid = 0;
        if (!known[i].pid && free < 0)
            free = i;
    }
    if (!begin)
        return;
    owner_forget(pid);
    if (free >= 0) {
        known[free].session = next_session();
        known[free].pid = pid;
    }
}

/* The current process. */
static OWNER me(void)
{
    static const char name[] = "RIPPLE_D3D_SESSION";
    char buf[24];
    OWNER o;
    int i;

    o.pid = GetCurrentProcessId();
    for (i = 0; i < MAX_KNOWN; i++)
        if (known[i].pid == o.pid) {
            o.session = known[i].session;
            return o;
        }
    /* A process DllMain didn't see (or the table was full): its number in its environment. */
    if (GetEnvironmentVariable(name, buf, sizeof buf) == 17 && buf[8] == ':' &&
        hex8(buf) == o.pid) {
        o.session = hex8(buf + 9);
        return o;
    }
    /* First use in this process (a child process inherits its parent's variable, with the
       parent's id): a new number. Records left under this id belong to an ended process. */
    o.session = next_session();
    wsprintf(buf, "%08lx:%08lx", o.pid, o.session);
    SetEnvironmentVariable(name, buf);
    owner_forget(o.pid);
    return o;
}

static int mine(const OWNER *o, const OWNER *m)
{
    return o->pid == m->pid && o->session == m->session;
}

static RPROC *process(void)
{
    OWNER m = me();
    int i, free = -1;

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (mine(&processes[i].o, &m))
            return processes[i].p;
        if (!processes[i].o.pid && free < 0)
            free = i;
    }
    /* A slot of a process that has ended may be reused: its memory went with it. */
    for (i = 0; free < 0 && i < MAX_PROCESSES; i++)
        if (!process_alive(processes[i].o.pid))
            free = i;
    if (free < 0)
        return NULL;
    processes[free].p = (RPROC *)alloc(sizeof(RPROC));
    processes[free].o = m;
    if (!processes[free].p)
        processes[free].o.pid = 0;
    return processes[free].p;
}

static int gl_load_core(RPROC *p)
{
    PROC *slot = (PROC *)&p->gl;
    int i;

    if (p->core_ok)
        return 1;
    /* OPENGL32 stays loaded: GDI's pixel format calls look for it. The context and every
       OpenGL function come from Mesa itself, so that making the context current goes through
       no window, device context or OPENGL32 (RippleMakeCurrent, make_current). */
    if (!p->opengl)
        p->opengl = LoadLibrary("OPENGL32.DLL");
    if (!p->mesa)
        p->mesa = LoadLibrary("RIPPLEGL.DLL");
    if (!p->opengl || !p->mesa)
        return 0;
    p->gl.DrvGetProcAddress = (PROC (WINAPI *)(LPCSTR))GetProcAddress(p->mesa,
                                                                       "DrvGetProcAddress");
    for (i = 0; i < GL_CORE_COUNT; i++) {
        slot[i] = gl_names[i][0] == 'g' ? (p->gl.DrvGetProcAddress ?
                                           p->gl.DrvGetProcAddress(gl_names[i]) : NULL) :
                  GetProcAddress(p->mesa, gl_names[i]);
        if (!slot[i]) {
            dbg("rd3d: ripplegl lacks "); dbg(gl_names[i]); dbg("\r\n");
            return 0;
        }
    }
    p->present_color = (BOOL (WINAPI *)(void))GetProcAddress(p->mesa, "RipplePresentColor");
    p->core_ok = 1;
    return 1;
}

static int gl_load_ext(RPROC *p)
{
    PROC *slot = (PROC *)&p->gl;
    int i, ok = 1;

    if (p->ext_ok)
        return 1;
    for (i = GL_CORE_COUNT; i < (int)GL_ALL_COUNT; i++) {
        slot[i] = p->gl.DrvGetProcAddress(gl_names[i]);
        if (!slot[i]) {
            dbg("rd3d: OpenGL lacks "); dbg(gl_names[i]); dbg("\r\n");
            if (i < GL_CORE_COUNT + 11)     /* framebuffers and multitexture are needed */
                ok = 0;
        }
    }
    p->ext_ok = ok;
    return ok;
}

#define MAX_LEVELS 12                   /* mipmap levels of a 2048x2048 texture */
static int d8_on;                       /* the Direct3D 8 interface is offered (d3d8_enabled) */
static int tnl_on;                      /* with hardware vertex processing (D3D8 is "2") */
static int buf_on;                      /* and buffers the driver allocates (unless D3D8Buffers is "0") */

/* ---- Surface handles: DirectX 7 names surfaces in the command stream by handle --------------- */
typedef struct {
    OWNER o;
    LPDDRAWI_DIRECTDRAW_LCL lcl;
    LPDDRAWI_DDRAWSURFACE_LCL *surf;    /* by handle; process heap */
    DWORD n;
    /* Direct3D 8 vertex and index buffers (execute buffers), by handle: the runtime's surface
       records for them live only during CreateSurfaceEx, so their memory and size are kept here.
       A buffer the driver allocated (ours, d3d_buffer_new) may have a copy in an OpenGL buffer
       object of one context (buffer_object), as of the writes count in its header. */
    struct buf8 {
        FLATPTR mem;
        DWORD size;
        int ours, fixed;                /* allocated by the driver; written once (HINTSTATIC) */
        GLuint name;                    /* the buffer object, in context ctx */
        void *ctx;
        DWORD writes;                   /* the header's count when it was uploaded */
    } *buf;
    DWORD nbuf;
    struct trash { GLuint name; void *ctx; } *trash;    /* buffer objects to delete */
    DWORD ntrash, maxtrash;
    /* Direct3D 8 textures: copies of the runtime's records (struct shadow), by handle */
    struct shadow **shadow;
    DWORD nshadow;
    int dx8;                            /* a Direct3D 8 context was made on it */
} DDLOCAL;

/* The runtime's surface records for Direct3D 8 textures live only during CreateSurfaceEx too.
   A copy of each level's records, linked the way DirectDraw links them, stands in for them;
   the memory they name stays the runtime's. DirectDraw finds a surface's GBL_MORE through the
   pointer just before its GBL (GET_LPDDRAWSURFACE_GBL_MORE), so the copy keeps that layout. */
struct shadow {
    DDRAWI_DDRAWSURFACE_LCL l;
    DDRAWI_DDRAWSURFACE_MORE m;
    DDRAWI_DDRAWSURFACE_GBL_MORE gm;
    ATTACHLIST a;
    LPDDRAWI_DDRAWSURFACE_GBL_MORE gmp; /* just before g */
    DDRAWI_DDRAWSURFACE_GBL g;
    DWORD faces, levels;                /* in the first: 1 or 6 faces, each with this many levels */
    DWORD depth;                        /* in the first: a volume's slices at level 0 (else 1) */
};

#define MAX_DDLOCALS 32
static DDLOCAL ddlocals[MAX_DDLOCALS];

static DDLOCAL *ddlocal(LPDDRAWI_DIRECTDRAW_LCL lcl, int create)
{
    OWNER m = me();
    int i, free = -1;

    for (i = 0; i < MAX_DDLOCALS; i++) {
        if (ddlocals[i].lcl == lcl && mine(&ddlocals[i].o, &m))
            return &ddlocals[i];
        if (!ddlocals[i].lcl && free < 0)
            free = i;
    }
    for (i = 0; create && free < 0 && i < MAX_DDLOCALS; i++)
        if (!process_alive(ddlocals[i].o.pid))
            free = i;                   /* an ended process's: its memory went with it */
    if (!create || free < 0)
        return NULL;
    ddlocals[free].o = m;
    ddlocals[free].lcl = lcl;
    ddlocals[free].surf = NULL;
    ddlocals[free].n = 0;
    ddlocals[free].buf = NULL;
    ddlocals[free].nbuf = 0;
    ddlocals[free].trash = NULL;
    ddlocals[free].ntrash = ddlocals[free].maxtrash = 0;
    ddlocals[free].shadow = NULL;
    ddlocals[free].nshadow = 0;
    ddlocals[free].dx8 = 0;
    return &ddlocals[free];
}

static void handle_set(DDLOCAL *d, DWORD h, LPDDRAWI_DDRAWSURFACE_LCL s)
{
    if (h >= d->n) {
        DWORD n = h + 64 > d->n * 2 ? h + 64 : d->n * 2;     /* handles keep growing */
        LPDDRAWI_DDRAWSURFACE_LCL *a = (LPDDRAWI_DDRAWSURFACE_LCL *)alloc(n * sizeof(*a));
        if (!a)
            return;
        if (d->surf) {
            memcpy(a, d->surf, d->n * sizeof(*a));
            release(d->surf);
        }
        d->surf = a;
        d->n = n;
    }
    d->surf[h] = s;
}

/* ---- Direct3D 8 buffers the driver allocates ------------------------------------------------ */
/* With hardware vertex processing the runtime allocates its vertex and index buffers through the
   driver (D3DDEVCAPS_HWVERTEXBUFFER and rippledd.c's execute-buffer callbacks), so every Lock of
   one comes through the driver. Each is ordinary memory in a heap in the shared arena (DirectDraw
   may free it from another process), after a header that counts the writes: a draw uploads a
   buffer into its OpenGL buffer object only when it was written since (buffer_object), where
   vertices in the runtime's own memory went to the host again with every draw. */
#define HEAP_SHARED_9X 0x04000000UL     /* Windows 9x: a heap in the shared arena */
#define BUF_MAGIC 0x46554252UL          /* "RBUF" */
struct bufhead { DWORD magic, size, writes, pad; };
static HANDLE buf_heap;

int d3d_buffers_on(void)
{
    return buf_on;
}

void *d3d_buffer_new(DWORD size)
{
    struct bufhead *h;
    if (!buf_heap)
        buf_heap = HeapCreate(HEAP_SHARED_9X, 0x10000, 0);
    if (!buf_heap || size > 0x4000000UL)
        return NULL;
    h = (struct bufhead *)HeapAlloc(buf_heap, HEAP_ZERO_MEMORY, sizeof *h + size);
    if (!h)
        return NULL;
    h->magic = BUF_MAGIC;
    h->size = size;
    h->writes = 1;
    return h + 1;
}

static struct bufhead *buffer_head(FLATPTR mem)
{
    struct bufhead *h = (struct bufhead *)mem - 1;
    return mem && h->magic == BUF_MAGIC ? h : NULL;
}

void d3d_buffer_free(FLATPTR mem)
{
    struct bufhead *h = buffer_head(mem);
    if (h) {
        h->magic = 0;
        HeapFree(buf_heap, 0, h);
    }
}

/* The program (Lock) or the runtime (BUFFER_BLT) writes the buffer. */
void d3d_buffer_written(FLATPTR mem)
{
    struct bufhead *h = buffer_head(mem);
    if (h)
        h->writes++;
}

/* A buffer object to delete when its context is current again (buffer_object). */
static void buffer_trash(DDLOCAL *d, struct buf8 *b)
{
    if (!b->name)
        return;
    if (d->ntrash == d->maxtrash) {
        DWORD n = d->maxtrash ? d->maxtrash * 2 : 64;
        struct trash *a = (struct trash *)alloc(n * sizeof(*a));
        if (!a)
            return;                     /* the name is lost; the context frees it at its end */
        if (d->trash) {
            memcpy(a, d->trash, d->ntrash * sizeof(*a));
            release(d->trash);
        }
        d->trash = a;
        d->maxtrash = n;
    }
    d->trash[d->ntrash].name = b->name;
    d->trash[d->ntrash].ctx = b->ctx;
    d->ntrash++;
    b->name = 0;
    b->ctx = NULL;
}

/* A Direct3D 8 buffer's memory and size (mem 0: destroyed); ours if the driver allocated it,
   fixed if the program means to write it once. */
static void buffer_set(DDLOCAL *d, DWORD h, FLATPTR mem, DWORD size, int ours, int fixed)
{
    if (h < d->nbuf && d->buf[h].name) {
        if (d->buf[h].mem != mem)
            buffer_trash(d, &d->buf[h]);
        else                            /* The same handle at the same address: it may be a new buffer.
                                           The runtime gives a freed handle out again without a
                                           word, and the heap the freed block; with the same count of
                                           writes the old contents stayed on the GPU. Upload anew. */
            d->buf[h].writes = 0xFFFFFFFFUL;
    }
    if (h >= d->nbuf) {
        DWORD n = h + 64 > d->nbuf * 2 ? h + 64 : d->nbuf * 2;
        struct buf8 *a = (struct buf8 *)alloc(n * sizeof(*a));
        if (!a)
            return;
        if (d->buf) {
            memcpy(a, d->buf, d->nbuf * sizeof(*a));
            release(d->buf);
        }
        d->buf = a;
        d->nbuf = n;
    }
    d->buf[h].mem = mem;
    d->buf[h].size = size;
    d->buf[h].ours = ours && mem;      /* (CreateSurfaceEx checked its mark) */
    d->buf[h].fixed = fixed;
}

typedef char c_shadow_layout[FIELD_OFFSET(struct shadow, g) ==
                             FIELD_OFFSET(struct shadow, gmp) + sizeof(void *) ? 1 : -1];

/* The cube face of a surface (0 for +X .. 5 for -Z), or -1. */
static int cube_face(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    DWORD caps2 = s && s->lpSurfMore ? s->lpSurfMore->ddsCapsEx.dwCaps2 : 0;
    int k;
    for (k = 0; k < 6; k++)
        if (caps2 & (DDSCAPS2_CUBEMAP_POSITIVEX << k))
            return k;
    return -1;
}

/* A face's levels from its top surface down: each attached to the one above (a cube's top
   face also holds the other faces, so there only mipmap sublevels count). */
static DWORD face_chain(LPDDRAWI_DDRAWSURFACE_LCL top, int cube, LPDDRAWI_DDRAWSURFACE_LCL *lv)
{
    LPDDRAWI_DDRAWSURFACE_LCL m = top;
    DWORD n = 1;

    lv[0] = top;
    while (n < MAX_LEVELS && (m->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) {
        LPATTACHLIST a;
        LPDDRAWI_DDRAWSURFACE_LCL next = NULL;
        for (a = m->lpAttachList; a && !next; a = a->lpLink) {
            LPDDRAWI_DDRAWSURFACE_LCL x = a->lpAttached;
            if (!x || x == top || !x->lpGbl || !x->lpSurfMore || !(x->ddsCaps.dwCaps & DDSCAPS_MIPMAP))
                continue;
            if (cube && !(x->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL))
                continue;
            next = x;
        }
        if (!next)
            break;
        lv[n++] = m = next;
    }
    return n;
}

/* Copies of a texture's records: for each face (six for a cube map, in Direct3D's order +X,
   -X, +Y, -Y, +Z, -Z), its levels, linked through their attach lists; sh[face * levels +
   level], sh[0] saying how many faces and levels. NULL if it can't be made. */
static struct shadow *shadow_make(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    LPDDRAWI_DDRAWSURFACE_LCL top[6], lv[6][MAX_LEVELS];
    DWORD n, faces = 1, levels = MAX_LEVELS, f, i;
    struct shadow *sh;
    int cube = (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP) != 0;

    memset(top, 0, sizeof top);
    top[0] = s;
    if (cube) {
        LPATTACHLIST a;
        int k = cube_face(s);
        if (k < 0)
            return NULL;
        top[0] = NULL;
        top[k] = s;
        for (a = s->lpAttachList; a; a = a->lpLink) {
            LPDDRAWI_DDRAWSURFACE_LCL x = a->lpAttached;
            int fk = cube_face(x);
            if (fk >= 0 && !top[fk] && x->lpGbl &&
                !(x->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL))
                top[fk] = x;
        }
        faces = 6;
    }
    for (f = 0; f < faces; f++) {
        if (!top[f] || !top[f]->lpGbl || top[f]->lpGbl->wWidth != s->lpGbl->wWidth ||
            top[f]->lpGbl->wHeight != s->lpGbl->wHeight)
            return NULL;
        n = face_chain(top[f], cube, lv[f]);
        if (n < levels)
            levels = n;
    }
    sh = (struct shadow *)alloc(faces * levels * sizeof *sh);
    if (!sh)
        return NULL;
    for (f = 0; f < faces; f++)
        for (i = 0; i < levels; i++) {
            struct shadow *o = &sh[f * levels + i];
            LPDDRAWI_DDRAWSURFACE_LCL from = lv[f][i];
            LPDDRAWI_DDRAWSURFACE_GBL_MORE gm = GET_LPDDRAWSURFACE_GBL_MORE(from->lpGbl);
            o->l = *from;
            o->m = *from->lpSurfMore;
            o->g = *from->lpGbl;
            if (gm)
                o->gm = *gm;
            o->gm.dwContentsStamp = 0;  /* the runtime's stamp isn't kept: writes mark textures */
            o->gmp = &o->gm;
            o->l.lpGbl = &o->g;
            o->l.lpSurfMore = &o->m;
            o->l.lpAttachList = NULL;
            o->l.lpAttachListFrom = NULL;
            if (i + 1 < levels) {
                o->a.dwFlags = DDAL_IMPLICIT;
                o->a.lpLink = NULL;
                o->a.lpAttached = &sh[f * levels + i + 1].l;
                o->a.lpIAttached = NULL;
                o->l.lpAttachList = &o->a;
            }
        }
    sh[0].faces = faces;
    sh[0].levels = levels;
    sh[0].depth = 1;
    if (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_VOLUME)    /* slices one after another */
        sh[0].depth = s->lpSurfMore->ddsCapsEx.dwCaps4 ? s->lpSurfMore->ddsCapsEx.dwCaps4 : 1;
    if (cube)
        sh[0].m.ddsCapsEx.dwCaps2 |= DDSCAPS2_CUBEMAP;
    return sh;
}

/* A cube map's copy (only for surfaces of a Direct3D 8 session, which are copies). */
static int is_cube(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    return s->lpSurfMore && (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP) &&
           ((const struct shadow *)s)->faces == 6;
}

/* A volume texture's copy (likewise). */
static int is_volume(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    return s->lpSurfMore && (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_VOLUME) &&
           ((const struct shadow *)s)->faces == 1;
}

/* A Direct3D 8 texture's cube faces and mipmap levels below the top have surface handles of
   their own: a program renders into one by that handle (SETRENDERTARGET), and without an entry
   the render target stayed what it was (six cube faces all drawn into the first). set: enter
   the copies' records under those handles; else take out the entries that point into sh. */
static void sub_handles(DDLOCAL *d, DWORD top, struct shadow *sh, int set)
{
    DWORD i, n = sh ? sh[0].faces * sh[0].levels : 0;

    for (i = 1; i < n; i++) {
        DWORD h = sh[i].m.dwSurfaceHandle;
        if (!h || h == top)
            continue;
        if (set)
            handle_set(d, h, &sh[i].l);
        else if (h < d->n && d->surf[h] == &sh[i].l)
            d->surf[h] = NULL;
    }
}

static void shadow_set(DDLOCAL *d, DWORD h, struct shadow *sh)
{
    if (h >= d->nshadow) {
        DWORD n = h + 64 > d->nshadow * 2 ? h + 64 : d->nshadow * 2;
        struct shadow **a = (struct shadow **)alloc(n * sizeof(*a));
        if (!a) {
            release(sh);
            return;
        }
        if (d->shadow) {
            memcpy(a, d->shadow, d->nshadow * sizeof(*a));
            release(d->shadow);
        }
        d->shadow = a;
        d->nshadow = n;
    }
    sub_handles(d, h, d->shadow[h], 0);
    release(d->shadow[h]);
    d->shadow[h] = sh;
    sub_handles(d, h, sh, 1);
}

static void shadows_free(DDLOCAL *d)
{
    DWORD i;
    for (i = 0; d->shadow && i < d->nshadow; i++)
        release(d->shadow[i]);
    release(d->shadow);
    d->shadow = NULL;
    d->nshadow = 0;
}

static BYTE *buffer_get(LPDDRAWI_DIRECTDRAW_LCL lcl, DWORD h, DWORD *size)
{
    DDLOCAL *d = ddlocal(lcl, 0);
    if (!d || !h || h >= d->nbuf || !d->buf[h].mem)
        return NULL;
    *size = d->buf[h].size;
    return (BYTE *)d->buf[h].mem;
}

/* The surface of s's flipping chain (its attached surfaces, the ring back to root) that carries
   handle h, or NULL. Only surfaces DirectDraw links to s are looked at, never other entries of
   the table, which may name surfaces released since. */
static LPDDRAWI_DDRAWSURFACE_LCL chain_find(LPDDRAWI_DDRAWSURFACE_LCL root,
                                            LPDDRAWI_DDRAWSURFACE_LCL s, DWORD h, int depth)
{
    LPATTACHLIST a;
    LPDDRAWI_DDRAWSURFACE_LCL r;

    if (!s || depth > 8)
        return NULL;
    if (s != root && s->lpSurfMore && s->lpSurfMore->dwSurfaceHandle == h)
        return s;
    a = s->lpAttachList;
    if (!a)
        return NULL;
    if (a->lpAttached && a->lpAttached != root &&
        (r = chain_find(root, a->lpAttached, h, depth + 1)) != NULL)
        return r;
    if (a->lpLink && a->lpLink->lpAttached && a->lpLink->lpAttached != root)
        return chain_find(root, a->lpLink->lpAttached, h, depth + 1);
    return NULL;
}

/* The surface that carries handle h now. A flip moves the handles of a flipping chain's
   surfaces along with their memory (the surface record the program calls the back buffer keeps
   its role, the handle follows the picture), so the record CreateSurfaceEx gave for h may carry
   another handle by now: then the one in its chain that carries h is taken, and both records go
   back into the table under the handles they carry now. Without that, Direct3D drew into the
   buffer on the screen and every other flip showed the one that never got a picture. */
static LPDDRAWI_DDRAWSURFACE_LCL handle_get(LPDDRAWI_DIRECTDRAW_LCL lcl, DWORD h)
{
    DDLOCAL *d = ddlocal(lcl, 0);
    LPDDRAWI_DDRAWSURFACE_LCL s, o;
    DWORD h2;

    if (!d || !h || h >= d->n || !(s = d->surf[h]))
        return NULL;
    if (!s->lpSurfMore || s->lpSurfMore->dwSurfaceHandle == h)
        return s;
    o = chain_find(s, s, h, 0);
    if (!o)
        return s;
    d->surf[h] = o;
    h2 = s->lpSurfMore->dwSurfaceHandle;
    if (h2 && h2 < d->n)
        d->surf[h2] = s;
    return o;
}

/* ---- Pixel formats ----------------------------------------------------------------------------- */
typedef struct {
    DWORD bytes;                        /* a texel's; 0 for block-compressed formats */
    GLenum format, type, internal;
    DWORD block;                        /* DXT: a 4x4 block's bytes (8 or 16), else 0 */
    int bump;                           /* V8U8: two channels, blue reads as 1 */
} PIXFMT;

static void display_format(DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = shared->bpp;
    if (shared->bpp == 16) {
        pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F;
    } else {
        pf->dwRBitMask = 0xFF0000; pf->dwGBitMask = 0xFF00; pf->dwBBitMask = 0xFF;
    }
}

static void surface_format(LPDDRAWI_DDRAWSURFACE_LCL s, DDPIXELFORMAT *pf)
{
    if (s->dwFlags & DDRAWISURF_HASPIXELFORMAT)
        *pf = s->lpGbl->ddpfSurface;
    else
        display_format(pf);
}

/* The OpenGL format of an RGB surface; 0 bytes if there is none. */
static PIXFMT gl_format(const DDPIXELFORMAT *pf)
{
    PIXFMT f;
    int alpha = (pf->dwFlags & DDPF_ALPHAPIXELS) != 0;

    memset(&f, 0, sizeof f);
    if ((pf->dwFlags & DDPF_FOURCC) && d8_on) {    /* DXT, offered with Direct3D 8 */
        switch (pf->dwFourCC) {
        case 0x31545844UL: f.block = 8; f.internal = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
        case 0x32545844UL:              /* DXT2 and DXT4 are premultiplied DXT3 and DXT5 */
        case 0x33545844UL: f.block = 16; f.internal = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
        case 0x34545844UL:
        case 0x35545844UL: f.block = 16; f.internal = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
        /* signed formats the runtime also names by their D3DFORMAT (it does for mipmapped
           Q8W8V8U8 textures, which DirectDraw then can't size: CreateSurface does) */
        case 60: f.bytes = 2; f.format = GL_RG; f.type = GL_BYTE; f.internal = GL_RG8_SNORM; f.bump = 1; break;
        case 63: f.bytes = 4; f.format = GL_RGBA; f.type = GL_BYTE; f.internal = GL_RGBA8_SNORM; break;
        /* and, should it name these the same way: A8, L8, A8L8 (below) */
        case 28: f.bytes = 1; f.format = GL_ALPHA; f.type = GL_UNSIGNED_BYTE; f.internal = GL_ALPHA8; break;
        case 50: f.bytes = 1; f.format = GL_LUMINANCE; f.type = GL_UNSIGNED_BYTE; f.internal = GL_LUMINANCE8; break;
        case 51: f.bytes = 2; f.format = GL_LUMINANCE_ALPHA; f.type = GL_UNSIGNED_BYTE;
                 f.internal = GL_LUMINANCE8_ALPHA8; break;
        }
        return f;
    }
    if ((pf->dwFlags & DDPF_LUMINANCE) && d8_on) {
        /* Direct3D 8 and 9's L8 and A8L8 (lightmaps, fonts, detail textures): Direct3D samples
           them as (L, L, L, 1) and (L, L, L, A), which is what OpenGL's luminance formats give.
           A8L8 has the luminance in the low byte, so in memory it is L then A, OpenGL's order. */
        if (pf->dwLuminanceBitCount == 8 && !alpha && pf->dwLuminanceBitMask == 0xFF) {
            f.bytes = 1; f.format = GL_LUMINANCE; f.type = GL_UNSIGNED_BYTE; f.internal = GL_LUMINANCE8;
        } else if (pf->dwLuminanceBitCount == 16 && alpha && pf->dwLuminanceBitMask == 0xFF &&
                   pf->dwLuminanceAlphaBitMask == 0xFF00) {
            f.bytes = 2; f.format = GL_LUMINANCE_ALPHA; f.type = GL_UNSIGNED_BYTE;
            f.internal = GL_LUMINANCE8_ALPHA8;
        }
        return f;
    }
    if ((pf->dwFlags & DDPF_ALPHA) && d8_on) {
        /* A8: (0, 0, 0, A) in both */
        if (pf->dwAlphaBitDepth == 8) {
            f.bytes = 1; f.format = GL_ALPHA; f.type = GL_UNSIGNED_BYTE; f.internal = GL_ALPHA8;
        }
        return f;
    }
    if ((pf->dwFlags & DDPF_BUMPDUDV) && d8_on) {
        /* Direct3D 8 and 9's bump maps, signed, sampled as -1..1: V8U8 (du, dv) and Q8W8V8U8
           (signed normal maps, as the runtime describes them: 32 bits with du and dv's masks) */
        if (!(pf->dwFlags & DDPF_BUMPLUMINANCE) && pf->dwBumpDuBitMask == 0xFF &&
            pf->dwBumpDvBitMask == 0xFF00) {
            if (pf->dwBumpBitCount == 16) {
                f.bytes = 2; f.format = GL_RG; f.type = GL_BYTE; f.internal = GL_RG8_SNORM; f.bump = 1;
            } else if (pf->dwBumpBitCount == 32) {
                f.bytes = 4; f.format = GL_RGBA; f.type = GL_BYTE; f.internal = GL_RGBA8_SNORM;
            }
        }
        return f;
    }
    if (!(pf->dwFlags & DDPF_RGB))
        return f;
    switch (pf->dwRGBBitCount) {
    case 32:
        if (pf->dwRBitMask == 0xFF0000) {
            f.bytes = 4; f.format = GL_BGRA; f.type = GL_UNSIGNED_BYTE;
            f.internal = alpha ? GL_RGBA8 : GL_RGB8;
        }
        break;
    case 24:
        if (pf->dwRBitMask == 0xFF0000) {
            f.bytes = 3; f.format = GL_BGR; f.type = GL_UNSIGNED_BYTE; f.internal = GL_RGB8;
        }
        break;
    case 16:
        if (pf->dwRBitMask == 0xF800) {
            f.bytes = 2; f.format = GL_RGB; f.type = GL_UNSIGNED_SHORT_5_6_5; f.internal = GL_RGB8;
        } else if (pf->dwRBitMask == 0x7C00) {
            f.bytes = 2; f.format = GL_BGRA; f.type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
            f.internal = alpha ? GL_RGB5_A1 : GL_RGB5;
        } else if (pf->dwRBitMask == 0x0F00) {
            f.bytes = 2; f.format = GL_BGRA; f.type = GL_UNSIGNED_SHORT_4_4_4_4_REV;
            f.internal = GL_RGBA4;
        }
        break;
    }
    return f;
}

/* A DXT surface's bytes: whole 4x4 blocks, row after row. */
static DWORD dxt_bytes(const PIXFMT *f, DWORD w, DWORD h)
{
    return ((w + 3) / 4) * ((h + 3) / 4) * f->block;
}

DWORD d3d_dxt_block(const DDPIXELFORMAT *pf)
{
    return gl_format(pf).block;
}

DWORD d3d_fourcc_bytes(const DDPIXELFORMAT *pf)
{
    return (pf->dwFlags & DDPF_FOURCC) ? gl_format(pf).bytes : 0;
}

int d3d_surface_format_ok(LPDDSURFACEDESC desc)
{
    DDPIXELFORMAT *pf = &desc->ddpfPixelFormat;

    if (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER)
        return (pf->dwFlags & DDPF_ZBUFFER) &&
               (pf->dwZBufferBitDepth == 16 || pf->dwZBufferBitDepth == 32);
    if (desc->ddsCaps.dwCaps & DDSCAPS_TEXTURE) {
        PIXFMT f = gl_format(pf);
        return f.bytes != 0 || f.block != 0;
    }
    return 0;
}

/* ---- Contexts ----------------------------------------------------------------------------------- */
#define MAX_PS 64                       /* Direct3D 8 pixel shaders a context keeps */
#define PS_VARIANTS 4                   /* programs a pixel shader keeps (fog, cube map stages) */
#define STAGES 8                        /* texture stages kept: pixel shaders sample s0..s7 */
#define FF_STAGES 2                     /* fixed-function stages offered to Direct3D 7 */
#define FF8_STAGES 4                    /* and drawn for Direct3D 8 and 9, before FOG_UNIT */
#define FOG_UNIT 4                      /* the OpenGL texture unit that does fixed-function fog */
#define MAX_VS 64                       /* Direct3D 8 and 9 vertex shaders a context keeps */
#define VS_VARIANTS 4                   /* programs a vertex shader keeps (fog) */
#define MAX_LIGHTS 128                  /* lights kept, by slot (light_slot: Direct3D's indices may be
                                           any number) */

/* A vertex shader: its declaration and code as the runtime gave them, the translator's layout
   of its inputs, and its programs by fog key (vs_arb.c). */
struct vs8 {
    DWORD handle;
    DWORD *decl, ndecl, *code, ncode;   /* words; process heap */
    int failed;                         /* couldn't be translated: its draws are skipped */
    VS_ARB info;
    DWORD nvar;
    struct { DWORD key; GLuint prog; } var[VS_VARIANTS];
};
#define VP_CACHE 16                     /* vertex programs a context keeps */
#define TNL_DIRTY_XF    1               /* world, view, projection, viewport */
#define TNL_DIRTY_LIGHT 2               /* lights, material, ambient */
#define TNL_DIRTY_FOG   4
#define TNL_DIRTY_TEX   8               /* texture matrices */
#define TNL_DIRTY_ALL   15

typedef struct {
    GLuint name;
    FLATPTR mem;                        /* what was uploaded: memory, size, format */
    DWORD w, h, fmt;
    DWORD levels;                       /* mipmap levels uploaded (1: the top level only) */
    FLATPTR lmem[MAX_LEVELS];           /* each level's memory (lmem[0] is mem) */
    FLATPTR fmem[6];                    /* a cube map: each face's top level (fmem[0] is mem), else 0 */
    DWORD stamp;                        /* DirectDraw's contents stamps then (0: none kept) */
    DWORD gpu_gen;                      /* the render-target picture copied in (texture_from_gpu) */
    DWORD scene;                        /* the scene it was uploaded in */
    int dirty;                          /* written since (Lock, texture blit) */
} TEXTURE;

#define TARGETS  16                     /* colour buffers a context keeps (gpu_buffer): a flip chain
                                           and the textures a frame renders into (3DMark 2001's
                                           Dragothic: ten), so their pictures can stay on the GPU */
#define ZBUFFERS 4                      /* depth buffers a context keeps */

/* A surface's picture on the GPU: a render target's colour or a z-buffer's depth, kept by the
   surface's memory, so that switching between targets (every flip of a chain does) needs no
   copy as long as the GPU's picture is as new as the memory. */
typedef struct {
    FLATPTR mem;                        /* the surface's memory (0: not used yet) */
    DWORD w, h;                         /* the size of rb's storage */
    DWORD used;                         /* when last attached (RCTX.clock) */
    GLuint rb;                          /* colour, or depth and stencil */
    GLenum format;                      /* rb's: colour_format, or depth and stencil */
    DWORD serial;                       /* display mode in which this page was attached */
    int deferred;                       /* exclusive flip-chain page newer on the GPU */
    int tex;                            /* deferred holds a texture's picture (finish_target): its
                                           format, pitch and size below, not the screen's */
    DDPIXELFORMAT pf;
    LONG pitch;
    DWORD tw, th;
    DWORD gen;                          /* texture pictures deferred so far */
    int written;                        /* colour: the memory is newer than rb's picture (the
                                           program wrote it, or rb is new): it goes to the GPU
                                           before the next draw (rt_upload) */
} GPUBUF;

typedef struct {
    RPROC *p;
    HWND wnd;
    HDC dc;
    DWORD rc;                           /* Mesa's handle (DHGLRC) */
    LPDDRAWI_DIRECTDRAW_LCL ddl;
    LPDDRAWI_DDRAWSURFACE_LCL rt, z;
    GLuint fbo, white, black, fog_tex;
    DWORD fb_w, fb_h;
    FLATPTR fb_z;                       /* the z-buffer whose depth is attached (0: none) */
    GPUBUF cb[TARGETS];                 /* the picture of each render target */
    GPUBUF zb[ZBUFFERS];                /* the depth of each z-buffer */
    GPUBUF *cur;                        /* the attached colour buffer (NULL: none yet) */
    GLuint read_fbo;                    /* read a previously displayed page without disturbing fbo */
    int scanout_enabled, chain_active;
    LPDDRAWI_DDRAWSURFACE_LCL front, back; /* members seen by the most recent GPU Flip */
    DWORD clock;
    DWORD vp_x, vp_y, vp_w, vp_h;       /* the viewport (0 wide: none given yet) */
    int dirty;                          /* drawn since the last read-back */
    DWORD rs[256];                      /* render states */
    /* User clip planes (Direct3D 8 and 9 with hardware vertex processing, clip_state): as the
       program set them, the ones enabled in OpenGL, and the equations loaded there. */
    float clip_plane[CLIP_PLANES][4], clip_eq[CLIP_PLANES][4];
    DWORD clip_gl;
    DWORD tss[STAGES][32];              /* texture stage states */
    int stages_dirty;
    DWORD scene;                        /* scenes ended so far */
    DWORD bound[STAGES];                /* surface handle of each stage's texture, 0 if none */
    int keyed;                          /* a bound texture is drawn with its colour key */
    BYTE ff_kind[STAGES];               /* each unit's enabled cube (1) or 3D (2) target (ff_target) */
    TEXTURE *tex;                       /* by surface handle */
    DWORD ntex;
    DWORD ntextures;                    /* OpenGL textures made (see TEXTURE_BUDGET) */
    float *pos;                         /* positions, 4 floats a vertex, then fog (convert) */
    DWORD npos;
    WORD *ibuf;                         /* indices of one command, rebased; grows as needed */
    DWORD nibuf;
    /* Direct3D 8: the vertex format (SETVERTEXSHADER with an FVF code), stream zero (a buffer
       surface's handle, or the command buffer's own vertices) and the index buffer. */
    DWORD fvf8, stream_handle, stream_stride, index_handle, index_stride;
    DWORD sx_handle[VS_ARB_STREAMS], sx_stride[VS_ARB_STREAMS];    /* streams 1 and up: their vertex
                                           buffers (a vertex shader's declaration reads them) */
    int stream_user;
    int dx8;                            /* made by the Direct3D 8 or 9 runtime */
    DWORD skipped8;                     /* Direct3D 8 draws not drawn (see stream_draw) */
    GLuint array_bound, element_bound;  /* buffer objects bound for vertices and indices (bind_buffer) */
    /* Direct3D 8 pixel shaders: ARB fragment programs by shader handle, the one set (0: none,
       the fixed-function stages), constants c0..c7 */
    struct ps8 {
        DWORD handle;
        DWORD *code, bytes;             /* its tokens, for more variants; process heap */
        int failed;                     /* couldn't be translated: its draws are skipped */
        int version2;                   /* shader model 2: it declares its samplers' kinds */
        int bump;                       /* version 1 with texbem: the stages' bump matrices */
        DWORD fog_base;                 /* the fog constants' first program.env */
        DWORD nvar;                     /* programs made: by fog, and the cube map stages of a
                                           version 1 shader (the translator's fog argument) */
        struct { DWORD key; GLuint prog; } var[PS_VARIANTS];
    } ps[MAX_PS];
    int fp_fog;                         /* the bound program blends fog: load its constants */
    DWORD ps_bound;
    GLuint fp_prog;                     /* bound to GL_FRAGMENT_PROGRAM_ARB */
    int fp_on;                          /* GL_FRAGMENT_PROGRAM_ARB enabled */
    float psc[RB_PS2_CONSTANTS][4];     /* c0..c7 for version 1, c0..c31 for 2.0 */
    int psc_dirty;
    /* Hardware vertex processing (Direct3D 8 and 9, tnl_on): the transforms, lights and material
       the runtime sets, and the ARB vertex programs made from them (tnl_arb.c) */
    float xf_world[16], xf_view[16], xf_proj[16], xf_tex[TNL_STAGES][16];
    D3DLIGHT7 light[MAX_LIGHTS];
    BYTE light_on[MAX_LIGHTS];
    DWORD light_index[MAX_LIGHTS];      /* Direct3D's index of each slot in use */
    BYTE light_used[MAX_LIGHTS];
    int lights_full;                    /* said once that a light found no slot */
    BYTE light_place[TNL_LIGHTS];       /* the light in each of the program's places */
    DWORD lights_loaded;                /* how many places those fill */
    D3DMATERIAL7 material;
    float zmin, zmax;                   /* the viewport's depth range */
    DWORD tnl_dirty;                    /* TNL_DIRTY_*: program.env to load again */
    int vp_on;                          /* a vertex program draws: OpenGL's viewport is Direct3D's */
    int sprites_on, psize_on;           /* point sprites; a program's point size (point_state) */
    int points_now;                     /* the draw is of points (tnl_key: their scaling) */
    GLuint vp_bound;
    struct { TNL_KEY key; GLuint prog; DWORD used; } vp[VP_CACHE];
    DWORD vp_clock;
    /* vertex shaders on the GPU (vs_*, with tnl_on): their records, c0..c255; program.env is
       shared with the fixed-function programs, so whose it holds is kept (ENV_*) */
    struct vs8 *vs[MAX_VS];
    float vsc[VS_ARB_CONSTANTS][4];
    int vsc_dirty;
    DWORD vsc_lo, vsc_hi;               /* the constants the program set since the last load */
    DWORD vs_limit;                     /* how many it has: 96 (Direct3D 8) or 256 (9) */
    int env_owner;
    DWORD vs_loaded;                    /* the shader whose def constants program.env holds */
    DWORD attribs_on;                   /* generic attribute arrays enabled (bits) */
} RCTX;
#define ENV_TNL 1
#define ENV_VS  2

static long ps_program(RCTX *c);
static void attribs_off(RCTX *c);
static void ps_apply(RCTX *c, long prog);
static void skip8(RCTX *c, const char *why);

#define MAX_CONTEXTS 64
/* Direct3D's context handle is the slot number plus one. */
static struct { OWNER o; RCTX *c; } contexts[MAX_CONTEXTS];
static int scanout_requested;           /* a process has opted in since the DLL loaded */

static RCTX *context_of(ULONG_PTR handle)
{
    DWORD i = (DWORD)handle - 1;
    OWNER m = me();

    if (i >= MAX_CONTEXTS || !mine(&contexts[i].o, &m))
        return NULL;
    return contexts[i].c;
}

/* Records under the id of an ended process (a new process has just got the same id): forgotten,
   not freed, as their memory went with that process. */
static void owner_forget(DWORD pid)
{
    int i;

    for (i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].o.pid == pid) {
            processes[i].o.pid = 0;
            processes[i].p = NULL;
        }
    for (i = 0; i < MAX_CONTEXTS; i++)
        if (contexts[i].o.pid == pid) {
            contexts[i].o.pid = 0;
            contexts[i].c = NULL;
        }
    for (i = 0; i < MAX_DDLOCALS; i++)
        if (ddlocals[i].o.pid == pid)
            memset(&ddlocals[i], 0, sizeof ddlocals[i]);
}

static GLF *gl_of(RCTX *c)
{
    return &c->p->gl;
}

/* The context is current in the calling thread for the length of one call into the driver:
   every call makes it current here and lets go of it at its end (let_go). An OpenGL context is
   current in one thread at a time, and programs call Direct3D from more than one: Re-Volt makes
   its device and sends its first commands in one thread and draws in another, and a context
   left current in the first kept every draw of the second from the GPU. DirectDraw lets one
   thread at a time into the driver.
   Mesa makes it current with no drawable (the drawing goes to framebuffer objects), through
   its own entry point: wglMakeCurrent checks the window's pixel format through GDI and KERNEL32,
   which take locks of the whole system, and DirectDraw holds Windows' 16-bit lock during these
   calls. Tomb Raider: The Last Revelation's drawing thread hung in it after a few hundred
   frames, while a thread of another program waited for the 16-bit lock. */
static int make_current(RCTX *c)
{
    static DWORD logged;

    if (gl_of(c)->RippleMakeCurrent(c->rc))
        return 1;
    if (logged++ < 8) {
        dbg("rd3d: context "); dbg_hex(c->rc); dbg(" not made current in thread ");
        dbg_hex(GetCurrentThreadId()); dbg("\r\n");
    }
    return 0;
}

/* The end of a call that used the context (make_current). */
static void let_go(RCTX *c)
{
    gl_of(c)->RippleMakeCurrent(0);
}

static LRESULT CALLBACK hidden_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProc(w, m, wp, lp);
}

/* The memory at mem was written (a write Lock, a read-back): the textures made from it, in
   every context of this process, are uploaded again before their next use. */
static void textures_written(FLATPTR mem)
{
    OWNER m = me();
    DWORD i, j;

    for (i = 0; i < MAX_CONTEXTS; i++) {
        RCTX *c = mine(&contexts[i].o, &m) ? contexts[i].c : NULL;
        if (!c)
            continue;
        for (j = 0; j < c->ntex; j++) {
            TEXTURE *t = &c->tex[j];
            DWORD l;
            if (!t->name)
                continue;
            for (l = 0; l < t->levels && t->lmem[l] != mem; l++)
                ;
            if (t->mem == mem || l < t->levels || t->fmem[1] == mem || t->fmem[2] == mem ||
                t->fmem[3] == mem || t->fmem[4] == mem || t->fmem[5] == mem) {
                t->dirty = 1;
                c->stages_dirty = 1;
            }
        }
    }
}

/* The memory mem now holds a newer picture than the colour buffers this process's contexts
   keep of it (a read-back, or the program wrote it): they load it before their next draw into
   it. keep: the buffer that holds that picture itself (NULL: none). */
static void targets_written(FLATPTR mem, const GPUBUF *keep)
{
    OWNER m = me();
    DWORD i, j;

    for (i = 0; i < MAX_CONTEXTS; i++) {
        RCTX *c = mine(&contexts[i].o, &m) ? contexts[i].c : NULL;
        if (!c)
            continue;
        for (j = 0; j < TARGETS; j++)
            if (c->cb[j].mem == mem && &c->cb[j] != keep) {
                c->cb[j].written = 1;
                c->cb[j].deferred = 0;
            }
    }
}

/* The OpenGL picture of the render target goes into the memory it was drawn for: the attached
   colour buffer's (a flip gives the target surface other memory, the picture stays with its
   memory). The pitch and format are the target's, the same for every surface of a chain. */
static int read_pixels(RCTX *c, GPUBUF *b)
{
    GLF *gl = gl_of(c);
    DDPIXELFORMAT pf;
    PIXFMT f;
    DWORD w = b->w, h = b->h, pitch;
    int texture = 0;

    if ((!b->deferred && (b != c->cur || !c->dirty)) || b->written || !b->mem)
        return 1;
    /* The old chain's memory may already belong to another mode. */
    if (b->serial != shared->mode_serial) {
        b->written = 1;
        b->deferred = 0;
        if (b == c->cur)
            c->dirty = 0;
        return 1;
    }
    if (b->deferred && b->tex) {        /* a texture's picture kept on the GPU (finish_target) */
        pf = b->pf;
        pitch = (DWORD)b->pitch;
        if (w > b->tw) w = b->tw;
        if (h > b->th) h = b->th;
        texture = 1;
    } else if (b->deferred) {
        display_format(&pf);
        pitch = shared->pitch;
    } else {
        LPDDRAWI_DDRAWSURFACE_GBL g;
        if (!c->rt)
            return 1;
        g = c->rt->lpGbl;
        surface_format(c->rt, &pf);
        pitch = g->lPitch;
        if (w > g->wWidth) w = g->wWidth;
        if (h > g->wHeight) h = g->wHeight;
        texture = (c->rt->ddsCaps.dwCaps & DDSCAPS_TEXTURE) != 0;
    }
    f = gl_format(&pf);
    if (!f.bytes || !make_current(c))
        return 0;
    if (b != c->cur) {
        if (!c->read_fbo)
            gl->GenFramebuffers(1, &c->read_fbo);
        if (!c->read_fbo)
            return 0;
        gl->BindFramebuffer(GL_FRAMEBUFFER, c->read_fbo);
        gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, b->rb);
        if (gl->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            gl->BindFramebuffer(GL_FRAMEBUFFER, c->fbo);
            return 0;
        }
    }
    tracef("rd3d: read back into %08lx (scene %lu)\r\n", b->mem, c->scene);
    gl->PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->PixelStorei(GL_PACK_ROW_LENGTH, pitch / f.bytes);
    {
        LARGE_INTEGER t0, t1;
        if (profile_on)
            QueryPerformanceCounter(&t0);
        gl->ReadPixels(0, 0, w, h, f.format, f.type, (GLvoid *)b->mem);
        if (profile_on) {
            QueryPerformanceCounter(&t1);
            profile_read_back(t0, t1);
        }
    }
    gl->PixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (b != c->cur)
        gl->BindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    else
        c->dirty = 0;
    b->deferred = 0;
    b->tex = 0;
    targets_written(b->mem, b);
    if (texture)
        textures_written(b->mem);
    return 1;
}

static void read_back(RCTX *c)
{
    if (c->cur)
        read_pixels(c, c->cur);
}

static int screen_page(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    LPDDRAWI_DDRAWSURFACE_GBL g = s->lpGbl;
    DWORD caps = s->ddsCaps.dwCaps;
    DDPIXELFORMAT pf, screen;

    if (!(caps & DDSCAPS_FLIP) || !(caps & DDSCAPS_VIDEOMEMORY) ||
        (caps & (DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_SYSTEMMEMORY)) ||
        (shared->bpp != 16 && shared->bpp != 32) || !shared->pitch ||
        g->wWidth != shared->width || g->wHeight != shared->height ||
        g->lPitch != (LONG)shared->pitch || !rdd_shown_page(g->fpVidMem))
        return 0;
    surface_format(s, &pf);
    display_format(&screen);
    return pf.dwRGBBitCount == screen.dwRGBBitCount && pf.dwRBitMask == screen.dwRBitMask &&
           pf.dwGBitMask == screen.dwGBitMask && pf.dwBBitMask == screen.dwBBitMask &&
           !(pf.dwFlags & DDPF_ALPHAPIXELS);
}

/* Only defer a page after the first Flip on its chain actually reached scanout. The render
   target LCL follows the back buffer across flips, while the picture stays with its page. */
static void finish_target(RCTX *c)
{
    if (!c->dirty)
        return;                         /* a previously deferred page stays on the GPU */
    if (c->scanout_enabled && c->chain_active && c->rt && c->cur && !c->cur->written &&
        c->cur->serial == shared->mode_serial && screen_page(c->rt)) {
        c->cur->deferred = 1;
        c->dirty = 0;
    } else if (c->rt && c->cur && (c->rt->ddsCaps.dwCaps & DDSCAPS_TEXTURE) && !c->cur->written &&
               c->cur->mem == c->rt->lpGbl->fpVidMem && gl_of(c)->CopyTexSubImage2D &&
               cube_face(c->rt) < 0 &&
               !(c->rt->lpSurfMore && (c->rt->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL))) {
        /* A texture rendered into: its picture stays on the GPU and goes into the texture there
           when a draw uses it (texture_from_gpu); memory gets it only when the program locks
           the surface (d3d_surface_access) or a texture blit reads it (gpu_sync). A cube map's
           face is read back instead, and the cube map uploaded again when a draw uses it
           (textures_written): simple, and enough for reflections drawn once a frame. */
        LPDDRAWI_DDRAWSURFACE_GBL g = c->rt->lpGbl;
        GPUBUF *b = c->cur;
        b->deferred = 1;
        b->tex = 1;
        surface_format(c->rt, &b->pf);
        b->pitch = g->lPitch;
        b->tw = g->wWidth;
        b->th = g->wHeight;
        b->gen++;
        c->dirty = 0;
        textures_written(b->mem);       /* their stages bind them again */
    } else {
        read_back(c);
    }
}

int d3d_surface_access(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    OWNER m = me();
    int i, j;

    for (i = 0; i < MAX_CONTEXTS; i++) {
        RCTX *c = mine(&contexts[i].o, &m) ? contexts[i].c : NULL;
        if (!c)
            continue;                   /* (a texture's picture may wait without a flip chain) */
        for (j = 0; j < TARGETS; j++) {
            GPUBUF *b = &c->cb[j];
            if (b->mem == s->lpGbl->fpVidMem && (b->deferred || (b == c->cur && c->dirty))) {
                int ok = read_pixels(c, b);
                let_go(c);
                if (!ok)
                    return 0;
            }
        }
    }
    return 1;
}

static int end_chain(RCTX *c)
{
    int i;
    if (!c->chain_active)
        return 1;
    for (i = 0; i < TARGETS; i++)
        if (!read_pixels(c, &c->cb[i]))
            return 0;
    c->chain_active = 0;
    c->front = c->back = NULL;
    return 1;
}

int d3d_leave_exclusive(void)
{
    OWNER m = me();
    int i;
    for (i = 0; i < MAX_CONTEXTS; i++) {
        RCTX *c = mine(&contexts[i].o, &m) ? contexts[i].c : NULL;
        if (c && c->chain_active) {
            int ok = end_chain(c);
            let_go(c);
            if (!ok)
                return 0;
        }
    }
    return 1;
}

int d3d_present(LPDDRAWI_DDRAWSURFACE_LCL front, LPDDRAWI_DDRAWSURFACE_LCL target)
{
    OWNER m;
    int i;
    if (!scanout_requested || !(front->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) || !screen_page(target))
        return 0;
    m = me();
    for (i = 0; i < MAX_CONTEXTS; i++) {
        RCTX *c = mine(&contexts[i].o, &m) ? contexts[i].c : NULL;
        if (c && c->scanout_enabled && c->rt && c->cur &&
            c->cur->mem == target->lpGbl->fpVidMem && c->cur->serial == shared->mode_serial &&
            !c->cur->written && screen_page(c->rt) && c->p->present_color) {
            int ok = rdd_prepare_scanout() && make_current(c) && c->p->present_color();
            let_go(c);
            if (ok) {
                if (!c->chain_active)
                    dbg("rd3d: GPU flip chain active\r\n");
                c->chain_active = 1;
                c->front = front;
                c->back = target;
                return 1;
            }
            /* A rejected resource or an unsupported host takes the ordinary flip path. */
            c->scanout_enabled = 0;
        }
    }
    return 0;
}

/* The viewport as OpenGL's scissor. With a guard band (d3d_driver_info) Direct3D clips
   primitives only to that and leaves clipping to the viewport to the driver: a viewport
   smaller than the target needs the scissor, one that covers it needs none. Rows are in
   Direct3D's order (see framebuffer), as in clear. Set when the viewport or the target's size
   changes and after clear and rt_upload, never for each draw. */
static void apply_scissor(RCTX *c)
{
    GLF *gl = gl_of(c);

    if (!c->vp_w || !c->vp_h ||
        (!c->vp_x && !c->vp_y && c->vp_w >= c->fb_w && c->vp_h >= c->fb_h)) {
        gl->Disable(GL_SCISSOR_TEST);
        return;
    }
    gl->Scissor(c->vp_x, c->vp_y, c->vp_w, c->vp_h);
    gl->Enable(GL_SCISSOR_TEST);
}

/* The GPU buffer of the surface at memory mem, w x h, from table t of n: a render target's
   colour or a z-buffer's depth. A few a context; past that, the one attached longest ago is
   taken over. That one is never newer than its memory: a target's picture is read back before
   another is attached (set_render_target, Flip), and depth never goes to memory. A buffer that
   is new, taken over or resized holds nothing defined: written says the memory is the
   picture. */
static GPUBUF *gpu_buffer(RCTX *c, GPUBUF *t, DWORD n, FLATPTR mem, DWORD w, DWORD h,
                          GLenum format)
{
    GLF *gl = gl_of(c);
    GPUBUF *b = &t[0];
    DWORD i;

    for (i = 0; i < n; i++) {
        if (t[i].mem == mem) {
            b = &t[i];
            break;
        }
        if (t[i].used < b->used)
            b = &t[i];                  /* not used yet, or attached longest ago */
    }
    if (t == c->cb && (b->mem != mem || b->w != w || b->h != h || b->format != format) &&
        b->deferred && !read_pixels(c, b))
        return NULL;
    if (b->mem != mem || b->serial != shared->mode_serial) {
        b->written = 1;
        b->deferred = 0;
        b->tex = 0;
    }
    b->serial = shared->mode_serial;
    if (!b->rb)
        gl->GenRenderbuffers(1, &b->rb);
    if (b->w != w || b->h != h || b->format != format) {
        gl->BindRenderbuffer(GL_RENDERBUFFER, b->rb);
        gl->RenderbufferStorage(GL_RENDERBUFFER, format, w, h);
        {
            GLenum e = gl->GetError();
            static DWORD logged;
            if (e && logged++ < 8) {
                dbg("rd3d: renderbuffer "); dbg_hex(w); dbg(" x "); dbg_hex(h); dbg(" format ");
                dbg_hex(format); dbg(": GL error "); dbg_hex(e); dbg("\r\n");
            }
        }
        b->w = w;
        b->h = h;
        b->format = format;
        b->written = 1;
    }
    b->mem = mem;
    b->used = ++c->clock;
    return b;
}

/* Why a target can't be drawn into, on the debug port, once for each kind of failure: GL's
   status (0: the call itself failed, as with no context current) and what each buffer really
   holds on the GPU (0 x 0: its storage was never made). */
static void framebuffer_failed(RCTX *c, GLenum st, GLuint colour, GLuint depth, DWORD w, DWORD h)
{
    static GLenum last_st;
    static DWORD last_w, last_h;
    GLF *gl = gl_of(c);
    GLuint rbs[2];
    int k;

    if (st == last_st && w == last_w && h == last_h)
        return;
    last_st = st; last_w = w; last_h = h;
    dbg("rd3d: framebuffer incomplete, status "); dbg_hex(st); dbg(", target "); dbg_hex(w);
    dbg(" x "); dbg_hex(h); dbg(", target caps "); dbg_hex(c->rt ? c->rt->ddsCaps.dwCaps : 0);
    dbg(", thread "); dbg_hex(GetCurrentThreadId()); dbg(", GL error "); dbg_hex(gl->GetError());
    dbg("\r\n");
    if (!gl->GetRenderbufferParameteriv)
        return;
    rbs[0] = colour;
    rbs[1] = depth;
    for (k = 0; k < 2; k++) {
        GLint rw = -1, rh = -1, rf = -1;
        if (!rbs[k])
            continue;
        gl->BindRenderbuffer(GL_RENDERBUFFER, rbs[k]);
        gl->GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &rw);
        gl->GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &rh);
        gl->GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &rf);
        dbg(k ? "rd3d:   depth " : "rd3d:   colour "); dbg_hex(rw); dbg(" x "); dbg_hex(rh);
        dbg(", format "); dbg_hex(rf); dbg("\r\n");
    }
}

/* The GPU format of a render target's colour buffer: 565 for a 565 target, so that reading the
   picture back is a copy; converting from RGBA8 made a 16-bit frame at 1024x768 take longer to
   read back (5.0 ms) than a 32-bit one twice its size (4.0 ms). Without alpha bits it also
   reads destination alpha as 1, as Direct3D does for a target without alpha. RGBA8 for the
   rest. */
static GLenum colour_format(LPDDRAWI_DDRAWSURFACE_LCL rt)
{
    DDPIXELFORMAT pf;

    if (!rt)
        return GL_RGBA8;
    surface_format(rt, &pf);
    if ((pf.dwFlags & DDPF_RGB) && pf.dwRGBBitCount == 16 && pf.dwRBitMask == 0xF800)
        return GL_RGB565;
    return GL_RGBA8;
}

/* The framebuffer object for the render target: its colour buffer and the depth buffer of
   its z-buffer (none without one: the depth test is off then, apply_depth_test). Rows are
   kept in Direct3D's order (GL row 0 is the top row), so a read-back copies straight into the
   surface. Called for every command buffer too: a flip gives the target surface other memory
   without telling the driver, and that memory's own buffer comes in here. */
static int framebuffer(RCTX *c)
{
    GLF *gl = gl_of(c);
    DWORD w = c->rt ? c->rt->lpGbl->wWidth : 1, h = c->rt ? c->rt->lpGbl->wHeight : 1;
    FLATPTR mem = c->rt ? c->rt->lpGbl->fpVidMem : 0;
    FLATPTR zmem = c->z ? c->z->lpGbl->fpVidMem : 0;
    GPUBUF *cb;
    GLuint depth;

    if (c->cur && c->cur->serial != shared->mode_serial) {
        c->cur->written = 1;
        c->dirty = 0;
        c->chain_active = 0;
        c->front = c->back = NULL;
    }
    if (c->fbo && c->cur && c->cur->mem == mem && c->cur->serial == shared->mode_serial &&
        c->fb_w == w && c->fb_h == h &&
        zmem == c->fb_z)
        return 1;
    if (c->cur && c->cur->mem != mem)
        finish_target(c);
    if (!c->fbo)
        gl->GenFramebuffers(1, &c->fbo);
    gl->BindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    cb = gpu_buffer(c, c->cb, TARGETS, mem, w, h, colour_format(c->rt));
    if (!cb)
        return 0;
    gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, cb->rb);
    depth = zmem ? gpu_buffer(c, c->zb, ZBUFFERS, zmem, w, h, GL_DEPTH24_STENCIL8)->rb : 0;
    gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    c->cur = cb;
    c->fb_z = zmem;
    {
        GLenum st = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            framebuffer_failed(c, st, cb->rb, depth, w, h);
            return 0;
        }
    }
    if (c->fb_w == w && c->fb_h == h)
        return 1;                       /* the same size: the view stays */
    c->fb_w = w;
    c->fb_h = h;
    gl->Viewport(0, 0, w, h);
    c->vp_on = 0;                       /* the next transformed draw sets its viewport again */
    if (gl->BindProgramARB)
        gl->Disable(GL_VERTEX_PROGRAM_ARB);
    if (gl->DepthRange)
        gl->DepthRange(0.0, 1.0);
    c->tnl_dirty |= TNL_DIRTY_XF;
    gl->MatrixMode(GL_PROJECTION);
    gl->LoadIdentity();
    /* Screen pixels (centres on whole numbers, as in Direct3D) and z 0..1 to OpenGL's cube. */
    gl->Ortho(-0.5, w - 0.5, -0.5, h - 0.5, 0.0, -1.0);
    gl->MatrixMode(GL_MODELVIEW);
    gl->LoadIdentity();
    apply_scissor(c);                   /* the viewport may not cover a target of this size */
    return 1;
}

static void context_free(RCTX *c)
{
    GLF *gl = gl_of(c);
    DWORD i;

    for (i = 0; i < MAX_PS; i++)
        release(c->ps[i].code);         /* the programs go with the OpenGL context */
    for (i = 0; i < MAX_VS; i++)
        if (c->vs[i]) {
            release(c->vs[i]->decl);
            release(c->vs[i]->code);
            release(c->vs[i]);
        }
    {
        DDLOCAL *d = ddlocal(c->ddl, 0);    /* its buffer objects go with the OpenGL context */
        for (i = 0; d && i < d->nbuf; i++)
            if (d->buf[i].ctx == (void *)c) {
                d->buf[i].name = 0;
                d->buf[i].ctx = NULL;
            }
        for (i = 0; d && i < d->ntrash; )
            if (d->trash[i].ctx == (void *)c)
                d->trash[i] = d->trash[--d->ntrash];
            else
                i++;
    }
    if (c->rc) {
        make_current(c);
        for (i = 0; i < c->ntex; i++)
            if (c->tex[i].name)
                gl->DeleteTextures(1, &c->tex[i].name);
        if (c->white)
            gl->DeleteTextures(1, &c->white);
        if (c->black)
            gl->DeleteTextures(1, &c->black);
        if (c->fog_tex)
            gl->DeleteTextures(1, &c->fog_tex);
        if (c->read_fbo)
            gl->DeleteFramebuffers(1, &c->read_fbo);
        if (c->fbo)
            gl->DeleteFramebuffers(1, &c->fbo);
        for (i = 0; i < TARGETS; i++)
            if (c->cb[i].rb)
                gl->DeleteRenderbuffers(1, &c->cb[i].rb);
        for (i = 0; i < ZBUFFERS; i++)
            if (c->zb[i].rb)
                gl->DeleteRenderbuffers(1, &c->zb[i].rb);
        let_go(c);
        gl->DrvDeleteContext(c->rc);
    }
    if (c->dc)
        ReleaseDC(c->wnd, c->dc);
    if (c->wnd)
        DestroyWindow(c->wnd);
    release(c->tex);
    release(c->pos);
    release(c->ibuf);
    release(c);
}

/* ---- Render states -------------------------------------------------------------------------------- */
static GLenum gl_cmp(DWORD f)
{
    return (f >= 1 && f <= 8) ? GL_NEVER + (f - 1) : GL_ALWAYS;
}

static GLenum gl_blend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO: return GL_ZERO;
    case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}

static void apply_blend(RCTX *c)
{
    GLF *gl = gl_of(c);
    DWORD s = c->rs[D3DRENDERSTATE_SRCBLEND], d = c->rs[D3DRENDERSTATE_DESTBLEND];

    if (s == D3DBLEND_BOTHSRCALPHA)
        gl->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else if (s == D3DBLEND_BOTHINVSRCALPHA)
        gl->BlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
    else
        gl->BlendFunc(gl_blend(s), gl_blend(d));
}

static void enable(GLF *gl, GLenum cap, DWORD on)
{
    if (on)
        gl->Enable(cap);
    else
        gl->Disable(cap);
}

/* The depth test: ZENABLE, and only with a z-buffer. A target without one has no depth buffer
   attached (framebuffer), and Direct3D draws untested then. */
static void apply_depth_test(RCTX *c)
{
    enable(gl_of(c), GL_DEPTH_TEST, c->z && c->rs[D3DRENDERSTATE_ZENABLE] != D3DZB_FALSE);
}

/* Fog is a texture unit of its own after Direct3D's stages (FOG_UNIT): a two-texel ramp whose
   alpha is the fog factor, looked up with a coordinate per vertex (convert), blends the colour
   towards the fog colour (apply_stages). OpenGL's own fog would need a fog varying, which the
   host's virgl renderer (0.10) can't translate: those draws came out wrong. */
static void apply_fog(RCTX *c)
{
    c->stages_dirty = 1;
}

/* Direct3D's alpha test, and the colour key: a texel of the key's colour is not drawn. Such
   texels have alpha 0 in the uploaded texture (keyed_copy), so while the program doesn't test
   alpha itself, a test for alpha above 0 leaves them out. */
static void apply_alpha_test(RCTX *c)
{
    GLF *gl = gl_of(c);

    if (c->rs[D3DRENDERSTATE_ALPHATESTENABLE]) {
        gl->Enable(GL_ALPHA_TEST);
        gl->AlphaFunc(gl_cmp(c->rs[D3DRENDERSTATE_ALPHAFUNC]),
                      (c->rs[D3DRENDERSTATE_ALPHAREF] & 255) / 255.0f);
    } else if (c->keyed) {
        gl->Enable(GL_ALPHA_TEST);
        gl->AlphaFunc(GL_GREATER, 0.0f);
    } else {
        gl->Disable(GL_ALPHA_TEST);
    }
}

static GLenum gl_stencil_op(DWORD op)
{
    switch (op) {
    case D3DSTENCILOP_ZERO: return GL_ZERO;
    case D3DSTENCILOP_REPLACE: return GL_REPLACE;
    case D3DSTENCILOP_INCRSAT: return GL_INCR;
    case D3DSTENCILOP_DECRSAT: return GL_DECR;
    case D3DSTENCILOP_INVERT: return GL_INVERT;
    case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
    case D3DSTENCILOP_DECR: return GL_DECR_WRAP;
    }
    return GL_KEEP;
}

/* The stencil test. Every z-buffer is depth and stencil on the GPU (gpu_buffer), so it works
   whatever format the z-buffer has in memory. */
static void apply_stencil(RCTX *c)
{
    GLF *gl = gl_of(c);
    DWORD *rs = c->rs;

    enable(gl, GL_STENCIL_TEST, rs[D3DRENDERSTATE_STENCILENABLE]);
    gl->StencilFunc(gl_cmp(rs[D3DRENDERSTATE_STENCILFUNC]), (GLint)rs[D3DRENDERSTATE_STENCILREF],
                    rs[D3DRENDERSTATE_STENCILMASK]);
    gl->StencilOp(gl_stencil_op(rs[D3DRENDERSTATE_STENCILFAIL]),
                  gl_stencil_op(rs[D3DRENDERSTATE_STENCILZFAIL]),
                  gl_stencil_op(rs[D3DRENDERSTATE_STENCILPASS]));
    gl->StencilMask(rs[D3DRENDERSTATE_STENCILWRITEMASK]);
}

#define RS_COLORWRITEENABLE 168         /* Direct3D 8 and 9 */
#define RS_POINTSIZE 154                /* a float: pixels (Direct3D 8 and 9) */
#define RS_POINTSIZE_MIN 155
#define RS_POINTSPRITEENABLE 156
#define RS_POINTSCALEENABLE 157         /* sizes scaled by the distance from the eye (tnl_arb.c) */
#define RS_POINTSCALE_A 158             /* then B and C: floats */
#define RS_POINTSIZE_MAX 166
#define RS_CLIPPLANEENABLE 152          /* a bit a user clip plane */
#define MAX_POINT_SIZE 64.0f            /* the largest point offered (D3DCAPS8.MaxPointSize) */

/* The channels a draw writes (D3DRS_COLORWRITEENABLE: red 1, green 2, blue 4, alpha 8). A clear
   and the HAL's own uploads into the target write all of them (all). */
static void colour_mask(RCTX *c, int all)
{
    DWORD m = all ? 15 : c->rs[RS_COLORWRITEENABLE];
    gl_of(c)->ColorMask((GLboolean)(m & 1), (GLboolean)((m >> 1) & 1), (GLboolean)((m >> 2) & 1),
                        (GLboolean)((m >> 3) & 1));
}

/* Before a draw of points (Direct3D 8 and 9): their size in pixels, kept between the minimum and
   maximum the program set, and point sprites, which stretch each stage's texture over the
   point with (0, 0) at its top left (OpenGL's lower left: rows are in Direct3D's order, see
   framebuffer). program_size: the vertex program writes the size itself (a shader's oPts, or
   the fixed-function program scaling it by distance, tnl_arb.c). Not done: sizes in the
   vertex; such points get the render state's size. */
static void point_state(RCTX *c, int program_size)
{
    GLF *gl = gl_of(c);
    float size = *(const float *)&c->rs[RS_POINTSIZE];
    float lo = *(const float *)&c->rs[RS_POINTSIZE_MIN], hi = *(const float *)&c->rs[RS_POINTSIZE_MAX];
    int sprites = c->rs[RS_POINTSPRITEENABLE] != 0, u;

    if (!gl->PointSize || !gl->PointParameterfv)
        return;
    if (!(hi >= 1.0f) || hi > MAX_POINT_SIZE)
        hi = MAX_POINT_SIZE;
    if (size < lo) size = lo;
    if (size > hi) size = hi;
    if (!(size >= 1.0f)) size = 1.0f;
    gl->PointSize(size);
    if (program_size != c->psize_on) {
        enable(gl, GL_VERTEX_PROGRAM_POINT_SIZE, program_size);
        c->psize_on = program_size;
    }
    if (sprites != c->sprites_on) {
        GLfloat origin = (GLfloat)GL_LOWER_LEFT;
        enable(gl, GL_POINT_SPRITE, sprites);
        gl->PointParameterfv(GL_POINT_SPRITE_COORD_ORIGIN, &origin);
        for (u = 0; u < STAGES; u++) {
            gl->ActiveTexture(GL_TEXTURE0 + u);
            gl->TexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, sprites);
        }
        gl->ActiveTexture(GL_TEXTURE0);
        c->sprites_on = sprites;
    }
}

static void render_state(RCTX *c, DWORD state, DWORD v)
{
    GLF *gl = gl_of(c);

    if (state < 256)
        c->rs[state] = v;
    switch (state) {
    case D3DRENDERSTATE_ZENABLE:
        apply_depth_test(c);
        break;
    case D3DRENDERSTATE_ZWRITEENABLE:
        gl->DepthMask((GLboolean)(v ? GL_TRUE : GL_FALSE));
        break;
    case D3DRENDERSTATE_ZFUNC:
        gl->DepthFunc(gl_cmp(v));
        break;
    case D3DRENDERSTATE_ALPHABLENDENABLE:
        enable(gl, GL_BLEND, v);
        break;
    case D3DRENDERSTATE_SRCBLEND:
    case D3DRENDERSTATE_DESTBLEND:
        apply_blend(c);
        break;
    case D3DRENDERSTATE_ALPHATESTENABLE:
    case D3DRENDERSTATE_ALPHAREF:
    case D3DRENDERSTATE_ALPHAFUNC:
        apply_alpha_test(c);
        break;
    case D3DRENDERSTATE_COLORKEYENABLE:   /* textures are uploaded with or without their key */
        c->stages_dirty = 1;
        break;
    case RS_COLORWRITEENABLE:
        colour_mask(c, 0);
        break;
    case D3DRENDERSTATE_STENCILENABLE:
    case D3DRENDERSTATE_STENCILFAIL:
    case D3DRENDERSTATE_STENCILZFAIL:
    case D3DRENDERSTATE_STENCILPASS:
    case D3DRENDERSTATE_STENCILFUNC:
    case D3DRENDERSTATE_STENCILREF:
    case D3DRENDERSTATE_STENCILMASK:
    case D3DRENDERSTATE_STENCILWRITEMASK:
        apply_stencil(c);
        break;
    case D3DRENDERSTATE_CULLMODE:
        /* Rows are flipped (row 0 at the top in GL too), so a triangle Direct3D sees clockwise
           is counter-clockwise for OpenGL. */
        if (v == D3DCULL_NONE) {
            gl->Disable(GL_CULL_FACE);
        } else {
            gl->Enable(GL_CULL_FACE);
            gl->FrontFace(GL_CCW);
            gl->CullFace(v == D3DCULL_CCW ? GL_BACK : GL_FRONT);
        }
        break;
    case D3DRENDERSTATE_SHADEMODE:
        gl->ShadeModel(v == D3DSHADE_FLAT ? GL_FLAT : GL_SMOOTH);
        break;
    case D3DRENDERSTATE_FILLMODE:
        gl->PolygonMode(GL_FRONT_AND_BACK, v == D3DFILL_POINT ? GL_POINT :
                        v == D3DFILL_WIREFRAME ? GL_LINE : GL_FILL);
        break;
    case D3DRENDERSTATE_FOGENABLE:
    case D3DRENDERSTATE_FOGCOLOR:
    case D3DRENDERSTATE_FOGTABLEMODE:
    case D3DRENDERSTATE_FOGSTART:
    case D3DRENDERSTATE_FOGEND:
    case D3DRENDERSTATE_FOGDENSITY:
        apply_fog(c);
        break;
    case D3DRENDERSTATE_SPECULARENABLE:
        enable(gl, GL_COLOR_SUM, v);
        break;
    case D3DRENDERSTATE_TEXTUREFACTOR:
        c->stages_dirty = 1;
        break;
    case D3DRENDERSTATE_SCENECAPTURE:
        if (!v) {                       /* EndScene: the picture into the render target */
            finish_target(c);
            c->scene++;
        }
        break;
    }
}

/* ---- Textures -------------------------------------------------------------------------------- */
static TEXTURE *texture_slot(RCTX *c, DWORD h)
{
    if (h >= c->ntex) {
        DWORD n = h + 64 > c->ntex * 2 ? h + 64 : c->ntex * 2;
        TEXTURE *a = (TEXTURE *)alloc(n * sizeof(TEXTURE));
        if (!a)
            return NULL;
        if (c->tex) {
            memcpy(a, c->tex, c->ntex * sizeof(TEXTURE));
            release(c->tex);
        }
        c->tex = a;
        c->ntex = n;
    }
    return &c->tex[h];
}

static GLenum gl_address(DWORD a)
{
    switch (a) {
    case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP: return GL_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER: return GL_CLAMP_TO_BORDER;
    }
    return GL_REPEAT;
}

/* The levels of a mipmapped texture: the top level, then each smaller one, attached to the one
   before it (the DDK's sample drivers walk them the same way). A level that isn't half the
   size of the one before (at least 1) ends the walk, as does one without memory. Returns how
   many there are, at least 1. */
static DWORD mip_chain(LPDDRAWI_DDRAWSURFACE_LCL s, LPDDRAWI_DDRAWSURFACE_LCL *lv)
{
    DWORD n = 1;

    lv[0] = s;
    while (n < MAX_LEVELS && (s->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && s->lpAttachList) {
        LPDDRAWI_DDRAWSURFACE_LCL next = s->lpAttachList->lpAttached;
        DWORD w = s->lpGbl->wWidth > 1 ? s->lpGbl->wWidth / 2 : 1;
        DWORD h = s->lpGbl->wHeight > 1 ? s->lpGbl->wHeight / 2 : 1;
        if (!next || next == lv[0] || !(next->ddsCaps.dwCaps & DDSCAPS_MIPMAP) ||
            !next->lpGbl || !next->lpGbl->fpVidMem || next->lpGbl->wWidth != w ||
            next->lpGbl->wHeight != h)
            break;
        lv[n++] = s = next;
    }
    return n;
}

/* DirectDraw's contents stamps of all levels in one number (0: none kept): it changes when any
   level is written. */
static DWORD chain_stamp(LPDDRAWI_DDRAWSURFACE_LCL *lv, DWORD n)
{
    DWORD i, stamp = 0;

    for (i = 0; i < n; i++)
        stamp += GET_LPDDRAWSURFACE_GBL_MORE(lv[i]->lpGbl)->dwContentsStamp * (2 * i + 1);
    return stamp;
}

/* Has the surface behind a texture been written since it was uploaded? Video memory is only
   written through our Lock (DirectDraw's own blits lock too) or a texture blit, which mark the
   texture; DirectDraw's contents stamp says so too when it keeps one. Surfaces in system memory
   are written without telling the driver: without a stamp they are taken again once a scene. */
static int texture_changed(RCTX *c, TEXTURE *t, LPDDRAWI_DDRAWSURFACE_LCL s, DWORD stamp)
{
    if (t->dirty || (stamp && stamp != t->stamp))
        return 1;
    return !stamp && (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) && t->scene != c->scene;
}

/* At most this many OpenGL textures a context. DirectX 7's texture manager makes and drops
   surfaces all the time (thousands of handles a minute in 3DMark 2000) and tells a driver
   nothing when they go; past the budget the texture used longest ago is deleted, and made
   again from its surface should it be wanted. */
#define TEXTURE_BUDGET 512

static void texture_evict(RCTX *c, DWORD keep)
{
    DWORD i, oldest = 0, when = 0xFFFFFFFF;
    int s;

    for (i = 1; i < c->ntex; i++) {
        if (!c->tex[i].name || i == keep || c->tex[i].scene >= when)
            continue;
        for (s = 0; s < STAGES && c->bound[s] != i; s++)
            ;
        if (s < STAGES)
            continue;                   /* in use right now */
        oldest = i;
        when = c->tex[i].scene;
    }
    if (!oldest)
        return;
    gl_of(c)->DeleteTextures(1, &c->tex[oldest].name);
    memset(&c->tex[oldest], 0, sizeof c->tex[oldest]);
    c->ntextures--;
}

/* One channel of a texel, given its mask in the format, scaled to 8 bits. */
static DWORD channel(DWORD v, DWORD mask)
{
    DWORD bits = 0, m;

    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        v >>= 1;
    }
    v &= mask;
    for (m = mask; m; m >>= 1)
        bits++;
    return bits >= 8 ? v >> (bits - 8) : (v * 255 + mask / 2) / mask;
}

/* A level of a texture with a colour key, as 32-bit BGRA for OpenGL: texels of the key colour
   (red, green and blue compared) become black with alpha 0, the others keep their colour and
   alpha (255 if the format has none). NULL if there is no memory for it. */
static DWORD *keyed_copy(LPDDRAWI_DDRAWSURFACE_GBL g, const DDPIXELFORMAT *pf, DWORD key)
{
    DWORD bytes = pf->dwRGBBitCount / 8, x, y;
    DWORD rgb = pf->dwRBitMask | pf->dwGBitMask | pf->dwBBitMask;
    DWORD amask = (pf->dwFlags & DDPF_ALPHAPIXELS) ? pf->dwRGBAlphaBitMask : 0;
    DWORD *out = (DWORD *)alloc((DWORD)g->wWidth * g->wHeight * 4), *o = out;

    if (!out)
        return NULL;
    key &= rgb;
    for (y = 0; y < g->wHeight; y++) {
        const BYTE *row = (const BYTE *)g->fpVidMem + y * g->lPitch;
        for (x = 0; x < g->wWidth; x++) {
            DWORD v = bytes == 4 ? ((const DWORD *)row)[x] : ((const WORD *)row)[x];
            if ((v & rgb) == key) {
                *o++ = 0;
                continue;
            }
            *o++ = (amask ? channel(v, amask) : 255) << 24 | channel(v, pf->dwRBitMask) << 16 |
                   channel(v, pf->dwGBitMask) << 8 | channel(v, pf->dwBBitMask);
        }
    }
    return out;
}

/* All levels of a texture to the bound OpenGL texture: new storage (TexImage2D) or the same
   storage again (TexSubImage2D). A texture drawn with its colour key (keyed) goes as 32-bit
   copies with the key's texels made transparent (keyed_copy). */
static void texture_upload(GLF *gl, const PIXFMT *f, const DDPIXELFORMAT *pf,
                           LPDDRAWI_DDRAWSURFACE_LCL *lv, DWORD levels, int fresh, int keyed)
{
    DWORD l;

    if (f->block) {                     /* DXT: each level's blocks as they are, row after row */
        for (l = 0; l < levels && gl->CompressedTexImage2D; l++) {
            LPDDRAWI_DDRAWSURFACE_GBL g = lv[l]->lpGbl;
            DWORD bytes = dxt_bytes(f, g->wWidth, g->wHeight);
            if (fresh || !gl->CompressedTexSubImage2D)
                gl->CompressedTexImage2D(GL_TEXTURE_2D, l, f->internal, g->wWidth, g->wHeight, 0,
                                         bytes, (const GLvoid *)g->fpVidMem);
            else
                gl->CompressedTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, g->wWidth, g->wHeight,
                                            f->internal, bytes, (const GLvoid *)g->fpVidMem);
        }
        return;
    }
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (l = 0; l < levels; l++) {
        LPDDRAWI_DDRAWSURFACE_GBL g = lv[l]->lpGbl;
        if (keyed) {
            DWORD *copy = keyed_copy(g, pf, lv[0]->ddckCKSrcBlt.dwColorSpaceLowValue);
            if (!copy)
                continue;
            gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            if (fresh)
                gl->TexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, g->wWidth, g->wHeight, 0, GL_BGRA,
                               GL_UNSIGNED_BYTE, copy);
            else
                gl->TexSubImage2D(GL_TEXTURE_2D, l, 0, 0, g->wWidth, g->wHeight, GL_BGRA,
                                  GL_UNSIGNED_BYTE, copy);
            release(copy);
            continue;
        }
        gl->PixelStorei(GL_UNPACK_ROW_LENGTH, g->lPitch / f->bytes);
        if (fresh && !l && f->bump)     /* Direct3D reads V8U8 as (du, dv, 1, 1) */
            gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
        else if (fresh && !l)           /* the name may have held a bump map before */
            gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
        if (fresh)
            gl->TexImage2D(GL_TEXTURE_2D, l, f->internal, g->wWidth, g->wHeight, 0, f->format,
                           f->type, (const GLvoid *)g->fpVidMem);
        else
            gl->TexSubImage2D(GL_TEXTURE_2D, l, 0, 0, g->wWidth, g->wHeight, f->format, f->type,
                              (const GLvoid *)g->fpVidMem);
    }
    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* Direct3D's minification and mipmap filters as one OpenGL filter. Without levels below the
   top, mipmap filtering would make the texture incomplete (nothing drawn), so it is left out. */
static GLint min_filter(const DWORD *st, DWORD levels)
{
    int linear = st[D3DTSS_MINFILTER] != D3DTFN_POINT;

    if (levels < 2 || st[D3DTSS_MIPFILTER] == D3DTFP_NONE || st[D3DTSS_MIPFILTER] == 0)
        return linear ? GL_LINEAR : GL_NEAREST;
    if (st[D3DTSS_MIPFILTER] == D3DTFP_POINT)
        return linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
}

/* A Direct3D 8/9 cube map (the HAL's copy of its records, shadow_make) on the stage's unit:
   six faces, each with its levels, on OpenGL's cube map target. Sampled by pixel shaders; the
   fixed-function stages don't (as on Windows 2000). */
static int cube_bind(RCTX *c, DWORD h, int stage, const struct shadow *sh)
{
    GLF *gl = gl_of(c);
    DWORD *st = c->tss[stage];
    DWORD levels = sh[0].levels, face, l;
    const DDRAWI_DDRAWSURFACE_GBL *g0 = &sh[0].g;
    TEXTURE *t = texture_slot(c, h);
    DDPIXELFORMAT pf;
    PIXFMT f;

    if (!t || sh[0].faces != 6 || !levels || !g0->fpVidMem)
        return 0;
    surface_format((LPDDRAWI_DDRAWSURFACE_LCL)&sh[0].l, &pf);
    f = gl_format(&pf);
    if ((!f.bytes && !f.block) || (f.block && !gl->CompressedTexImage2D))
        return 0;
    if (!t->name) {
        if (c->ntextures >= TEXTURE_BUDGET)
            texture_evict(c, h);
        gl->GenTextures(1, &t->name);
        c->ntextures++;
    }
    gl->BindTexture(GL_TEXTURE_CUBE_MAP, t->name);
    if (t->mem != g0->fpVidMem || t->w != g0->wWidth || t->levels != levels ||
        t->fmt != f.internal || t->dirty) {
        gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (face = 0; face < 6; face++)
            for (l = 0; l < levels; l++) {
                const DDRAWI_DDRAWSURFACE_GBL *g = &sh[face * levels + l].g;
                GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
                if (f.block) {
                    gl->CompressedTexImage2D(target, l, f.internal, g->wWidth, g->wHeight, 0,
                                             dxt_bytes(&f, g->wWidth, g->wHeight),
                                             (const GLvoid *)g->fpVidMem);
                } else {
                    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, g->lPitch / f.bytes);
                    gl->TexImage2D(target, l, f.internal, g->wWidth, g->wHeight, 0, f.format,
                                   f.type, (const GLvoid *)g->fpVidMem);
                }
            }
        gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, levels - 1);
        gl->TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_SWIZZLE_B, f.bump ? GL_ONE : GL_BLUE);
        t->mem = g0->fpVidMem;
        for (face = 0; face < 6; face++)   /* a face rendered into marks the cube (textures_written) */
            t->fmem[face] = sh[face * levels].g.fpVidMem;
        t->w = g0->wWidth;
        t->h = g0->wHeight;
        t->fmt = f.internal;
        t->levels = levels;
        t->lmem[0] = g0->fpVidMem;
        tracef("rd3d: cube map %lx: %lux%lu, %lu levels\r\n", h, (DWORD)g0->wWidth,
               (DWORD)g0->wHeight, levels);
    }
    t->dirty = 0;
    t->scene = c->scene;
    gl->TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER,
                      st[D3DTSS_MAGFILTER] == D3DTFG_POINT ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, min_filter(st, levels));
    gl->TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return 1;
}

/* ---- DXT decoding (volumes: OpenGL has no 3D S3TC) -------------------------------------------- */
static DWORD dxt_expand(DWORD v, int bits)
{
    return bits == 5 ? (v << 3) | (v >> 2) : bits == 6 ? (v << 2) | (v >> 4) : (v << 4) | v;
}

/* The colours of a DXT colour block: two RGB565 ends and two between them, or one between and
   transparent black when the first end is not above the second (DXT1 only: four selects the
   four-colour form). */
static void dxt_colours(const BYTE *b, int four, DWORD *c)
{
    DWORD c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8, e[2][3], k;
    e[0][0] = dxt_expand(c0 >> 11, 5); e[0][1] = dxt_expand((c0 >> 5) & 63, 6); e[0][2] = dxt_expand(c0 & 31, 5);
    e[1][0] = dxt_expand(c1 >> 11, 5); e[1][1] = dxt_expand((c1 >> 5) & 63, 6); e[1][2] = dxt_expand(c1 & 31, 5);
    c[0] = 0xff000000UL | e[0][0] << 16 | e[0][1] << 8 | e[0][2];
    c[1] = 0xff000000UL | e[1][0] << 16 | e[1][1] << 8 | e[1][2];
    if (four || c0 > c1) {
        c[2] = c[3] = 0xff000000UL;
        for (k = 0; k < 3; k++) {
            c[2] |= ((2 * e[0][k] + e[1][k]) / 3) << (16 - 8 * k);
            c[3] |= ((e[0][k] + 2 * e[1][k]) / 3) << (16 - 8 * k);
        }
    } else {
        c[2] = 0xff000000UL;
        for (k = 0; k < 3; k++)
            c[2] |= ((e[0][k] + e[1][k]) / 2) << (16 - 8 * k);
        c[3] = 0;
    }
}

/* One slice of 4x4 blocks (block bytes 8: DXT1, else DXT3 or DXT5 by internal) into width x
   height ARGB texels. */
static void dxt_decode(GLenum internal, const BYTE *in, DWORD width, DWORD height, DWORD *out)
{
    DWORD bx, by, x, y, step = internal == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
    int dxt1 = step == 8, dxt5 = internal == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    for (by = 0; by < (height + 3) / 4; by++)
        for (bx = 0; bx < (width + 3) / 4; bx++, in += step) {
            const BYTE *colour = dxt1 ? in : in + 8;
            DWORD c[4], a[8], k;
            DWORD bits = colour[4] | colour[5] << 8 | colour[6] << 16 | (DWORD)colour[7] << 24;
            dxt_colours(colour, !dxt1, c);
            if (dxt5) {
                a[0] = in[0];
                a[1] = in[1];
                if (a[0] > a[1]) {
                    for (k = 1; k < 7; k++)
                        a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
                } else {
                    for (k = 1; k < 5; k++)
                        a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
                    a[6] = 0;
                    a[7] = 255;
                }
            }
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    DWORD i = 4 * y + x, texel = c[(bits >> (2 * i)) & 3];
                    if (bx * 4 + x >= width || by * 4 + y >= height)
                        continue;
                    if (!dxt1 && !dxt5)
                        texel = (texel & 0xffffff) | dxt_expand((in[i / 2] >> (4 * (i & 1))) & 15, 4) << 24;
                    else if (dxt5) {
                        DWORD at = 16 + 3 * i, index = ((in[at / 8] | in[at / 8 + 1] << 8) >> (at % 8)) & 7;
                        texel = (texel & 0xffffff) | a[index] << 24;
                    }
                    out[(by * 4 + y) * width + bx * 4 + x] = texel;
                }
        }
}

/* A volume's level: its slices one after another, rows as tight as the texel size allows
   (CreateSurface sizes video memory volumes so). */
static DWORD volume_pitch(const DDRAWI_DDRAWSURFACE_GBL *g, const PIXFMT *f)
{
    DWORD tight = f->block ? ((g->wWidth + 3) / 4) * f->block : g->wWidth * f->bytes;
    return g->lPitch >= (LONG)tight && g->lPitch < (LONG)(tight + 4) ? (DWORD)g->lPitch : tight;
}

/* A Direct3D 8/9 volume texture (the HAL's copy, shadow_make) on the stage's unit: each level
   with half the slices of the one above, on OpenGL's 3D target. Sampled by pixel shaders. */
static int volume_bind(RCTX *c, DWORD h, int stage, const struct shadow *sh)
{
    GLF *gl = gl_of(c);
    DWORD *st = c->tss[stage];
    DWORD levels = sh[0].levels, depth = sh[0].depth, l;
    const DDRAWI_DDRAWSURFACE_GBL *g0 = &sh[0].g;
    TEXTURE *t = texture_slot(c, h);
    DDPIXELFORMAT pf;
    PIXFMT f;

    if (!t || !gl->TexImage3D || !levels || !g0->fpVidMem)
        return 0;
    surface_format((LPDDRAWI_DDRAWSURFACE_LCL)&sh[0].l, &pf);
    f = gl_format(&pf);
    if (!f.bytes && !f.block)
        return 0;
    if (!t->name) {
        if (c->ntextures >= TEXTURE_BUDGET)
            texture_evict(c, h);
        gl->GenTextures(1, &t->name);
        c->ntextures++;
    }
    gl->BindTexture(GL_TEXTURE_3D, t->name);
    if (t->mem != g0->fpVidMem || t->w != g0->wWidth || t->levels != levels ||
        t->fmt != f.internal || t->dirty) {
        gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (l = 0; l < levels; l++) {
            const DDRAWI_DDRAWSURFACE_GBL *g = &sh[l].g;
            DWORD d = depth >> l ? depth >> l : 1;
            if (f.block) {              /* DXT: decoded here, slice by slice, to 32-bit ARGB */
                DWORD slice = volume_pitch(g, &f) * ((g->wHeight + 3) / 4), z;
                DWORD *texels = (DWORD *)alloc(g->wWidth * g->wHeight * d * 4);
                if (!texels)
                    continue;
                for (z = 0; z < d; z++)
                    dxt_decode(f.internal, (const BYTE *)g->fpVidMem + z * slice, g->wWidth,
                               g->wHeight, texels + z * g->wWidth * g->wHeight);
                gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                gl->PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                gl->TexImage3D(GL_TEXTURE_3D, l, GL_RGBA8, g->wWidth, g->wHeight, d, 0, GL_BGRA,
                               GL_UNSIGNED_BYTE, texels);
                release(texels);
                continue;
            }
            gl->PixelStorei(GL_UNPACK_ROW_LENGTH, volume_pitch(g, &f) / f.bytes);
            gl->PixelStorei(GL_UNPACK_IMAGE_HEIGHT, g->wHeight);
            gl->TexImage3D(GL_TEXTURE_3D, l, f.internal, g->wWidth, g->wHeight, d, 0, f.format,
                           f.type, (const GLvoid *)g->fpVidMem);
        }
        gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        t->mem = g0->fpVidMem;
        memset(t->fmem, 0, sizeof t->fmem);
        t->w = g0->wWidth;
        t->h = g0->wHeight;
        t->fmt = f.internal;
        t->levels = levels;
        t->lmem[0] = g0->fpVidMem;
        tracef("rd3d: volume %lx: %lux%lux%lu, %lu levels\r\n", h, (DWORD)g0->wWidth,
               (DWORD)g0->wHeight, depth, levels);
    }
    t->dirty = 0;
    t->scene = c->scene;
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER,
                      st[D3DTSS_MAGFILTER] == D3DTFG_POINT ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, min_filter(st, levels));
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, gl_address(st[D3DTSS_ADDRESSU]));
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, gl_address(st[D3DTSS_ADDRESSV]));
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, gl_address(st[25]));  /* D3DTSS_ADDRESSW */
    return 1;
}

/* Bind the texture behind a surface handle, uploading it if it is new or has changed. 0 if
   there is no usable texture. */
/* A 2D texture's filters, levels, addressing and border colour from its stage's states. */
static void sampler_2d(GLF *gl, const DWORD *st, DWORD levels)
{
    GLfloat border[4], bias;

    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                      st[D3DTSS_MAGFILTER] == D3DTFG_POINT ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter(st, levels));
    /* MAXMIPLEVEL is the largest level the program allows (0: the top) */
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL,
                      st[D3DTSS_MAXMIPLEVEL] < levels ? st[D3DTSS_MAXMIPLEVEL] : levels - 1);
    bias = *(const float *)&st[D3DTSS_MIPMAPLODBIAS];
    gl->TexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, &bias);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_address(st[D3DTSS_ADDRESSU]));
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_address(st[D3DTSS_ADDRESSV]));
    border[0] = ((st[D3DTSS_BORDERCOLOR] >> 16) & 255) / 255.0f;
    border[1] = ((st[D3DTSS_BORDERCOLOR] >> 8) & 255) / 255.0f;
    border[2] = (st[D3DTSS_BORDERCOLOR] & 255) / 255.0f;
    border[3] = ((st[D3DTSS_BORDERCOLOR] >> 24) & 255) / 255.0f;
    gl->TexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
}

/* The picture rendered into the texture at mem and kept on the GPU (finish_target), if any. */
static GPUBUF *gpu_picture(RCTX *c, FLATPTR mem)
{
    int i;
    for (i = 0; i < TARGETS; i++)
        if (c->cb[i].mem == mem && c->cb[i].deferred && c->cb[i].tex)
            return &c->cb[i];
    return NULL;
}

/* A texture rendered into: its picture copied into the OpenGL texture on the GPU, once for
   each picture (b->gen), instead of read back into memory and uploaded again. Only the top
   level: such textures are drawn with one. */
static int texture_from_gpu(RCTX *c, DWORD h, TEXTURE *t, LPDDRAWI_DDRAWSURFACE_LCL s, GPUBUF *b,
                            const DWORD *st)
{
    GLF *gl = gl_of(c);
    LPDDRAWI_DDRAWSURFACE_GBL g = s->lpGbl;
    LPDDRAWI_DDRAWSURFACE_LCL lv[MAX_LEVELS];
    PIXFMT f = gl_format(&b->pf);
    DWORD w = g->wWidth, ht = g->wHeight, fmt = f.internal | 0x40000000UL;  /* made on the GPU */

    if (!f.bytes)
        return 0;
    if (!t->name) {
        if (c->ntextures >= TEXTURE_BUDGET)
            texture_evict(c, h);
        gl->GenTextures(1, &t->name);
        c->ntextures++;
    }
    gl->BindTexture(GL_TEXTURE_2D, t->name);
    if (t->mem != g->fpVidMem || t->w != w || t->h != ht || t->fmt != fmt || t->levels != 1) {
        gl->TexImage2D(GL_TEXTURE_2D, 0, f.internal, w, ht, 0, f.format, f.type, NULL);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        t->mem = g->fpVidMem;
        memset(t->fmem, 0, sizeof t->fmem);
        t->w = w;
        t->h = ht;
        t->fmt = fmt;
        t->levels = 1;
        t->lmem[0] = g->fpVidMem;
        t->gpu_gen = b->gen - 1;
    }
    if (t->gpu_gen != b->gen) {
        if (!c->read_fbo)
            gl->GenFramebuffers(1, &c->read_fbo);
        gl->BindFramebuffer(GL_FRAMEBUFFER, c->read_fbo);
        gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, b->rb);
        gl->CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w < b->w ? w : b->w, ht < b->h ? ht : b->h);
        gl->BindFramebuffer(GL_FRAMEBUFFER, c->fbo);
        t->gpu_gen = b->gen;
        tracef("rd3d: texture %lx from its render-target picture on the GPU (%lu)\r\n", h, b->gen);
    }
    mip_chain(s, lv);
    t->stamp = chain_stamp(lv, 1);
    t->scene = c->scene;
    t->dirty = 0;
    sampler_2d(gl, st, 1);
    return 1;
}

static int texture_bind(RCTX *c, DWORD h, int stage)
{
    GLF *gl = gl_of(c);
    LPDDRAWI_DDRAWSURFACE_LCL s = handle_get(c->ddl, h);
    LPDDRAWI_DDRAWSURFACE_LCL lv[MAX_LEVELS];
    LPDDRAWI_DDRAWSURFACE_GBL g;
    TEXTURE *t;
    DDPIXELFORMAT pf;
    PIXFMT f;
    DWORD stamp, fmt, levels, l;
    int keyed;
    DWORD *st = c->tss[stage];

    if (!s || !(g = s->lpGbl) || !g->fpVidMem)
        return 0;
    if (c->dx8 && s->lpSurfMore && (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP))
        return cube_bind(c, h, stage, (const struct shadow *)s);    /* the copy (shadow_make) */
    if (c->dx8 && is_volume(s))
        return volume_bind(c, h, stage, (const struct shadow *)s);
    t = texture_slot(c, h);
    if (!t)
        return 0;
    {
        GPUBUF *b = gpu_picture(c, g->fpVidMem);     /* rendered into, still on the GPU */
        if (b)
            return texture_from_gpu(c, h, t, s, b, st);
    }
    surface_format(s, &pf);
    f = gl_format(&pf);
    if (!f.bytes && !f.block)
        return 0;
    if (f.block && !gl->CompressedTexImage2D)
        return 0;
    if (!t->name) {
        if (c->ntextures >= TEXTURE_BUDGET)
            texture_evict(c, h);
        gl->GenTextures(1, &t->name);
        c->ntextures++;
    }
    gl->BindTexture(GL_TEXTURE_2D, t->name);
    levels = mip_chain(s, lv);
    stamp = chain_stamp(lv, levels);
    /* The colour key, if the program turned it on and the texture has one: the key's colour
       is uploaded as transparent, so a change of either is a new upload (fmt) */
    keyed = c->rs[D3DRENDERSTATE_COLORKEYENABLE] && (s->dwFlags & DDRAWISURF_HASCKEYSRCBLT) &&
            !f.block;
    fmt = f.internal ^ (f.format << 8) ^ (f.type << 16) ^
          (keyed ? 0x80000000UL ^ s->ddckCKSrcBlt.dwColorSpaceLowValue : 0);
    if (keyed)
        c->keyed = 1;
    if (t->mem != g->fpVidMem || t->w != g->wWidth || t->h != g->wHeight || t->fmt != fmt ||
        t->levels != levels) {
        /* new: storage and contents */
        texture_upload(gl, &f, &pf, lv, levels, 1, keyed);
        t->mem = g->fpVidMem;
        memset(t->fmem, 0, sizeof t->fmem);
        t->w = g->wWidth;
        t->h = g->wHeight;
        t->fmt = fmt;
        t->levels = levels;
        for (l = 0; l < levels; l++)
            t->lmem[l] = lv[l]->lpGbl->fpVidMem;
        /* Levels 1 and down exist only with mipmaps; without, the top level is all there is
           and the texture is complete without the others. */
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        tracef("rd3d: texture %lx new: %lux%lu, %lu levels, %lu bits, caps %08lx, memory %08lx, "
               "pitch %ld, stamp %lx, sum %08lx\r\n", h, (DWORD)g->wWidth, (DWORD)g->wHeight,
               levels, pf.dwRGBBitCount, s->ddsCaps.dwCaps, g->fpVidMem, g->lPitch, stamp,
               trace_on && !f.block ? sum_rows((BYTE *)g->fpVidMem, g->lPitch, g->wWidth * f.bytes,
                                   g->wHeight) : 0);
    } else if (texture_changed(c, t, s, stamp)) {
        /* written since: the contents again, into the same storage */
        tracef("rd3d: texture %lx again: dirty %d, stamp %lx (was %lx), scene %lu (was %lu), "
               "sum %08lx\r\n", h, t->dirty, stamp, t->stamp, c->scene, t->scene,
               trace_on && !f.block ? sum_rows((BYTE *)g->fpVidMem, g->lPitch, g->wWidth * f.bytes,
                                   g->wHeight) : 0);
        texture_upload(gl, &f, &pf, lv, levels, 0, keyed);
    }
    t->stamp = stamp;
    t->scene = c->scene;
    t->dirty = 0;
    sampler_2d(gl, st, levels);
    return 1;
}

/* Texture stage operations to OpenGL's texture combiners. */
static GLenum combine_source(DWORD arg, int stage)
{
    switch (arg & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE: return GL_PRIMARY_COLOR;
    case D3DTA_CURRENT: return stage ? GL_PREVIOUS : GL_PRIMARY_COLOR;
    case D3DTA_TEXTURE: return GL_TEXTURE;
    case D3DTA_TFACTOR: return GL_CONSTANT;
    }
    return GL_PRIMARY_COLOR;            /* specular: no better source */
}

static GLenum combine_operand(DWORD arg, int alpha)
{
    if (alpha || (arg & D3DTA_ALPHAREPLICATE))
        return (arg & D3DTA_COMPLEMENT) ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    return (arg & D3DTA_COMPLEMENT) ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
}

static void combine(GLF *gl, int stage, int alpha, DWORD op, DWORD a1, DWORD a2)
{
    GLenum mode = GL_MODULATE, src2 = 0;
    GLint scale = 1;
    GLenum which = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    GLenum src = alpha ? GL_SOURCE0_ALPHA : GL_SOURCE0_RGB;
    GLenum opd = alpha ? GL_OPERAND0_ALPHA : GL_OPERAND0_RGB;
    DWORD first = a1, second = a2;

    switch (op) {
    case D3DTOP_SELECTARG1: mode = GL_REPLACE; break;
    case D3DTOP_SELECTARG2: mode = GL_REPLACE; first = a2; break;
    case D3DTOP_MODULATE: break;
    case D3DTOP_MODULATE2X: scale = 2; break;
    case D3DTOP_MODULATE4X: scale = 4; break;
    case D3DTOP_ADD: mode = GL_ADD; break;
    case D3DTOP_ADDSIGNED: mode = GL_ADD_SIGNED; break;
    case D3DTOP_ADDSIGNED2X: mode = GL_ADD_SIGNED; scale = 2; break;
    case D3DTOP_SUBTRACT: mode = GL_SUBTRACT; break;
    case D3DTOP_ADDSMOOTH: mode = GL_ADD; break;
    case D3DTOP_BLENDDIFFUSEALPHA: mode = GL_INTERPOLATE; src2 = GL_PRIMARY_COLOR; break;
    case D3DTOP_BLENDTEXTUREALPHA: mode = GL_INTERPOLATE; src2 = GL_TEXTURE; break;
    case D3DTOP_BLENDFACTORALPHA: mode = GL_INTERPOLATE; src2 = GL_CONSTANT; break;
    case D3DTOP_BLENDTEXTUREALPHAPM: mode = GL_INTERPOLATE; src2 = GL_TEXTURE; break;
    case D3DTOP_BLENDCURRENTALPHA: mode = GL_INTERPOLATE; src2 = stage ? GL_PREVIOUS : GL_PRIMARY_COLOR; break;
    case D3DTOP_DOTPRODUCT3: mode = alpha ? GL_MODULATE : GL_DOT3_RGBA; break;
    }
    gl->TexEnvi(GL_TEXTURE_ENV, which, mode);
    gl->TexEnvi(GL_TEXTURE_ENV, src, combine_source(first, stage));
    gl->TexEnvi(GL_TEXTURE_ENV, opd, combine_operand(first, alpha));
    gl->TexEnvi(GL_TEXTURE_ENV, src + 1, combine_source(second, stage));
    gl->TexEnvi(GL_TEXTURE_ENV, opd + 1, combine_operand(second, alpha));
    if (src2) {
        gl->TexEnvi(GL_TEXTURE_ENV, src + 2, src2);
        gl->TexEnvi(GL_TEXTURE_ENV, opd + 2, GL_SRC_ALPHA);
    }
    gl->TexEnvi(GL_TEXTURE_ENV, alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE, scale);
}

static int uses_texture(DWORD op, DWORD a1, DWORD a2)
{
    if (op == D3DTOP_DISABLE)
        return 0;
    if ((a1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE && op != D3DTOP_SELECTARG2)
        return 1;
    if ((a2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE && op != D3DTOP_SELECTARG1)
        return 1;
    return op == D3DTOP_BLENDTEXTUREALPHA || op == D3DTOP_BLENDTEXTUREALPHAPM;
}

static void white_texture(RCTX *c)
{
    GLF *gl = gl_of(c);
    static const DWORD white = 0xFFFFFFFF;

    if (!c->white) {
        gl->GenTextures(1, &c->white);
        gl->BindTexture(GL_TEXTURE_2D, c->white);
        gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &white);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    gl->BindTexture(GL_TEXTURE_2D, c->white);
}

/* What a pixel shader reads from a stage without a texture: opaque black (the fixed-function
   stages read white there). */
static void black_texture(RCTX *c)
{
    GLF *gl = gl_of(c);
    static const DWORD black = 0xFF000000;

    if (!c->black) {
        gl->GenTextures(1, &c->black);
        gl->BindTexture(GL_TEXTURE_2D, c->black);
        gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &black);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    gl->BindTexture(GL_TEXTURE_2D, c->black);
}

/* The fog unit (see apply_fog): colour = previous * f + fog colour * (1 - f), with f the ramp
   texture's alpha at the vertex's coordinate; alpha passes through. Off without fog. */
static void apply_fog_unit(RCTX *c)
{
    GLF *gl = gl_of(c);
    static const DWORD ramp[2] = { 0x00FFFFFF, 0xFFFFFFFF };    /* alpha 0, alpha 1 */
    DWORD col = c->rs[D3DRENDERSTATE_FOGCOLOR];
    GLfloat fc[4];

    gl->ActiveTexture(GL_TEXTURE0 + FOG_UNIT);
    if (!c->rs[D3DRENDERSTATE_FOGENABLE]) {
        gl->Disable(GL_TEXTURE_2D);
        return;
    }
    if (!c->fog_tex) {
        gl->GenTextures(1, &c->fog_tex);
        gl->BindTexture(GL_TEXTURE_2D, c->fog_tex);
        gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, ramp);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl->BindTexture(GL_TEXTURE_2D, c->fog_tex);
    gl->Enable(GL_TEXTURE_2D);
    fc[0] = ((col >> 16) & 255) / 255.0f;
    fc[1] = ((col >> 8) & 255) / 255.0f;
    fc[2] = (col & 255) / 255.0f;
    fc[3] = 1.0f;
    gl->TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    gl->TexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, fc);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + 1, GL_CONSTANT);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + 1, GL_SRC_COLOR);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + 2, GL_TEXTURE);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + 2, GL_SRC_ALPHA);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    gl->TexEnvi(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1);
}

/* Set up OpenGL's texture units from the stage states, before drawing. A stage without a
   texture whose operation needs one passes the colour on, as the hardware of the time did. */
/* What kind of texture a handle names: 0 plain, 1 cube map, 2 volume. */
static int texture_kind(RCTX *c, DWORD h)
{
    LPDDRAWI_DDRAWSURFACE_LCL s = handle_get(c->ddl, h);
    if (!c->dx8 || !s)
        return 0;
    return is_cube(s) ? 1 : is_volume(s) ? 2 : 0;
}

/* The fixed-function unit's cube map and 3D targets on or off for a texture of that kind (the
   2D target is the caller's). */
static void ff_target(RCTX *c, int unit, int kind)
{
    GLF *gl = gl_of(c);
    if (c->ff_kind[unit] == kind)
        return;
    if (c->ff_kind[unit] == 1)
        gl->Disable(GL_TEXTURE_CUBE_MAP);
    else if (c->ff_kind[unit] == 2)
        gl->Disable(GL_TEXTURE_3D);
    if (kind == 1)
        gl->Enable(GL_TEXTURE_CUBE_MAP);
    else if (kind == 2)
        gl->Enable(GL_TEXTURE_3D);
    c->ff_kind[unit] = (BYTE)kind;
}

static void apply_stages(RCTX *c)
{
    GLF *gl = gl_of(c);
    DWORD tf = c->rs[D3DRENDERSTATE_TEXTUREFACTOR];
    GLfloat factor[4];
    int s, off = 0;

    factor[0] = ((tf >> 16) & 255) / 255.0f;
    factor[1] = ((tf >> 8) & 255) / 255.0f;
    factor[2] = (tf & 255) / 255.0f;
    factor[3] = ((tf >> 24) & 255) / 255.0f;
    c->keyed = 0;                       /* texture_bind sets it again */
    if (c->dx8 && ps_program(c) > 0) {
        /* a pixel shader: each stage's texture on its unit, for the program's samplers; the
           stages' operations don't apply */
        for (s = 0; s < STAGES; s++) {
            DWORD map = c->tss[s][D3DTSS_TEXTUREMAP];
            c->bound[s] = 0;
            gl->ActiveTexture(GL_TEXTURE0 + s);
            if (map && texture_bind(c, map, s))
                c->bound[s] = map;
            else
                black_texture(c);
        }
        gl->ActiveTexture(GL_TEXTURE0);
        apply_alpha_test(c);
        c->stages_dirty = 0;
        return;
    }
    for (s = 0; s < STAGES; s++) {
        DWORD *st = c->tss[s];
        DWORD cop = st[D3DTSS_COLOROP], aop = st[D3DTSS_ALPHAOP];
        int need = uses_texture(cop, st[D3DTSS_COLORARG1], st[D3DTSS_COLORARG2]) ||
                   uses_texture(aop, st[D3DTSS_ALPHAARG1], st[D3DTSS_ALPHAARG2]);
        int on = 0;

        if (s >= (c->dx8 ? FF8_STAGES : FF_STAGES))
            off = 1;                    /* the stages after those are only for pixel shaders */
        c->bound[s] = 0;
        gl->ActiveTexture(GL_TEXTURE0 + s);
        if (off || cop == D3DTOP_DISABLE) {
            off = 1;
        } else if (need) {
            on = texture_bind(c, st[D3DTSS_TEXTUREMAP], s);
            if (on)
                c->bound[s] = st[D3DTSS_TEXTUREMAP];
        } else {
            white_texture(c);
            on = 1;
        }
        if (!on) {
            gl->Disable(GL_TEXTURE_2D);
            ff_target(c, s, 0);
            continue;
        }
        gl->Enable(GL_TEXTURE_2D);
        /* a cube map or volume (Direct3D 8 and 9) samples through its own target, which
           OpenGL's fixed-function units prefer to 2D while it is enabled */
        ff_target(c, s, c->bound[s] ? texture_kind(c, c->bound[s]) : 0);
        gl->TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        gl->TexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, factor);
        combine(gl, s, 0, cop, st[D3DTSS_COLORARG1], st[D3DTSS_COLORARG2]);
        if (aop == D3DTOP_DISABLE)
            combine(gl, s, 1, D3DTOP_SELECTARG1, D3DTA_CURRENT, D3DTA_CURRENT);
        else
            combine(gl, s, 1, aop, st[D3DTSS_ALPHAARG1], st[D3DTSS_ALPHAARG2]);
    }
    apply_fog_unit(c);
    gl->ActiveTexture(GL_TEXTURE0);
    apply_alpha_test(c);
    c->stages_dirty = 0;
}

/* Has a texture in use changed since it was uploaded (the program wrote into it)? */
static int textures_changed(RCTX *c)
{
    LPDDRAWI_DDRAWSURFACE_LCL lv[MAX_LEVELS];
    int s;

    for (s = 0; s < STAGES; s++) {
        DWORD h = c->bound[s];
        LPDDRAWI_DDRAWSURFACE_LCL l;
        TEXTURE *t;

        if (!h || h >= c->ntex)
            continue;
        l = handle_get(c->ddl, h);
        t = &c->tex[h];
        if (!l || l->lpGbl->fpVidMem != t->mem || mip_chain(l, lv) != t->levels ||
            texture_changed(c, t, l, chain_stamp(lv, t->levels)))
            return 1;
    }
    return 0;
}

/* The OpenGL texture of a surface handle goes, in every context of this process (DirectX 7's
   texture manager makes and drops thousands of surfaces a minute; their textures must not
   stay behind on the GPU). */
static void texture_forget(DWORD h)
{
    OWNER m = me();
    int i;

    for (i = 0; i < MAX_CONTEXTS; i++) {
        RCTX *c = mine(&contexts[i].o, &m) ? contexts[i].c : NULL;
        if (!c || h >= c->ntex || !c->tex[h].name)
            continue;
        make_current(c);
        gl_of(c)->DeleteTextures(1, &c->tex[h].name);
        let_go(c);
        memset(&c->tex[h], 0, sizeof c->tex[h]);
        c->ntextures--;
        c->stages_dirty = 1;
    }
}

/* A surface is written by the program (our Lock, not read-only): the textures made from it
   are uploaded again before their next use. */
void d3d_surface_written(LPDDRAWI_DDRAWSURFACE_LCL s)
{
    FLATPTR mem = s->lpGbl->fpVidMem;

    targets_written(mem, NULL);         /* goes to the GPU before the next 3D draw into it */
    textures_written(mem);
}

static void stage_state(RCTX *c, DWORD stage, DWORD state, DWORD v)
{
    if (stage < STAGES && state < 32) {
        c->tss[stage][state] = v;
        if (state == D3DTSS_ADDRESS)    /* U and V at once (DirectX 6) */
            c->tss[stage][D3DTSS_ADDRESSU] = c->tss[stage][D3DTSS_ADDRESSV] = v;
    }
    c->stages_dirty = 1;
}

/* ---- Vertices -------------------------------------------------------------------------------- */
typedef struct {
    BYTE *base;                         /* the vertex data */
    DWORD stride, count;
    int diffuse, specular;              /* byte offsets, -1 if none */
    int tex[8];                         /* byte offset of each texture coordinate set */
    int texsize[8];
    int xyz;                            /* untransformed (D3DFVF_XYZ..): the HAL transforms them */
    int shader;                         /* laid out by the vertex shader's declaration (vs_begin) */
    int normal;                         /* their normal's byte offset, -1 if none */
    GLuint vbo;                         /* the buffer object holding them (base is in start's copy), or 0 */
    BYTE *start;
    GLuint ibo;                         /* indices: the buffer object holding them, or 0 */
    GLenum itype;                       /* their type if not 16-bit ones (GL_UNSIGNED_INT), else 0 */
    long first_vertex;                  /* the vertex of stream 0 that base is at, for the other
                                           streams of the draw; -1: not a whole vertex */
    int pos;                            /* the position's byte offset (0 but for a declaration) */
    int psize_at;                       /* points: 1 + the byte offset of each vertex's size (D3DFVF_PSIZE,
                                           untransformed vertices), 0: none */
    BYTE src[12];                       /* the stream each attribute comes from, 0 but for a
                                           declaration: position, normal, diffuse, specular,
                                           texture coordinate sets 0..7 */
} VERTICES;

/* Bind a buffer object (0: none, for arrays and indices in memory) for vertices or indices. */
static void bind_buffer(RCTX *c, GLenum target, GLuint name)
{
    GLF *gl = gl_of(c);
    GLuint *bound = target == GL_ARRAY_BUFFER_ARB ? &c->array_bound : &c->element_bound;
    if (*bound != name && gl->BindBufferARB) {
        gl->BindBufferARB(target, name);
        *bound = name;
    }
}

/* Where an array of v starts: in its buffer object (an offset) or in memory. */
static const BYTE *vertex_data(RCTX *c, const VERTICES *v, DWORD first)
{
    bind_buffer(c, GL_ARRAY_BUFFER_ARB, v->vbo);
    if (v->vbo)
        return (const BYTE *)(DWORD)(v->base - v->start) + first * v->stride;
    return v->base + first * v->stride;
}

static int parse_fvf(DWORD fvf, VERTICES *v)
{
    DWORD off = 0, n, i, pos = fvf & D3DFVF_POSITION_MASK;

    v->xyz = 0;
    v->normal = -1;
    if (pos == D3DFVF_XYZRHW) {
        off = 16;
    } else if (tnl_on && pos >= D3DFVF_XYZ && pos <= D3DFVF_XYZB5 && !(pos & 1)) {
        /* Direct3D 8 and 9 with hardware vertex processing: the position, blend weights (the
           world matrix alone is used), the normal */
        v->xyz = 1;
        off = 12 + (pos >= D3DFVF_XYZB1 ? (pos - D3DFVF_XYZRHW) * 2 : 0);  /* 4 bytes a weight */
        if (fvf & D3DFVF_NORMAL) {
            v->normal = off;
            off += 12;
        }
    } else {
        return 0;
    }
    v->psize_at = 0;
    if (fvf & D3DFVF_RESERVED1) {       /* Direct3D 8's point size */
        if (v->xyz)
            v->psize_at = (int)off + 1;
        off += 4;
    }
    v->diffuse = v->specular = -1;
    if (fvf & D3DFVF_DIFFUSE) { v->diffuse = off; off += 4; }
    if (fvf & D3DFVF_SPECULAR) { v->specular = off; off += 4; }
    n = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (i = 0; i < 8; i++) {
        v->tex[i] = -1;
        v->texsize[i] = 0;
    }
    for (i = 0; i < n && i < 8; i++) {
        DWORD f = (fvf >> (16 + i * 2)) & 3;
        DWORD size = f == D3DFVF_TEXTUREFORMAT1 ? 1 : f == D3DFVF_TEXTUREFORMAT3 ? 3 :
                     f == D3DFVF_TEXTUREFORMAT4 ? 4 : 2;
        v->tex[i] = off;
        v->texsize[i] = size;
        off += size * 4;
    }
    v->stride = off;
    return 1;
}

/* e to the -x, for x >= 0, without the C run-time: (1 - x/1024) to the 1024th power. Plenty
   for fog. */
static float exp_neg(float x)
{
    float y;
    int i;

    if (x <= 0.0f)
        return 1.0f;
    y = 1.0f - x / 1024.0f;
    if (y <= 0.0f)
        return 0.0f;
    for (i = 0; i < 10; i++)
        y *= y;
    return y;
}

/* The fog factor of a vertex, 1 for none: the specular colour's alpha for vertex fog (which
   Direct3D works out), from the depth for table fog (per vertex here). */
static float fog_factor(RCTX *c, const VERTICES *v, const BYTE *b)
{
    DWORD mode = c->rs[D3DRENDERSTATE_FOGTABLEMODE];
    float f, z = ((const float *)b)[2];

    if (mode == D3DFOG_NONE) {
        f = v->specular >= 0 ? b[v->specular + 3] / 255.0f : 1.0f;
    } else if (mode == D3DFOG_LINEAR) {
        float start = *(float *)&c->rs[D3DRENDERSTATE_FOGSTART];
        float end = *(float *)&c->rs[D3DRENDERSTATE_FOGEND];
        f = end != start ? (end - z) / (end - start) : 1.0f;
    } else {
        float d = *(float *)&c->rs[D3DRENDERSTATE_FOGDENSITY] * z;
        f = exp_neg(mode == D3DFOG_EXP2 ? d * d : d);
    }
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

/* Direct3D adds the specular colour before fog, OpenGL's colour sum after the texture units
   and so after FOG_UNIT: fog and specular together draw with the specular colour times the
   vertex's fog factor f (convert). tex * f + fog * (1 - f) + spec * f is Direct3D's
   (tex + spec) * f + fog * (1 - f), apart from clamping. */
static int fogged_specular(RCTX *c, const VERTICES *v)
{
    return c->rs[D3DRENDERSTATE_FOGENABLE] && c->rs[D3DRENDERSTATE_SPECULARENABLE] &&
           v->specular >= 0;
}

/* Positions as OpenGL wants them for perspective-correct interpolation: x, y, z times w, and
   w, from the screen coordinates and 1/w. With fog, the fog unit's texture coordinates follow
   (the fog factor f, between the ramp's two texel centres: 0.25 + f / 2), and with specular
   too its red, green and blue times f, as floats (fogged_specular). */
static float *convert(RCTX *c, const VERTICES *v, DWORD first, DWORD count)
{
    int spec = fogged_specular(c, v);
    DWORD i, need = count * (spec ? 8 : 5);
    float *p;
    BYTE *b;

    if (need > c->npos) {
        release(c->pos);
        c->npos = need + 1024;
        c->pos = (float *)alloc(c->npos * sizeof(float));
        if (!c->pos) {
            c->npos = 0;
            return NULL;
        }
    }
    p = c->pos;
    b = v->base + first * v->stride;
    for (i = 0; i < count; i++, p += 4, b += v->stride) {
        float *f = (float *)b;
        float w = f[3] > 0.0f ? 1.0f / f[3] : 1.0f;
        p[0] = f[0] * w;
        p[1] = f[1] * w;
        p[2] = f[2] * w;
        p[3] = w;
    }
    if (c->rs[D3DRENDERSTATE_FOGENABLE]) {
        float *fog = c->pos + count * 4;     /* after the positions */
        float *s = fog + count;              /* then the specular colours */
        b = v->base + first * v->stride;
        for (i = 0; i < count; i++, b += v->stride) {
            float f = fog_factor(c, v, b);
            fog[i] = 0.25f + 0.5f * f;
            if (spec) {
                const BYTE *bgra = b + v->specular;
                f /= 255.0f;
                s[0] = bgra[2] * f;
                s[1] = bgra[1] * f;
                s[2] = bgra[0] * f;
                s += 3;
            }
        }
    }
    return c->pos;
}

/* Point OpenGL's arrays at vertices first..first+count of v (converted already: positions
   at the start of c->pos). */
static void arrays(RCTX *c, const VERTICES *v, DWORD first, DWORD count)
{
    GLF *gl = gl_of(c);
    const BYTE *b = vertex_data(c, v, first);
    int s;

    attribs_off(c);
    gl->EnableClientState(GL_VERTEX_ARRAY);
    gl->VertexPointer(4, GL_FLOAT, 0, c->pos);
    if (v->diffuse >= 0) {
        gl->EnableClientState(GL_COLOR_ARRAY);
        gl->ColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, v->stride, b + v->diffuse);
    } else {
        gl->DisableClientState(GL_COLOR_ARRAY);
        gl->Color4ub(255, 255, 255, 255);
    }
    if (v->specular >= 0 && gl->SecondaryColorPointer) {
        gl->EnableClientState(GL_SECONDARY_COLOR_ARRAY);
        if (fogged_specular(c, v))      /* times the fog factor, after the fog coordinates */
            gl->SecondaryColorPointer(3, GL_FLOAT, 0, c->pos + count * 5);
        else
            gl->SecondaryColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, v->stride, b + v->specular);
    } else {
        gl->DisableClientState(GL_SECONDARY_COLOR_ARRAY);
    }
    /* The fog values (0.25 + 0.5 f, see convert): the fixed-function fog unit's coordinates, or,
       under a pixel shader, whose stages may use that unit, OpenGL's fog coordinate. */
    if (c->fp_on && gl->FogCoordPointer) {
        if (c->rs[D3DRENDERSTATE_FOGENABLE]) {
            gl->Fogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
            gl->EnableClientState(GL_FOG_COORDINATE_ARRAY);
            gl->FogCoordPointer(GL_FLOAT, 0, c->pos + count * 4);
        } else {
            gl->DisableClientState(GL_FOG_COORDINATE_ARRAY);
        }
    } else {
        if (gl->FogCoordPointer)
            gl->DisableClientState(GL_FOG_COORDINATE_ARRAY);
        gl->ClientActiveTexture(GL_TEXTURE0 + FOG_UNIT);
        if (c->rs[D3DRENDERSTATE_FOGENABLE]) {
            gl->EnableClientState(GL_TEXTURE_COORD_ARRAY);
            gl->TexCoordPointer(1, GL_FLOAT, 0, c->pos + count * 4);
        } else {
            gl->DisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    }
    for (s = 0; s < STAGES; s++) {
        DWORD set = c->tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF;
        if (s == FOG_UNIT && !c->fp_on)
            continue;                   /* the fog unit's, set above */
        gl->ClientActiveTexture(GL_TEXTURE0 + s);
        if (set < 8 && v->tex[set] >= 0) {
            gl->EnableClientState(GL_TEXTURE_COORD_ARRAY);
            gl->TexCoordPointer(v->texsize[set], GL_FLOAT, v->stride,
                                b + v->tex[set]);
        } else {
            gl->DisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    }
    gl->ClientActiveTexture(GL_TEXTURE0);
}

static void clip_state(RCTX *c, int kind);

/* The render target's memory into the GPU's picture: the program drew into the surface
   itself (text with GDI, a locked surface) since the last read-back, and the next 3D draw
   would otherwise be read back over it. A plain copy: no depth test, blending or texturing. */
static void rt_upload(RCTX *c)
{
    GLF *gl = gl_of(c);
    LPDDRAWI_DDRAWSURFACE_GBL g;
    DDPIXELFORMAT pf;
    PIXFMT f;
    FLATPTR mem;
    int u;

    if (!c->cur)
        return;
    c->cur->written = 0;
    mem = c->cur->mem;                  /* the attached buffer's memory, as in read_back */
    if (!c->rt || !gl->WindowPos2i)
        return;
    g = c->rt->lpGbl;
    surface_format(c->rt, &pf);
    f = gl_format(&pf);
    if (!f.bytes || !mem)
        return;
    tracef("rd3d: the program's drawing in %08lx back to the GPU\r\n", mem);
    gl->Disable(GL_DEPTH_TEST);
    gl->Disable(GL_BLEND);
    gl->Disable(GL_ALPHA_TEST);
    gl->Disable(GL_COLOR_SUM);
    gl->Disable(GL_SCISSOR_TEST);       /* all of the target, not only the viewport */
    gl->Disable(GL_STENCIL_TEST);
    clip_state(c, 0);                   /* nor behind a user clip plane of the last draw */
    for (u = 0; u <= FOG_UNIT; u++) {
        gl->ActiveTexture(GL_TEXTURE0 + u);
        gl->Disable(GL_TEXTURE_2D);
        ff_target(c, u, 0);
    }
    gl->ActiveTexture(GL_TEXTURE0);
    gl->WindowPos2i(0, 0);              /* row 0 of the memory is the GPU's row 0 (see framebuffer) */
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, g->lPitch / f.bytes);
    colour_mask(c, 1);                  /* the memory's picture, every channel */
    gl->DrawPixels(c->fb_w < g->wWidth ? c->fb_w : g->wWidth,
                   c->fb_h < g->wHeight ? c->fb_h : g->wHeight, f.format, f.type,
                   (const GLvoid *)mem);
    colour_mask(c, 0);
    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    apply_depth_test(c);
    enable(gl, GL_BLEND, c->rs[D3DRENDERSTATE_ALPHABLENDENABLE]);
    apply_alpha_test(c);
    enable(gl, GL_COLOR_SUM, c->rs[D3DRENDERSTATE_SPECULARENABLE]);
    apply_scissor(c);
    enable(gl, GL_STENCIL_TEST, c->rs[D3DRENDERSTATE_STENCILENABLE]);
    c->stages_dirty = 1;
    c->dirty = 1;
}

/* ---- Hardware vertex processing --------------------------------------------------------------- */
/* With D3D8 = "2" the Direct3D 8 and 9 devices process vertices "in hardware": the runtime sends
   untransformed vertices with the world, view and projection matrices, the lights and the
   material, and the HAL draws them through an ARB vertex program that does Direct3D's fixed-
   function work (tnl_arb.c). Its state is in program.env; OpenGL's viewport and depth range are
   Direct3D's while such programs draw (vp_on), the whole target's for transformed vertices. */

/* Square root and cosine without the C run-time: Newton's method from a guess by halving the
   exponent; the cosine's series, enough for the half angles of a spot light (0 to pi/2). */
static float f_sqrt(float x)
{
    float y;
    DWORD i;
    int k;
    if (x <= 0.0f)
        return 0.0f;
    i = 0x1FBD1DF5UL + (*(DWORD *)&x >> 1);
    y = *(float *)&i;
    for (k = 0; k < 3; k++)
        y = 0.5f * (y + x / y);
    return y;
}

static float f_cos(float x)
{
    float x2, term = 1.0f, sum = 1.0f;
    int k;
    if (x < 0.0f)
        x = -x;
    if (x > 3.14159265f)
        x = 3.14159265f;
    x2 = x * x;
    for (k = 1; k <= 9; k++) {
        term *= -x2 / (float)((2 * k - 1) * (2 * k));
        sum += term;
    }
    return sum;
}

static void mat_identity(float *m)
{
    int i;
    for (i = 0; i < 16; i++)
        m[i] = (i % 5) ? 0.0f : 1.0f;
}

/* out = a b, Direct3D's row-major matrices (vectors are rows: v a b) */
static void mat_mul(const float *a, const float *b, float *out)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] +
                             a[i * 4 + 3] * b[12 + j];
}

/* out = the inverse of m; 0 if m has none. */
static int mat_invert(const float *m, float *out)
{
    double a[4][8], t, best;
    int i, j, k, p;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            a[i][j] = m[i * 4 + j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    for (i = 0; i < 4; i++) {           /* Gauss-Jordan, the largest pivot of the column first */
        p = i;
        best = a[i][i] < 0.0 ? -a[i][i] : a[i][i];
        for (k = i + 1; k < 4; k++) {
            t = a[k][i] < 0.0 ? -a[k][i] : a[k][i];
            if (t > best) {
                best = t;
                p = k;
            }
        }
        if (best < 1e-30)
            return 0;
        if (p != i)
            for (j = 0; j < 8; j++) {
                t = a[i][j];
                a[i][j] = a[p][j];
                a[p][j] = t;
            }
        t = 1.0 / a[i][i];
        for (j = 0; j < 8; j++)
            a[i][j] *= t;
        for (k = 0; k < 4; k++)
            if (k != i && a[k][i] != 0.0) {
                t = a[k][i];
                for (j = 0; j < 8; j++)
                    a[k][j] -= t * a[i][j];
            }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            out[i * 4 + j] = (float)a[i][j + 4];
    return 1;
}

static void tnl_reset(RCTX *c)
{
    DWORD s;
    mat_identity(c->xf_world);
    mat_identity(c->xf_view);
    mat_identity(c->xf_proj);
    for (s = 0; s < TNL_STAGES; s++)
        mat_identity(c->xf_tex[s]);
    c->zmin = 0.0f;
    c->zmax = 1.0f;
    c->tnl_dirty = TNL_DIRTY_ALL;
}

static float *tnl_matrix(RCTX *c, DWORD type)
{
    if (type == D3DTRANSFORMSTATE_WORLD || type == 256)     /* 256: Direct3D 8's D3DTS_WORLD */
        return c->xf_world;
    if (type == D3DTRANSFORMSTATE_VIEW)
        return c->xf_view;
    if (type == D3DTRANSFORMSTATE_PROJECTION)
        return c->xf_proj;
    if (type >= D3DTRANSFORMSTATE_TEXTURE0 && type < D3DTRANSFORMSTATE_TEXTURE0 + TNL_STAGES)
        return c->xf_tex[type - D3DTRANSFORMSTATE_TEXTURE0];
    return NULL;                        /* the other world matrices: no vertex blending */
}

/* SETTRANSFORM, or MULTIPLYTRANSFORM (the new matrix is m times the old one). */
static void tnl_transform(RCTX *c, DWORD type, const float *m, int multiply)
{
    float *t = tnl_matrix(c, type), r[16];
    if (!t)
        return;
    if (multiply) {
        mat_mul(m, t, r);
        memcpy(t, r, sizeof r);
    } else {
        memcpy(t, m, sizeof r);
    }
    if (t == c->xf_view)
        c->tnl_dirty |= TNL_DIRTY_XF | TNL_DIRTY_LIGHT; /* the lights are kept in view space */
    else if (t == c->xf_world || t == c->xf_proj)
        c->tnl_dirty |= TNL_DIRTY_XF;
    else
        c->tnl_dirty |= TNL_DIRTY_TEX;
}

/* The slot of Direct3D's light index, made if create (a free one, else one whose light is
   off); -1 if there is none. */
static int light_slot(RCTX *c, DWORD index, int create)
{
    int i, free = -1, off = -1;
    for (i = 0; i < MAX_LIGHTS; i++) {
        if (c->light_used[i] && c->light_index[i] == index)
            return i;
        if (!c->light_used[i] && free < 0)
            free = i;
        else if (c->light_used[i] && !c->light_on[i] && off < 0)
            off = i;
    }
    if (!create)
        return -1;
    if (free < 0)
        free = off;
    if (free < 0) {
        if (!c->lights_full++)
            dbg("rd3d: more lights on than the HAL keeps\r\n");
        return -1;
    }
    memset(&c->light[free], 0, sizeof c->light[0]);
    c->light_on[free] = 0;
    c->light_used[free] = 1;
    c->light_index[free] = index;
    return free;
}

static void tnl_light(RCTX *c, const D3DHAL_DP2SETLIGHT *l, const D3DLIGHT7 *data)
{
    int i = light_slot(c, l->dwIndex, 1);
    if (i < 0)
        return;
    if (l->dwDataType == D3DHAL_SETLIGHT_ENABLE)
        c->light_on[i] = 1;
    else if (l->dwDataType == D3DHAL_SETLIGHT_DISABLE)
        c->light_on[i] = 0;
    else if (l->dwDataType == D3DHAL_SETLIGHT_DATA && data)
        c->light[i] = *data;
    c->tnl_dirty |= TNL_DIRTY_LIGHT;
}

/* What the vertex program for the next draw of v does. */
static void tnl_key(RCTX *c, const VERTICES *v, TNL_KEY *k)
{
    DWORD i, n = 0, s;

    memset(k, 0, sizeof *k);
    k->diffuse = v->diffuse >= 0;
    k->specular = v->specular >= 0;
    k->normal = v->normal >= 0;
    if (c->rs[D3DRENDERSTATE_LIGHTING]) {
        k->lighting = 1;
        k->normalize = c->rs[D3DRENDERSTATE_NORMALIZENORMALS] != 0;
        k->local_viewer = c->rs[D3DRENDERSTATE_LOCALVIEWER] != 0;
        if (c->rs[D3DRENDERSTATE_COLORVERTEX]) {
            static const DWORD src[4] = { D3DRENDERSTATE_DIFFUSEMATERIALSOURCE,
                D3DRENDERSTATE_AMBIENTMATERIALSOURCE, D3DRENDERSTATE_SPECULARMATERIALSOURCE,
                D3DRENDERSTATE_EMISSIVEMATERIALSOURCE };
            for (i = 0; i < 4; i++) {
                DWORD m = c->rs[src[i]];
                k->mcs[i] = (BYTE)(m == D3DMCS_COLOR1 && k->diffuse ? TNL_COLOR1 :
                                   m == D3DMCS_COLOR2 && k->specular ? TNL_COLOR2 : TNL_MATERIAL);
            }
        }
        for (i = 0; i < MAX_LIGHTS && n < TNL_LIGHTS; i++) {
            DWORD type = c->light[i].dltType;
            if (!c->light_on[i] || type < D3DLIGHT_POINT || type > D3DLIGHT_DIRECTIONAL)
                continue;
            k->light[n] = (BYTE)type;
            if (c->light_place[n] != i) {
                c->light_place[n] = (BYTE)i;
                c->tnl_dirty |= TNL_DIRTY_LIGHT;
            }
            n++;
        }
    }
    if (n != c->lights_loaded)
        c->tnl_dirty |= TNL_DIRTY_LIGHT;
    if (c->rs[D3DRENDERSTATE_FOGENABLE]) {
        DWORD table = c->rs[D3DRENDERSTATE_FOGTABLEMODE], vtx = c->rs[D3DRENDERSTATE_FOGVERTEXMODE];
        if (table >= D3DFOG_EXP && table <= D3DFOG_LINEAR) {
            k->fog = (BYTE)table;       /* per vertex here, from the depth buffer's z */
            k->fog_depth = TNL_DEPTH_SCREEN;
        } else if (vtx >= D3DFOG_EXP && vtx <= D3DFOG_LINEAR) {
            k->fog = (BYTE)vtx;
            k->fog_depth = c->rs[D3DRENDERSTATE_RANGEFOGENABLE] ? TNL_DEPTH_RANGE : TNL_DEPTH_VIEW;
        } else {
            k->fog = TNL_FOG_ALPHA;
        }
        k->fog_coord = c->fp_on != 0;   /* fragment programs read OpenGL's fog coordinate */
        k->fog_unit = FOG_UNIT;
        k->fog_specular = c->rs[D3DRENDERSTATE_SPECULARENABLE] && (k->specular || k->lighting);
    }
    for (s = 0; s < TNL_STAGES; s++) {
        DWORD tci = c->tss[s][D3DTSS_TEXCOORDINDEX], set = tci & 0xFFFF, flags;
        switch (tci & 0xFFFF0000UL) {
        case D3DTSS_TCI_CAMERASPACENORMAL:           k->tex[s] = TNL_TEX_NORMAL; break;
        case D3DTSS_TCI_CAMERASPACEPOSITION:         k->tex[s] = TNL_TEX_POSITION; break;
        case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: k->tex[s] = TNL_TEX_REFLECTION; break;
        default:
            if (set < 8 && v->tex[set] >= 0) {
                k->tex[s] = (BYTE)(1 + set);
                k->tex_size[s] = (BYTE)v->texsize[set];
            }
            break;
        }
        flags = c->tss[s][D3DTSS_TEXTURETRANSFORMFLAGS];
        if (k->tex[s] && (flags & 7))
            k->xform[s] = (BYTE)((flags & 7) | (flags & D3DTTFF_PROJECTED ? TNL_PROJECTED : 0));
    }
    k->point_scale = (BYTE)(c->points_now && c->rs[RS_POINTSCALEENABLE]);
    k->point_vertex = (BYTE)(c->points_now && v->psize_at);
}

/* The program for k: kept ones by their key (VP_CACHE, the least recently used goes), new ones
   made now. 0 if OpenGL refused it (remembered, so it isn't tried again each draw). */
static GLuint tnl_program(RCTX *c, const TNL_KEY *k)
{
    GLF *gl = gl_of(c);
    DWORD i, slot = 0;
    char *text;
    unsigned int n;
    GLuint prog = 0;

    for (i = 0; i < VP_CACHE; i++)
        if (c->vp[i].used && !memcmp(&c->vp[i].key, k, sizeof *k)) {
            c->vp[i].used = ++c->vp_clock;
            return c->vp[i].prog;
        }
    for (i = 1; i < VP_CACHE; i++)
        if (c->vp[i].used < c->vp[slot].used)
            slot = i;
    if (c->vp[slot].prog) {
        if (c->vp_bound == c->vp[slot].prog)
            c->vp_bound = 0;
        gl->DeleteProgramsARB(1, &c->vp[slot].prog);
        c->vp[slot].prog = 0;
    }
    text = (char *)alloc(TNL_TEXT_BYTES);
    n = text ? tnl_text(k, text, TNL_TEXT_BYTES) : 0;
    if (n) {
        gl->GetError();                 /* earlier errors aren't this one's */
        gl->GenProgramsARB(1, &prog);
        gl->BindProgramARB(GL_VERTEX_PROGRAM_ARB, prog);
        c->vp_bound = prog;
        gl->ProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, n - 1, text);
        if (gl->GetError() != GL_NO_ERROR) {
            const char *e = (const char *)gl->GetString(GL_PROGRAM_ERROR_STRING_ARB);
            dbg("rd3d: a vertex program was refused: ");
            dbg(e ? e : "?");
            dbg("\r\n");
            if (trace_on)
                dbg(text);
            gl->DeleteProgramsARB(1, &prog);
            c->vp_bound = prog = 0;
        } else {
            tracef("rd3d: vertex program %lu: lighting %lu, fog %lu, texture coordinates %lu %lu\r\n",
                   (DWORD)prog, (DWORD)k->lighting, (DWORD)k->fog, (DWORD)k->tex[0], (DWORD)k->tex[1]);
        }
    } else {
        dbg("rd3d: a vertex program too long\r\n");
    }
    release(text);
    memcpy(&c->vp[slot].key, k, sizeof *k);
    c->vp[slot].prog = prog;
    c->vp[slot].used = ++c->vp_clock;
    return prog;
}

static void env4(GLF *gl, DWORD i, float x, float y, float z, float w)
{
    GLfloat v[4];
    v[0] = x;
    v[1] = y;
    v[2] = z;
    v[3] = w;
    gl->ProgramEnvParameter4fvARB(GL_VERTEX_PROGRAM_ARB, i, v);
}

/* The columns of m, the program's DP4 rows. */
static void env_columns(GLF *gl, DWORD at, const float *m)
{
    DWORD j;
    for (j = 0; j < 4; j++)
        env4(gl, at + j, m[j], m[4 + j], m[8 + j], m[12 + j]);
}

static void env_color(GLF *gl, DWORD i, const D3DCOLORVALUE *col)
{
    env4(gl, i, col->r, col->g, col->b, col->a);
}

static void env_argb(GLF *gl, DWORD i, DWORD argb)
{
    env4(gl, i, ((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f,
         (argb >> 24) / 255.0f);
}

/* program.env for the next draw: what changed since the last load (tnl_dirty). */
static void tnl_load(RCTX *c, const TNL_KEY *k)
{
    GLF *gl = gl_of(c);
    float wv[16], wvp[16], a[16], m[16];
    DWORD i, n;

    if (c->tnl_dirty & TNL_DIRTY_XF) {
        float w = (float)(c->vp_w ? c->vp_w : c->fb_w), h = (float)(c->vp_w ? c->vp_h : c->fb_h);
        float c00, c01, c02, c10, c11, c12, c20, c21, c22, det;
        mat_mul(c->xf_world, c->xf_view, wv);
        mat_mul(wv, c->xf_proj, wvp);
        /* Direct3D's clip space to OpenGL's: y up (the target's row 0 is OpenGL's row 0, see
           framebuffer), z from -w to w, and pixel centres half a pixel in (Direct3D's are on
           whole numbers) */
        mat_identity(a);
        a[5] = -1.0f;
        a[10] = 2.0f;
        a[14] = -1.0f;
        a[12] = w > 0.0f ? 1.0f / w : 0.0f;
        a[13] = h > 0.0f ? 1.0f / h : 0.0f;
        mat_mul(wvp, a, m);
        env_columns(gl, TNL_ENV_MVP, m);
        env_columns(gl, TNL_ENV_MV, wv);
        /* normals: n times the inverse transpose of world x view's 3x3, whose columns are the
           inverse's rows */
        c00 = wv[5] * wv[10] - wv[6] * wv[9];
        c01 = wv[6] * wv[8] - wv[4] * wv[10];
        c02 = wv[4] * wv[9] - wv[5] * wv[8];
        c10 = wv[2] * wv[9] - wv[1] * wv[10];
        c11 = wv[0] * wv[10] - wv[2] * wv[8];
        c12 = wv[1] * wv[8] - wv[0] * wv[9];
        c20 = wv[1] * wv[6] - wv[2] * wv[5];
        c21 = wv[2] * wv[4] - wv[0] * wv[6];
        c22 = wv[0] * wv[5] - wv[1] * wv[4];
        det = wv[0] * c00 + wv[1] * c01 + wv[2] * c02;
        det = det != 0.0f ? 1.0f / det : 0.0f;
        /* the inverse is the cofactors' transpose over det: its row i is (c0i, c1i, c2i) */
        env4(gl, TNL_ENV_NORMAL + 0, c00 * det, c10 * det, c20 * det, 0.0f);
        env4(gl, TNL_ENV_NORMAL + 1, c01 * det, c11 * det, c21 * det, 0.0f);
        env4(gl, TNL_ENV_NORMAL + 2, c02 * det, c12 * det, c22 * det, 0.0f);
    }
    if (c->tnl_dirty & TNL_DIRTY_LIGHT) {
        const float *v = c->xf_view;
        env_color(gl, TNL_ENV_MAT + 0, &c->material.dcvDiffuse);
        env_color(gl, TNL_ENV_MAT + 1, &c->material.dcvAmbient);
        env_color(gl, TNL_ENV_MAT + 2, &c->material.dcvSpecular);
        env_color(gl, TNL_ENV_MAT + 3, &c->material.dcvEmissive);
        env4(gl, TNL_ENV_POWER, c->material.dvPower, 0.0f, 0.0f, 0.0f);
        env_argb(gl, TNL_ENV_AMBIENT, c->rs[D3DRENDERSTATE_AMBIENT]);
        for (n = 0; n < TNL_LIGHTS && k->light[n]; n++) {
            const D3DLIGHT7 *l = &c->light[c->light_place[n]];
            DWORD b = TNL_ENV_LIGHT + n * TNL_LIGHT_SIZE;
            float px = l->dvPosition.x, py = l->dvPosition.y, pz = l->dvPosition.z;
            float dx = l->dvDirection.x, dy = l->dvDirection.y, dz = l->dvDirection.z;
            float vx, vy, vz, len, ct, cp;
            env_color(gl, b + 0, &l->dcvDiffuse);
            env_color(gl, b + 1, &l->dcvSpecular);
            env_color(gl, b + 2, &l->dcvAmbient);
            /* into view space: the position with the view's translation, the direction without */
            env4(gl, b + 3, px * v[0] + py * v[4] + pz * v[8] + v[12],
                 px * v[1] + py * v[5] + pz * v[9] + v[13], px * v[2] + py * v[6] + pz * v[10] + v[14],
                 l->dvRange);
            vx = dx * v[0] + dy * v[4] + dz * v[8];
            vy = dx * v[1] + dy * v[5] + dz * v[9];
            vz = dx * v[2] + dy * v[6] + dz * v[10];
            len = f_sqrt(vx * vx + vy * vy + vz * vz);
            len = len > 0.0f ? 1.0f / len : 0.0f;
            if (l->dltType == D3DLIGHT_DIRECTIONAL)
                len = -len;                     /* towards the light */
            env4(gl, b + 4, vx * len, vy * len, vz * len, 0.0f);
            env4(gl, b + 5, l->dvAttenuation0, l->dvAttenuation1, l->dvAttenuation2,
                 l->dvFalloff > 0.000001f ? l->dvFalloff : 0.000001f);
            ct = f_cos(l->dvTheta * 0.5f);
            cp = f_cos(l->dvPhi * 0.5f);
            env4(gl, b + 6, ct, cp, ct - cp > 0.000001f ? 1.0f / (ct - cp) : 1000000.0f, 0.0f);
        }
        c->lights_loaded = n;
    }
    if (c->tnl_dirty & TNL_DIRTY_FOG) {
        float s = *(float *)&c->rs[D3DRENDERSTATE_FOGSTART], e = *(float *)&c->rs[D3DRENDERSTATE_FOGEND];
        env4(gl, TNL_ENV_FOG, e != s ? e / (e - s) : 1.0f, e != s ? 1.0f / (e - s) : 0.0f,
             *(float *)&c->rs[D3DRENDERSTATE_FOGDENSITY], 0.0f);
    }
    if (c->tnl_dirty & TNL_DIRTY_TEX)
        for (i = 0; i < TNL_STAGES; i++)
            env_columns(gl, TNL_ENV_TEX + 4 * i, c->xf_tex[i]);
    if (k->point_scale || k->point_vertex) {    /* each such draw: points are few */
        float lo = *(const float *)&c->rs[RS_POINTSIZE_MIN], hi = *(const float *)&c->rs[RS_POINTSIZE_MAX];
        if (!(hi >= 1.0f) || hi > MAX_POINT_SIZE)
            hi = MAX_POINT_SIZE;
        if (!(lo >= 1.0f))
            lo = 1.0f;
        if (lo > hi)
            lo = hi;
        env4(gl, TNL_ENV_POINT, (float)(c->vp_w ? c->vp_h : c->fb_h) * *(const float *)&c->rs[RS_POINTSIZE],
             lo, hi, 1.0e-12f);
        env4(gl, TNL_ENV_POINT + 1, *(const float *)&c->rs[RS_POINTSCALE_A],
             *(const float *)&c->rs[RS_POINTSCALE_A + 1], *(const float *)&c->rs[RS_POINTSCALE_A + 2],
             (float)(c->vp_w ? c->vp_h : c->fb_h));
    }
    c->tnl_dirty = 0;
}

/* Before a draw of untransformed vertices: the program bound and on, its state loaded, the
   viewport Direct3D's. 0 if it can't be drawn this way. */
static int tnl_begin(RCTX *c, const VERTICES *v)
{
    GLF *gl = gl_of(c);
    TNL_KEY k;
    GLuint prog;

    if (!gl->GenProgramsARB || !gl->ProgramStringARB || !gl->BindProgramARB || !gl->DeleteProgramsARB ||
        !gl->ProgramEnvParameter4fvARB || !gl->NormalPointer || !gl->DepthRange)
        return 0;
    tnl_key(c, v, &k);
    prog = tnl_program(c, &k);
    if (!prog)
        return 0;
    if (c->env_owner == ENV_VS) {       /* a Direct3D 9 shader's constants were over this state */
        c->tnl_dirty = TNL_DIRTY_ALL;
        c->env_owner = ENV_TNL;
    }
    if (c->vp_bound != prog) {
        gl->BindProgramARB(GL_VERTEX_PROGRAM_ARB, prog);
        c->vp_bound = prog;
    }
    if (!c->vp_on) {
        gl->Enable(GL_VERTEX_PROGRAM_ARB);
        c->vp_on = 1;
        c->tnl_dirty |= TNL_DIRTY_XF;
    }
    if (c->tnl_dirty & TNL_DIRTY_XF) {
        if (c->vp_w)
            gl->Viewport(c->vp_x, c->vp_y, c->vp_w, c->vp_h);
        else
            gl->Viewport(0, 0, c->fb_w, c->fb_h);
        gl->DepthRange(c->zmin, c->zmax);
    }
    tnl_load(c, &k);
    return 1;
}

/* Back to transformed vertices: no program, the whole target's viewport. */
static void tnl_end(RCTX *c)
{
    GLF *gl;
    if (!c->vp_on)
        return;
    gl = gl_of(c);
    gl->Disable(GL_VERTEX_PROGRAM_ARB);
    gl->Viewport(0, 0, c->fb_w, c->fb_h);
    gl->DepthRange(0.0, 1.0);
    gl->DisableClientState(GL_NORMAL_ARRAY);
    c->vp_on = 0;
}

/* The generic attribute arrays a vertex shader drew with, off (attribute 0 would stand for the
   position of the other draws). */
static void attribs_off(RCTX *c)
{
    GLF *gl = gl_of(c);
    DWORD n;
    for (n = 0; c->attribs_on && n < VS_ARB_INPUTS; n++)
        if (c->attribs_on & (1UL << n)) {
            gl->DisableVertexAttribArrayARB(n);
            c->attribs_on &= ~(1UL << n);
        }
}

/* ---- Vertex shaders on the GPU (with tnl_on) ---- */
static struct vs8 *vs_find(RCTX *c, DWORD handle)
{
    DWORD i;
    for (i = 0; handle && i < MAX_VS; i++)
        if (c->vs[i] && c->vs[i]->handle == handle)
            return c->vs[i];
    return NULL;
}

static void vs_delete(RCTX *c, DWORD handle)
{
    GLF *gl = gl_of(c);
    DWORD i, j;
    for (i = 0; handle && i < MAX_VS; i++)
        if (c->vs[i] && c->vs[i]->handle == handle) {
            struct vs8 *s = c->vs[i];
            for (j = 0; j < s->nvar; j++) {
                if (s->var[j].prog && gl->DeleteProgramsARB)
                    gl->DeleteProgramsARB(1, &s->var[j].prog);
                if (c->vp_bound == s->var[j].prog)
                    c->vp_bound = 0;
            }
            if (c->vs_loaded == handle)
                c->vs_loaded = 0;
            release(s->decl);
            release(s->code);
            release(s);
            c->vs[i] = NULL;
        }
}

/* The program for s with fog key key, made when first needed; 0 if it can't be made. */
static GLuint vs_program(RCTX *c, struct vs8 *s, DWORD key)
{
    GLF *gl = gl_of(c);
    DWORD i;
    char *text;
    unsigned int n;
    GLuint prog = 0;

    for (i = 0; i < s->nvar; i++)
        if (s->var[i].key == key)
            return s->var[i].prog;
    if (s->failed)
        return 0;
    if (s->nvar == VS_VARIANTS) {       /* full: the last one makes room */
        i = VS_VARIANTS - 1;
        if (s->var[i].prog)
            gl->DeleteProgramsARB(1, &s->var[i].prog);
        if (c->vp_bound == s->var[i].prog)
            c->vp_bound = 0;
        s->nvar--;
    }
    text = (char *)alloc(VS_ARB_TEXT_BYTES);
    n = text ? vs_to_arb((const unsigned int *)s->code, s->ncode, (const unsigned int *)s->decl,
                         s->ndecl, key, text, VS_ARB_TEXT_BYTES, &s->info) : 0;
    if (!n) {
        tracef("rd3d: vertex shader %lx not translated: reason %lu, detail %lx\r\n", s->handle,
               (DWORD)s->info.reason, (DWORD)s->info.detail);
        dbg("rd3d: a vertex shader was not translated, reason ");
        dbg_hex(s->info.reason);
        dbg(", detail ");
        dbg_hex(s->info.detail);
        dbg("\r\n");
        s->failed = 1;
    } else {
        gl->GetError();
        gl->GenProgramsARB(1, &prog);
        gl->BindProgramARB(GL_VERTEX_PROGRAM_ARB, prog);
        c->vp_bound = prog;
        gl->ProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, n - 1, text);
        if (gl->GetError() != GL_NO_ERROR) {
            const char *e = (const char *)gl->GetString(GL_PROGRAM_ERROR_STRING_ARB);
            dbg("rd3d: a vertex shader's program was refused: ");
            dbg(e ? e : "?");
            dbg("\r\n");
            if (trace_on)
                dbg(text);
            gl->DeleteProgramsARB(1, &prog);
            c->vp_bound = prog = 0;
            s->failed = 1;
        } else {
            tracef("rd3d: vertex shader %lx: program %lu, key %lx, inputs %lx\r\n", s->handle,
                   (DWORD)prog, key, (DWORD)s->info.inputs);
            i = s->nvar++;
            s->var[i].key = key;
            s->var[i].prog = prog;
        }
    }
    release(text);
    return prog;
}

static void vs_create(RCTX *c, const RB_DP2_CREATE_VERTEX_SHADER *r)
{
    const BYTE *p = (const BYTE *)(r + 1);
    struct vs8 *s;
    DWORD i;

    vs_delete(c, r->handle);
    for (i = 0; i < MAX_VS && c->vs[i]; i++)
        ;
    if (i == MAX_VS || !(s = (struct vs8 *)alloc(sizeof *s))) {
        dbg("rd3d: too many vertex shaders\r\n");
        return;
    }
    s->handle = r->handle;
    s->decl = (DWORD *)alloc(r->declBytes + 4);
    s->code = (DWORD *)alloc(r->codeBytes + 4);
    if (!s->decl || !s->code) {
        release(s->decl);
        release(s->code);
        release(s);
        return;
    }
    memcpy(s->decl, p, r->declBytes);
    memcpy(s->code, p + r->declBytes, r->codeBytes);
    s->ndecl = r->declBytes / 4;
    s->ncode = r->codeBytes / 4;
    c->vs[i] = s;
    if (!s->ncode) {                    /* a declaration alone, for the fixed-function pipeline
                                           (decl_vertices) */
        if (!vs_declared((const unsigned int *)s->decl, s->ndecl, &s->info)) {
            s->failed = 1;
            dbg("rd3d: a vertex declaration can't be read\r\n");
        }
        return;
    }
    vs_program(c, s, 0);                /* now, so a shader that can't be made says so early */
}

/* Before a draw through a vertex shader: its program bound and on, c0..c255 and its def
   constants in program.env, OpenGL's viewport Direct3D's. 0 if it can't be drawn. */
static int vs_begin(RCTX *c)
{
    GLF *gl = gl_of(c);
    struct vs8 *s = vs_find(c, c->fvf8);
    DWORD key = 0, i, lo, hi, table = c->rs[D3DRENDERSTATE_FOGTABLEMODE];
    GLuint prog;
    GLfloat v[4];

    if (!s || !gl->VertexAttribPointerARB || !gl->EnableVertexAttribArrayARB ||
        !gl->DisableVertexAttribArrayARB || !gl->ProgramLocalParameter4fvARB || !gl->DepthRange)
        return 0;
    if (c->rs[D3DRENDERSTATE_FOGENABLE]) {
        key = VS_ARB_FOG | (FOG_UNIT << VS_ARB_UNIT_SHIFT);
        if (c->fp_on)
            key |= VS_ARB_FOG_COORD;
        if (table >= D3DFOG_EXP && table <= D3DFOG_LINEAR)
            key |= table << VS_ARB_TABLE_SHIFT;
        if (c->rs[D3DRENDERSTATE_SPECULARENABLE])
            key |= VS_ARB_FOG_SPEC;
    }
    prog = vs_program(c, s, key);
    if (!prog)
        return 0;
    if (c->vp_bound != prog) {
        gl->BindProgramARB(GL_VERTEX_PROGRAM_ARB, prog);
        c->vp_bound = prog;
    }
    if (!c->vp_on) {
        gl->Enable(GL_VERTEX_PROGRAM_ARB);
        c->vp_on = 1;
        c->tnl_dirty |= TNL_DIRTY_XF;
    }
    if (c->tnl_dirty & TNL_DIRTY_XF) {  /* the viewport (cleared by tnl_load) */
        if (c->vp_w)
            gl->Viewport(c->vp_x, c->vp_y, c->vp_w, c->vp_h);
        else
            gl->Viewport(0, 0, c->fb_w, c->fb_h);
        gl->DepthRange(c->zmin, c->zmax);
    }
    /* c0..c95 for Direct3D 8 lie below the fixed-function programs' state (TNL_ENV_BASE); only
       Direct3D 9's c0..c255 reach into it, so only they take it over (env_owner) */
    if (c->vs_limit > TNL_ENV_BASE && c->env_owner != ENV_VS) {
        c->env_owner = ENV_VS;
        c->vsc_dirty = 1;
    }
    if (c->vsc_dirty || c->vs_loaded != s->handle) {
        lo = 0;                         /* all of them, and this shader's def constants */
        hi = c->vs_limit;
    } else {
        lo = c->vsc_lo;                 /* the ones the program changed since */
        hi = c->vsc_hi < c->vs_limit ? c->vsc_hi : c->vs_limit;
    }
    if (lo < hi) {
        for (i = lo; i < hi; i++)
            gl->ProgramEnvParameter4fvARB(GL_VERTEX_PROGRAM_ARB, i, c->vsc[i]);
        for (i = 0; i < s->info.defs; i++)
            if (s->info.def[i].reg >= lo && s->info.def[i].reg < hi)
                gl->ProgramEnvParameter4fvARB(GL_VERTEX_PROGRAM_ARB, s->info.def[i].reg, s->info.def[i].v);
    }
    c->vsc_dirty = 0;
    c->vsc_lo = c->vsc_hi = 0;
    c->vs_loaded = s->handle;
    /* the half pixel, and table fog's constants (as tnl_load's) */
    v[0] = 1.0f / (float)(c->vp_w ? c->vp_w : c->fb_w);
    v[1] = 1.0f / (float)(c->vp_w ? c->vp_h : c->fb_h);
    v[2] = v[3] = 0.0f;
    gl->ProgramLocalParameter4fvARB(GL_VERTEX_PROGRAM_ARB, 0, v);
    if (key & VS_ARB_FOG) {
        float fs = *(float *)&c->rs[D3DRENDERSTATE_FOGSTART], fe = *(float *)&c->rs[D3DRENDERSTATE_FOGEND];
        v[0] = fe != fs ? fe / (fe - fs) : 1.0f;
        v[1] = fe != fs ? 1.0f / (fe - fs) : 0.0f;
        v[2] = *(float *)&c->rs[D3DRENDERSTATE_FOGDENSITY];
        gl->ProgramLocalParameter4fvARB(GL_VERTEX_PROGRAM_ARB, 1, v);
    }
    c->tnl_dirty &= ~TNL_DIRTY_XF;      /* the viewport is set; the fixed-function matrices are
                                           reloaded anyway when they get program.env back */
    return 1;
}

static GLuint buffer_object(RCTX *c, DWORD h, GLenum target);

/* Where one attribute of a draw lies: stream st's data for vertex first, plus offset, with that
   stream's buffer object bound (or none: its memory). Stream 0 is v's; the others are the
   vertex buffers set for streams 1 and up, read at the same vertex. *stride: that stream's. */
static const BYTE *stream_at(RCTX *c, const VERTICES *v, DWORD first, DWORD st, DWORD offset,
                             DWORD *stride)
{
    DWORD bytes;
    const BYTE *mem;
    GLuint vbo;

    if (!st) {
        *stride = v->stride;
        return vertex_data(c, v, first) + offset;
    }
    mem = buffer_get(c->ddl, c->sx_handle[st], &bytes);
    vbo = buffer_object(c, c->sx_handle[st], GL_ARRAY_BUFFER_ARB);
    *stride = c->sx_stride[st];
    bind_buffer(c, GL_ARRAY_BUFFER_ARB, vbo);
    tracef("rd3d: stream %lu: buffer %lx stride %lu, object %lu, vertex %ld + %lu, offset %lu\r\n", st,
           c->sx_handle[st], *stride, (DWORD)vbo, v->first_vertex, first, offset);
    return (vbo ? (const BYTE *)0 : mem) + ((DWORD)v->first_vertex + first) * *stride + offset;
}

/* The streams a declaration's fixed-function attributes come from, as bits. */
static DWORD src_streams(const VERTICES *v)
{
    DWORD i, streams = 0;
    for (i = 0; i < sizeof v->src; i++)
        streams |= 1UL << v->src[i];
    return streams;
}

/* The streams a draw reads besides stream 0 (bit n: stream n) have vertex buffers that reach
   count vertices from first on (count 0: an indexed draw from buffer objects, range unknown). */
static int streams_ok(RCTX *c, const VERTICES *v, DWORD first, DWORD count, DWORD streams)
{
    DWORD st, bytes;

    for (st = 1; st < VS_ARB_STREAMS; st++) {
        if (!(streams & (1UL << st)))
            continue;
        if (v->first_vertex < 0 || !c->sx_stride[st] || !buffer_get(c->ddl, c->sx_handle[st], &bytes) ||
            ((DWORD)v->first_vertex + first + count) * c->sx_stride[st] > bytes)
            return 0;
    }
    return 1;
}

/* The shader's inputs as generic attribute arrays from vertex first on; the conventional
   arrays off. An input may come from another stream than stream 0 (v): that stream's vertex
   buffer, from its buffer object or its memory, at the same vertex. 0 if such a stream has no
   buffer or the draw reaches past its end. */
static int vs_arrays(RCTX *c, const VERTICES *v, DWORD first, DWORD count)
{
    GLF *gl = gl_of(c);
    struct vs8 *s = vs_find(c, c->fvf8);
    DWORD n, u, streams = 0;

    for (n = 0; s && n < VS_ARB_INPUTS; n++)
        if (s->info.inputs & (1UL << n))
            streams |= 1UL << s->info.stream[n];
    if (!streams_ok(c, v, first, count, streams))
        return 0;

    gl->DisableClientState(GL_VERTEX_ARRAY);
    gl->DisableClientState(GL_COLOR_ARRAY);
    if (gl->NormalPointer)
        gl->DisableClientState(GL_NORMAL_ARRAY);
    if (gl->SecondaryColorPointer)
        gl->DisableClientState(GL_SECONDARY_COLOR_ARRAY);
    if (gl->FogCoordPointer)
        gl->DisableClientState(GL_FOG_COORDINATE_ARRAY);
    for (u = 0; u < STAGES; u++) {
        gl->ClientActiveTexture(GL_TEXTURE0 + u);
        gl->DisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    gl->ClientActiveTexture(GL_TEXTURE0);
    for (n = 0; n < VS_ARB_INPUTS; n++) {
        DWORD bit = 1UL << n;
        if (s && (s->info.inputs & bit)) {
            DWORD type = s->info.type[n], stride;
            const BYTE *at = stream_at(c, v, first, s->info.stream[n], s->info.offset[n], &stride);
            if (type <= VS_ARB_FLOAT4)
                gl->VertexAttribPointerARB(n, type + 1, GL_FLOAT, GL_FALSE, stride, at);
            else if (type == VS_ARB_COLOR)
                gl->VertexAttribPointerARB(n, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at);
            else if (type == VS_ARB_UBYTE4)
                gl->VertexAttribPointerARB(n, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, at);
            else
                gl->VertexAttribPointerARB(n, type == VS_ARB_SHORT2 ? 2 : 4, GL_SHORT, GL_FALSE,
                                           stride, at);
            if (!(c->attribs_on & bit)) {
                gl->EnableVertexAttribArrayARB(n);
                c->attribs_on |= bit;
            }
        } else if (c->attribs_on & bit) {
            gl->DisableVertexAttribArrayARB(n);
            c->attribs_on &= ~bit;
        }
    }
    return 1;
}

/* The arrays of untransformed vertices first..first+count: as the program reads them, each set
   of texture coordinates in its own unit's array. */
static void tnl_arrays(RCTX *c, const VERTICES *v, DWORD first)
{
    GLF *gl = gl_of(c);
    const BYTE *b;
    DWORD u, stride;

    attribs_off(c);
    gl->EnableClientState(GL_VERTEX_ARRAY);
    b = stream_at(c, v, first, v->src[0], v->pos, &stride);
    gl->VertexPointer(3, GL_FLOAT, stride, b);
    if (v->normal >= 0) {
        gl->EnableClientState(GL_NORMAL_ARRAY);
        b = stream_at(c, v, first, v->src[1], v->normal, &stride);
        gl->NormalPointer(GL_FLOAT, stride, b);
    } else {
        gl->DisableClientState(GL_NORMAL_ARRAY);
    }
    if (v->diffuse >= 0) {
        gl->EnableClientState(GL_COLOR_ARRAY);
        b = stream_at(c, v, first, v->src[2], v->diffuse, &stride);
        gl->ColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, b);
    } else {
        gl->DisableClientState(GL_COLOR_ARRAY);
    }
    if (gl->SecondaryColorPointer) {
        if (v->specular >= 0) {
            gl->EnableClientState(GL_SECONDARY_COLOR_ARRAY);
            b = stream_at(c, v, first, v->src[3], v->specular, &stride);
            gl->SecondaryColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, b);
        } else {
            gl->DisableClientState(GL_SECONDARY_COLOR_ARRAY);
        }
    }
    if (v->psize_at && c->points_now && gl->VertexAttribPointerARB && gl->EnableVertexAttribArrayARB) {
        /* each point's size: the generic attribute the program reads it from (tnl_arb.c) */
        b = stream_at(c, v, first, 0, (DWORD)v->psize_at - 1, &stride);
        gl->VertexAttribPointerARB(TNL_ATTR_PSIZE, 1, GL_FLOAT, 0, stride, b);
        gl->EnableVertexAttribArrayARB(TNL_ATTR_PSIZE);
        c->attribs_on |= 1UL << TNL_ATTR_PSIZE;
    }
    if (gl->FogCoordPointer)
        gl->DisableClientState(GL_FOG_COORDINATE_ARRAY);
    for (u = 0; u < STAGES; u++) {
        gl->ClientActiveTexture(GL_TEXTURE0 + u);
        if (v->tex[u] >= 0) {
            gl->EnableClientState(GL_TEXTURE_COORD_ARRAY);
            b = stream_at(c, v, first, v->src[4 + u], v->tex[u], &stride);
            gl->TexCoordPointer(v->texsize[u], GL_FLOAT, stride, b);
        } else {
            gl->DisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    }
    gl->ClientActiveTexture(GL_TEXTURE0);
}

/* The user clip planes of the next draw (SETCLIPPLANE, D3DRS_CLIPPLANEENABLE). kind 0: none
   (vertices the program transformed itself are not clipped by them, and Direct3D 7 has none
   here); 1: the fixed-function pipeline, whose planes are in world space; 2: a vertex shader,
   whose planes are in Direct3D's clip space.

   OpenGL defines no user clipping for an ARB vertex program's output. Mesa clips its
   result.position all the same, against each plane taken to clip space: the plane given to
   glClipPlane times the inverse of the modelview matrix of that moment (the identity here),
   times the inverse of the projection matrix (framebuffer's glOrtho). So a plane is worked out
   in the clip space the programs write, which is OpenGL's (tnl_load's adjustments: y up, z from
   -w, the half pixel), and multiplied by that projection matrix beforehand, so that Mesa's
   inverse gives it back. The equations depend on the view, the projection, the viewport and
   the target's size: they are worked out anew for each draw that clips, and loaded when they
   differ from the ones there. */
static void clip_state(RCTX *c, int kind)
{
    GLF *gl = gl_of(c);
    DWORD want = kind && c->dx8 && gl->ClipPlane ? c->rs[RS_CLIPPLANEENABLE] & ((1UL << CLIP_PLANES) - 1) : 0;
    DWORD i, j;
    float inv[16];

    if (!want && !c->clip_gl)
        return;
    if (want) {
        float a[16], m[16], vp[16];
        float w = (float)(c->vp_w ? c->vp_w : c->fb_w), h = (float)(c->vp_w ? c->vp_h : c->fb_h);
        mat_identity(a);
        a[5] = -1.0f;
        a[10] = 2.0f;
        a[14] = -1.0f;
        a[12] = w > 0.0f ? 1.0f / w : 0.0f;
        a[13] = h > 0.0f ? 1.0f / h : 0.0f;
        if (kind == 1) {
            mat_mul(c->xf_view, c->xf_proj, vp);
            mat_mul(vp, a, m);
        } else {
            memcpy(m, a, sizeof m);
        }
        if (!mat_invert(m, inv) || !c->fb_w || !c->fb_h)
            want = 0;                   /* no way back from clip space: nothing is clipped */
    }
    for (i = 0; i < CLIP_PLANES; i++) {
        DWORD bit = 1UL << i;
        if (want & bit) {
            const float *p = c->clip_plane[i];
            float fw = (float)c->fb_w, fh = (float)c->fb_h, g[4], e[4];
            /* a point q of clip space is q inv in the plane's space: the plane there is inv p */
            for (j = 0; j < 4; j++)
                g[j] = inv[j * 4] * p[0] + inv[j * 4 + 1] * p[1] + inv[j * 4 + 2] * p[2] +
                       inv[j * 4 + 3] * p[3];
            /* times glOrtho(-0.5, w - 0.5, -0.5, h - 0.5, 0, -1) */
            e[0] = g[0] * 2.0f / fw;
            e[1] = g[1] * 2.0f / fh;
            e[2] = g[2] * 2.0f;
            e[3] = g[3] - g[0] * (fw - 1.0f) / fw - g[1] * (fh - 1.0f) / fh - g[2];
            if (!(c->clip_gl & bit) || memcmp(e, c->clip_eq[i], sizeof e)) {
                GLdouble d[4];
                memcpy(c->clip_eq[i], e, sizeof e);
                for (j = 0; j < 4; j++)
                    d[j] = e[j];
                gl->ClipPlane(GL_CLIP_PLANE0 + i, d);
            }
            if (!(c->clip_gl & bit))
                gl->Enable(GL_CLIP_PLANE0 + i);
        } else if (c->clip_gl & bit) {
            gl->Disable(GL_CLIP_PLANE0 + i);
        }
    }
    c->clip_gl = want;
}

/* Draw vertices first..first+count (all of them converted), or count indices into them. */
static void draw(RCTX *c, const VERTICES *v, GLenum mode, DWORD first, DWORD count,
                 const WORD *idx, DWORD nidx, DWORD span)
{
    GLF *gl = gl_of(c);
    DWORD n = nidx ? span : count;      /* indexed: nidx, not idx, which is 0 for indices that start a
                                           buffer object (testing idx drew nothing of those) */

    if (!v->base || (v->count && first + n > v->count)) {
        dbg("rd3d: vertices out of range\r\n");
        return;
    }
    if (c->cur && c->cur->written)
        rt_upload(c);
    if (trace_on)                       /* what the draw uses: shader, textures, stage 0 and 1 */
        tracef("rd3d: draw state: ps %lx, fvf %lx, maps %lx %lx %lx %lx, op %lu(%lx,%lx) %lu(%lx,%lx), "
               "aop %lu, light %lu, tf %08lx, blend %lu %lu %lu, n %lu\r\n", c->ps_bound, c->fvf8,
               c->tss[0][D3DTSS_TEXTUREMAP], c->tss[1][D3DTSS_TEXTUREMAP], c->tss[2][D3DTSS_TEXTUREMAP],
               c->tss[3][D3DTSS_TEXTUREMAP], c->tss[0][D3DTSS_COLOROP], c->tss[0][D3DTSS_COLORARG1],
               c->tss[0][D3DTSS_COLORARG2], c->tss[1][D3DTSS_COLOROP], c->tss[1][D3DTSS_COLORARG1],
               c->tss[1][D3DTSS_COLORARG2], c->tss[0][D3DTSS_ALPHAOP], c->rs[D3DRENDERSTATE_LIGHTING],
               c->rs[D3DRENDERSTATE_TEXTUREFACTOR], c->rs[D3DRENDERSTATE_ALPHABLENDENABLE],
               c->rs[D3DRENDERSTATE_SRCBLEND], c->rs[D3DRENDERSTATE_DESTBLEND], n);
    if (c->stages_dirty || textures_changed(c))
        apply_stages(c);
    if (c->dx8) {                       /* a pixel shader, or none */
        long prog = ps_program(c);
        if (prog < 0) {
            skip8(c, "pixel shader");
            return;
        }
        ps_apply(c, prog);
    }
    if (v->shader) {                    /* through the vertex shader's program */
        if (!vs_begin(c)) {
            skip8(c, "vertex shader");
            return;
        }
        if (mode == GL_POINTS)
            point_state(c, vs_find(c, c->fvf8) && vs_find(c, c->fvf8)->info.psize);
        if (!vs_arrays(c, v, first, n)) {
            skip8(c, "vertex stream");
            return;
        }
        clip_state(c, 2);
        if (!nidx)
            gl->DrawArrays(mode, 0, count);
        else {
            bind_buffer(c, GL_ELEMENT_ARRAY_BUFFER_ARB, v->ibo);
            gl->DrawElements(mode, nidx, v->itype ? v->itype : GL_UNSIGNED_SHORT, idx);
        }
        c->dirty = 1;
        return;
    }
    if (v->xyz) {                       /* untransformed: through a vertex program */
        c->points_now = mode == GL_POINTS;
        if (!tnl_begin(c, v)) {
            skip8(c, "vertex processing");
            return;
        }
        if (!streams_ok(c, v, first, n, src_streams(v))) {
            skip8(c, "vertex stream");
            return;
        }
        if (mode == GL_POINTS)
            point_state(c, c->rs[RS_POINTSCALEENABLE] != 0 || v->psize_at);
        clip_state(c, 1);
        if (trace_on && src_streams(v) > 1)
            tracef("rd3d: declaration draw: position %d, normal %d, diffuse %d (stream %lu), specular %d, "
                   "stream 0 object %lu at %lu, %lu vertices\r\n", v->pos, v->normal, v->diffuse,
                   (DWORD)v->src[2], v->specular, (DWORD)v->vbo, (DWORD)(v->base - v->start), count);
        tnl_arrays(c, v, first);
        if (!nidx)
            gl->DrawArrays(mode, 0, count);
        else {
            bind_buffer(c, GL_ELEMENT_ARRAY_BUFFER_ARB, v->ibo);
            gl->DrawElements(mode, nidx, v->itype ? v->itype : GL_UNSIGNED_SHORT, idx);
        }
        c->dirty = 1;
        return;
    }
    tnl_end(c);
    if (mode == GL_POINTS)
        point_state(c, 0);
    clip_state(c, 0);
    if (!nidx) {
        if (!convert(c, v, first, count))
            return;
        arrays(c, v, first, count);
        gl->DrawArrays(mode, 0, count);
    } else {
        if (!convert(c, v, first, span))
            return;
        arrays(c, v, first, span);
        bind_buffer(c, GL_ELEMENT_ARRAY_BUFFER_ARB, v->ibo);
        gl->DrawElements(mode, nidx, v->itype ? v->itype : GL_UNSIGNED_SHORT, idx);
    }
    c->dirty = 1;
}

/* Indices of a command, made relative to their smallest one, in the context's index buffer
   (grown as needed: one command may hold any number of them); the span they cover. NULL
   without memory. */
static WORD *index_list(RCTX *c, const WORD *src, DWORD n, DWORD step, DWORD keep,
                        DWORD *lo, DWORD *span)
{
    DWORD i, j, k = 0, mn = 0xFFFF, mx = 0;
    WORD *buf;

    if (!c->ibuf || n * keep > c->nibuf) {
        release(c->ibuf);
        c->nibuf = n * keep + 1024;
        c->ibuf = (WORD *)alloc(c->nibuf * sizeof(WORD));
        if (!c->ibuf) {
            c->nibuf = 0;
            return NULL;
        }
    }
    buf = c->ibuf;
    for (i = 0; i < n; i++)
        for (j = 0; j < keep; j++) {
            WORD x = src[i * step + j];
            if (x < mn) mn = x;
            if (x > mx) mx = x;
        }
    for (i = 0; i < n; i++)
        for (j = 0; j < keep; j++)
            buf[k++] = (WORD)(src[i * step + j] - mn);
    *lo = n ? mn : 0;
    *span = n ? mx - mn + 1 : 0;
    return buf;
}

/* The same for 32-bit indices (Direct3D 8 and 9's D3DFMT_INDEX32), made relative to their
   smallest one: as 16-bit indices when they then fit, which a draw's range mostly does, else as
   32-bit ones (*wide). */
static void *index_list32(RCTX *c, const DWORD *src, DWORD n, DWORD *lo, DWORD *span, int *wide)
{
    DWORD i, mn = 0xFFFFFFFFUL, mx = 0;

    if (!c->ibuf || n * 2 > c->nibuf) {
        release(c->ibuf);
        c->nibuf = n * 2 + 1024;
        c->ibuf = (WORD *)alloc(c->nibuf * sizeof(WORD));
        if (!c->ibuf) {
            c->nibuf = 0;
            return NULL;
        }
    }
    for (i = 0; i < n; i++) {
        if (src[i] < mn) mn = src[i];
        if (src[i] > mx) mx = src[i];
    }
    *wide = n && mx - mn > 0xFFFFUL;
    if (*wide) {
        DWORD *b = (DWORD *)c->ibuf;
        for (i = 0; i < n; i++)
            b[i] = src[i] - mn;
    } else {
        for (i = 0; i < n; i++)
            c->ibuf[i] = (WORD)(src[i] - mn);
    }
    *lo = n ? mn : 0;
    *span = n ? mx - mn + 1 : 0;
    return c->ibuf;
}

/* ---- Clear ------------------------------------------------------------------------------------ */
static void clear(RCTX *c, DWORD flags, DWORD color, float depth, DWORD stencil,
                  const RECT *rects, DWORD n)
{
    GLF *gl = gl_of(c);
    GLbitfield mask = 0;
    DWORD i;

    if (c->cur && c->cur->written && (flags & D3DCLEAR_TARGET)) {
        /* a clear of the whole picture makes what the program drew moot (one of the depth
           only leaves that to the next draw or clear) */
        if ((flags & D3DCLEAR_TARGET) && (!n || (n == 1 && rects[0].left <= 0 &&
            rects[0].top <= 0 && (DWORD)rects[0].right >= c->fb_w &&
            (DWORD)rects[0].bottom >= c->fb_h)))
            c->cur->written = 0;
        else
            rt_upload(c);
    }
    if (flags & D3DCLEAR_TARGET) {
        mask |= GL_COLOR_BUFFER_BIT;
        colour_mask(c, 1);              /* Direct3D's Clear writes every channel */
        gl->ClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                       (color & 255) / 255.0f, ((color >> 24) & 255) / 255.0f);
        c->dirty = 1;
    }
    if (flags & D3DCLEAR_ZBUFFER) {
        mask |= GL_DEPTH_BUFFER_BIT;
        gl->ClearDepth(depth);
        gl->DepthMask(GL_TRUE);
    }
    if (flags & D3DCLEAR_STENCIL) {
        mask |= GL_STENCIL_BUFFER_BIT;
        gl->ClearStencil(stencil);
        gl->StencilMask(0xFFFFFFFF);    /* a clear writes every bit, as the depth does */
    }
    if (!mask)
        return;
    gl->Enable(GL_SCISSOR_TEST);
    for (i = 0; i < n; i++) {
        const RECT *r = &rects[i];
        gl->Scissor(r->left, r->top, r->right - r->left, r->bottom - r->top);
        gl->Clear(mask);
    }
    if (!n) {
        gl->Disable(GL_SCISSOR_TEST);
        gl->Clear(mask);
    }
    apply_scissor(c);                   /* the viewport's again */
    if (flags & D3DCLEAR_TARGET)
        colour_mask(c, 0);
    gl->DepthMask((GLboolean)(c->rs[D3DRENDERSTATE_ZWRITEENABLE] ? GL_TRUE : GL_FALSE));
    gl->StencilMask(c->rs[D3DRENDERSTATE_STENCILWRITEMASK]);
}

/* ---- Render target ------------------------------------------------------------------------------ */
/* The old target's picture goes into its memory first. The new target gets its own colour
   buffer (framebuffer, gpu_buffer): the picture it had on the GPU if that is as new as its
   memory, otherwise the memory goes in before the next draw (a clear of the whole target
   makes that moot). The depth is the new z-buffer's own. */
static int set_render_target(RCTX *c, LPDDRAWI_DDRAWSURFACE_LCL rt, LPDDRAWI_DDRAWSURFACE_LCL z)
{
    /* A target outside the flip chain ends it (its pages go back into memory), unless it is a
       texture: rendering into a texture between flips (3DMark 2001's Dragothic, ten a frame) is
       part of the frame, and the chain goes on. A chain with two back buffers has a third page,
       neither the front nor the back of the last flip, which the runtime makes the target every
       frame: it is a page of the chain like the others (taking it for an outsider read every
       page back each frame, 3.7 of a frame's 6.5 ms in 3DMark 2001 SE's Car Chase). */
    if (rt != c->rt && rt != c->front && rt != c->back && !screen_page(rt) &&
        !(rt->ddsCaps.dwCaps & DDSCAPS_TEXTURE) && !end_chain(c))
        return 0;
    finish_target(c);
    tracef("rd3d: render target: caps %08lx, memory %08lx, %lux%lu; z-buffer memory %08lx\r\n",
           rt->ddsCaps.dwCaps, rt->lpGbl->fpVidMem, (DWORD)rt->lpGbl->wWidth,
           (DWORD)rt->lpGbl->wHeight, z ? z->lpGbl->fpVidMem : 0);
    c->rt = rt;
    c->z = z;
    if (!framebuffer(c))
        return 0;
    apply_depth_test(c);                /* with or without a z-buffer now */
    return 1;
}

/* ---- The Direct3D callbacks -------------------------------------------------------------------- */
static void defaults(RCTX *c)
{
    int s;

    render_state(c, D3DRENDERSTATE_ZENABLE, c->z ? D3DZB_TRUE : D3DZB_FALSE);
    render_state(c, D3DRENDERSTATE_ZWRITEENABLE, TRUE);
    render_state(c, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
    render_state(c, D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
    render_state(c, D3DRENDERSTATE_DESTBLEND, D3DBLEND_ZERO);
    render_state(c, D3DRENDERSTATE_ALPHAFUNC, D3DCMP_ALWAYS);
    render_state(c, D3DRENDERSTATE_CULLMODE, D3DCULL_CCW);
    render_state(c, D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    render_state(c, D3DRENDERSTATE_FILLMODE, D3DFILL_SOLID);
    render_state(c, D3DRENDERSTATE_TEXTUREFACTOR, 0xFFFFFFFF);
    render_state(c, D3DRENDERSTATE_STENCILFAIL, D3DSTENCILOP_KEEP);
    render_state(c, D3DRENDERSTATE_STENCILZFAIL, D3DSTENCILOP_KEEP);
    render_state(c, D3DRENDERSTATE_STENCILPASS, D3DSTENCILOP_KEEP);
    render_state(c, D3DRENDERSTATE_STENCILFUNC, D3DCMP_ALWAYS);
    render_state(c, D3DRENDERSTATE_STENCILMASK, 0xFFFFFFFF);
    render_state(c, D3DRENDERSTATE_STENCILWRITEMASK, 0xFFFFFFFF);
    render_state(c, RS_COLORWRITEENABLE, 15);
    for (s = 0; s < STAGES; s++) {
        c->tss[s][D3DTSS_COLOROP] = s ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        c->tss[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        c->tss[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        c->tss[s][D3DTSS_ALPHAOP] = s ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        c->tss[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        c->tss[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        c->tss[s][D3DTSS_TEXCOORDINDEX] = s;
        c->tss[s][D3DTSS_ADDRESSU] = c->tss[s][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        c->tss[s][D3DTSS_MAGFILTER] = D3DTFG_POINT;
        c->tss[s][D3DTSS_MINFILTER] = D3DTFN_POINT;
        c->tss[s][D3DTSS_MIPFILTER] = D3DTFP_NONE;
    }
    c->stages_dirty = 1;
}

static DWORD WINAPI ContextCreate(LPD3DHAL_CONTEXTCREATEDATA d)
{
    RPROC *p = process();
    RCTX *c;
    PIXELFORMATDESCRIPTOR pfd;
    int i, fmt;
    DWORD kind = (DWORD)d->dwhContext;  /* the runtime's interface: 3 for Direct3D 7, 4 from 8 on */

    d->ddrval = D3DHAL_OUTOFCONTEXTS;
    d->dwhContext = 0;
    if (!p || !gl_load_core(p))
        return DDHAL_DRIVER_HANDLED;
    /* a free slot, or one of a process that has ended */
    for (i = 0; i < MAX_CONTEXTS && contexts[i].o.pid; i++)
        ;
    for (fmt = 0; i == MAX_CONTEXTS && fmt < MAX_CONTEXTS; fmt++)
        if (!process_alive(contexts[fmt].o.pid))
            i = fmt;
    if (i == MAX_CONTEXTS)
        return DDHAL_DRIVER_HANDLED;
    c = (RCTX *)alloc(sizeof(RCTX));
    if (!c)
        return DDHAL_DRIVER_HANDLED;
    c->p = p;
    c->dx8 = kind >= 4;
    c->vs_limit = p->d3d_version >= 0x900 ? VS_ARB_CONSTANTS : 96;    /* vertex shader constants */
    tnl_reset(c);
    if (!p->wndclass) {
        WNDCLASS wc;
        memset(&wc, 0, sizeof wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = hidden_wndproc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "RippleD3D";
        p->wndclass = RegisterClass(&wc);
    }
    c->wnd = CreateWindow("RippleD3D", "", WS_POPUP, 0, 0, 8, 8, NULL, NULL,
                          GetModuleHandle(NULL), NULL);
    c->dc = c->wnd ? GetDC(c->wnd) : NULL;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    fmt = c->dc ? ChoosePixelFormat(c->dc, &pfd) : 0;
    if (!fmt || !SetPixelFormat(c->dc, fmt, &pfd) || !(c->rc = p->gl.DrvCreateContext(c->dc))) {
        dbg("rd3d: no OpenGL context\r\n");
        context_free(c);
        return DDHAL_DRIVER_HANDLED;
    }
    make_current(c);
    if (!gl_load_ext(p)) {
        context_free(c);
        return DDHAL_DRIVER_HANDLED;
    }
    if (p->gl.ProvokingVertex)
        p->gl.ProvokingVertex(GL_FIRST_VERTEX_CONVENTION);  /* flat shading: Direct3D's vertex */
    {
        char v[8];
        trace_on = GetEnvironmentVariable("RIPPLE_D3D_TRACE", v, sizeof v) > 0;
        profile_on = GetEnvironmentVariable("RIPPLE_D3D_PROFILE", v, sizeof v) > 0;
        flip_start.QuadPart = 0;
        /* GPU presentation is the default on a virgl host; RIPPLE_D3D_SCANOUT=0 keeps the
           read-back path. Without virgl (Mesa's software renderer) the display stays VGA. */
        {
            const char *r = (const char *)p->gl.GetString(GL_RENDERER);
            c->scanout_enabled = !(GetEnvironmentVariable("RIPPLE_D3D_SCANOUT", v, sizeof v) == 1 &&
                                   v[0] == '0') &&
                                 p->present_color != NULL && r && memcmp(r, "virgl", 5) == 0;
        }
    }
    /* Multiple devices can share a surface. Resolve it in its original context before a
       second device can draw there, and leave such sessions on the coherent CPU path. */
    {
        OWNER m = me();
        int j;
        for (j = 0; j < MAX_CONTEXTS; j++) {
            RCTX *other = mine(&contexts[j].o, &m) ? contexts[j].c : NULL;
            if (other) {
                c->scanout_enabled = 0;
                if (other->scanout_enabled) {
                    if (!end_chain(other)) {
                        let_go(other);
                        context_free(c);
                        return DDHAL_DRIVER_HANDLED;
                    }
                    other->scanout_enabled = 0;
                    let_go(other);
                    make_current(c);
                }
            }
        }
    }
    c->ddl = d->lpDDLcl;
    if (c->dx8 && d8_on) {              /* its textures' records are copied from now on */
        DDLOCAL *l = ddlocal(c->ddl, 1);
        if (l)
            l->dx8 = 1;
    }
    c->rt = d->lpDDSLcl;
    c->z = d->lpDDSZLcl;
    if (!framebuffer(c)) {
        context_free(c);
        return DDHAL_DRIVER_HANDLED;
    }
    defaults(c);
    contexts[i].o = me();
    contexts[i].c = c;
    if (c->scanout_enabled)
        scanout_requested = 1;
    dbg("rd3d: context "); dbg_hex((DWORD)c); dbg(" on "); dbg((const char *)p->gl.GetString(GL_RENDERER));
    if (c->dx8) {
        dbg(", Direct3D 8 interface "); dbg_hex(kind);
    }
    dbg("\r\n");
    d->dwhContext = i + 1;
    d->ddrval = DD_OK;
    let_go(c);
    return DDHAL_DRIVER_HANDLED;
}

static void context_remove(int i)
{
    RCTX *c = contexts[i].c;
    if (c && c->chain_active) {
        end_chain(c);
        rdd_desktop(0);
    }
    contexts[i].o.pid = 0;
    contexts[i].c = NULL;
    if (c)
        context_free(c);
}

static DWORD WINAPI ContextDestroy(LPD3DHAL_CONTEXTDESTROYDATA d)
{
    RCTX *c = context_of(d->dwhContext);
    if (c) {
        read_back(c);
        context_remove(d->dwhContext - 1);
    }
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI ContextDestroyAll(LPD3DHAL_CONTEXTDESTROYALLDATA d)
{
    int i;
    for (i = 0; i < MAX_CONTEXTS; i++)
        if (contexts[i].o.pid && contexts[i].o.pid == d->dwPID) {
            OWNER m = me();
            if (!mine(&contexts[i].o, &m))
                contexts[i].c = NULL;   /* its memory went with the process */
            context_remove(i);
        }
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI ValidateTextureStageState(LPD3DHAL_VALIDATETEXTURESTAGESTATEDATA d)
{
    d->dwNumPasses = 1;
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Surface handles: DirectX 7 tells the driver of every surface it may name in commands. A
   surface comes with the ones attached to it: a flipping chain gets a handle for each surface
   (walked as the DDK's sample driver does); a mipmapped texture is named by its top level. A
   surface without memory is one being destroyed. */
static void surface_handles(DDLOCAL *l, LPDDRAWI_DDRAWSURFACE_LCL root,
                            LPDDRAWI_DDRAWSURFACE_LCL s, int depth)
{
    LPATTACHLIST a;
    DWORD h;

    if (!s || !s->lpSurfMore || depth > 8)
        return;
    h = s->lpSurfMore->dwSurfaceHandle;
    if (!h)
        return;
    if (s->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER) {    /* a Direct3D 8 vertex or index buffer */
        handle_set(l, h, NULL);         /* never dereference the record later */
        buffer_set(l, h, s->lpGbl ? s->lpGbl->fpVidMem : 0, s->lpGbl ? s->lpGbl->dwLinearSize : 0,
                   buf_on && s->lpGbl && s->lpGbl->dwReserved1 == D3D_BUFFER_MARK,
                   (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_HINTSTATIC) != 0);
        tracef("rd3d: buffer %lx: caps %08lx, caps2 %08lx, memory %08lx, %lu bytes\r\n", h,
               s->ddsCaps.dwCaps, s->lpSurfMore->ddsCapsEx.dwCaps2,
               s->lpGbl ? s->lpGbl->fpVidMem : 0, s->lpGbl ? s->lpGbl->dwLinearSize : 0);
        return;
    }
    if (l->dx8 && depth == 0 && (s->ddsCaps.dwCaps & DDSCAPS_TEXTURE)) {    /* cube maps too */
        struct shadow *sh;
        if ((s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_VOLUME) &&
            !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) && (s->dwFlags & DDRAWISURF_HASPIXELFORMAT)) {
            /* A video memory volume, allocated by now (CreateSurface: rows tight, slices one
               after another). The runtime reads each level's slice pitch from the field that
               held the allocation's block height (lSlicePitch in DirectX 8, dwBlockSizeY here). */
            LPDDRAWI_DDRAWSURFACE_LCL m = s;
            int k;
            for (k = 0; m && k < MAX_LEVELS; k++) {
                LPDDRAWI_DDRAWSURFACE_GBL g = m->lpGbl;
                PIXFMT vf = gl_format(&g->ddpfSurface);
                DWORD block = vf.block;         /* DXT: rows of 4x4 blocks */
                g->lPitch = block ? (LONG)(((g->wWidth + 3) / 4) * block) :
                                    (LONG)(g->wWidth * (vf.bytes ? vf.bytes : g->ddpfSurface.dwRGBBitCount / 8));
                g->dwBlockSizeY = (DWORD)g->lPitch * (block ? (g->wHeight + 3) / 4 : g->wHeight);
                m = (m->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && m->lpAttachList ? m->lpAttachList->lpAttached : NULL;
                if (m == s)
                    break;
            }
        }
        sh = s->lpGbl && s->lpGbl->fpVidMem ? shadow_make(s) : NULL;
        handle_set(l, h, sh ? &sh->l : NULL);
        shadow_set(l, h, sh);
        texture_forget(h);              /* a handle may come back with other memory */
        tracef("rd3d: texture handle %lx: caps %08lx, memory %08lx, %lux%lu, copied %d\r\n", h,
               s->ddsCaps.dwCaps, s->lpGbl ? s->lpGbl->fpVidMem : 0,
               s->lpGbl ? (DWORD)s->lpGbl->wWidth : 0, s->lpGbl ? (DWORD)s->lpGbl->wHeight : 0,
               sh != NULL);
        return;
    }
    handle_set(l, h, s->lpGbl && s->lpGbl->fpVidMem ? s : NULL);
    if (!s->lpGbl || !s->lpGbl->fpVidMem)
        texture_forget(h);
    tracef("rd3d: handle %lx: caps %08lx, caps2 %08lx, memory %08lx, %lux%lu\r\n", h,
           s->ddsCaps.dwCaps, s->lpSurfMore->ddsCapsEx.dwCaps2,
           s->lpGbl ? s->lpGbl->fpVidMem : 0, s->lpGbl ? (DWORD)s->lpGbl->wWidth : 0,
           s->lpGbl ? (DWORD)s->lpGbl->wHeight : 0);
    if ((s->ddsCaps.dwCaps & DDSCAPS_MIPMAP) ||
        (s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP))
        return;
    a = s->lpAttachList;
    if (!a)
        return;
    if (a->lpLink && a->lpLink->lpAttached != root)
        surface_handles(l, root, a->lpLink->lpAttached, depth + 1);
    if (a->lpAttached != root)
        surface_handles(l, root, a->lpAttached, depth + 1);
}

static DWORD WINAPI CreateSurfaceEx(LPDDHAL_CREATESURFACEEXDATA d)
{
    DDLOCAL *l = ddlocal(d->lpDDLcl, 1);

    d->ddRVal = DD_OK;
    if (l)
        surface_handles(l, d->lpDDSLcl, d->lpDDSLcl, 0);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI GetDriverState(LPDDHAL_GETDRIVERSTATEDATA d)
{
    d->ddRVal = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI DestroyDDLocal(LPDDHAL_DESTROYDDLOCALDATA d)
{
    DDLOCAL *l = ddlocal(d->pDDLcl, 0);
    if (l) {
        release(l->surf);
        release(l->buf);                /* buffer objects go with their contexts */
        release(l->trash);
        shadows_free(l);
        memset(l, 0, sizeof *l);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Copy a rectangle between two surfaces (DirectX 7's texture manager uses this). */
static void tex_blt(RCTX *c, const D3DHAL_DP2TEXBLT *b)
{
    LPDDRAWI_DDRAWSURFACE_LCL ds = handle_get(c->ddl, b->dwDDDestSurface);
    LPDDRAWI_DDRAWSURFACE_LCL ss = handle_get(c->ddl, b->dwDDSrcSurface);
    LPDDRAWI_DDRAWSURFACE_LCL dl[MAX_LEVELS], sl[MAX_LEVELS];
    DDPIXELFORMAT pf;
    PIXFMT f;
    DWORD bytes, levels, n, l;
    DWORD faces, face;
    RECTL r = b->rSrc;

    tracef("rd3d: texture blit %lx -> %lx, %ld,%ld-%ld,%ld to %ld,%ld, flags %lx%s\r\n",
           b->dwDDSrcSurface, b->dwDDDestSurface, r.left, r.top, r.right, r.bottom,
           b->pDest.x, b->pDest.y, b->dwFlags, ds && ss ? "" : " (unknown handle)");
    if (!ds || !ss || !ds->lpGbl->fpVidMem || !ss->lpGbl->fpVidMem)
        return;
    {
        GPUBUF *pic = gpu_picture(c, ss->lpGbl->fpVidMem);  /* the source was rendered into */
        if (pic)
            read_pixels(c, pic);
    }
    surface_format(ss, &pf);
    f = gl_format(&pf);
    bytes = f.bytes ? f.bytes : pf.dwRGBBitCount / 8;   /* FOURCC-named formats have no bit count */
    if (!bytes && !f.block)
        return;
    if (r.right <= r.left || r.bottom <= r.top) {
        r.left = r.top = 0;
        r.right = ss->lpGbl->wWidth;
        r.bottom = ss->lpGbl->wHeight;
    }
    /* Mipmapped textures: every level the two have in common, with the rectangle and the
       point scaled to it (the DDK's rule for this command). */
    /* A cube map (Direct3D 8 and 9: the HAL's copies, shadow_make): each of its six faces. */
    faces = c->dx8 && is_cube(ds) && is_cube(ss) ? 6 : 1;
    for (face = 0; face < faces; face++) {
    LPDDRAWI_DDRAWSURFACE_LCL dtop = faces > 1 ? &((struct shadow *)ds)[face * ((struct shadow *)ds)->levels].l : ds;
    LPDDRAWI_DDRAWSURFACE_LCL stop = faces > 1 ? &((struct shadow *)ss)[face * ((struct shadow *)ss)->levels].l : ss;
    levels = mip_chain(dtop, dl);
    n = mip_chain(stop, sl);
    if (n < levels)
        levels = n;
    for (l = 0; l < levels; l++) {
        LPDDRAWI_DDRAWSURFACE_GBL dg = dl[l]->lpGbl, sg = sl[l]->lpGbl;
        LONG left = r.left >> l, top = r.top >> l, x = b->pDest.x >> l, y = b->pDest.y >> l;
        LONG w = (r.right >> l) - left, h = (r.bottom >> l) - top, i;
        if (w < 1)
            w = 1;
        if (h < 1)
            h = 1;
        /* never outside either surface */
        if (left < 0 || top < 0 || x < 0 || y < 0 || left >= (LONG)sg->wWidth ||
            top >= (LONG)sg->wHeight || x >= (LONG)dg->wWidth || y >= (LONG)dg->wHeight)
            break;
        if (w > (LONG)sg->wWidth - left)
            w = (LONG)sg->wWidth - left;
        if (w > (LONG)dg->wWidth - x)
            w = (LONG)dg->wWidth - x;
        if (h > (LONG)sg->wHeight - top)
            h = (LONG)sg->wHeight - top;
        if (h > (LONG)dg->wHeight - y)
            h = (LONG)dg->wHeight - y;
        /* A copy in memory, so the source's newest picture must be there first, and the
           target's too (the part not copied stays); a context's read-back may make another
           one current. */
        if (!d3d_surface_access(sl[l]) || !d3d_surface_access(dl[l]) || !make_current(c))
            return;
        if (f.block) {                  /* DXT: whole 4x4 blocks, rows of blocks */
            DWORD sp = ((sg->wWidth + 3) / 4) * f.block, dp = ((dg->wWidth + 3) / 4) * f.block;
            LONG bw = (w + 3) / 4, bh = (h + 3) / 4;
            for (i = 0; i < bh; i++)
                memcpy((BYTE *)dg->fpVidMem + (y / 4 + i) * dp + (x / 4) * f.block,
                       (BYTE *)sg->fpVidMem + (top / 4 + i) * sp + (left / 4) * f.block,
                       bw * f.block);
        } else {
            for (i = 0; i < h; i++)
                memcpy((BYTE *)dg->fpVidMem + (y + i) * dg->lPitch + x * bytes,
                       (BYTE *)sg->fpVidMem + (top + i) * sg->lPitch + left * bytes, w * bytes);
        }
        d3d_surface_written(dl[l]);     /* a render target too: its GPU picture is older now */
    }
    }                                   /* faces */
    if (b->dwDDDestSurface < c->ntex)
        c->tex[b->dwDDDestSurface].dirty = 1;            /* upload again */
    c->stages_dirty = 1;
}

/* A box of one volume copied into another (the runtime's managed volumes), at each level both
   have, the box and the point scaled to it. Both are the HAL's copies (shadow_make). */
static void volume_blt(RCTX *c, const RB_DP2_VOLUME_BLT *b)
{
    LPDDRAWI_DDRAWSURFACE_LCL ds = handle_get(c->ddl, b->dest), ss = handle_get(c->ddl, b->source);
    const struct shadow *dsh, *ssh;
    DDPIXELFORMAT pf;
    PIXFMT f;
    DWORD levels, l;

    tracef("rd3d: volume blit %lx -> %lx, box %lu,%lu,%lu-%lu,%lu,%lu\r\n", b->source, b->dest,
           b->box[0], b->box[1], b->box[4], b->box[2], b->box[3], b->box[5]);
    if (!c->dx8 || !ds || !ss || !is_volume(ds) || !is_volume(ss))
        return;
    dsh = (const struct shadow *)ds;
    ssh = (const struct shadow *)ss;
    surface_format(ss, &pf);
    f = gl_format(&pf);
    if (!f.bytes)
        return;
    levels = dsh->levels < ssh->levels ? dsh->levels : ssh->levels;
    for (l = 0; l < levels; l++) {
        const DDRAWI_DDRAWSURFACE_GBL *sg = &ssh[l].g, *dg = &dsh[l].g;
        DWORD sd = ssh->depth >> l ? ssh->depth >> l : 1, dd = dsh->depth >> l ? dsh->depth >> l : 1;
        DWORD x0 = b->box[0] >> l, y0 = b->box[1] >> l, x1 = b->box[2] >> l, y1 = b->box[3] >> l;
        DWORD z0 = b->box[4] >> l, z1 = b->box[5] >> l, dx = b->x >> l, dy = b->y >> l, dz = b->z >> l;
        DWORD sp = sg->lPitch > 0 ? (DWORD)sg->lPitch : sg->wWidth * f.bytes, dp = volume_pitch(dg, &f);
        DWORD y, z;
        if (b->box[2] <= b->box[0] || b->box[3] <= b->box[1] || b->box[5] <= b->box[4]) {
            x0 = y0 = z0 = 0;           /* no box: all of it */
            x1 = sg->wWidth;
            y1 = sg->wHeight;
            z1 = sd;
        }
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        if (z1 <= z0) z1 = z0 + 1;
        if (x1 > sg->wWidth || y1 > sg->wHeight || z1 > sd || dx + x1 - x0 > dg->wWidth ||
            dy + y1 - y0 > dg->wHeight || dz + z1 - z0 > dd || !sg->fpVidMem || !dg->fpVidMem)
            break;
        for (z = z0; z < z1; z++)
            for (y = y0; y < y1; y++)
                memcpy((BYTE *)dg->fpVidMem + (dz + z - z0) * dp * dg->wHeight + (dy + y - y0) * dp + dx * f.bytes,
                       (const BYTE *)sg->fpVidMem + z * sp * sg->wHeight + y * sp + x0 * f.bytes,
                       (x1 - x0) * f.bytes);
    }
    if (b->dest < c->ntex)
        c->tex[b->dest].dirty = 1;      /* upload again */
    c->stages_dirty = 1;
}

#define CMD(p) ((LPD3DHAL_DP2COMMAND)(p))
#define ALIGN4(p) ((BYTE *)(((DWORD)(p) + 3) & ~3))

/* ---- Direct3D 8 pixel shaders ------------------------------------------------------------- */
/* A shader is translated when the runtime creates it: its tokens to TGSI text by the Windows
   2000 driver's translator (win2k/d3d8_ps.c), that to an ARB fragment program (tgsi_arb.c),
   which Mesa compiles. Inputs: the diffuse and specular colours, texture coordinate sets 0..7;
   constants (c0..c7 of version 1, c0..c31 of 2.0) are program.env; the translator's
   immediates are program.local.
   Fog after the shader: a second program, made when a draw first needs it. The HAL already
   computes each vertex's Direct3D fog factor f as 0.25 + 0.5 f (for the fixed-function fog
   ramp); under a shader it goes in as OpenGL's fog coordinate (arrays), and the translator is
   asked for linear fog from a vertex depth read there, with start 0.75 and end 0.25, which
   gives back f. */
#define PS_FOG (RB_PS_FOG_VERTEX | (3 << RB_PS_FOG_MODE_SHIFT))     /* linear, from IN[10] */

static struct ps8 *ps_find(RCTX *c, DWORD handle, int create)
{
    int i, free = -1;
    if (!handle)
        return NULL;
    for (i = 0; i < MAX_PS; i++) {
        if (c->ps[i].handle == handle)
            return &c->ps[i];
        if (!c->ps[i].handle && free < 0)
            free = i;
    }
    if (!create || free < 0)
        return NULL;
    memset(&c->ps[free], 0, sizeof c->ps[free]);
    c->ps[free].handle = handle;
    return &c->ps[free];
}

static void ps_delete(RCTX *c, DWORD handle)
{
    GLF *gl = gl_of(c);
    struct ps8 *p = ps_find(c, handle, 0);
    DWORD i;
    if (!p)
        return;
    for (i = 0; i < p->nvar; i++) {
        if (p->var[i].prog && gl->DeleteProgramsARB)
            gl->DeleteProgramsARB(1, &p->var[i].prog);
        if (c->fp_prog == p->var[i].prog)
            c->fp_prog = 0;
    }
    release(p->code);
    memset(p, 0, sizeof *p);
}

/* The program for p, with or without fog; 0 if it can't be made (p->failed is then set). */
/* key: the translator's fog argument (fog, and the cube map stages of a version 1 shader). */
static GLuint ps_compile(RCTX *c, struct ps8 *p, DWORD key)
{
    GLF *gl = gl_of(c);
    char *tgsi, *arb;
    RB_PS_INFO info;
    TGSI_ARB ai;
    DWORD n = 0, m = 0, i;
    GLuint prog = 0;

    if (p->failed)
        return 0;
    tgsi = (char *)alloc(RB_PS_TEXT_BYTES);
    arb = (char *)alloc(TGSI_ARB_TEXT_BYTES);
    memset(&info, 0, sizeof info);
    memset(&ai, 0, sizeof ai);
    ai.fog_unit = TGSI_ARB_FOGCOORD;    /* fog values in OpenGL's fog coordinate (arrays) */
    if (tgsi && arb && p->code)
        n = rb_ps_translate((const unsigned int *)p->code, p->bytes / 4, key, tgsi,
                            RB_PS_TEXT_BYTES, &info);
    if (n)
        m = tgsi_to_arb(tgsi, arb, TGSI_ARB_TEXT_BYTES, &ai);
    if (!n || !m || !gl->GenProgramsARB || !gl->ProgramStringARB || !gl->BindProgramARB ||
        !gl->ProgramLocalParameter4fvARB) {
        tracef("rd3d: pixel shader %lx not translated: reason %lu, detail %lx, line %lu\r\n",
               p->handle, (DWORD)info.reason, (DWORD)info.detail, n ? (DWORD)ai.line : 0);
        dbg("rd3d: a pixel shader was not translated\r\n");
        p->failed = 1;
    } else {
        gl->GetError();                 /* earlier errors aren't this one's */
        gl->GenProgramsARB(1, &prog);
        gl->BindProgramARB(GL_FRAGMENT_PROGRAM_ARB, prog);
        c->fp_prog = prog;
        gl->ProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, m - 1, arb);
        if (gl->GetError() != GL_NO_ERROR) {
            const char *e = (const char *)gl->GetString(GL_PROGRAM_ERROR_STRING_ARB);
            dbg("rd3d: a pixel shader's program was refused: ");
            dbg(e ? e : "?");
            dbg("\r\n");
            if (trace_on)
                dbg(arb);
            if (gl->DeleteProgramsARB)
                gl->DeleteProgramsARB(1, &prog);
            c->fp_prog = prog = 0;
            p->failed = 1;
        } else {
            for (i = 0; i < ai.immediates; i++)
                gl->ProgramLocalParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, i, (const GLfloat *)ai.imm[i]);
            if (key & 3)
                p->fog_base = ai.constants - 2;
            p->bump = !p->version2 && info.constants == RB_PS_BUMP_CONSTANTS;
            tracef("rd3d: pixel shader %lx%s: program %lu, samplers %lx, coordinates %lx\r\n",
                   p->handle, key ? " (variant)" : "", (DWORD)prog, (DWORD)info.samplers,
                   (DWORD)info.coords);
        }
    }
    release(tgsi);
    release(arb);
    return prog;
}

static void ps_create(RCTX *c, DWORD handle, const DWORD *code, DWORD bytes)
{
    struct ps8 *p;
    ps_delete(c, handle);
    p = ps_find(c, handle, 1);
    if (!p) {
        dbg("rd3d: too many pixel shaders\r\n");
        return;
    }
    p->code = (DWORD *)alloc(bytes);
    if (p->code) {
        memcpy(p->code, code, bytes);
        p->bytes = bytes;
    }
    p->version2 = bytes >= 4 && code[0] >= 0xFFFF0200UL;
    p->var[0].key = 0;
    p->var[0].prog = ps_compile(c, p, 0);   /* now, so a shader that can't be made says so early */
    p->nvar = 1;
}

/* The stages t0..t3 whose texture is a cube map (bits 0-3) or a volume (bits 8-11): a version 1
   shader's tex reads them as such. Shifted by RB_PS_CUBE_SHIFT, the translator's argument. */
static DWORD cube_stages(RCTX *c)
{
    DWORD s, mask = 0;
    for (s = 0; s < RB_PS_COORD_SETS; s++) {
        DWORD map = c->tss[s][D3DTSS_TEXTUREMAP];
        LPDDRAWI_DDRAWSURFACE_LCL l = map ? handle_get(c->ddl, map) : NULL;
        if (l && is_cube(l))
            mask |= 1UL << s;
        if (l && is_volume(l))
            mask |= 1UL << (RB_PS_VOLUME_SHIFT - RB_PS_CUBE_SHIFT + s);
    }
    return mask;
}

/* The program for the pixel shader set, the fog state and (version 1) the cube map stages, 0
   for the fixed-function stages; -1 if a shader is set but its program couldn't be made (its
   draws are skipped). Programs are made when first needed and kept, PS_VARIANTS a shader. */
static long ps_program(RCTX *c)
{
    struct ps8 *p;
    DWORD key, i;
    if (!c->ps_bound)
        return 0;
    p = ps_find(c, c->ps_bound, 0);
    if (!p || p->failed)
        return -1;
    key = c->rs[D3DRENDERSTATE_FOGENABLE] ? PS_FOG : 0;
    if (!p->version2)
        key |= cube_stages(c) << RB_PS_CUBE_SHIFT;
    c->fp_fog = (key & 3) != 0;
    for (i = 0; i < p->nvar; i++)
        if (p->var[i].key == key)
            return p->var[i].prog ? (long)p->var[i].prog : -1;
    if (p->nvar == PS_VARIANTS) {       /* full: the last one makes room */
        GLF *gl = gl_of(c);
        i = PS_VARIANTS - 1;
        if (p->var[i].prog && gl->DeleteProgramsARB)
            gl->DeleteProgramsARB(1, &p->var[i].prog);
        if (c->fp_prog == p->var[i].prog)
            c->fp_prog = 0;
        p->nvar--;
    }
    i = p->nvar++;
    p->var[i].key = key;
    p->var[i].prog = ps_compile(c, p, key);
    return p->var[i].prog ? (long)p->var[i].prog : -1;
}

/* Before a draw: the fragment program on or off, bound, its constants loaded. */
static void ps_apply(RCTX *c, long prog)
{
    GLF *gl = gl_of(c);
    struct ps8 *p;
    DWORD i;
    if (prog <= 0) {
        if (c->fp_on) {
            gl->Disable(GL_FRAGMENT_PROGRAM_ARB);
            c->fp_on = 0;
        }
        return;
    }
    if (!c->fp_on) {
        gl->Enable(GL_FRAGMENT_PROGRAM_ARB);
        c->fp_on = 1;
    }
    if (c->fp_prog != (GLuint)prog) {
        gl->BindProgramARB(GL_FRAGMENT_PROGRAM_ARB, (GLuint)prog);
        c->fp_prog = (GLuint)prog;
    }
    if (!gl->ProgramEnvParameter4fvARB)
        return;
    if (c->psc_dirty) {
        for (i = 0; i < RB_PS2_CONSTANTS; i++)
            gl->ProgramEnvParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, i, c->psc[i]);
        c->psc_dirty = 0;
    }
    p = ps_find(c, c->ps_bound, 0);
    if (p && p->bump) {
        /* texbem's c8..c11: each stage's bump matrix; c12..c15: its luminance scale and offset */
        DWORD s;
        for (s = 0; s < RB_PS_COORD_SETS; s++) {
            const DWORD *t = c->tss[s];
            GLfloat m[4], lum[4];
            m[0] = *(const float *)&t[D3DTSS_BUMPENVMAT00];
            m[1] = *(const float *)&t[D3DTSS_BUMPENVMAT01];
            m[2] = *(const float *)&t[D3DTSS_BUMPENVMAT10];
            m[3] = *(const float *)&t[D3DTSS_BUMPENVMAT11];
            lum[0] = *(const float *)&t[D3DTSS_BUMPENVLSCALE];
            lum[1] = *(const float *)&t[D3DTSS_BUMPENVLOFFSET];
            lum[2] = lum[3] = 0.0f;
            gl->ProgramEnvParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, RB_PS_CONSTANTS + s, m);
            gl->ProgramEnvParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, RB_PS_CONSTANTS + 4 + s, lum);
        }
        c->psc_dirty = 1;               /* those registers may be a later shader's constants */
    }
    p = ps_find(c, c->ps_bound, 0);
    if (p && c->fp_fog) {
        DWORD col = c->rs[D3DRENDERSTATE_FOGCOLOR];
        GLfloat fc[4], range[4];
        fc[0] = ((col >> 16) & 255) / 255.0f;
        fc[1] = ((col >> 8) & 255) / 255.0f;
        fc[2] = (col & 255) / 255.0f;
        fc[3] = 1.0f;
        range[0] = 0.75f;               /* start and end that turn 0.25 + 0.5 f back into f */
        range[1] = 0.25f;
        range[2] = range[3] = 0.0f;
        gl->ProgramEnvParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, p->fog_base, fc);
        gl->ProgramEnvParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, p->fog_base + 1, range);
        c->psc_dirty = 1;               /* those registers may be a later shader's constants */
    }
}

/* The buffer object holding buffer h, bound to target (GL_ARRAY_BUFFER_ARB for vertices,
   GL_ELEMENT_ARRAY_BUFFER_ARB for indices): made on first use and uploaded again only after
   the buffer was written. 0 when the buffer is the runtime's (or another context has it). */
static GLuint buffer_object(RCTX *c, DWORD h, GLenum target)
{
    GLF *gl = gl_of(c);
    DDLOCAL *d = ddlocal(c->ddl, 0);
    struct buf8 *b;
    struct bufhead *head;
    DWORD i;

    if (!d || !h || h >= d->nbuf || !gl->BufferDataARB || !gl->GenBuffersARB)
        return 0;
    for (i = 0; i < d->ntrash; )        /* names this context let go of */
        if (d->trash[i].ctx == (void *)c) {
            gl->DeleteBuffersARB(1, &d->trash[i].name);
            d->trash[i] = d->trash[--d->ntrash];
        } else {
            i++;
        }
    b = &d->buf[h];
    /* Only buffers the program fills once (HINTSTATIC): the runtime appends to its dynamic ones
       (DrawPrimitiveUP's) after a single Lock, so their writes are not all seen here. */
    if (!b->ours || !b->fixed || !(head = buffer_head(b->mem)) || (b->name && b->ctx != (void *)c))
        return 0;
    if (!b->name) {
        gl->GenBuffersARB(1, &b->name);
        if (!b->name)
            return 0;
        b->ctx = c;
        b->writes = head->writes - 1;
    }
    bind_buffer(c, target, b->name);
    if (b->writes != head->writes) {
        gl->BufferDataARB(target, (int)b->size, (const void *)b->mem,
                          b->fixed ? GL_STATIC_DRAW_ARB : GL_DYNAMIC_DRAW_ARB);
        b->writes = head->writes;
        tracef("rd3d: buffer %lx to the GPU, %lu bytes\r\n", h, b->size);
    }
    return b->name;
}

/* ---- Direct3D 8 draws ----------------------------------------------------------------------- */
/* The vertices of a declaration that came without code: the fixed-function pipeline reads them
   through it, each register a fixed attribute (Direct3D 8's D3DVSDE_*: 0 position, 3 normal,
   5 diffuse, 6 specular, 7 to 14 texture coordinate sets; blend weights and indices, point size
   and the tweening inputs are not read), each from its own stream. 0 if the position or a type
   is not what the pipeline's program reads. */
static int decl_vertices(const struct vs8 *s, VERTICES *v)
{
    const VS_ARB *d = &s->info;
    DWORD i;

    if (s->failed || !(d->inputs & 1) || d->type[0] != 2)   /* the position: three floats */
        return 0;
    v->xyz = 1;
    v->pos = d->offset[0];
    v->src[0] = d->stream[0];
    v->normal = v->diffuse = v->specular = -1;
    for (i = 0; i < 8; i++) {
        v->tex[i] = -1;
        v->texsize[i] = 0;
    }
    if (d->inputs & (1UL << 3)) {
        if (d->type[3] != 2)
            return 0;
        v->normal = d->offset[3];
        v->src[1] = d->stream[3];
    }
    if (d->inputs & (1UL << 5)) {
        if (d->type[5] != VS_ARB_COLOR)
            return 0;
        v->diffuse = d->offset[5];
        v->src[2] = d->stream[5];
    }
    if (d->inputs & (1UL << 6)) {
        if (d->type[6] != VS_ARB_COLOR)
            return 0;
        v->specular = d->offset[6];
        v->src[3] = d->stream[6];
    }
    for (i = 0; i < 8; i++)
        if (d->inputs & (1UL << (7 + i))) {
            if (d->type[7 + i] > VS_ARB_FLOAT4)
                return 0;
            v->tex[i] = d->offset[7 + i];
            v->texsize[i] = d->type[7 + i] + 1;
            v->src[4 + i] = d->stream[7 + i];
        }
    return 1;
}

/* Stream zero's vertices: a buffer surface named by its handle, or, for SETSTREAMSOURCEUM, the
   vertices that came with the command buffer. 0 when there is nothing usable. */
static int stream_vertices(RCTX *c, LPD3DHAL_DRAWPRIMITIVES2DATA d, VERTICES *v)
{
    BYTE *data;
    DWORD bytes;

    memset(v, 0, sizeof *v);
    if (c->fvf8 & 1) {                  /* a vertex shader's handle: its declaration lays it out */
        struct vs8 *s = vs_find(c, c->fvf8);
        if (!tnl_on || !c->stream_stride)
            return 0;
        if (s && !s->ncode) {           /* no code: the fixed-function pipeline, through it */
            if (!decl_vertices(s, v))
                return 0;
        } else {
            v->shader = 1;
            v->diffuse = v->specular = v->normal = -1;
        }
    } else if (!parse_fvf(c->fvf8, v) || !c->stream_stride || c->stream_stride < v->stride) {
        return 0;
    }
    v->stride = c->stream_stride;       /* the stream's own (fields may be followed by padding) */
    if (c->stream_user) {
        if (d->dwFlags & D3DHALDP2_USERMEMVERTICES) {
            if (!d->lpVertices)
                return 0;
            data = (BYTE *)d->lpVertices + d->dwVertexOffset;
            bytes = d->dwVertexLength * c->stream_stride;
        } else {
            if (!d->lpDDVertex || !d->lpDDVertex->lpGbl->fpVidMem ||
                d->dwVertexOffset > d->lpDDVertex->lpGbl->dwLinearSize)
                return 0;
            data = (BYTE *)d->lpDDVertex->lpGbl->fpVidMem + d->dwVertexOffset;
            bytes = d->lpDDVertex->lpGbl->dwLinearSize - d->dwVertexOffset;
        }
    } else {
        data = buffer_get(c->ddl, c->stream_handle, &bytes);
        if (!data)
            return 0;
        if (v->shader || v->xyz) {      /* drawn by a vertex program: from a buffer object */
            v->vbo = buffer_object(c, c->stream_handle, GL_ARRAY_BUFFER_ARB);
            v->start = data;
        }
    }
    v->base = data;
    v->count = bytes / c->stream_stride;
    return 1;
}

/* A D3DPRIMITIVETYPE with its primitive count: the OpenGL mode and the number of vertices. */
static GLenum prim_mode(DWORD type, DWORD prims, DWORD *verts)
{
    switch (type) {
    case RbPointList:     *verts = prims;     return GL_POINTS;
    case RbLineList:      *verts = prims * 2; return GL_LINES;
    case RbLineStrip:     *verts = prims + 1; return GL_LINE_STRIP;
    case RbTriangleList:  *verts = prims * 3; return GL_TRIANGLES;
    case RbTriangleStrip: *verts = prims + 2; return GL_TRIANGLE_STRIP;
    case RbTriangleFan:   *verts = prims + 2; return GL_TRIANGLE_FAN;
    }
    *verts = 0;
    return 0;
}

static void skip8(RCTX *c, const char *why)
{
    if (!c->skipped8++) {
        dbg("rd3d: Direct3D 8 draw not drawn: "); dbg(why); dbg("\r\n");
    }
}

/* Make v start at byte offset start of the stream. The runtime packs vertices of different
   formats into one buffer, so a draw may start at any byte, not only at a whole vertex. 0 if
   that is outside the stream. */
static int stream_from(VERTICES *v, long start)
{
    DWORD bytes = v->count * v->stride;
    if (start < 0 || (v->count && (DWORD)start >= bytes))
        return 0;
    v->base += start;
    if (v->count)
        v->count = (bytes - (DWORD)start) / v->stride;
    /* the same vertex in the draw's other streams (vs_arrays) */
    v->first_vertex = v->first_vertex >= 0 && v->stride && start % (long)v->stride == 0 ?
                      v->first_vertex + start / (long)v->stride : -1;
    return 1;
}

/* One Direct3D 8 draw from stream zero. first is the byte offset of vertex zero in the stream
   (a negative base vertex makes it negative); an indexed draw reads 16-bit indices from the
   index buffer at index_byte. */
static void stream_draw(RCTX *c, LPD3DHAL_DRAWPRIMITIVES2DATA d, DWORD type, long first,
                        DWORD prims, int indexed, DWORD index_byte)
{
    VERTICES v;
    DWORD verts, lo, span;
    GLenum mode = prim_mode(type, prims, &verts);

    tracef("rd3d: draw type %lu, first byte %ld, %lu primitives%s, stream %lx stride %lu%s, fvf %lx\r\n",
           type, first, prims, indexed ? " indexed" : "", c->stream_handle, c->stream_stride,
           c->stream_user ? " (user)" : "", c->fvf8);
    if (!prims)
        return;
    if (!verts) {                       /* (not !mode: GL_POINTS is 0, and point lists were never drawn) */
        skip8(c, "primitive type");
        return;
    }
    if ((c->fvf8 & 1) && !tnl_on) {     /* a vertex shader's handle, without hardware vertex processing */
        skip8(c, "vertex shader");
        return;
    }
    if (!stream_vertices(c, d, &v)) {
        skip8(c, "stream zero");
        return;
    }
    if (!indexed) {
        if (!stream_from(&v, first)) {
            skip8(c, "first vertex");
            return;
        }
        draw(c, &v, mode, 0, verts, NULL, 0, 0);
    } else {
        DWORD size = 0;
        BYTE *ib = buffer_get(c->ddl, c->index_handle, &size);
        WORD *idx;
        DWORD isz = c->index_stride;    /* 16-bit indices, or 32-bit ones */
        int wide = 0;
        if (!ib || (isz != 2 && isz != 4)) {
            skip8(c, "index buffer");
            return;
        }
        if (index_byte > size || verts > (size - index_byte) / isz) {
            skip8(c, "index range");
            return;
        }
        if (v.vbo && stream_from(&v, first)) {
            /* vertices on the GPU: the indices as they are, from vertex zero on, from their own
               buffer object if they have one (no copy, no scan for their range) */
            v.ibo = buffer_object(c, c->index_handle, GL_ELEMENT_ARRAY_BUFFER_ARB);
            v.itype = isz == 4 ? GL_UNSIGNED_INT : 0;
            draw(c, &v, mode, 0, 0, v.ibo ? (const WORD *)index_byte : (const WORD *)(ib + index_byte),
                 verts, 0);
            return;
        }
        if (isz == 4) {
            idx = (WORD *)index_list32(c, (const DWORD *)(ib + index_byte), verts, &lo, &span, &wide);
            v.itype = wide ? GL_UNSIGNED_INT : 0;
        } else {
            idx = index_list(c, (const WORD *)(ib + index_byte), verts, 1, 1, &lo, &span);
        }
        if (!idx) {
            dbg("rd3d: no memory for the indices of a draw\r\n");
            return;
        }
        /* the indices were made relative to the smallest one, lo */
        if (!stream_from(&v, first + (long)lo * (long)v.stride)) {
            skip8(c, "base vertex");
            return;
        }
        draw(c, &v, mode, 0, 0, idx, verts, span);
    }
}

/* A copy between two buffers (the runtime's managed vertex and index buffers). */
static void buffer_blt(RCTX *c, const RB_DP2_BUFFER_BLT *b)
{
    DWORD dsize = 0, ssize = 0, offset = b->range[0], size = b->range[1];
    BYTE *dst = buffer_get(c->ddl, b->dest, &dsize), *src = buffer_get(c->ddl, b->source, &ssize);
    if (!dst || !src || offset > ssize || size > ssize - offset || b->offset > dsize ||
        size > dsize - b->offset) {
        dbg("rd3d: buffer copy out of range\r\n");
        return;
    }
    memmove(dst + b->offset, src + offset, size);
    {
        DDLOCAL *d = ddlocal(c->ddl, 0);
        if (d && b->dest < d->nbuf && d->buf[b->dest].ours)
            d3d_buffer_written(d->buf[b->dest].mem);    /* its buffer object is out of date */
    }
}

/* Commands that carry n records of size bytes, each followed by a variable part of the length
   in the record's words w1 (and w2, if not 0): the bytes they take, or 0xFFFFFFFF. */
static DWORD variable_records(const BYTE *p, const BYTE *end, DWORD n, DWORD size, DWORD w1, DWORD w2)
{
    DWORD used = 0, k, left = (DWORD)(end - p);
    for (k = 0; k < n; k++) {
        const DWORD *r = (const DWORD *)(p + used);
        DWORD extra;
        if (left - used < size)
            return 0xFFFFFFFFUL;
        extra = r[w1] + (w2 ? r[w2] : 0);
        if (extra < r[w1] || extra > left - used - size)
            return 0xFFFFFFFFUL;
        used += size + extra;
    }
    return used;
}

static DWORD WINAPI DrawPrimitives2(LPD3DHAL_DRAWPRIMITIVES2DATA d)
{
    RCTX *c = context_of(d->dwhContext);
    BYTE *cmd, *end, *start;
    VERTICES v;
    GLF *gl;

    if (!c) {
        d->ddrval = D3DHAL_CONTEXT_BAD;
        return DDHAL_DRIVER_HANDLED;
    }
    gl = gl_of(c);
    if (!make_current(c) || !framebuffer(c)) { /* a new size, or other memory after a flip */
        let_go(c);
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    memset(&v, 0, sizeof v);
    if (!parse_fvf(d->dwVertexType, &v))
        v.stride = d->dwVertexSize ? d->dwVertexSize : 32;
    if (d->dwFlags & D3DHALDP2_USERMEMVERTICES)
        v.base = (BYTE *)d->lpVertices + d->dwVertexOffset;
    else if (d->lpDDVertex)
        v.base = (BYTE *)d->lpDDVertex->lpGbl->fpVidMem + d->dwVertexOffset;
    v.count = d->dwVertexLength;
    start = (BYTE *)d->lpDDCommands->lpGbl->fpVidMem + d->dwCommandOffset;
    cmd = start;
    end = start + d->dwCommandLength;
    tracef("rd3d: commands %lu bytes, flags %lx, vertices %08lx+%lu, %lu of them\r\n", d->dwCommandLength,
           d->dwFlags, (DWORD)v.base, d->dwVertexOffset, d->dwVertexLength);
    d->ddrval = DD_OK;

    while (cmd < end) {
        LPD3DHAL_DP2COMMAND k = CMD(cmd);
        BYTE *data = cmd + sizeof(D3DHAL_DP2COMMAND);
        DWORD n = k->wPrimitiveCount, i;

        tracef("rd3d: command %lu, count %lu\r\n", (DWORD)k->bCommand, n);

        switch (k->bCommand) {
        case D3DDP2OP_RENDERSTATE: {
            LPD3DHAL_DP2RENDERSTATE rs = (LPD3DHAL_DP2RENDERSTATE)data;
            for (i = 0; i < n; i++) {
                render_state(c, rs[i].RenderState, rs[i].dwState);
                if (d->lpdwRStates && rs[i].RenderState < D3DHAL_MAX_RSTATES)
                    d->lpdwRStates[rs[i].RenderState] = rs[i].dwState;
                switch (rs[i].RenderState) {    /* the vertex programs' program.env */
                case D3DRENDERSTATE_AMBIENT:
                    c->tnl_dirty |= TNL_DIRTY_LIGHT;
                    break;
                case D3DRENDERSTATE_FOGSTART:
                case D3DRENDERSTATE_FOGEND:
                case D3DRENDERSTATE_FOGDENSITY:
                    c->tnl_dirty |= TNL_DIRTY_FOG;
                    break;
                }
            }
            cmd = data + n * sizeof(D3DHAL_DP2RENDERSTATE);
            break;
        }
        case D3DDP2OP_TEXTURESTAGESTATE: {
            LPD3DHAL_DP2TEXTURESTAGESTATE ts = (LPD3DHAL_DP2TEXTURESTAGESTATE)data;
            for (i = 0; i < n; i++) {
                DWORD value = ts[i].dwValue;
                /* Direct3D 8 filters: mip NONE, POINT, LINEAR are 0..2 (Direct3D 7 uses 1..3), and
                   magnification ANISOTROPIC is 3 (FLATCUBIC in Direct3D 7). */
                if (c->dx8 && ts[i].TSState == D3DTSS_MIPFILTER)
                    value++;
                else if (c->dx8 && ts[i].TSState == D3DTSS_MAGFILTER && value == 3)
                    value = D3DTFG_LINEAR;
                stage_state(c, ts[i].wStage, ts[i].TSState, value);
            }
            cmd = data + n * sizeof(D3DHAL_DP2TEXTURESTAGESTATE);
            break;
        }
        case D3DDP2OP_VIEWPORTINFO: {
            /* the rectangle to clip to (apply_scissor): the last one counts */
            const D3DHAL_DP2VIEWPORTINFO *vp = (const D3DHAL_DP2VIEWPORTINFO *)data;
            if (n) {
                vp += n - 1;
                c->vp_x = vp->dwX;
                c->vp_y = vp->dwY;
                c->vp_w = vp->dwWidth;
                c->vp_h = vp->dwHeight;
                apply_scissor(c);
                c->tnl_dirty |= TNL_DIRTY_XF;   /* the half pixel, and OpenGL's viewport */
            }
            cmd = data + n * sizeof(D3DHAL_DP2VIEWPORTINFO);
            break;
        }
        case D3DDP2OP_WINFO:
            cmd = data + n * sizeof(D3DHAL_DP2WINFO);
            break;
        case D3DDP2OP_ZRANGE:
            if (n) {
                const D3DHAL_DP2ZRANGE *z = (const D3DHAL_DP2ZRANGE *)data + (n - 1);
                c->zmin = z->dvMinZ;
                c->zmax = z->dvMaxZ;
                c->tnl_dirty |= TNL_DIRTY_XF;
            }
            cmd = data + n * sizeof(D3DHAL_DP2ZRANGE);
            break;
        /* hardware vertex processing's state (tnl_*) */
        case D3DDP2OP_SETTRANSFORM: {
            const D3DHAL_DP2SETTRANSFORM *t = (const D3DHAL_DP2SETTRANSFORM *)data;
            if (data + n * sizeof *t > end)
                goto bad;
            for (i = 0; i < n; i++)
                tnl_transform(c, (DWORD)t[i].xfrmType, (const float *)&t[i].matrix, 0);
            cmd = data + n * sizeof *t;
            break;
        }
        case D3DDP2OP_SETMATERIAL:
            if (data + n * sizeof(D3DMATERIAL7) > end)
                goto bad;
            if (n) {
                c->material = ((const D3DMATERIAL7 *)data)[n - 1];
                c->tnl_dirty |= TNL_DIRTY_LIGHT;
            }
            cmd = data + n * sizeof(D3DMATERIAL7);
            break;
        case D3DDP2OP_CREATELIGHT: {
            const D3DHAL_DP2CREATELIGHT *l = (const D3DHAL_DP2CREATELIGHT *)data;
            if (data + n * sizeof *l > end)
                goto bad;
            for (i = 0; i < n; i++)
                light_slot(c, l[i].dwIndex, 1);     /* a new light: off, no data yet */
            cmd = data + n * sizeof *l;
            break;
        }
        case D3DDP2OP_SETLIGHT: {
            BYTE *at = data;            /* each record, with the light itself after a DATA one */
            for (i = 0; i < n; i++) {
                const D3DHAL_DP2SETLIGHT *l = (const D3DHAL_DP2SETLIGHT *)at;
                const D3DLIGHT7 *ld = NULL;
                if (at + sizeof *l > end)
                    goto bad;
                at += sizeof *l;
                if (l->dwDataType == D3DHAL_SETLIGHT_DATA) {
                    if (at + sizeof(D3DLIGHT7) > end)
                        goto bad;
                    ld = (const D3DLIGHT7 *)at;
                    at += sizeof(D3DLIGHT7);
                }
                tnl_light(c, l, ld);
            }
            cmd = at;
            break;
        }
        case D3DDP2OP_SETRENDERTARGET: {
            LPD3DHAL_DP2SETRENDERTARGET t = (LPD3DHAL_DP2SETRENDERTARGET)data;
            for (i = 0; i < n; i++) {
                LPDDRAWI_DDRAWSURFACE_LCL rt = handle_get(c->ddl, t[i].hRenderTarget);
                if (rt && !set_render_target(c, rt, handle_get(c->ddl, t[i].hZBuffer))) {
                    d->ddrval = DDERR_GENERIC;
                    goto done;
                }
            }
            cmd = data + n * sizeof(D3DHAL_DP2SETRENDERTARGET);
            break;
        }
        case D3DDP2OP_CLEAR: {
            LPD3DHAL_DP2CLEAR cl = (LPD3DHAL_DP2CLEAR)data;
            clear(c, cl->dwFlags, cl->dwFillColor, cl->dvFillDepth, cl->dwFillStencil, cl->Rects, n);
            cmd = data + sizeof(D3DHAL_DP2CLEAR) - sizeof(RECT) + n * sizeof(RECT);
            break;
        }
        case D3DDP2OP_POINTS: {
            LPD3DHAL_DP2POINTS pt = (LPD3DHAL_DP2POINTS)data;
            for (i = 0; i < n; i++)
                draw(c, &v, GL_POINTS, pt[i].wVStart, pt[i].wCount, NULL, 0, 0);
            cmd = data + n * sizeof(D3DHAL_DP2POINTS);
            break;
        }
        case D3DDP2OP_LINELIST:
            draw(c, &v, GL_LINES, ((LPD3DHAL_DP2LINELIST)data)->wVStart, n * 2, NULL, 0, 0);
            cmd = data + sizeof(D3DHAL_DP2LINELIST);
            break;
        case D3DDP2OP_LINESTRIP:
            draw(c, &v, GL_LINE_STRIP, ((LPD3DHAL_DP2LINESTRIP)data)->wVStart, n + 1, NULL, 0, 0);
            cmd = data + sizeof(D3DHAL_DP2LINESTRIP);
            break;
        case D3DDP2OP_TRIANGLELIST:
            draw(c, &v, GL_TRIANGLES, ((LPD3DHAL_DP2TRIANGLELIST)data)->wVStart, n * 3, NULL, 0, 0);
            cmd = data + sizeof(D3DHAL_DP2TRIANGLELIST);
            break;
        case D3DDP2OP_TRIANGLESTRIP:
            draw(c, &v, GL_TRIANGLE_STRIP, ((LPD3DHAL_DP2TRIANGLESTRIP)data)->wVStart, n + 2, NULL, 0, 0);
            cmd = data + sizeof(D3DHAL_DP2TRIANGLESTRIP);
            break;
        case D3DDP2OP_TRIANGLEFAN:
            draw(c, &v, GL_TRIANGLE_FAN, ((LPD3DHAL_DP2TRIANGLEFAN)data)->wVStart, n + 2, NULL, 0, 0);
            cmd = data + sizeof(D3DHAL_DP2TRIANGLEFAN);
            break;
        case D3DDP2OP_INDEXEDLINELIST:
        case D3DDP2OP_INDEXEDTRIANGLELIST:
        case D3DDP2OP_INDEXEDLINELIST2:
        case D3DDP2OP_INDEXEDTRIANGLELIST2:
        case D3DDP2OP_INDEXEDLINESTRIP:
        case D3DDP2OP_INDEXEDTRIANGLESTRIP:
        case D3DDP2OP_INDEXEDTRIANGLEFAN: {
            DWORD base = 0, lo, span, step, keep, count, size;
            GLenum mode;
            WORD *src, *idx;
            switch (k->bCommand) {
            case D3DDP2OP_INDEXEDLINELIST:      mode = GL_LINES; step = 2; keep = 2; count = n; size = 4; break;
            case D3DDP2OP_INDEXEDTRIANGLELIST:  mode = GL_TRIANGLES; step = 4; keep = 3; count = n; size = 8; break;
            case D3DDP2OP_INDEXEDLINELIST2:     mode = GL_LINES; step = 2; keep = 2; count = n; size = 4; break;
            case D3DDP2OP_INDEXEDTRIANGLELIST2: mode = GL_TRIANGLES; step = 3; keep = 3; count = n; size = 6; break;
            case D3DDP2OP_INDEXEDLINESTRIP:     mode = GL_LINE_STRIP; step = 1; keep = 1; count = n + 1; size = 2; break;
            case D3DDP2OP_INDEXEDTRIANGLESTRIP: mode = GL_TRIANGLE_STRIP; step = 1; keep = 1; count = n + 2; size = 2; break;
            default:                            mode = GL_TRIANGLE_FAN; step = 1; keep = 1; count = n + 2; size = 2; break;
            }
            src = (WORD *)data;
            if (k->bCommand != D3DDP2OP_INDEXEDLINELIST && k->bCommand != D3DDP2OP_INDEXEDTRIANGLELIST) {
                base = ((LPD3DHAL_DP2STARTVERTEX)data)->wVStart;
                src = (WORD *)(data + sizeof(D3DHAL_DP2STARTVERTEX));
            }
            idx = index_list(c, src, count, step, keep, &lo, &span);
            if (idx)
                draw(c, &v, mode, base + lo, 0, idx, count * keep, span);
            else
                dbg("rd3d: no memory for the indices of a command\r\n");
            cmd = (BYTE *)src + count * size;
            break;
        }
        case D3DDP2OP_LINELIST_IMM: {
            VERTICES iv = v;
            iv.base = ALIGN4(data);
            iv.count = n * 2;
            draw(c, &iv, GL_LINES, 0, n * 2, NULL, 0, 0);
            cmd = iv.base + n * 2 * v.stride;
            break;
        }
        case D3DDP2OP_TRIANGLEFAN_IMM: {
            VERTICES iv = v;
            iv.base = ALIGN4(data + sizeof(D3DHAL_DP2TRIANGLEFAN_IMM));
            iv.count = n + 2;
            draw(c, &iv, GL_TRIANGLE_FAN, 0, n + 2, NULL, 0, 0);
            cmd = iv.base + (n + 2) * v.stride;
            break;
        }
        /* Direct3D 8 (the runtime sends these only to a driver that answered GetDriverInfo2) */
        case RbDp2SetVertexShader:
        case RbDp2DeleteVertexShader:
        case RbDp2SetPixelShader:
        case RbDp2DeletePixelShader: {
            const RB_DP2_HANDLE *h = (const RB_DP2_HANDLE *)data;
            if (data + n * sizeof *h > end)
                goto bad;
            if (n && k->bCommand == RbDp2SetVertexShader)
                c->fvf8 = h[n - 1].handle;
            if (n && k->bCommand == RbDp2SetPixelShader && c->ps_bound != h[n - 1].handle) {
                c->ps_bound = h[n - 1].handle;
                c->stages_dirty = 1;        /* the stages are set up the other way */
            }
            if (k->bCommand == RbDp2DeletePixelShader)
                for (i = 0; i < n; i++)
                    ps_delete(c, h[i].handle);
            if (k->bCommand == RbDp2DeleteVertexShader)
                for (i = 0; i < n; i++)
                    vs_delete(c, h[i].handle);
            cmd = data + n * sizeof *h;
            break;
        }
        case RbDp2SetStreamSource: {
            const RB_DP2_STREAM_SOURCE *s = (const RB_DP2_STREAM_SOURCE *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            for (i = 0; i < n; i++)
                if (!s[i].stream) {
                    c->stream_user = 0;
                    c->stream_handle = s[i].handle;
                    c->stream_stride = s[i].stride;
                } else if (s[i].stream < VS_ARB_STREAMS) {
                    c->sx_handle[s[i].stream] = s[i].handle;
                    c->sx_stride[s[i].stream] = s[i].stride;
                }
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2SetStreamSourceUm: {
            const RB_DP2_STREAM_SOURCE_UM *s = (const RB_DP2_STREAM_SOURCE_UM *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            for (i = 0; i < n; i++)
                if (!s[i].stream) {
                    c->stream_user = 1;
                    c->stream_handle = 0;
                    c->stream_stride = s[i].stride;
                }
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2SetIndices: {
            const RB_DP2_INDICES *s = (const RB_DP2_INDICES *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            if (n) {
                c->index_handle = s[n - 1].handle;
                c->index_stride = s[n - 1].stride;
            }
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2DrawPrimitive: {
            const RB_DP2_DRAW_PRIMITIVE *s = (const RB_DP2_DRAW_PRIMITIVE *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            for (i = 0; i < n; i++)
                stream_draw(c, d, s[i].type, (long)(s[i].start * c->stream_stride), s[i].count, 0, 0);
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2DrawPrimitive2: {
            const RB_DP2_DRAW_PRIMITIVE2 *s = (const RB_DP2_DRAW_PRIMITIVE2 *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            for (i = 0; i < n; i++)
                stream_draw(c, d, s[i].type, (long)s[i].firstOffset, s[i].count, 0, 0);
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2ClippedTriangleFan: {
            const RB_DP2_CLIPPED_FAN *s = (const RB_DP2_CLIPPED_FAN *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            for (i = 0; i < n; i++)
                stream_draw(c, d, RbTriangleFan, (long)s[i].firstOffset, s[i].count, 0, 0);
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2DrawIndexedPrimitive: {
            const RB_DP2_DRAW_INDEXED *s = (const RB_DP2_DRAW_INDEXED *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            for (i = 0; i < n; i++)
                stream_draw(c, d, s[i].type, s[i].baseVertex * (long)c->stream_stride, s[i].count, 1,
                            s[i].startIndex * c->index_stride);
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2DrawIndexedPrimitive2: {
            const RB_DP2_DRAW_INDEXED2 *s = (const RB_DP2_DRAW_INDEXED2 *)data;
            if (data + n * sizeof *s > end)
                goto bad;
            for (i = 0; i < n; i++)
                stream_draw(c, d, s[i].type, s[i].baseOffset, s[i].count, 1, s[i].startOffset);
            cmd = data + n * sizeof *s;
            break;
        }
        case RbDp2BufferBlt: {
            const RB_DP2_BUFFER_BLT *b = (const RB_DP2_BUFFER_BLT *)data;
            if (data + n * sizeof *b > end)
                goto bad;
            for (i = 0; i < n; i++)
                buffer_blt(c, &b[i]);
            cmd = data + n * sizeof *b;
            break;
        }
        case RbDp2SetVertexShaderConst:
        case RbDp2SetPixelShaderConst: {
            /* each record: register, count, count vectors; vertex shaders' are skipped */
            DWORD used = 0, left = (DWORD)(end - data), j;
            for (i = 0; i < n; i++) {
                const RB_DP2_SHADER_CONST *r = (const RB_DP2_SHADER_CONST *)(data + used);
                const float *f = (const float *)(r + 1);
                if (left - used < sizeof *r || r->count > (left - used - sizeof *r) / 16)
                    goto bad;
                for (j = 0; k->bCommand == RbDp2SetPixelShaderConst && j < r->count; j++)
                    if (r->reg + j < RB_PS2_CONSTANTS) {
                        memcpy(c->psc[r->reg + j], f + 4 * j, 16);
                        c->psc_dirty = 1;
                    }
                for (j = 0; k->bCommand == RbDp2SetVertexShaderConst && j < r->count; j++)
                    if (r->reg + j < VS_ARB_CONSTANTS) {
                        DWORD reg = r->reg + j;
                        memcpy(c->vsc[reg], f + 4 * j, 16);
                        if (c->vsc_lo == c->vsc_hi || reg < c->vsc_lo)
                            c->vsc_lo = reg;
                        if (reg + 1 > c->vsc_hi)
                            c->vsc_hi = reg + 1;
                    }
                used += sizeof *r + r->count * 16;
            }
            cmd = data + used;
            break;
        }
        case RbDp2CreateVertexShader:
        case RbDp2CreatePixelShader: {
            DWORD used = k->bCommand == RbDp2CreateVertexShader ?
                variable_records(data, end, n, sizeof(RB_DP2_CREATE_VERTEX_SHADER), 1, 2) :
                variable_records(data, end, n, sizeof(RB_DP2_CREATE_PIXEL_SHADER), 1, 0);
            if (used == 0xFFFFFFFFUL)
                goto bad;
            if (k->bCommand == RbDp2CreatePixelShader) {
                DWORD at = 0;
                for (i = 0; i < n; i++) {
                    const RB_DP2_CREATE_PIXEL_SHADER *r = (const RB_DP2_CREATE_PIXEL_SHADER *)(data + at);
                    ps_create(c, r->handle, (const DWORD *)(r + 1), r->codeBytes);
                    at += sizeof *r + r->codeBytes;
                }
            } else {
                DWORD at = 0;
                for (i = 0; i < n; i++) {
                    const RB_DP2_CREATE_VERTEX_SHADER *r = (const RB_DP2_CREATE_VERTEX_SHADER *)(data + at);
                    if (tnl_on)
                        vs_create(c, r);
                    at += sizeof *r + r->declBytes + r->codeBytes;
                }
            }
            cmd = data + used;
            break;
        }
        case RbDp2MultiplyTransform: {
            const RB_DP2_MULTIPLY_TRANSFORM *t = (const RB_DP2_MULTIPLY_TRANSFORM *)data;
            if (data + n * sizeof *t > end)
                goto bad;
            for (i = 0; i < n; i++)
                tnl_transform(c, t[i].type, (const float *)t[i].matrix, 1);
            cmd = data + n * sizeof *t;
            break;
        }
        case RbDp2AddDirtyRect:
            cmd = data + n * sizeof(RB_DP2_DIRTY_RECT);
            break;
        case RbDp2AddDirtyBox:
            cmd = data + n * sizeof(RB_DP2_DIRTY_BOX);
            break;
        case RbDp2VolumeBlt: {
            const RB_DP2_VOLUME_BLT *vb = (const RB_DP2_VOLUME_BLT *)data;
            if (data + n * sizeof *vb > end)
                goto bad;
            for (i = 0; i < n; i++)
                volume_blt(c, &vb[i]);
            cmd = data + n * sizeof *vb;
            break;
        }
        case D3DDP2OP_TEXBLT: {
            LPD3DHAL_DP2TEXBLT b = (LPD3DHAL_DP2TEXBLT)data;
            for (i = 0; i < n; i++)
                tex_blt(c, &b[i]);
            cmd = data + n * sizeof(D3DHAL_DP2TEXBLT);
            break;
        }
        case D3DDP2OP_STATESET:
            cmd = data + n * sizeof(D3DHAL_DP2STATESET);
            break;
        case D3DDP2OP_SETPRIORITY:
            cmd = data + n * sizeof(D3DHAL_DP2SETPRIORITY);
            break;
        case D3DDP2OP_SETTEXLOD:
            cmd = data + n * sizeof(D3DHAL_DP2SETTEXLOD);
            break;
        case D3DDP2OP_SETCLIPPLANE: {
            const D3DHAL_DP2SETCLIPPLANE *p = (const D3DHAL_DP2SETCLIPPLANE *)data;
            if (data + n * sizeof *p > end)
                goto bad;
            for (i = 0; i < n; i++)
                if (p[i].dwIndex < CLIP_PLANES)     /* (clip_state reads them at the next draw) */
                    memcpy(c->clip_plane[p[i].dwIndex], p[i].plane, sizeof c->clip_plane[0]);
            cmd = data + n * sizeof *p;
            break;
        }
        default: {
            LPVOID next = NULL;
            HRESULT r = parse_unknown ? parse_unknown(cmd, &next) : D3DERR_COMMAND_UNPARSED;
            if (FAILED(r) || !next) {
                dbg("rd3d: command not understood: "); dbg_hex(k->bCommand); dbg("\r\n");
                d->dwErrorOffset = cmd - start;
                d->ddrval = D3DERR_COMMAND_UNPARSED;
                goto done;
            }
            cmd = (BYTE *)next;
            break;
        }
        }
    }
    goto done;
bad:                                    /* a Direct3D 8 command longer than the buffer */
    dbg("rd3d: command runs past the buffer: "); dbg_hex(CMD(cmd)->bCommand); dbg("\r\n");
    d->dwErrorOffset = cmd - start;
    d->ddrval = D3DERR_COMMAND_UNPARSED;
done:
    d->dwFlags &= ~(D3DHALDP2_SWAPVERTEXBUFFER | D3DHALDP2_SWAPCOMMANDBUFFER);
    let_go(c);
    return DDHAL_DRIVER_HANDLED;
}

/* ---- What the HAL tells Direct3D -------------------------------------------------------------- */
static D3DHAL_GLOBALDRIVERDATA global_data;
static D3DHAL_CALLBACKS callbacks;
static DDSURFACEDESC texture_formats[6];

static void texture_format(DDSURFACEDESC *d, DWORD bits, DWORD r, DWORD g, DWORD b, DWORD a)
{
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
    d->ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d->ddpfPixelFormat.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    d->ddpfPixelFormat.dwRGBBitCount = bits;
    d->ddpfPixelFormat.dwRBitMask = r;
    d->ddpfPixelFormat.dwGBitMask = g;
    d->ddpfPixelFormat.dwBBitMask = b;
    d->ddpfPixelFormat.dwRGBAlphaBitMask = a;
}

static void prim_caps(D3DPRIMCAPS *p)
{
    memset(p, 0, sizeof *p);
    p->dwSize = sizeof *p;
    p->dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                    D3DPMISCCAPS_MASKZ;
    p->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL |
                      D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                      D3DPRASTERCAPS_MIPMAPLODBIAS;
    p->dwZCmpCaps = p->dwAlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
        D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL |
        D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    p->dwSrcBlendCaps = p->dwDestBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE |
        D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA |
        D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA |
        D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT |
        D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA;
    p->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB |
        D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
        D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
        D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
    p->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA |
                       D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_TRANSPARENCY;
    /* With the Direct3D 8 interface on: cube maps (for pixel shaders). The Direct3D 8 and 9
       runtimes on Windows 98 decide from this record whether a cube map may go to video
       memory, whatever D3DCAPS8 says. */
    if (d8_on)
        p->dwTextureCaps |= 0x800 | 0x2000;     /* D3DPTEXTURECAPS_CUBEMAP, VOLUMEMAP */
    p->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR |
        D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR |
        D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR |
        D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
        D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    p->dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE |
        D3DPTBLENDCAPS_DECALALPHA | D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_ADD;
    p->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR |
        D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
}

void d3d_init(RDD_SHARED *s, DDCORECAPS *caps)
{
    D3DDEVICEDESC_V1 *h = &global_data.hwCaps;

    shared = s;
    memset(&global_data, 0, sizeof global_data);
    global_data.dwSize = sizeof global_data;
    h->dwSize = sizeof *h;
    h->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRICAPS | D3DDD_LINECAPS |
                 D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH;
    h->dcmColorModel = D3DCOLOR_RGB;
    h->bClipping = TRUE;
    h->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY |
        D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
        D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
        D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_HWRASTERIZATION |
        D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX;
    prim_caps(&h->dpcTriCaps);
    prim_caps(&h->dpcLineCaps);
    h->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    h->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_32;
    texture_format(&texture_formats[0], 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
    texture_format(&texture_formats[1], 32, 0xFF0000, 0xFF00, 0xFF, 0);
    texture_format(&texture_formats[2], 16, 0xF800, 0x07E0, 0x001F, 0);
    texture_format(&texture_formats[3], 16, 0x7C00, 0x03E0, 0x001F, 0x8000);
    texture_format(&texture_formats[4], 16, 0x0F00, 0x00F0, 0x000F, 0xF000);
    texture_format(&texture_formats[5], 16, 0x7C00, 0x03E0, 0x001F, 0);
    global_data.dwNumTextureFormats = 6;
    global_data.lpTextureFormats = texture_formats;

    memset(&callbacks, 0, sizeof callbacks);
    callbacks.dwSize = sizeof callbacks;
    callbacks.ContextCreate = ContextCreate;
    callbacks.ContextDestroy = ContextDestroy;
    callbacks.ContextDestroyAll = ContextDestroyAll;

    s->hal.lpD3DGlobalDriverData = (DWORD)&global_data;
    s->hal.lpD3DHALCallbacks = (DWORD)&callbacks;

    caps->dwCaps |= DDCAPS_3D;
    caps->ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
    caps->dwZBufferBitDepths = DDBD_16 | DDBD_32;
}

static int copy_answer(LPDDHAL_GETDRIVERINFODATA d, const void *data, DWORD size)
{
    DWORD n = d->dwExpectedSize < size ? d->dwExpectedSize : size;
    memset(d->lpvData, 0, d->dwExpectedSize);
    memcpy(d->lpvData, data, n);
    d->dwActualSize = size;
    d->ddRVal = DD_OK;
    return 1;
}

static void extended_caps(D3DHAL_D3DEXTENDEDCAPS *x)
{
    memset(x, 0, sizeof *x);
    x->dwSize = sizeof *x;
    x->dwMinTextureWidth = x->dwMinTextureHeight = 1;
    x->dwMaxTextureWidth = x->dwMaxTextureHeight = 2048;
    x->dwMaxTextureRepeat = 2048;
    x->dwMaxTextureAspectRatio = 2048;
    x->dwMaxAnisotropy = 1;
    x->dvGuardBandLeft = -4096.0f;
    x->dvGuardBandTop = -4096.0f;
    x->dvGuardBandRight = 4095.0f;
    x->dvGuardBandBottom = 4095.0f;
    x->dwFVFCaps = FF_STAGES;
    x->dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
        D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X |
        D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
        D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
        D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
        D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_DOTPRODUCT3;
    x->dwStencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE |
        D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT |
        D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
    x->wMaxTextureBlendStages = FF_STAGES;
    x->wMaxSimultaneousTextures = FF_STAGES;
    x->dvMaxVertexW = 1.0e10f;
}

/* ---- Direct3D 8 (GetDriverInfo2) ---------------------------------------------------------------- */
/* The Direct3D 8 and 9 runtimes ask a driver whose DDHALINFO has DDHALINFO_GETDRIVERINFO2 for its
   capabilities and formats this way: the stereo-mode GUID, with a header that names the request
   (win2k/d3d8ddi.h; the same interface as on Windows 2000). Vertices are transformed by the
   runtime (no hardware transform and lighting), and no shaders are offered yet. */
static const GUID guid_gdi2 =
    {0xf828169c, 0xa8e8, 0x11d2, {0xa1, 0xf2, 0x00, 0xa0, 0xc9, 0x83, 0xea, 0xf6}};

static ULONG fbits(float f)
{
    union { float f; ULONG u; } x;
    x.f = f;
    return x.u;
}

/* Formats and what each can be used for (the operations in the word of dwRBitMask). */
static const ULONG d8_formats[][2] = {
    { RbFmtX8R8G8B8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatOffscreenTarget | RbFormatSameTarget |
                     RbFormatDisplayMode | RbFormat3D },
    { RbFmtA8R8G8B8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatOffscreenTarget | RbFormatSameTargetUpToAlpha },
    { RbFmtR5G6B5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatOffscreenTarget | RbFormatSameTarget |
                   RbFormatDisplayMode | RbFormat3D },
    { RbFmtX1R5G5B5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA1R5G5B5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA4R4G4B4, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtD16, RbFormatZStencil },
    { RbFmtD24S8, RbFormatZStencil },
    { RbFmtD24X8, RbFormatZStencil },
    { RbFmtDxt1, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },     /* DXT: CreateSurface sizes them, gl_format uploads them */
    { RbFmtDxt2, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtDxt3, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtDxt4, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtDxt5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    /* bump maps for texbem and texbeml: signed (gl_format) */
    { RbFmtV8U8, RbFormatTexture | RbFormatCubeTexture | RbFormatBumpMap },
    { RbFmtQ8W8V8U8, RbFormatTexture | RbFormatCubeTexture | RbFormatBumpMap },
    /* luminance and alpha alone (gl_format) */
    { RbFmtL8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA8L8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
};
#define D8_FORMATS (sizeof d8_formats / sizeof d8_formats[0])

static void d8_caps(RB_D3DCAPS8 *c)
{
    const D3DDEVICEDESC_V1 *hw = &global_data.hwCaps;
    const D3DPRIMCAPS *tri = &hw->dpcTriCaps;
    D3DHAL_D3DEXTENDEDCAPS x;
    RPROC *p;

    extended_caps(&x);
    memset(c, 0, sizeof *c);
    p = process();                      /* the runtime version it announced (RbGdi2DxVersion) */
    c->deviceType = 1;                  /* D3DDEVTYPE_HAL */
    /* 0x00400000: D3DCAPS2_PRESENT_INTERVAL_IMMEDIATE (d3dhal.h, for drivers). A Direct3D 8 or 9
       runtime takes D3DPRESENT_INTERVAL_IMMEDIATE from this bit, not from presentationIntervals,
       and without it refuses a device that asks for that interval. No present here waits for
       the vertical blank. */
    c->caps2 = RB_D3DCAPS2_CANRENDERWINDOWED | 0x00400000UL;
    c->presentationIntervals = RB_D3DPRESENT_INTERVAL_IMMEDIATE | RB_D3DPRESENT_INTERVAL_ONE;
    c->devCaps = hw->dwDevCaps & ~D3DDEVCAPS_FLOATTLVERTEX;
    c->primitiveMiscCaps = tri->dwMiscCaps | 0x80;     /* D3DPMISCCAPS_COLORWRITEENABLE (colour_mask) */
    c->rasterCaps = tri->dwRasterCaps & ~D3DPRASTERCAPS_SUBPIXEL;
    c->zCmpCaps = tri->dwZCmpCaps;
    c->srcBlendCaps = tri->dwSrcBlendCaps;
    c->destBlendCaps = tri->dwDestBlendCaps;
    c->alphaCmpCaps = tri->dwAlphaCmpCaps;
    c->shadeCaps = tri->dwShadeCaps;
    c->textureCaps = tri->dwTextureCaps | 0x4000;      /* D3DPTEXTURECAPS_MIPMAP */
    /* Cube maps and volumes, for pixel shaders and the fixed-function stages (ff_target). */
    c->textureCaps |= 0x800 | 0x10000;  /* D3DPTEXTURECAPS_CUBEMAP, MIPCUBEMAP */
    c->cubeTextureFilterCaps = tri->dwTextureFilterCaps;
    c->textureCaps |= 0x2000 | 0x8000;  /* D3DPTEXTURECAPS_VOLUMEMAP, MIPVOLUMEMAP */
    c->volumeTextureFilterCaps = tri->dwTextureFilterCaps;
    c->volumeTextureAddressCaps = tri->dwTextureAddressCaps;
    c->maxVolumeExtent = 256;
    c->textureFilterCaps = tri->dwTextureFilterCaps;
    c->textureAddressCaps = tri->dwTextureAddressCaps;
    c->stencilCaps = x.dwStencilCaps;
    c->maxTextureWidth = x.dwMaxTextureWidth;
    c->maxTextureHeight = x.dwMaxTextureHeight;
    c->maxTextureRepeat = x.dwMaxTextureRepeat;
    c->maxTextureAspectRatio = x.dwMaxTextureAspectRatio;
    c->maxAnisotropy = x.dwMaxAnisotropy;
    c->maxVertexW = fbits(x.dvMaxVertexW);
    c->guardBandLeft = fbits(x.dvGuardBandLeft);
    c->guardBandTop = fbits(x.dvGuardBandTop);
    c->guardBandRight = fbits(x.dvGuardBandRight);
    c->guardBandBottom = fbits(x.dvGuardBandBottom);
    c->fvfCaps = x.dwFVFCaps;
    c->textureOpCaps = x.dwTextureOpCaps;
    c->maxTextureBlendStages = x.wMaxTextureBlendStages;
    c->maxSimultaneousTextures = x.wMaxSimultaneousTextures;
    c->maxPointSize = fbits(MAX_POINT_SIZE);       /* and point sprites (point_state) */
    c->maxPrimitiveCount = 0xFFFF;
    c->maxVertexIndex = 0xFFFFFF;       /* 32-bit index buffers too (index_list32) */
    c->maxStreams = 1;                  /* (more with hardware vertex processing, below) */
    c->maxStreamStride = 255;
    c->vertexShaderVersion = 0xFFFE0000UL;  /* none: the runtime runs vertex shaders */
    if (tnl_on) {
        /* fixed-function vertex processing on the GPU (tnl_begin): Direct3D 7's lights */
        c->devCaps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
        c->vertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 |
                                  D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS |
                                  D3DVTXPCAPS_LOCALVIEWER;
        if (buf_on)                     /* vertex and index buffers in memory the driver allocates
                                           (rippledd.c), so that it sees their Locks */
            c->devCaps |= 0x02000000UL | 0x04000000UL;     /* D3DDEVCAPS_HWVERTEXBUFFER, HWINDEXBUFFER */
        c->maxActiveLights = TNL_LIGHTS;
        c->maxVertexBlendMatrices = 1;      /* the world matrix alone: no vertex blending */
        c->maxUserClipPlanes = CLIP_PLANES;     /* (clip_state) */
        /* vertex shaders as ARB vertex programs (vs_arb.c): vs_1_1 for Direct3D 8, vs_2_0
           (without flow control) for a Direct3D 9 runtime */
        c->maxStreams = VS_ARB_STREAMS;     /* a declaration may read several: a vertex shader's
                                               inputs (vs_arrays), the fixed-function attributes of
                                               one without code (decl_vertices) */
        if (p && p->d3d_version >= 0x900) {
            c->vertexShaderVersion = 0xFFFE0200UL;
            c->maxVertexShaderConst = VS_ARB_CONSTANTS;
        } else {
            c->vertexShaderVersion = 0xFFFE0101UL;
            c->maxVertexShaderConst = 96;
        }
    }
    /* Pixel shaders as ARB fragment programs (ps_create): 1.3 for Direct3D 8, 2.0 with eight
       samplers when the runtime that asks has announced itself as Direct3D 9 (it uses this record
       for drivers of the Direct3D 8 interface). The runtime sends stage states only for the
       blend stages reported, so those are as many as the samplers. */
    if (p && p->d3d_version >= 0x900) {
        c->pixelShaderVersion = 0xFFFF0200UL;
        c->maxTextureBlendStages = RB_PS2_SAMPLERS;
        c->maxSimultaneousTextures = FF8_STAGES;    /* what the fixed-function stages really draw
                                                       (pixel shaders have their samplers anyway) */
    } else {
        c->pixelShaderVersion = 0xFFFF0103UL;
        c->maxSimultaneousTextures = c->maxTextureBlendStages = RB_PS_COORD_SETS;
    }
    c->fvfCaps = STAGES;                    /* texture coordinate sets */
    c->maxPixelShaderValue = fbits(1.0f);
    tracef("rd3d: Direct3D 8 device caps %08lx, vertex shaders %08lx\r\n", c->devCaps,
           c->vertexShaderVersion);
}

/* The Direct3D 8 and 9 interface, on by default since 0.6.0: vertices processed on the GPU, in
   buffers the driver allocates. The registry value HKLM\Software\moose-ripple, D3D8 chooses
   otherwise: "0" gives the runtime no answer, so it drives the HAL through the Direct3D 7
   interface and processes vertices itself; "1" offers the interface with the runtime's vertex
   processing; "2" is the default. D3D8Buffers "0" keeps buffers in the runtime's memory. Not an
   environment variable: the runtime asks once per boot, in the first program that uses it, and
   keeps the answer for all others. */
int d3d8_enabled(void)
{
    HKEY k;
    char v[4];
    DWORD n = sizeof v, type;
    char mode = '2';
    int buffers = 1;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\moose-ripple", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueEx(k, "D3D8", NULL, &type, (BYTE *)v, &n) == ERROR_SUCCESS &&
            type == REG_SZ && v[0] >= '0' && v[0] <= '2')
            mode = v[0];
        n = sizeof v;
        if (RegQueryValueEx(k, "D3D8Buffers", NULL, &type, (BYTE *)v, &n) == ERROR_SUCCESS &&
            type == REG_SZ && v[0] == '0')
            buffers = 0;
        RegCloseKey(k);
    }
    d8_on = mode != '0';
    tnl_on = mode == '2';               /* and vertices processed on the GPU (tnl_begin) */
    buf_on = tnl_on && buffers;         /* in buffers the driver allocates */
    return d8_on;
}

static int d8_info(LPDDHAL_GETDRIVERINFODATA d)
{
    RB_GDI2_HEADER *h = (RB_GDI2_HEADER *)d->lpvData;
    DWORD room;

    if (!h || h->magic != RB_GDI2_MAGIC || !d8_on)  /* the reply's size is the request's */
        return 0;
    room = h->expectedSize;
    dbg("rd3d: GetDriverInfo2 "); dbg_hex(h->type); dbg("\r\n");
    switch (h->type) {
    case RbGdi2Caps8: {
        RB_D3DCAPS8 caps;
        d8_caps(&caps);
        memcpy(h, &caps, room < sizeof caps ? room : sizeof caps);
        d->dwActualSize = sizeof caps;
        break;
    }
    case RbGdi2FormatCount: {
        RB_GDI2_FORMAT_COUNT *f = (RB_GDI2_FORMAT_COUNT *)h;
        if (room < sizeof *f)
            goto unavailable;
        f->count = D8_FORMATS;
        f->reserved = 0;
        d->dwActualSize = sizeof *f;
        break;
    }
    case RbGdi2Format: {
        RB_GDI2_FORMAT *f = (RB_GDI2_FORMAT *)h;
        DWORD i = f->index;
        if (room < sizeof *f || i >= D8_FORMATS)
            goto unavailable;
        memset(&f->format, 0, sizeof f->format);
        f->format.dwSize = sizeof f->format;
        f->format.dwFlags = RB_DDPF_D3DFORMAT;
        f->format.dwFourCC = d8_formats[i][0];
        f->format.dwRBitMask = d8_formats[i][1];
        d->dwActualSize = sizeof *f;
        break;
    }
    case RbGdi2DxVersion: {
        RB_GDI2_DX_VERSION *v = (RB_GDI2_DX_VERSION *)h;
        RPROC *p = process();
        if (room < sizeof *v)
            goto unavailable;
        /* The Direct3D 9 runtime says 0x902, then, after the Direct3D 9 caps go unanswered,
           0x802 just before it asks for these: keep the highest, per process (the DLL's data is
           shared, and a Direct3D 8 program after a Direct3D 9 one was offered shader model 2) */
        if (p && v->version > p->d3d_version)
            p->d3d_version = v->version;
        d->dwActualSize = sizeof *v;
        break;
    }
    default:
        goto unavailable;
    }
    d->ddRVal = DD_OK;
    return 1;
unavailable:                            /* such as the Direct3D 9 requests */
    d->dwActualSize = 0;
    d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    return 1;
}

int d3d_driver_info(LPDDHAL_GETDRIVERINFODATA d)
{
    if (IsEqualGUID(&d->guidInfo, &guid_gdi2) && d8_info(d))
        return 1;
    if (d8_on && IsEqualGUID(&d->guidInfo, &GUID_DDMoreSurfaceCaps)) {
        /* Surface kinds beyond DDSCAPS that video memory takes: cube maps, for the Direct3D 8
           and 9 runtimes (without this they keep cube maps in system memory only). One pair of
           heap restrictions per heap, none of them restricting. */
        struct {
            DDMORESURFACECAPS c;
            DDSCAPSEX more[2 * 4];
        } m;
        DWORD heaps = shared->hal.dwNumHeaps;
        if (heaps > 5)
            heaps = 5;
        memset(&m, 0, sizeof m);
        m.c.dwSize = sizeof(DDMORESURFACECAPS) + (heaps ? heaps - 1 : 0) * 2 * sizeof(DDSCAPSEX);
        m.c.ddsCapsMore.dwCaps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME;
        return copy_answer(d, &m, m.c.dwSize);
    }
    if (IsEqualGUID(&d->guidInfo, &GUID_D3DCallbacks3)) {
        D3DHAL_CALLBACKS3 cb;
        memset(&cb, 0, sizeof cb);
        cb.dwSize = sizeof cb;
        cb.dwFlags = D3DHAL3_CB32_VALIDATETEXTURESTAGESTATE | D3DHAL3_CB32_DRAWPRIMITIVES2;
        cb.ValidateTextureStageState = ValidateTextureStageState;
        cb.DrawPrimitives2 = DrawPrimitives2;
        return copy_answer(d, &cb, sizeof cb);
    }
    if (IsEqualGUID(&d->guidInfo, &GUID_D3DExtendedCaps)) {
        D3DHAL_D3DEXTENDEDCAPS x;
        extended_caps(&x);
        return copy_answer(d, &x, sizeof x);
    }
    if (IsEqualGUID(&d->guidInfo, &GUID_ZPixelFormats)) {
        struct { DWORD n; DDPIXELFORMAT f[3]; } z;
        memset(&z, 0, sizeof z);
        z.n = 3;
        z.f[0].dwSize = z.f[1].dwSize = z.f[2].dwSize = sizeof(DDPIXELFORMAT);
        z.f[0].dwFlags = z.f[1].dwFlags = DDPF_ZBUFFER;
        z.f[0].dwZBufferBitDepth = 16;
        z.f[0].dwZBitMask = 0xFFFF;
        z.f[1].dwZBufferBitDepth = 32;
        z.f[1].dwZBitMask = 0x00FFFFFF;
        /* 24 bits of depth and 8 of stencil */
        z.f[2].dwFlags = DDPF_ZBUFFER | DDPF_STENCILBUFFER;
        z.f[2].dwZBufferBitDepth = 32;
        z.f[2].dwStencilBitDepth = 8;
        z.f[2].dwZBitMask = 0x00FFFFFF;
        z.f[2].dwStencilBitMask = 0xFF000000;
        return copy_answer(d, &z, sizeof z);
    }
    if (IsEqualGUID(&d->guidInfo, &GUID_Miscellaneous2Callbacks)) {
        DDHAL_DDMISCELLANEOUS2CALLBACKS m;
        memset(&m, 0, sizeof m);
        m.dwSize = sizeof m;
        m.dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE |
                    DDHAL_MISC2CB32_DESTROYDDLOCAL;
        m.CreateSurfaceEx = CreateSurfaceEx;
        m.GetDriverState = GetDriverState;
        m.DestroyDDLocal = DestroyDDLocal;
        return copy_answer(d, &m, sizeof m);
    }
    if (IsEqualGUID(&d->guidInfo, &GUID_D3DParseUnknownCommandCallback)) {
        parse_unknown = (PFND3DPARSEUNKNOWNCOMMAND)d->lpvData;
        d->dwActualSize = 0;
        d->ddRVal = DD_OK;
        return 1;
    }
    return 0;
}
