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
#define GL_CLAMP_TO_BORDER              0x812D
#define GL_MIRRORED_REPEAT              0x8370
#define GL_TEXTURE0                     0x84C0
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
#define GL_FOG_COORDINATE_SOURCE        0x8450
#define GL_FOG_COORDINATE               0x8451
#define GL_FRAGMENT_DEPTH               0x8452
#define GL_FOG_COORDINATE_ARRAY         0x8457
#define GL_FIRST_VERTEX_CONVENTION      0x8E4D
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

/* ---- Surface handles: DirectX 7 names surfaces in the command stream by handle --------------- */
typedef struct {
    OWNER o;
    LPDDRAWI_DIRECTDRAW_LCL lcl;
    LPDDRAWI_DDRAWSURFACE_LCL *surf;    /* by handle; process heap */
    DWORD n;
} DDLOCAL;

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
    DWORD bytes;
    GLenum format, type, internal;
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

int d3d_surface_format_ok(LPDDSURFACEDESC desc)
{
    DDPIXELFORMAT *pf = &desc->ddpfPixelFormat;

    if (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER)
        return (pf->dwFlags & DDPF_ZBUFFER) &&
               (pf->dwZBufferBitDepth == 16 || pf->dwZBufferBitDepth == 32);
    if (desc->ddsCaps.dwCaps & DDSCAPS_TEXTURE)
        return gl_format(pf).bytes != 0;
    return 0;
}

/* ---- Contexts ----------------------------------------------------------------------------------- */
#define STAGES 2
#define FOG_UNIT STAGES                 /* the OpenGL texture unit that does fog */

#define MAX_LEVELS 12                   /* mipmap levels of a 2048x2048 texture */

typedef struct {
    GLuint name;
    FLATPTR mem;                        /* what was uploaded: memory, size, format */
    DWORD w, h, fmt;
    DWORD levels;                       /* mipmap levels uploaded (1: the top level only) */
    FLATPTR lmem[MAX_LEVELS];           /* each level's memory (lmem[0] is mem) */
    DWORD stamp;                        /* DirectDraw's contents stamps then (0: none kept) */
    DWORD scene;                        /* the scene it was uploaded in */
    int dirty;                          /* written since (Lock, texture blit) */
} TEXTURE;

#define TARGETS  4                      /* colour buffers a context keeps (gpu_buffer) */
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
    GLuint fbo, white, fog_tex;
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
    DWORD tss[STAGES][32];              /* texture stage states */
    int stages_dirty;
    DWORD scene;                        /* scenes ended so far */
    DWORD bound[STAGES];                /* surface handle of each stage's texture, 0 if none */
    int keyed;                          /* a bound texture is drawn with its colour key */
    TEXTURE *tex;                       /* by surface handle */
    DWORD ntex;
    DWORD ntextures;                    /* OpenGL textures made (see TEXTURE_BUDGET) */
    float *pos;                         /* positions, 4 floats a vertex, then fog (convert) */
    DWORD npos;
    WORD *ibuf;                         /* indices of one command, rebased; grows as needed */
    DWORD nibuf;
} RCTX;

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
            if (t->mem == mem || l < t->levels) {
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
    if (b->deferred) {
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
    DWORD caps = s->ddsCaps.dwCaps, off = g->fpVidMem - shared->lfb;
    DDPIXELFORMAT pf, screen;

    if (!(caps & DDSCAPS_FLIP) || !(caps & DDSCAPS_VIDEOMEMORY) ||
        (caps & (DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_SYSTEMMEMORY)) ||
        (shared->bpp != 16 && shared->bpp != 32) || !shared->pitch ||
        g->wWidth != shared->width || g->wHeight != shared->height ||
        g->lPitch != (LONG)shared->pitch || off % shared->pitch ||
        off / shared->pitch > RDD_MAX_Y_OFFSET || off / shared->pitch + shared->height > shared->lines)
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
        if (!c || (!c->dirty && !c->chain_active))
            continue;
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

    if (c->rc) {
        make_current(c);
        for (i = 0; i < c->ntex; i++)
            if (c->tex[i].name)
                gl->DeleteTextures(1, &c->tex[i].name);
        if (c->white)
            gl->DeleteTextures(1, &c->white);
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

/* Bind the texture behind a surface handle, uploading it if it is new or has changed. 0 if
   there is no usable texture. */
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
    GLfloat border[4], bias;

    if (!s || !(g = s->lpGbl) || !g->fpVidMem)
        return 0;
    t = texture_slot(c, h);
    if (!t)
        return 0;
    surface_format(s, &pf);
    f = gl_format(&pf);
    if (!f.bytes)
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
    keyed = c->rs[D3DRENDERSTATE_COLORKEYENABLE] && (s->dwFlags & DDRAWISURF_HASCKEYSRCBLT);
    fmt = f.internal ^ (f.format << 8) ^ (f.type << 16) ^
          (keyed ? 0x80000000UL ^ s->ddckCKSrcBlt.dwColorSpaceLowValue : 0);
    if (keyed)
        c->keyed = 1;
    if (t->mem != g->fpVidMem || t->w != g->wWidth || t->h != g->wHeight || t->fmt != fmt ||
        t->levels != levels) {
        /* new: storage and contents */
        texture_upload(gl, &f, &pf, lv, levels, 1, keyed);
        t->mem = g->fpVidMem;
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
               trace_on ? sum_rows((BYTE *)g->fpVidMem, g->lPitch, g->wWidth * f.bytes,
                                   g->wHeight) : 0);
    } else if (texture_changed(c, t, s, stamp)) {
        /* written since: the contents again, into the same storage */
        tracef("rd3d: texture %lx again: dirty %d, stamp %lx (was %lx), scene %lu (was %lu), "
               "sum %08lx\r\n", h, t->dirty, stamp, t->stamp, c->scene, t->scene,
               trace_on ? sum_rows((BYTE *)g->fpVidMem, g->lPitch, g->wWidth * f.bytes,
                                   g->wHeight) : 0);
        texture_upload(gl, &f, &pf, lv, levels, 0, keyed);
    }
    t->stamp = stamp;
    t->scene = c->scene;
    t->dirty = 0;
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
    for (s = 0; s < STAGES; s++) {
        DWORD *st = c->tss[s];
        DWORD cop = st[D3DTSS_COLOROP], aop = st[D3DTSS_ALPHAOP];
        int need = uses_texture(cop, st[D3DTSS_COLORARG1], st[D3DTSS_COLORARG2]) ||
                   uses_texture(aop, st[D3DTSS_ALPHAARG1], st[D3DTSS_ALPHAARG2]);
        int on = 0;

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
            continue;
        }
        gl->Enable(GL_TEXTURE_2D);
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
} VERTICES;

static int parse_fvf(DWORD fvf, VERTICES *v)
{
    DWORD off = 0, n, i;

    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW)
        return 0;                       /* only transformed vertices without T&L */
    off = 16;
    if (fvf & D3DFVF_RESERVED1)
        off += 4;
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
    BYTE *b = v->base + first * v->stride;
    int s;

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
    gl->ClientActiveTexture(GL_TEXTURE0 + FOG_UNIT);
    if (c->rs[D3DRENDERSTATE_FOGENABLE]) {
        gl->EnableClientState(GL_TEXTURE_COORD_ARRAY);
        gl->TexCoordPointer(1, GL_FLOAT, 0, c->pos + count * 4);
    } else {
        gl->DisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    for (s = 0; s < STAGES; s++) {
        DWORD set = c->tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF;
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
    for (u = 0; u <= FOG_UNIT; u++) {
        gl->ActiveTexture(GL_TEXTURE0 + u);
        gl->Disable(GL_TEXTURE_2D);
    }
    gl->ActiveTexture(GL_TEXTURE0);
    gl->WindowPos2i(0, 0);              /* row 0 of the memory is the GPU's row 0 (see framebuffer) */
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, g->lPitch / f.bytes);
    gl->DrawPixels(c->fb_w < g->wWidth ? c->fb_w : g->wWidth,
                   c->fb_h < g->wHeight ? c->fb_h : g->wHeight, f.format, f.type,
                   (const GLvoid *)mem);
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

/* Draw vertices first..first+count (all of them converted), or count indices into them. */
static void draw(RCTX *c, const VERTICES *v, GLenum mode, DWORD first, DWORD count,
                 const WORD *idx, DWORD nidx, DWORD span)
{
    GLF *gl = gl_of(c);
    DWORD n = idx ? span : count;

    if (!v->base || (v->count && first + n > v->count)) {
        dbg("rd3d: vertices out of range\r\n");
        return;
    }
    if (c->cur && c->cur->written)
        rt_upload(c);
    if (c->stages_dirty || textures_changed(c))
        apply_stages(c);
    if (!idx) {
        if (!convert(c, v, first, count))
            return;
        arrays(c, v, first, count);
        gl->DrawArrays(mode, 0, count);
    } else {
        if (!convert(c, v, first, span))
            return;
        arrays(c, v, first, span);
        gl->DrawElements(mode, nidx, GL_UNSIGNED_SHORT, idx);
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
    if (rt != c->rt && rt != c->front && rt != c->back && !end_chain(c))
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
    DWORD bytes, levels, n, l;
    RECTL r = b->rSrc;

    tracef("rd3d: texture blit %lx -> %lx, %ld,%ld-%ld,%ld to %ld,%ld, flags %lx%s\r\n",
           b->dwDDSrcSurface, b->dwDDDestSurface, r.left, r.top, r.right, r.bottom,
           b->pDest.x, b->pDest.y, b->dwFlags, ds && ss ? "" : " (unknown handle)");
    if (!ds || !ss || !ds->lpGbl->fpVidMem || !ss->lpGbl->fpVidMem)
        return;
    surface_format(ss, &pf);
    bytes = pf.dwRGBBitCount / 8;
    if (!bytes)
        return;
    if (r.right <= r.left || r.bottom <= r.top) {
        r.left = r.top = 0;
        r.right = ss->lpGbl->wWidth;
        r.bottom = ss->lpGbl->wHeight;
    }
    /* Mipmapped textures: every level the two have in common, with the rectangle and the
       point scaled to it (the DDK's rule for this command). */
    levels = mip_chain(ds, dl);
    n = mip_chain(ss, sl);
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
        for (i = 0; i < h; i++)
            memcpy((BYTE *)dg->fpVidMem + (y + i) * dg->lPitch + x * bytes,
                   (BYTE *)sg->fpVidMem + (top + i) * sg->lPitch + left * bytes, w * bytes);
        d3d_surface_written(dl[l]);     /* a render target too: its GPU picture is older now */
    }
    if (b->dwDDDestSurface < c->ntex)
        c->tex[b->dwDDDestSurface].dirty = 1;            /* upload again */
    c->stages_dirty = 1;
}

#define CMD(p) ((LPD3DHAL_DP2COMMAND)(p))
#define ALIGN4(p) ((BYTE *)(((DWORD)(p) + 3) & ~3))

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
    d->ddrval = DD_OK;

    while (cmd < end) {
        LPD3DHAL_DP2COMMAND k = CMD(cmd);
        BYTE *data = cmd + sizeof(D3DHAL_DP2COMMAND);
        DWORD n = k->wPrimitiveCount, i;

        switch (k->bCommand) {
        case D3DDP2OP_RENDERSTATE: {
            LPD3DHAL_DP2RENDERSTATE rs = (LPD3DHAL_DP2RENDERSTATE)data;
            for (i = 0; i < n; i++) {
                render_state(c, rs[i].RenderState, rs[i].dwState);
                if (d->lpdwRStates && rs[i].RenderState < D3DHAL_MAX_RSTATES)
                    d->lpdwRStates[rs[i].RenderState] = rs[i].dwState;
            }
            cmd = data + n * sizeof(D3DHAL_DP2RENDERSTATE);
            break;
        }
        case D3DDP2OP_TEXTURESTAGESTATE: {
            LPD3DHAL_DP2TEXTURESTAGESTATE ts = (LPD3DHAL_DP2TEXTURESTAGESTATE)data;
            for (i = 0; i < n; i++)
                stage_state(c, ts[i].wStage, ts[i].TSState, ts[i].dwValue);
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
            }
            cmd = data + n * sizeof(D3DHAL_DP2VIEWPORTINFO);
            break;
        }
        case D3DDP2OP_WINFO:
            cmd = data + n * sizeof(D3DHAL_DP2WINFO);
            break;
        case D3DDP2OP_ZRANGE:
            cmd = data + n * sizeof(D3DHAL_DP2ZRANGE);
            break;
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
        case D3DDP2OP_SETCLIPPLANE:
            cmd = data + n * sizeof(D3DHAL_DP2SETCLIPPLANE);
            break;
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

int d3d_driver_info(LPDDHAL_GETDRIVERINFODATA d)
{
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
        memset(&x, 0, sizeof x);
        x.dwSize = sizeof x;
        x.dwMinTextureWidth = x.dwMinTextureHeight = 1;
        x.dwMaxTextureWidth = x.dwMaxTextureHeight = 2048;
        x.dwMaxTextureRepeat = 2048;
        x.dwMaxTextureAspectRatio = 2048;
        x.dwMaxAnisotropy = 1;
        x.dvGuardBandLeft = -4096.0f;
        x.dvGuardBandTop = -4096.0f;
        x.dvGuardBandRight = 4095.0f;
        x.dvGuardBandBottom = 4095.0f;
        x.dwFVFCaps = STAGES;
        x.dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
            D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X |
            D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
            D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
            D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
            D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_DOTPRODUCT3;
        x.dwStencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE |
            D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT |
            D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
        x.wMaxTextureBlendStages = STAGES;
        x.wMaxSimultaneousTextures = STAGES;
        x.dvMaxVertexW = 1.0e10f;
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
