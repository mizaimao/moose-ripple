/* Stock NT5 Direct3D 7 callbacks. An offscreen DirectDraw surface owns the
 * host image. The runtime's surface numbers are local to a DirectDraw object.
 * Numeric context handles never expose display-driver addresses.
 *
 * A texture lives in system memory or in "video memory" that the runtime
 * allocates in the process (display_ddraw.c); either is written without
 * telling the driver. Each texture used in a scene is read once, compared
 * with its last upload by checksum, and converted to B8G8R8A8 for the host
 * when it changed. The texture manager's copies arrive as texture blits
 * between the two. One texture stage is
 * drawn. A draw this path cannot reproduce is consumed and counted, and the
 * rest of the command buffer is still drawn. */
#include "display_private.h"
#include <d3dnthal.h>
#include "rb2kd3d7.h"
#include "d3d8ddi.h"
#include "d3d8_ps.h"

/* These Direct3D 7 HRESULTs are declared only in the user-side d3d.h. */
#ifndef D3DERR_UNSUPPORTEDCOLOROPERATION
#define D3DERR_UNSUPPORTEDCOLOROPERATION MAKE_DDHRESULT(2073)
#define D3DERR_UNSUPPORTEDALPHAOPERATION MAKE_DDHRESULT(2075)
#define D3DERR_UNSUPPORTEDCOLORARG MAKE_DDHRESULT(2074)
#define D3DERR_TOOMANYOPERATIONS MAKE_DDHRESULT(2077)
#endif

enum { MaxContexts = 16, MaxCommandBytes = 1 << 20, TextureStages = 8, MaxTextures = 1024, MaxLights = 32,
       MaxBuffers = 4096, MaxShaders = 512, MaxVertexShaders = 512 };
/* DP2 SETLIGHT operations (D3DHAL_SETLIGHT_*). */
enum { SetLightEnable, SetLightDisable, SetLightData };
typedef char light_record[sizeof(D3DLIGHT7) == 26 * sizeof(ULONG) ? 1 : -1];
typedef char d8_lights_match[RB2K_D7_LIGHTS == 8 ? 1 : -1];
typedef char material_record[sizeof(D3DNTHAL_DP2SETMATERIAL) == 17 * sizeof(ULONG) ? 1 : -1];
typedef struct {
    ULONG texture, colorOp, colorArg1, colorArg2, alphaOp, alphaArg1, alphaArg2;
    ULONG coordIndex, addressU, addressV, magFilter, minFilter, mipFilter, border, transform;
} D7_STAGE;
/* A pixel shader of a context: its handle, its tokens, the stages it
 * samples and reads, and its miniport programs, one per fog setting
 * (program zero when it could not be translated). */
enum { ShaderVariants = 4 };
typedef struct {
    ULONG handle, samplers, coords, tokenCount, constants;   /* constants: 8, or 32 for shader model 2 */
    ULONG *tokens;
    struct { ULONG fog, program; } variants[ShaderVariants];
} D8_SHADER;
/* A vertex shader of a context: its handle, its declaration and code (one
 * allocation), the constants and vertex words it reads, and its miniport
 * programs without and with oFog in the specular alpha, each without and with clip distances
 * (index: 1 fog, 2 clip; zero until made; failed when it cannot be translated). */
typedef struct {
    ULONG handle, declCount, tokenCount, constants, words;
    ULONG *decl, *tokens;
    ULONG program[4];
    BOOL failed[4];
    ULONG streams;                          /* the streams the declaration reads, one bit each */
    ULONG bases[4][RB_VS_STREAMS];          /* where each program places them (words) */
    BOOL pointSize;     /* it writes oPts */
    /* For draws from host buffers: the programs that read vertex attributes (RB_VS_ATTRIBUTES), by
     * the same index; the elements their inputs are fetched from; the streams those name, and for
     * each the bytes of a vertex they reach. */
    ULONG hostProgram[4];
    BOOL hostFailed[4];
    ULONG elementCount, elements[RB_VS_ELEMENTS], hostStreams, extents[RB_VS_STREAMS];
} D8_VSHADER;
/* A pixel program made from the texture stage states, for draws with a
 * vertex shader and no pixel shader: the states it was made from (seven
 * words a stage, then specular and fog), its program and what it samples. */
enum { StagePrograms = 32, StageKey = 8 * 8 + 2 };
typedef struct { ULONG key[StageKey], program, samplers; } D8_STAGE_PROGRAM;
/* A state block: the state commands recorded between STATESETBEGIN and
 * STATESETEND (or made from the current state by STATESETCREATE), replayed
 * by STATESETEXECUTE and refreshed from the current state by CAPTURE. */
enum { MaxStateBlocks = 256, RenderStates = 256, StageStates = 33 };
typedef struct { ULONG handle, bytes, capacity; BYTE *data; } D8_STATEBLOCK;
typedef char vs_layout_match[RB_VS_CONST_BASE == RB2K_D8_VS_CONST_BASE && RB_VS_CONSTANTS == RB2K_D8_VS_CONSTANTS &&
                             4 * RB_VS_VERTEX_CONSTANTS == RB2K_D8_VS_VERTEX_WORDS && RB_VS_VERTICES == RB2K_D8_VERTICES ? 1 : -1];
typedef char fog_codes_match[RB_PS_FOG_ALPHA == RB2K_D7_FOG_ALPHA && RB_PS_FOG_VERTEX == RB2K_D7_FOG_VERTEX &&
                             RB_PS_FOG_TABLE == RB2K_D7_FOG_TABLE && RB_PS_FOG_MODE_SHIFT == RB2K_D7_FOG_MODE_SHIFT &&
                             RB_PS_FOG_RANGE == RB2K_D7_FOG_RANGE && RB_PS_FOG_W == RB2K_D7_FOG_W ? 1 : -1];
typedef struct {
    ULONG id, target, depth;
    DISPLAY *display;
    PDD_DIRECTDRAW_LOCAL local;
    HANDLE owner;
    ULONG cull, shade, fill;
    ULONG zEnable, zWrite, zFunc;
    ULONG alphaTest, alphaFunc, alphaRef, blend, srcBlend, dstBlend;
    ULONG fog, specular, stencil, clipPlanes, colorKey, factor, lighting;
    /* The stencil test: comparison, reference, masks, and the operations on
     * fail, depth fail and pass (D3D values). */
    ULONG stencilFunc, stencilRef, stencilMask, stencilWriteMask, stencilFail, stencilZFail, stencilPass;
    ULONG matrices[3][16];      /* world, view, projection */
    ULONG texMatrices[TextureStages][16];   /* D3DTRANSFORMSTATE_TEXTURE0..7 */
    ULONG viewport[4], zRange[2];
    /* Lighting: D3DRENDERSTATE_AMBIENT, COLORVERTEX, LOCALVIEWER and
     * NORMALIZENORMALS, the diffuse, specular, ambient and emissive material
     * sources, the D3DMATERIAL7, and lights by index with their D3DLIGHT7. */
    ULONG ambient, colorVertex, localViewer, normalize, sources[4];
    /* Fog render states and the w range of the projection (D3DDP2OP_WINFO,
     * equal ends for an affine projection). */
    ULONG fogColor, fogTable, fogVertex, fogStart, fogEnd, fogDensity, rangeFog, wNear, wFar;
    ULONG material[17];
    ULONG lightOn;
    ULONG lights[MaxLights][26];
    /* Direct3D 8: the vertex format given as the vertex shader (a shader
     * handle has its low bit set), the pixel shader, stream zero (a
     * registered buffer, or the user memory of DrawPrimitiveUP) and the
     * index buffer. Only stream zero is offered. */
    BOOL dx8, streamUser, dx9Ddi;   /* dx9Ddi: a DirectX 9 runtime on the Direct3D 9 interface */
    ULONG fvf, pixelShader, streamHandle, streamStride, indexHandle, indexStride;
    ULONG streamHandles[RB_VS_STREAMS], streamStrides[RB_VS_STREAMS];   /* each stream; 0 as above */
    /* The Direct3D 9 interface: stream offsets (bytes), the current declaration (or vertex format)
     * and shader function handles, the scissor rectangle, and the declarations and functions made. */
    ULONG streamOffsets[RB_VS_STREAMS], declHandle, funcHandle;
    LONG vsInt[16][4];   /* SETVERTEXSHADERCONSTI: i0..i15 */
    ULONG planes[6][4];   /* SETCLIPPLANE: the user clip planes (IEEE), enabled by clipPlanes */
    ULONG vsBool;        /* SETVERTEXSHADERCONSTB: b0..b15, a bit each */
    RECTL scissor;
    struct D9_DECL *decls;
    struct D9_FUNC *funcs;
    struct D9_PAIR *pairs;
    ULONG pairLast;      /* the pair the last draw used, tried first */
    /* The buffer records of the streams and (last) the index buffer, found once and kept while
     * no buffer comes or goes (bufferEpoch). */
    struct D8_BUFFER *bufferCache[RB_VS_STREAMS + 1];
    ULONG bufferCacheEpoch;
    /* The context's stream arenas, one for vertices and one for indices (see buffer_draw): the
     * miniport's buffer and the bytes used. They go with the context, before the transport can
     * be reset under them. */
    struct { ULONG host, used; } arenas[RB2K_D8_BUFFER_KINDS];
    BOOL inScene, queued;
    ULONG minStart, maxEnd;     /* diagnostics for one command buffer */
    RB2K_D3D7_DRAW draw;        /* the batch being filled */
    /* The pixel and vertex shaders, in tables made with the first of each: the records in use
     * are below the counts, and the last ones found are tried first. */
    D8_SHADER *shaders;
    ULONG shaderCount, shaderLast;
    ULONG psConst[32][4];       /* pixel shader constants c0..c31 */
    D8_VSHADER *vshaders;
    ULONG vshaderCount, vshaderLast;
    ULONG vsConst[RB_VS_CONSTANTS][4];   /* vertex shader constants */
    D8_STAGE_PROGRAM stagePrograms[StagePrograms];
    ULONG stageNext;
    /* Every render and texture stage state as last set, for state blocks;
     * the block being recorded (its handle) and the blocks. */
    ULONG rs[RenderStates], rsSet[RenderStates / 32], tss[TextureStages][StageStates], tssSet[TextureStages];
    ULONG recording, replaying;
    D8_STATEBLOCK *blocks;
    RB2K_D3D8_DRAW draw8;       /* the pixel shader batch being filled */
    D7_STAGE stages[TextureStages];
} D7_CONTEXT;
/* A system-memory texture registered through CreateSurfaceEx. The address
 * belongs to the owning process, which is current during its callbacks. */
typedef struct {
    PDD_DIRECTDRAW_LOCAL local;
    PDD_SURFACE_GLOBAL global;
    DISPLAY *display;
    HANDLE owner;
    ULONG handle, format, width, height, pitch, mipmaps;
    FLATPTR address;
    /* Mipmap levels found on the attach chains of each face (one face, or
     * the six of a cube map in Direct3D's order +X, -X, +Y, -Y, +Z, -Z),
     * level zero of face zero being the above. */
    ULONG levels, faces, levelPitch[6][RB2K_D7_LEVELS];
    ULONG depth;    /* a volume's slices at level zero (each level has half as many); 1 otherwise */
    FLATPTR levelAddress[6][RB2K_D7_LEVELS];
    ULONG image;    /* a render target texture: the image id (sampled as it is) */
    /* A cube map render target: the image of each face, and its draw count when last copied into
     * the host cube texture (host) that sampling uses. */
    BOOL rtCube;
    ULONG cubeImages[6], cubeDraws[6];
    /* A system-memory DXT texture that a Direct3D 9 runtime described by its bytes (adopt_blocks):
     * the format its FourCC names, while it is registered with none. */
    ULONG blocks;
    ULONG faceHandles[6];   /* a cube map: each face's own surface handle (a Direct3D 9 runtime copies faces by them) */
    /* Video-memory textures are written only through DirectDraw locks and
     * blits, which mark them dirty; others are checked once a scene. */
    BOOL tracked, dirty;
    ULONG host, sum, epoch;
    ULONG used;     /* useClock when a draw last chose it, for least-recently-used eviction */
} D7_TEXTURE;

/* A Direct3D 8 vertex or index buffer registered through CreateSurfaceEx.
 * The runtime keeps both in the owning process's memory; the address and
 * size are read from the surface at each draw. */
typedef struct D8_BUFFER {
    PDD_DIRECTDRAW_LOCAL local;
    PDD_SURFACE_GLOBAL global;
    DISPLAY *display;
    HANDLE owner;
    ULONG handle;
    /* Its copy on the host (RippleHostBuffers), made when a draw first reads it as vertices or
     * as indices: the miniport's buffer, its kind and size, and two sums of each BufferChunk
     * bytes the host holds (0, 0: nothing yet). The program writes the buffer without telling
     * the driver, so each draw sums the chunks it reads and uploads those that differ. */
    ULONG host, hostKind, hostBytes;
    ULONG *sums;
    /* An index buffer: the vertices its last scanned range of indices reaches (scanCount 0:
     * none), good while that range's chunks stay as they were. */
    ULONG scanAt, scanCount, scanWidth, scanLow, scanHigh;
    /* The program rewrote bytes the mirror holds: the next rewrittenLeft draws from the buffer are
     * streamed (rewrittenSpan of them this time, more the next), and the mirror is stale: it is
     * made anew before it is summed again. */
    ULONG rewrittenLeft, rewrittenSpan;
    BOOL stale;
    /* Within one call of DrawPrimitives2 the program cannot write its buffers (the call is the
     * runtime flushing draws it gathered), so in a call a chunk is summed once and a range is
     * streamed once: the chunks summed in call checkedCall (checkedFirst to checkedLast), and
     * the bytes streamed in call streamedCall with their place in the arena streamedHost. */
    ULONG checkedCall, checkedFirst, checkedLast;
    ULONG streamedCall, streamedFrom, streamedBytes, streamedPlace, streamedHost;
} D8_BUFFER;
/* Advanced when a buffer record comes or goes, for the contexts' bufferCache. */
static ULONG bufferEpoch = 1;
/* Counts the calls of DrawPrimitives2 (never zero), for what holds within one call. */
static ULONG dp2Call = 1;
static RB2K_D3D8_VPROGRAM *vprogramScratch;   /* a vertex program for host buffers on its way */
/* Vertex shaders that could not be made into programs for host buffers (host_program). */
static struct { ULONG count, reasons[RbPsReasons + 2], detail, elements, bytes, decl[4][24]; } hostProgramFails;

static void release_shaders(D7_CONTEXT *c);
static void release_arenas(D7_CONTEXT *c);
static void d9_release(D7_CONTEXT *c);
static void release_blocks(D7_CONTEXT *c);
static void stencil_state(const D7_CONTEXT *c, const DD_SLOT *z, ULONG *stencil, ULONG *ref);
static HSEMAPHORE registry;
static D8_BUFFER *buffers;
static BYTE *indexScratch;      /* indices copied out of the process before use (16- or 32-bit) */
static RB2K_D3D8_PROGRAM *programScratch;   /* a translated pixel shader on its way */
static ULONG indexScratchCount;   /* bytes */
static D7_CONTEXT contexts[MaxContexts];
static ULONG lastContext;
static D7_TEXTURE *textures;
static ULONG textureEpoch = 1;
/* Advanced after each queued draw: textures a draw chooses carry the same stamp. */
static ULONG useClock = 1;
static BYTE *texelScratch, *uploadScratch;
static ULONG texelScratchBytes;

static LONGLONG cycles(void);

/* A context-only callback takes the registry first, then the display lock.
 * DrvDisableDirectDraw drops the display lock before retiring the contexts. */
void rb_d7_init(void)
{
    cycles();   /* the first time stamp, so that a report measures the rate over a long time */
    if (!registry) registry = EngCreateSemaphore();
    if (!textures) textures = EngAllocMem(FL_ZERO_MEMORY, MaxTextures * sizeof(*textures), 't7bR');
    if (!buffers) buffers = EngAllocMem(FL_ZERO_MEMORY, MaxBuffers * sizeof(*buffers), 'b8bR');
    if (!programScratch) programScratch = EngAllocMem(0, sizeof(*programScratch), 's8bR');
    if (!vprogramScratch) vprogramScratch = EngAllocMem(0, sizeof(*vprogramScratch), 'S8bR');
    if (!uploadScratch)
        uploadScratch = EngAllocMem(0, sizeof(RB2K_D3D7_TEXTURE) + RB2K_D7_UPLOAD_BYTES, 'p7bR');
}

static D7_CONTEXT *owned_context(ULONG id)
{
    ULONG i;
    for (i = 0; id && i < MaxContexts; ++i)
        if (contexts[i].id == id && contexts[i].owner == EngGetCurrentProcessId())
            return &contexts[i];
    return NULL;
}

/* Draw records waiting to go to the miniport in one IOCTL_RB2K_D3D7_DRAWS
 * request: the target id, then a byte count and a draw for each. Records
 * exist only inside a DrawPrimitives2 call, under the registry semaphore;
 * they leave before any other request to the miniport and before the call
 * returns, so requests stay in order. */
static BYTE *drawQueue;
static ULONG drawQueueBytes;
static DISPLAY *drawQueueDisplay;

/* Time stamps in processor cycles: EngQueryPerformanceCounter reads the ACPI
 * power management timer on these HALs, an emulated port that QEMU answers
 * under the lock its drawing thread holds (see rb_counter_ticks). The rate is
 * measured against the performance counter when the counters are reported,
 * and kept once a second or more has been measured, as in the miniport. */
static LONGLONG cycles0, counter0;
static volatile LONGLONG keptRate;
static volatile LONG rateKept;   /* 1 while one caller stores the rate, 2 once it is there */
static LONGLONG cycles(void)
{
    LONGLONG now = (LONGLONG)__builtin_ia32_rdtsc();
    if (!counter0) { cycles0 = now; EngQueryPerformanceCounter(&counter0); }
    return now;
}

static LONGLONG cycle_rate(void)
{
    LONGLONG now = cycles(), counter, hz;
    ULONGLONG c, q;
    LONGLONG rate;
    if (rateKept == 2) return keptRate;
    EngQueryPerformanceCounter(&counter);
    EngQueryPerformanceFrequency(&hz);
    c = (ULONGLONG)(now - cycles0);
    q = (ULONGLONG)(counter - counter0);
    rate = q ? (LONGLONG)(c / q * (ULONGLONG)hz + c % q * (ULONGLONG)hz / q) : 0;
    if (q >= (ULONGLONG)hz && __sync_bool_compare_and_swap(&rateKept, 0, 1)) {
        keptRate = rate;
        rateKept = 2;
    }
    return rate;
}

static ULONG flush_draws(void)
{
    DISPLAY *d = drawQueueDisplay;
    ULONG written = 0, error;
    LONGLONG start, end;
    if (!drawQueueBytes) return 0;
    start = cycles();
    error = EngDeviceIoControl(d->miniport, IOCTL_RB2K_D3D7_DRAWS, drawQueue, drawQueueBytes, NULL, 0, &written);
    end = cycles();
    d->d7draw.drawTicks += end - start;
    ++d->d7draw.drawRequests;
    drawQueueBytes = 0;
    if (error) {
        ULONG i;
        ++d->d7draw.drawFailures;
        d->d7draw.lastDrawError = error;
        /* A transport reset releases the host textures: upload them again. */
        for (i = 0; textures && i < MaxTextures; ++i)
            if (textures[i].display == d) textures[i].host = 0;
        /* and the host buffers: made again, and filled, at their next draws. */
        for (i = 0; buffers && i < MaxBuffers; ++i)
            if (buffers[i].display == d) buffers[i].host = 0;
        for (i = 0; i < MaxContexts; ++i)
            if (contexts[i].display == d) RtlZeroMemory(contexts[i].arenas, sizeof(contexts[i].arenas));
    }
    return error;
}

/* Every other miniport request from this file goes through here. */
static ULONG miniport(DISPLAY *d, ULONG code, PVOID in, ULONG inSize, PVOID out, ULONG outSize, ULONG *written)
{
    flush_draws();
    *written = 0;
    return EngDeviceIoControl(d->miniport, code, in, inSize, out, outSize, written);
}

static BOOL request(DISPLAY *d, ULONG code, PVOID in, ULONG inSize, PVOID out, ULONG outSize)
{
    ULONG written;
    return !miniport(d, code, in, inSize, out, outSize, &written) && written == outSize;
}

static void release_texture(D7_TEXTURE *t, BOOL host)
{
    if (host && t->host) request(t->display, IOCTL_RB2K_D3D7_TEXTURE_FREE, &t->host, sizeof(t->host), NULL, 0);
    if (t->display) --t->display->d7draw.textures;
    RtlZeroMemory(t, sizeof(*t));
}

/* One event of the host buffers into the display's log for d7draw (RB2K_HOST_LOG_*): a buffer
 * registered (handle, address, bytes, 1 when its host copy stayed), released (handle, host
 * buffer), a host buffer made (handle, host buffer, bytes, kind), an upload (handle, host
 * buffer, offset, bytes, target, its first word), a draw (stream 0's handle, host buffer and
 * offset; the index buffer's host buffer and offset; count; lowest and highest vertex). */
static void host_log(DISPLAY *d, ULONG kind, ULONG a, ULONG b, ULONG c, ULONG e, ULONG f, ULONG g, ULONG h)
{
    ULONG *t = d->d7draw.hostLog[d->d7draw.hostLogCount++ % 48];
    t[0] = kind; t[1] = a; t[2] = b; t[3] = c; t[4] = e; t[5] = f; t[6] = g; t[7] = h;
}

/* A context's stream arenas go with it. */
static void release_arenas(D7_CONTEXT *c)
{
    ULONG k;
    for (k = 0; k < RB2K_D8_BUFFER_KINDS; ++k)
        if (c->arenas[k].host && c->display)
            request(c->display, IOCTL_RB2K_D8_BUFFER_FREE, &c->arenas[k].host, sizeof(ULONG), NULL, 0);
    RtlZeroMemory(c->arenas, sizeof(c->arenas));
}

/* A buffer record goes, with its copy on the host (host: tell the miniport) and its sums. */
static void release_buffer(D8_BUFFER *b, BOOL host)
{
    if (b->display) host_log(b->display, RB2K_HOST_LOG_RELEASE, b->handle, b->host, host, 0, 0, 0, 0);
    if (host && b->host && b->display) request(b->display, IOCTL_RB2K_D8_BUFFER_FREE, &b->host, sizeof(b->host), NULL, 0);
    if (b->sums) EngFreeMem(b->sums);
    RtlZeroMemory(b, sizeof(*b));
    ++bufferEpoch;
}

/* The host buffer counts of displays that are gone: a mode change makes a new display, and a
 * full-screen program's counts would end with its mode. Reports add them (rb_d7_drawstats). */
static struct {
    ULONG draws, left[8], buffers, summed, uploads, uploadBytes, scans, scanHits, rewrites, streamed, arenas;
    ULONG sumSkips, streamHits;
} hostGone;

static void host_counts_keep(DISPLAY *d)
{
    RB2K_D7_DRAWSTATS *s = &d->d7draw;
    ULONG i;
    hostGone.draws += s->hostDraws; hostGone.buffers += s->hostBuffers; hostGone.summed += s->hostSummed;
    hostGone.uploads += s->hostUploads; hostGone.uploadBytes += s->hostUploadBytes; hostGone.scans += s->hostScans;
    hostGone.scanHits += s->hostScanHits; hostGone.rewrites += s->hostRewrites; hostGone.streamed += s->hostStreamed;
    hostGone.arenas += s->hostArenas; hostGone.sumSkips += s->hostSumSkips; hostGone.streamHits += s->hostStreamHits;
    s->hostSumSkips = s->hostStreamHits = 0;
    for (i = 0; i < 8; ++i) { hostGone.left[i] += s->hostLeft[i]; s->hostLeft[i] = 0; }
    s->hostDraws = s->hostBuffers = s->hostSummed = s->hostUploads = s->hostUploadBytes = s->hostScans = 0;
    s->hostScanHits = s->hostRewrites = s->hostStreamed = s->hostArenas = 0;
}

void rb_d7_disable(DISPLAY *d)
{
    ULONG i;
    if (!registry) return;
    EngAcquireSemaphore(registry);
    host_counts_keep(d);
    for (i = 0; i < MaxContexts; ++i)
        if (contexts[i].display == d) {
            release_arenas(&contexts[i]);
            release_shaders(&contexts[i]);
            RtlZeroMemory(&contexts[i], sizeof(contexts[i]));
        }
    /* Every DirectDraw object of this display is going away. */
    for (i = 0; textures && i < MaxTextures; ++i)
        if (textures[i].display == d) release_texture(&textures[i], TRUE);
    for (i = 0; buffers && i < MaxBuffers; ++i)
        if (buffers[i].display == d && buffers[i].handle) release_buffer(&buffers[i], TRUE);
    EngReleaseSemaphore(registry);
}

static D7_TEXTURE *find_texture(PDD_DIRECTDRAW_LOCAL local, ULONG handle)
{
    ULONG i;
    for (i = 0; textures && handle && i < MaxTextures; ++i)
        if (textures[i].handle == handle && textures[i].local == local) return &textures[i];
    return NULL;
}

/* A cube map one of whose faces has this handle; face gets which. */
static D7_TEXTURE *find_face(PDD_DIRECTDRAW_LOCAL local, ULONG handle, ULONG *face)
{
    ULONG i, k;
    for (i = 0; textures && handle && i < MaxTextures; ++i)
        if (textures[i].handle && textures[i].local == local && textures[i].faces == 6)
            for (k = 0; k < 6; ++k)
                if (textures[i].faceHandles[k] == handle) { *face = k; return &textures[i]; }
    return NULL;
}

/* Decode a texture's pixel format. NT copies the surface format into the
 * kernel record without the flag that says the surface has its own, so a
 * plausible RGB format is taken as given and anything else as the desktop's. */
ULONG rb_d7_texture_format(const DDPIXELFORMAT *f, ULONG desktopBits)
{
    ULONG alpha = (f->dwFlags & DDPF_ALPHAPIXELS) ? f->dwRGBAlphaBitMask : 0;
    if (f->dwFlags & (RB_DDPF_D3DFORMAT | DDPF_FOURCC)) {
        switch (f->dwFourCC) {
        case RbFmtA8R8G8B8: return TexArgb8888;
        case RbFmtX8R8G8B8: return TexXrgb8888;
        case RbFmtR5G6B5: return TexRgb565;
        case RbFmtX1R5G5B5: return TexXrgb1555;
        case RbFmtA1R5G5B5: return TexArgb1555;
        case RbFmtA4R4G4B4: return TexArgb4444;
        case RbFmtL8: return TexL8;
        case RbFmtA8L8: return TexA8L8;
        case RbFmtL16: return TexL16;
        case RbFmtA8: return TexA8;
        case RbFmtV8U8: return TexV8U8;
        case RbFmtQ8W8V8U8: return TexQ8W8V8U8;
        case RbFmtDxt1: return TexDxt1;
        case RbFmtDxt2: case RbFmtDxt3: return TexDxt3;
        case RbFmtDxt4: case RbFmtDxt5: return TexDxt5;
        }
        return TexNone;
    }
    /* Luminance and alpha-only formats, as the runtime describes L8, A8L8 and A8
     * (the bit count and masks share their places with the RGB ones). */
    if (f->dwFlags == 0x00020000 && f->dwRGBBitCount == 8 && f->dwRBitMask == 0xff) return TexL8;   /* DDPF_LUMINANCE */
    if (f->dwFlags == 0x00020000 && f->dwRGBBitCount == 16 && f->dwRBitMask == 0xffff) return TexL16;
    if (f->dwFlags == (0x00020000 | DDPF_ALPHAPIXELS) && f->dwRGBBitCount == 16 && f->dwRBitMask == 0xff &&
        f->dwRGBAlphaBitMask == 0xff00) return TexA8L8;
    if (f->dwFlags == 0x00000002 && f->dwRGBBitCount == 8) return TexA8;   /* DDPF_ALPHA */
    /* Bump maps (DDPF_BUMPDUDV): du in the red mask's place, dv in the green's. */
    if (f->dwFlags == 0x00080000 && f->dwRBitMask == 0xff && f->dwGBitMask == 0xff00)
        return f->dwRGBBitCount == 16 ? TexV8U8 : f->dwRGBBitCount == 32 ? TexQ8W8V8U8 : TexNone;
    if (!(f->dwFlags & DDPF_RGB) || !f->dwRGBBitCount) return desktopBits == 32 ? TexXrgb8888 : TexNone;
    if (f->dwFlags & ~(DDPF_RGB | DDPF_ALPHAPIXELS)) return TexNone;
    if (f->dwRGBBitCount == 32 && f->dwRBitMask == 0xff0000 && f->dwGBitMask == 0xff00 &&
        f->dwBBitMask == 0xff)
        return alpha == 0xff000000 ? TexArgb8888 : alpha ? TexNone : TexXrgb8888;
    if (f->dwRGBBitCount != 16) return TexNone;
    if (f->dwRBitMask == 0xf800 && f->dwGBitMask == 0x07e0 && f->dwBBitMask == 0x1f && !alpha)
        return TexRgb565;
    if (f->dwRBitMask == 0x7c00 && f->dwGBitMask == 0x03e0 && f->dwBBitMask == 0x1f)
        return alpha == 0x8000 ? TexArgb1555 : alpha ? TexNone : TexXrgb1555;
    if (f->dwRBitMask == 0x0f00 && f->dwGBitMask == 0x00f0 && f->dwBBitMask == 0x0f && alpha == 0xf000)
        return TexArgb4444;
    return TexNone;
}

ULONG rb_d7_texel_bytes(ULONG format)
{
    return format == TexArgb8888 || format == TexXrgb8888 || format == TexQ8W8V8U8 ? 4 :
           format == TexL8 || format == TexA8 ? 1 : 2;
}

static BOOL compressed(ULONG format) { return RB_TEX_COMPRESSED(format); }

ULONG rb_d7_row_bytes(ULONG format, ULONG width)
{
    return compressed(format) ? (width + 3) / 4 * (format == TexDxt1 ? 8 : 16) : width * rb_d7_texel_bytes(format);
}

ULONG rb_d7_rows(ULONG format, ULONG height)
{
    return compressed(format) ? (height + 3) / 4 : height;
}

/* Record or forget a texture. System memory belongs to the application;
 * a video-memory texture is runtime-allocated user memory (display_ddraw.c).
 * The runtime calls this again with a zero address when a system-memory
 * surface is destroyed, and DestroySurface reports video-memory ones. */
/* The cube face of a surface (0 for +X .. 5 for -Z), or -1. */
static int cube_face(PDD_SURFACE_LOCAL s)
{
    ULONG caps2 = s && s->lpSurfMore ? s->lpSurfMore->ddsCapsEx.dwCaps2 : 0;
    int k;
    for (k = 0; k < 6; ++k) if (caps2 & (DDSCAPS2_CUBEMAP_POSITIVEX << k)) return k;
    return -1;
}

/* The levels of one face below its top surface: each is attached to the
 * one above and is half its size (a cube's top face also holds the other
 * faces, so only mipmap sublevels count there). Returns the level count;
 * stop gets why the chain ended early (5 when it did not). */
static ULONG face_levels(D7_TEXTURE *t, ULONG face, PDD_SURFACE_LOCAL top, BOOL cube, ULONG *stop)
{
    PDD_SURFACE_LOCAL m = top;
    ULONG levels = 1;
    *stop = 5;
    t->levelAddress[face][0] = top->lpGbl->fpVidMem;
    t->levelPitch[face][0] = top->lpGbl->lPitch > 0 ? (ULONG)top->lpGbl->lPitch : 0;
    while (t->mipmaps && levels < RB2K_D7_LEVELS) {
        PDD_ATTACHLIST a;
        PDD_SURFACE_GLOBAL mg;
        PDD_SURFACE_LOCAL n = NULL;
        if (!m->lpAttachList) { *stop = 0; break; }
        for (a = m->lpAttachList; a && !n; a = a->lpLink) {
            PDD_SURFACE_LOCAL c = a->lpAttached;
            if (!c) continue;
            if (!cube) { n = c; break; }
            if ((c->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && c->lpSurfMore &&
                (c->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) n = c;
        }
        if (!(m = n)) { *stop = 1; break; }
        if (!(m->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) { *stop = 2; break; }
        mg = m->lpGbl;
        if (!mg || mg->wWidth != max(1, t->width >> levels) || mg->wHeight != max(1, t->height >> levels)) {
            *stop = 3;
            break;
        }
        if (!mg->fpVidMem || mg->lPitch <= 0) { *stop = 4; break; }
        t->levelAddress[face][levels] = mg->fpVidMem;
        t->levelPitch[face][levels] = (ULONG)mg->lPitch;
        ++levels;
    }
    return levels;
}

static void register_texture(DISPLAY *d, PDD_DIRECTDRAW_LOCAL local, PDD_SURFACE_LOCAL l)
{
    PDD_SURFACE_GLOBAL g = l->lpGbl;
    ULONG handle = l->lpSurfMore ? l->lpSurfMore->dwSurfaceHandle : 0, i;
    D7_TEXTURE *t;
    if (!textures || !handle) return;
    t = find_texture(local, handle);
    if (t) release_texture(t, TRUE);
    if (!g->fpVidMem) {
        ++d->d7draw.regNoMemory;
        d->d7draw.noMemoryCaps = l->ddsCaps.dwCaps;
        d->d7draw.noMemoryCaps2 = l->lpSurfMore ? l->lpSurfMore->ddsCapsEx.dwCaps2 : 0;
        d->d7draw.noMemoryFormat = g->ddpfSurface.dwFlags;
        return;
    }
    for (i = 0; i < MaxTextures && textures[i].handle; ++i) {}
    if (i == MaxTextures) { ++d->d7draw.regFull; return; }
    {
        ULONG k, live = 1;
        for (k = 0; k < MaxTextures; ++k) if (textures[k].handle) ++live;
        if (live > d->d7draw.regPeak) d->d7draw.regPeak = live;
    }
    t = &textures[i];
    t->local = local;
    t->global = g;
    t->display = d;
    t->owner = EngGetCurrentProcessId();
    t->handle = handle;
    t->address = g->fpVidMem;
    t->pitch = g->lPitch > 0 ? (ULONG)g->lPitch : 0;
    t->width = g->wWidth;
    t->height = g->wHeight;
    t->format = rb_d7_texture_format(&g->ddpfSurface, d->mode.BitsPerPlane);
    t->mipmaps = (l->ddsCaps.dwCaps & DDSCAPS_MIPMAP) != 0;
    t->blocks = (l->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) && compressed(t->format) ? t->format : 0;
    RtlZeroMemory(t->faceHandles, sizeof(t->faceHandles));
    if (t->mipmaps) ++d->d7draw.regMipmaps;
    t->tracked = !(l->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY);
    t->dirty = TRUE;
    t->faces = 1;
    t->depth = 1;
    {
        DD_SLOT *slot = rb_dd_slot(d, g);
        if (slot && slot->texture && !slot->lost && slot->owner == t->owner) {
            if (l->lpSurfMore && (l->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP)) {
                /* A cube map render target: Direct3D draws into each face's image by the face's own
                 * handle; sampling copies the faces drawn since into a host cube texture (cube_ready). */
                PDD_SURFACE_LOCAL top[6];
                PDD_ATTACHLIST a;
                int f = cube_face(l);
                ULONG k;
                RtlZeroMemory(top, sizeof(top));
                if (f >= 0) top[f] = l;
                for (a = l->lpAttachList; a; a = a->lpLink) {
                    int af = a->lpAttached ? cube_face(a->lpAttached) : -1;
                    if (af >= 0 && !top[af]) top[af] = a->lpAttached;
                }
                for (k = 0; k < 6; ++k) {
                    DD_SLOT *fs = top[k] && top[k]->lpGbl && top[k]->lpSurfMore ? rb_dd_slot(d, top[k]->lpGbl) : NULL;
                    if (!fs || !fs->texture || fs->lost || fs->owner != t->owner || fs->width != slot->width ||
                        fs->height != slot->width) {
                        t->format = TexNone;
                        ++d->d7draw.textures;
                        return;
                    }
                    fs->local = local;
                    fs->d3dHandle = top[k]->lpSurfMore->dwSurfaceHandle;
                    t->cubeImages[k] = fs->id;
                    t->cubeDraws[k] = ~0UL;
                }
                t->rtCube = TRUE;
                t->faces = 6;
                t->format = slot->alpha ? TexArgb8888 : TexXrgb8888;
                t->levels = 1;
                t->mipmaps = FALSE;
                t->dirty = FALSE;
                ++d->d7draw.targetTextures;
                ++d->d7draw.textures;
                return;
            }
            /* A render target texture: Direct3D draws into its image by the
             * same handle, and samples the image as it is. */
            t->image = slot->id;
            t->format = slot->alpha ? TexArgb8888 : TexXrgb8888;
            t->levels = 1;
            t->mipmaps = FALSE;
            t->dirty = FALSE;
            slot->local = local;
            slot->d3dHandle = handle;
            ++d->d7draw.targetTextures;
            ++d->d7draw.textures;
            return;
        }
    }
    if (l->lpSurfMore && (l->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP)) {
        /* A cube map: the other five faces hang off this one. */
        PDD_SURFACE_LOCAL top[6];
        PDD_ATTACHLIST a;
        ULONG k, stop;
        int face = cube_face(l);
        RtlZeroMemory(top, sizeof(top));
        if (face >= 0) top[face] = l;
        for (a = l->lpAttachList; a; a = a->lpLink) {
            int f = cube_face(a->lpAttached);
            if (f >= 0 && !top[f] && a->lpAttached->lpGbl && !(a->lpAttached->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL))
                top[f] = a->lpAttached;
        }
        t->faces = 6;
        t->levels = RB2K_D7_LEVELS;
        for (k = 0; k < 6; ++k) {
            ULONG levels;
            if (!top[k] || top[k]->lpGbl->wWidth != t->width || top[k]->lpGbl->wHeight != t->height ||
                !top[k]->lpGbl->fpVidMem) { t->format = TexNone; t->levels = 1; break; }
            t->faceHandles[k] = top[k]->lpSurfMore ? top[k]->lpSurfMore->dwSurfaceHandle : 0;
            levels = face_levels(t, k, top[k], TRUE, &stop);
            if (levels < t->levels) t->levels = levels;
        }
        if (t->width != t->height) t->format = TexNone;
        ++d->d7draw.cubeTextures;
    } else {
        ULONG stop;
        if (l->lpSurfMore && (l->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_VOLUME)) {
            /* A volume: its slices follow each other in each level's memory. */
            t->depth = l->lpSurfMore->ddsCapsEx.dwCaps4;   /* the depth */
            if (!t->depth || t->depth > RB2K_D7_VOLUME_SIDE) t->format = TexNone;
            ++d->d7draw.volumeTextures;
        }
        t->levels = face_levels(t, 0, l, FALSE, &stop);
        if (t->mipmaps) {
            if (stop < 5 && (t->width >> t->levels || t->height >> t->levels)) ++d->d7draw.mipStops[stop];
            else ++d->d7draw.mipChains;
        }
    }
    if (l->lpSurfMore && (l->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_TEXTUREMANAGE)) ++d->d7draw.regManaged;
    if (!t->width || t->width > RB2K_D7_TEXTURE_SIDE || !t->height || t->height > RB2K_D7_TEXTURE_SIDE ||
        t->pitch < rb_d7_row_bytes(t->format, t->width))
        t->format = TexNone;
    ++d->d7draw.textures;
    ++d->d7draw.registered[t->format < 7 ? t->format : 7];   /* 7: compressed and the rest */
    if (!(l->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) ++d->d7draw.videoTextures;
}

/* A DirectDraw lock, unlock or blit of a texture surface (display_ddraw.c).
 * Mark every registered texture with that surface as a level dirty. This
 * runs without the registry semaphore: it only sets flags, and a texture
 * registered meanwhile starts dirty anyway. */
void rb_d7_texture_written(DISPLAY *d, PDD_SURFACE_GLOBAL g)
{
    ULONG i, level, face;
    if (!textures || !g) return;
    ++d->d7draw.textureWrites;
    for (i = 0; i < MaxTextures; ++i) {
        D7_TEXTURE *t = &textures[i];
        if (!t->handle || t->display != d) continue;
        if (t->global == g) t->dirty = TRUE;
        else for (face = 0; face < t->faces; ++face)
            for (level = 0; level < t->levels; ++level)
                if (t->levelAddress[face][level] == g->fpVidMem) t->dirty = TRUE;
    }
}

/* DestroySurface for a video-memory texture (display_ddraw.c), called
 * before it takes the display lock. */
void rb_d7_surface_destroyed(DISPLAY *d, PDD_SURFACE_GLOBAL g)
{
    ULONG i;
    if (!registry || !textures) return;
    EngAcquireSemaphore(registry);
    EngAcquireSemaphore(d->ddLock);
    for (i = 0; i < MaxTextures; ++i)
        if (textures[i].display == d && textures[i].global == g && textures[i].handle)
            release_texture(&textures[i], TRUE);
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
}

static D8_BUFFER *find_buffer(PDD_DIRECTDRAW_LOCAL local, ULONG handle)
{
    ULONG i;
    for (i = 0; buffers && handle && i < MaxBuffers; ++i)
        if (buffers[i].handle == handle && buffers[i].local == local) return &buffers[i];
    return NULL;
}

/* The runtime registers a buffer again with a zero address when it goes. */
static void register_buffer(DISPLAY *d, PDD_DIRECTDRAW_LOCAL local, PDD_SURFACE_LOCAL l)
{
    ULONG handle = l->lpSurfMore ? l->lpSurfMore->dwSurfaceHandle : 0, i;
    D8_BUFFER *b;
    if (!buffers || !handle) return;
    b = find_buffer(local, handle);
    if (b && l->lpGbl->fpVidMem && b->host && b->display == d && b->hostBytes == l->lpGbl->dwLinearSize) {
        /* Registered again with memory of the same size: its copy on the host stays, and the
         * sums find what differs. */
        b->global = l->lpGbl;
        b->owner = EngGetCurrentProcessId();
        host_log(d, RB2K_HOST_LOG_REGISTER, handle, (ULONG)l->lpGbl->fpVidMem, l->lpGbl->dwLinearSize, 1, 0, 0, 0);
        return;
    }
    if (b) release_buffer(b, TRUE);
    if (!l->lpGbl->fpVidMem) return;
    for (i = 0; i < MaxBuffers && buffers[i].handle; ++i) {}
    if (i == MaxBuffers) return;
    b = &buffers[i];
    b->local = local;
    b->global = l->lpGbl;
    b->display = d;
    b->owner = EngGetCurrentProcessId();
    b->handle = handle;
    host_log(d, RB2K_HOST_LOG_REGISTER, handle, (ULONG)l->lpGbl->fpVidMem, l->lpGbl->dwLinearSize, 0, 0, 0, 0);
    ++d->d7draw.d8Buffers;
    ++bufferEpoch;
}

static DWORD APIENTRY register_surface(PDD_CREATESURFACEEXDATA a)
{
    PDD_SURFACE_LOCAL l = a->lpDDSLcl;
    DISPLAY *d = (DISPLAY *)a->lpDDLcl->lpGbl->dhpdev;
    DD_SLOT *slot;
    a->ddRVal = DD_OK;
    if (!l || !l->lpGbl) return DDHAL_DRIVER_HANDLED;
    if (l->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER) {
        EngAcquireSemaphore(registry);
        EngAcquireSemaphore(d->ddLock);
        if (d->nativeD3D7) register_buffer(d, a->lpDDLcl, l);
        EngReleaseSemaphore(d->ddLock);
        EngReleaseSemaphore(registry);
        return DDHAL_DRIVER_HANDLED;
    }
    /* Textures, including render target textures (whose image is also a
     * target: register_texture gives it its handle). */
    if ((l->ddsCaps.dwCaps & DDSCAPS_TEXTURE) &&
        (!(l->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) || rb_dd_slot(d, l->lpGbl))) {
        EngAcquireSemaphore(registry);
        EngAcquireSemaphore(d->ddLock);
        ++d->d7.surfaceCalls;
        if (d->nativeD3D7) register_texture(d, a->lpDDLcl, l);
        EngReleaseSemaphore(d->ddLock);
        EngReleaseSemaphore(registry);
        return DDHAL_DRIVER_HANDLED;
    }
    if (!(l->ddsCaps.dwCaps & (DDSCAPS_3DDEVICE | (d->z16Render ? DDSCAPS_ZBUFFER : 0))) ||
        (l->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) return DDHAL_DRIVER_HANDLED;
    EngAcquireSemaphore(d->ddLock);
    ++d->d7.surfaceCalls;
    slot = rb_dd_slot(d, l->lpGbl);
    if (!slot && d->nativeD3D7 && rb_dd_chain_front(d, l)) slot = rb_dd_chain_front_slot(d, l);
    /* DirectDraw registers a 3D-tagged window primary before it will blit
     * to it. Its storage is the scanout mapping. Only offscreen targets
     * need a host image and a Direct3D surface handle. */
    if (!slot && d->nativeD3D7 && (rb_dd_mapped_primary(d, l) || rb_dd_chain_front(d, l)))
        a->ddRVal = a->dwFlags ? DDERR_UNSUPPORTED : DD_OK;
    else if (!d->ddEnabled || !slot || slot->lost || slot->owner != EngGetCurrentProcessId())
        a->ddRVal = DDERR_SURFACELOST;
    else if (slot->depth16 != !!(l->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) ||
             a->dwFlags || !l->lpSurfMore || !l->lpSurfMore->dwSurfaceHandle ||
             (slot->local && slot->local != a->lpDDLcl))
        a->ddRVal = DDERR_UNSUPPORTED;
    else {
        slot->local = a->lpDDLcl;
        slot->d3dHandle = l->lpSurfMore->dwSurfaceHandle;
        if (slot->chain) {
            /* Direct3D registers only the front of a full-screen chain; the
             * back buffer hangs off it and has a handle of its own. */
            PDD_ATTACHLIST at;
            for (at = l->lpAttachList; at; at = at->lpLink) {
                PDD_SURFACE_LOCAL b = at->lpAttached;
                DD_SLOT *bs = b ? rb_dd_slot(d, b->lpGbl) : NULL;
                if (!bs || !bs->chain || bs == slot || !b->lpSurfMore || !b->lpSurfMore->dwSurfaceHandle ||
                    (bs->local && bs->local != a->lpDDLcl)) continue;
                bs->local = a->lpDDLcl;
                bs->d3dHandle = b->lpSurfMore->dwSurfaceHandle;
                ++d->d7draw.chainBacks;
            }
        }
    }
    EngReleaseSemaphore(d->ddLock);
    return DDHAL_DRIVER_HANDLED;
}

/* The queue is shared by all DirectDraw clients. Flush only this target.
 * A surface observer may already have finished the same commands. */
static HRESULT finish_scene(D7_CONTEXT *c)
{
    ULONG written;
    if (!c->queued) return DD_OK;
    c->queued = FALSE;
    return miniport(c->display, IOCTL_RB2K_D3D7_FLUSH, &c->target,
                    sizeof(c->target), NULL, 0, &written) ? DDERR_SURFACELOST : DD_OK;
}

static DWORD APIENTRY destroy_local(PDD_DESTROYDDLOCALDATA a)
{
    DISPLAY *d = (DISPLAY *)a->pDDLcl->lpGbl->dhpdev;
    ULONG i;
    EngAcquireSemaphore(registry);
    EngAcquireSemaphore(d->ddLock);
    a->ddRVal = DD_OK;
    for (i = 0; i < MaxContexts; ++i) {
        if (contexts[i].display == d && contexts[i].local == a->pDDLcl) {
            if (FAILED(finish_scene(&contexts[i]))) a->ddRVal = DDERR_SURFACELOST;
            release_arenas(&contexts[i]);
            release_shaders(&contexts[i]);
            d9_release(&contexts[i]);
            RtlZeroMemory(&contexts[i], sizeof(contexts[i]));
        }
    }
    for (i = 0; i < RB2K_DD_SLOTS; ++i) {
        DD_SLOT *s = &d->slots[i];
        if (s->local == a->pDDLcl) { s->local = NULL; s->d3dHandle = 0; }
    }
    for (i = 0; textures && i < MaxTextures; ++i)
        if (textures[i].local == a->pDDLcl) release_texture(&textures[i], TRUE);
    for (i = 0; buffers && i < MaxBuffers; ++i)
        if (buffers[i].local == a->pDDLcl && buffers[i].handle) release_buffer(&buffers[i], TRUE);
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY get_driver_state(PDD_GETDRIVERSTATEDATA a)
{
    /* The runtime owns managed textures. This HAL does not maintain the
     * optional per-application texturing statistics requested by GetInfo. */
    switch (a->dwFlags) {
    case D3DDEVINFOID_TEXTUREMANAGER:
    case D3DDEVINFOID_D3DTEXTUREMANAGER:
        a->ddRVal = DDERR_CURRENTLYNOTAVAIL;
        return DDHAL_DRIVER_NOTHANDLED;
    case D3DDEVINFOID_TEXTURING:
        a->ddRVal = DDERR_CURRENTLYNOTAVAIL;
        break;
    default:
        a->ddRVal = DDERR_UNSUPPORTED;
    }
    return DDHAL_DRIVER_HANDLED;
}

static void default_stages(D7_CONTEXT *c)
{
    ULONG stage;
    for (stage = 0; stage < TextureStages; ++stage) {
        D7_STAGE *s = &c->stages[stage];
        RtlZeroMemory(s, sizeof(*s));
        s->colorOp = stage ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        s->alphaOp = stage ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        s->colorArg1 = s->alphaArg1 = D3DTA_TEXTURE;
        s->colorArg2 = s->alphaArg2 = D3DTA_CURRENT;
        s->coordIndex = stage;
        s->addressU = s->addressV = D3DTADDRESS_WRAP;
        s->magFilter = D3DTFG_POINT;
        s->minFilter = D3DTFN_POINT;
        s->mipFilter = D3DTFP_NONE;
    }
}

static DWORD APIENTRY create_context(LPD3DNTHAL_CONTEXTCREATEDATA a)
{
    PDD_DIRECTDRAW_LOCAL local = a->lpDDLcl;
    ULONG i, kind = (ULONG)a->dwhContext;
    DISPLAY *d;
    DD_SLOT *s, *z = NULL;
    a->dwhContext = 0;
    a->ddrval = DDERR_UNSUPPORTED;
    if (!local || !local->lpGbl || !a->lpDDSLcl || !a->lpDDSLcl->lpGbl ||
        !(a->lpDDSLcl->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) ||
        (a->lpDDSZLcl && (!a->lpDDSZLcl->lpGbl || !(a->lpDDSZLcl->ddsCaps.dwCaps & DDSCAPS_ZBUFFER))))
        return DDHAL_DRIVER_HANDLED;
    d = (DISPLAY *)local->lpGbl->dhpdev;
    EngAcquireSemaphore(registry);
    EngAcquireSemaphore(d->ddLock);
    s = rb_dd_slot(d, a->lpDDSLcl->lpGbl);
    if (a->lpDDSZLcl) z = rb_dd_slot(d, a->lpDDSZLcl->lpGbl);
    ++d->d7.contextCalls;
    ++d->d7draw.d8ContextKinds[kind < 7 ? kind : 7];
    if (!d->ddEnabled || !s || s->depth16 || s->lost || s->owner != EngGetCurrentProcessId() ||
        (s->local && s->local != local) || (a->lpDDSZLcl && (!d->z16Render || !z || !z->depth16 ||
        z->lost || z->owner != s->owner || (z->local && z->local != local) ||
        z->width < s->width || z->height < s->height))) a->ddrval = DDERR_SURFACELOST;
    else {
        a->ddrval = DDERR_OUTOFMEMORY;
        for (i = 0; lastContext != 0xffffffffUL && i < MaxContexts; ++i) {
            D7_CONTEXT *c = &contexts[i];
            ULONG m;
            if (c->id) continue;
            RtlZeroMemory(c, sizeof(*c));
            c->display = d;
            c->local = local;
            c->owner = s->owner;
            c->target = s->id;
            c->depth = z ? z->id : 0;
            c->zEnable = z ? D3DZB_TRUE : D3DZB_FALSE;
            c->zWrite = TRUE;
            c->zFunc = D3DCMP_LESSEQUAL;
            c->cull = D3DCULL_CCW;
            c->shade = D3DSHADE_GOURAUD;
            c->fill = D3DFILL_SOLID;
            c->alphaFunc = D3DCMP_ALWAYS;
            c->srcBlend = D3DBLEND_ONE;
            c->dstBlend = D3DBLEND_ZERO;
            c->factor = 0xffffffff;
            c->stencilFunc = D3DCMP_ALWAYS;
            c->stencilMask = c->stencilWriteMask = 0xffffffff;
            c->stencilFail = c->stencilZFail = c->stencilPass = D3DSTENCILOP_KEEP;
            c->lighting = TRUE;
            c->colorVertex = c->localViewer = TRUE;
            c->sources[0] = D3DMCS_COLOR1;
            c->sources[1] = D3DMCS_COLOR2;
            c->fogEnd = c->fogDensity = c->wNear = c->wFar = 0x3f800000;
            for (m = 0; m < 3; ++m)
                c->matrices[m][0] = c->matrices[m][5] = c->matrices[m][10] = c->matrices[m][15] = 0x3f800000;
            for (m = 0; m < TextureStages; ++m)
                c->texMatrices[m][0] = c->texMatrices[m][5] = c->texMatrices[m][10] = c->texMatrices[m][15] = 0x3f800000;
            c->viewport[2] = s->width;
            c->viewport[3] = s->height;
            c->zRange[1] = 0x3f800000;
            default_stages(c);
            /* The Direct3D 8 and 9 runtimes pass 4 here; Direct3D 7 passes 3. */
            c->dx8 = kind >= 4;
            c->dx9Ddi = c->dx8 && d->d3d9Ddi && d->d7draw.d8Version >= 0x900;
            c->scissor.left = c->scissor.top = 0;
            c->scissor.right = c->scissor.bottom = 8192;
            c->id = ++lastContext;
            a->dwhContext = c->id;
            a->ddrval = DD_OK;
            break;
        }
    }
    if (FAILED(a->ddrval)) d->d7.lastError = a->ddrval;
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY destroy_context(LPD3DNTHAL_CONTEXTDESTROYDATA a)
{
    D7_CONTEXT *c;
    EngAcquireSemaphore(registry);
    c = owned_context(a->dwhContext);
    a->ddrval = c ? DD_OK : DDERR_INVALIDOBJECT;
    if (c) {
        DISPLAY *d = c->display;
        EngAcquireSemaphore(d->ddLock);
        a->ddrval = finish_scene(c);
        release_arenas(c);
        release_shaders(c);
        d9_release(c);
        RtlZeroMemory(c, sizeof(*c));
        EngReleaseSemaphore(d->ddLock);
    }
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

/* ---- texture stages zero and one, reduced to what the miniport draws ---- */

enum { StageOk, StageDisabled, StageUnsupported };

/* Stage zero's current colour is the diffuse colour; stage one's is stage
 * zero's result. */
static ULONG stage_argument(ULONG arg, ULONG stage, BOOL *ok)
{
    ULONG select = arg & D3DTA_SELECTMASK;
    if (arg & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE) ||
        select > D3DTA_SPECULAR) *ok = FALSE;
    if (select == D3DTA_CURRENT && !stage) arg = (arg & ~D3DTA_SELECTMASK) | D3DTA_DIFFUSE;
    return arg;
}

static BOOL samples_texture(ULONG op, ULONG arg1, ULONG arg2)
{
    if (op == D3DTOP_BLENDTEXTUREALPHA || op == D3DTOP_BLENDTEXTUREALPHAPM) return TRUE;
    return (op != D3DTOP_SELECTARG2 && (arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE) ||
           (op != D3DTOP_SELECTARG1 && (arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE);
}

/* Fill the colour and alpha words of stage n. A stage that reads a missing
 * texture passes its current colour through, as the 9x HAL and the RGB
 * device do; a disabled stage one leaves stage zero's result. */
static ULONG stage_words(const D7_CONTEXT *c, ULONG n, ULONG *color, ULONG *alpha, BOOL *texture)
{
    const D7_STAGE *s = &c->stages[n];
    ULONG colorOp = s->colorOp, alphaOp = s->alphaOp, current = n ? D3DTA_CURRENT : D3DTA_DIFFUSE;
    BOOL ok = TRUE;
    ULONG c1 = stage_argument(s->colorArg1, n, &ok), c2 = stage_argument(s->colorArg2, n, &ok);
    ULONG a1 = stage_argument(s->alphaArg1, n, &ok), a2 = stage_argument(s->alphaArg2, n, &ok);
    *texture = FALSE;
    if (colorOp == D3DTOP_DISABLE ||
        ((samples_texture(colorOp, c1, c2) || (alphaOp != D3DTOP_DISABLE && samples_texture(alphaOp, a1, a2))) &&
         !s->texture)) {
        *color = *alpha = D3DTOP_SELECTARG1 | (current << 8);
        return StageDisabled;
    }
    if (alphaOp == D3DTOP_DISABLE) { alphaOp = D3DTOP_SELECTARG1; a1 = current; }
    /* DOTPRODUCT3 puts its result in all four channels: the alpha does the same. */
    if (colorOp == D3DTOP_DOTPRODUCT3) { alphaOp = colorOp; a1 = c1; a2 = c2; }
    if (!ok || colorOp < D3DTOP_SELECTARG1 ||
        (colorOp > D3DTOP_BLENDCURRENTALPHA && colorOp != D3DTOP_DOTPRODUCT3) ||
        alphaOp < D3DTOP_SELECTARG1 ||
        (alphaOp > D3DTOP_BLENDCURRENTALPHA && alphaOp != D3DTOP_DOTPRODUCT3)) return StageUnsupported;
    *color = colorOp | (c1 << 8) | (c2 << 16);
    *alpha = alphaOp | (a1 << 8) | (a2 << 16);
    *texture = samples_texture(colorOp, c1, c2) || samples_texture(alphaOp, a1, a2);
    return StageOk;
}

/* The fixed-function stages a Direct3D 8 or 9 runtime is offered: four for Direct3D 8 (the
 * texture registers of its version 1 shaders), eight once a runtime has announced Direct3D 9 (the
 * samplers and coordinate sets of shader model 2, which the stage program uses). */
static ULONG stage_limit(const DISPLAY *d)
{
    return d->d7draw.d8Version >= 0x900 ? RB2K_D8_STAGES : RB_PS_COORD_SETS;
}

/* Direct3D 8 fixed-function stages, drawn by the stage program (rb_stage_translate):
 * up to stage_limit, with any operation up to DOTPRODUCT3 except PREMODULATE, and bump
 * mapping only as a colour operation. Texture matrices and bump matrices have constants
 * for the first RB_STAGE_TRANSFORMS stages only. */
static BOOL d3d8_stages_valid(const D7_CONTEXT *c)
{
    ULONG i, k, limit = stage_limit(c->display);
    for (i = 0; i < TextureStages && c->stages[i].colorOp != D3DTOP_DISABLE; ++i) {
        const D7_STAGE *s = &c->stages[i];
        const ULONG args[4] = { s->colorArg1, s->colorArg2, s->alphaArg1, s->alphaArg2 };
        if (i >= limit || s->colorOp == D3DTOP_PREMODULATE || s->colorOp > D3DTOP_DOTPRODUCT3 ||
            s->alphaOp == D3DTOP_PREMODULATE || (s->alphaOp >= D3DTOP_BUMPENVMAP && s->alphaOp != D3DTOP_DOTPRODUCT3))
            return FALSE;
        if (i >= RB_STAGE_TRANSFORMS && ((s->transform & (0xff | RB_TTFF_PROJECTED)) ||
            s->colorOp == D3DTOP_BUMPENVMAP || s->colorOp == D3DTOP_BUMPENVMAPLUMINANCE)) return FALSE;
        for (k = 0; k < 4; ++k)
            if ((args[k] & D3DTA_SELECTMASK) > 5 /* D3DTA_TEMP */ ||
                (args[k] & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE))) return FALSE;
    }
    return TRUE;
}

static DWORD APIENTRY validate_stages(LPD3DNTHAL_VALIDATETEXTURESTAGESTATEDATA a)
{
    D7_CONTEXT *c;
    ULONG color, alpha;
    BOOL texture;
    EngAcquireSemaphore(registry);
    c = owned_context(a->dwhContext);
    if (!c) a->ddrval = DDERR_INVALIDOBJECT;
    else if (a->dwFlags) a->ddrval = DDERR_INVALIDPARAMS;
    else if (c->dx8) a->ddrval = d3d8_stages_valid(c) ? DD_OK : D3DERR_UNSUPPORTEDCOLOROPERATION;
    else if (c->stages[1].colorOp != D3DTOP_DISABLE && c->stages[2].colorOp != D3DTOP_DISABLE)
        a->ddrval = D3DERR_TOOMANYOPERATIONS;
    else a->ddrval = stage_words(c, 0, &color, &alpha, &texture) == StageUnsupported ||
                     stage_words(c, 1, &color, &alpha, &texture) == StageUnsupported ?
                     D3DERR_UNSUPPORTEDCOLOROPERATION : DD_OK;
    a->dwNumPasses = SUCCEEDED(a->ddrval) ? 1 : 0;
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

static DD_SLOT *target(D7_CONTEXT *c, ULONG key, BOOL byHandle)
{
    ULONG i;
    for (i = 0; key && i < RB2K_DD_SLOTS; ++i) {
        DD_SLOT *s = &c->display->slots[i];
        if (s->global && !s->lost && s->owner == c->owner &&
            (byHandle ? s->local == c->local && s->d3dHandle == key :
                        (!s->local || s->local == c->local) && s->id == key))
            return s;
    }
    return NULL;
}

/* The render target and depth buffer a draw or clear goes to. Direct3D 9 sets the two
 * separately, and programs leave the scene's depth buffer set while a full-screen pass fills a
 * larger texture with the depth test off. So a depth buffer smaller than the target is not an
 * error: *small is set and *depth is NULL, and the caller decides (render_surfaces for draws). */
static HRESULT target_surfaces(D7_CONTEXT *c, DD_SLOT **color, DD_SLOT **depth, BOOL *small)
{
    DD_SLOT *s = target(c, c->target, FALSE);
    DD_SLOT *z = c->depth ? target(c, c->depth, FALSE) : NULL;
    ULONG why = !s ? 1 : s->depth16 ? 2 : !c->depth ? 0 : !z ? 3 : !z->depth16 ? 4 : 0;
    RB2K_D7_DRAWSTATS *st = &c->display->d7draw;
    *small = !why && z && (z->width < s->width || z->height < s->height);
    if (why || *small) {
        st->surfaceRefusal[0] = why ? why : 5;
        st->surfaceRefusal[1] = s ? s->id : 0;
        st->surfaceRefusal[2] = s ? s->width << 16 | s->height : 0;
        st->surfaceRefusal[3] = z ? z->id : 0;
        st->surfaceRefusal[4] = z ? z->width << 16 | z->height : 0;
        st->surfaceRefusal[5] = (c->zEnable ? 1 : 0) | (c->zWrite ? 2 : 0) | (c->stencil ? 4 : 0);
    }
    if (why) {
        ++st->surfaceRefusals;
        return DDERR_SURFACELOST;
    }
    if (*small) z = NULL;
    if (s->locked || (z && z->locked)) return DDERR_SURFACEBUSY;
    *color = s;
    *depth = z;
    ++s->draws;   /* a cube map face is copied again when sampled (cube_ready) */
    return DD_OK;
}

/* The surfaces of a draw. With a depth buffer smaller than the target the draw goes ahead
 * without one when it uses neither the depth test nor the stencil (smallDepthDraws). With one of
 * them on, what Direct3D draws is undefined: the draw is left out (S_FALSE, a surface refusal),
 * not failed, because a failed draw makes the runtime drop the rest of its command batch. */
static HRESULT render_surfaces(D7_CONTEXT *c, DD_SLOT **color, DD_SLOT **depth)
{
    BOOL small;
    HRESULT hr = target_surfaces(c, color, depth, &small);
    if (FAILED(hr) || !small) return hr;
    if (c->zEnable || c->stencil) {
        ++c->display->d7draw.surfaceRefusals;
        return S_FALSE;
    }
    ++c->display->d7draw.smallDepthDraws;
    return DD_OK;
}

static void render_state(D7_CONTEXT *c, ULONG state, ULONG value)
{
    switch (state) {
    case D3DRENDERSTATE_CULLMODE: c->cull = value; break;
    case D3DRENDERSTATE_SHADEMODE: c->shade = value; break;
    case D3DRENDERSTATE_FILLMODE: c->fill = value; break;
    case D3DRENDERSTATE_ZENABLE: c->zEnable = value; break;
    case D3DRENDERSTATE_ZWRITEENABLE: c->zWrite = value; break;
    case D3DRENDERSTATE_ZFUNC: c->zFunc = value; break;
    case D3DRENDERSTATE_ALPHATESTENABLE: c->alphaTest = value; break;
    case D3DRENDERSTATE_ALPHAFUNC: c->alphaFunc = value; break;
    case D3DRENDERSTATE_ALPHAREF: c->alphaRef = value; break;
    case D3DRENDERSTATE_ALPHABLENDENABLE: c->blend = value; break;
    case D3DRENDERSTATE_SRCBLEND: c->srcBlend = value; break;
    case D3DRENDERSTATE_DESTBLEND: c->dstBlend = value; break;
    case D3DRENDERSTATE_FOGENABLE: c->fog = value; break;
    case D3DRENDERSTATE_FOGCOLOR: c->fogColor = value; break;
    case D3DRENDERSTATE_FOGTABLEMODE: c->fogTable = value; break;
    case D3DRENDERSTATE_FOGVERTEXMODE: c->fogVertex = value; break;
    case D3DRENDERSTATE_FOGSTART: c->fogStart = value; break;
    case D3DRENDERSTATE_FOGEND: c->fogEnd = value; break;
    case D3DRENDERSTATE_FOGDENSITY: c->fogDensity = value; break;
    case D3DRENDERSTATE_RANGEFOGENABLE: c->rangeFog = value; break;
    case D3DRENDERSTATE_SPECULARENABLE: c->specular = value; break;
    case D3DRENDERSTATE_STENCILENABLE: c->stencil = value; break;
    case D3DRENDERSTATE_STENCILFAIL: c->stencilFail = value; break;
    case D3DRENDERSTATE_STENCILZFAIL: c->stencilZFail = value; break;
    case D3DRENDERSTATE_STENCILPASS: c->stencilPass = value; break;
    case D3DRENDERSTATE_STENCILFUNC: c->stencilFunc = value; break;
    case D3DRENDERSTATE_STENCILREF: c->stencilRef = value; break;
    case D3DRENDERSTATE_STENCILMASK: c->stencilMask = value; break;
    case D3DRENDERSTATE_STENCILWRITEMASK: c->stencilWriteMask = value; break;
    case D3DRENDERSTATE_CLIPPLANEENABLE: c->clipPlanes = value; break;
    case D3DRENDERSTATE_COLORKEYENABLE: c->colorKey = value; break;
    case D3DRENDERSTATE_TEXTUREFACTOR: c->factor = value; break;
    case D3DRENDERSTATE_LIGHTING: c->lighting = value; break;
    case D3DRENDERSTATE_AMBIENT: c->ambient = value; break;
    case D3DRENDERSTATE_COLORVERTEX: c->colorVertex = value; break;
    case D3DRENDERSTATE_LOCALVIEWER: c->localViewer = value; break;
    case D3DRENDERSTATE_NORMALIZENORMALS: c->normalize = value; break;
    case D3DRENDERSTATE_DIFFUSEMATERIALSOURCE: c->sources[0] = value; break;
    case D3DRENDERSTATE_SPECULARMATERIALSOURCE: c->sources[1] = value; break;
    case D3DRENDERSTATE_AMBIENTMATERIALSOURCE: c->sources[2] = value; break;
    case D3DRENDERSTATE_EMISSIVEMATERIALSOURCE: c->sources[3] = value; break;
    }
}

static void stage_state(D7_STAGE *s, ULONG state, ULONG value)
{
    switch (state) {
    case D3DTSS_TEXTUREMAP: s->texture = value; break;
    case D3DTSS_COLOROP: s->colorOp = value; break;
    case D3DTSS_COLORARG1: s->colorArg1 = value; break;
    case D3DTSS_COLORARG2: s->colorArg2 = value; break;
    case D3DTSS_ALPHAOP: s->alphaOp = value; break;
    case D3DTSS_ALPHAARG1: s->alphaArg1 = value; break;
    case D3DTSS_ALPHAARG2: s->alphaArg2 = value; break;
    case D3DTSS_TEXCOORDINDEX: s->coordIndex = value; break;
    case D3DTSS_ADDRESS: s->addressU = s->addressV = value; break;
    case D3DTSS_ADDRESSU: s->addressU = value; break;
    case D3DTSS_ADDRESSV: s->addressV = value; break;
    case D3DTSS_MAGFILTER: s->magFilter = value; break;
    case D3DTSS_MINFILTER: s->minFilter = value; break;
    case D3DTSS_MIPFILTER: s->mipFilter = value; break;
    case D3DTSS_BORDERCOLOR: s->border = value; break;
    case D3DTSS_TEXTURETRANSFORMFLAGS: s->transform = value; break;
    }
}

/* ---- texture upload ---- */

static ULONG expand(ULONG v, ULONG bits)
{
    return bits == 4 ? v * 17 : bits == 5 ? (v << 3) | (v >> 2) : (v << 2) | (v >> 4);
}

static ULONG convert(ULONG format, ULONG p)
{
    switch (format) {
    case TexArgb8888: return p;
    case TexXrgb8888: return p | 0xff000000;
    case TexRgb565:
        return 0xff000000 | expand((p >> 11) & 31, 5) << 16 | expand((p >> 5) & 63, 6) << 8 | expand(p & 31, 5);
    case TexArgb1555:
    case TexXrgb1555:
        return (format == TexXrgb1555 || (p & 0x8000) ? 0xff000000 : 0) |
               expand((p >> 10) & 31, 5) << 16 | expand((p >> 5) & 31, 5) << 8 | expand(p & 31, 5);
    case TexArgb4444:
        return expand((p >> 12) & 15, 4) << 24 | expand((p >> 8) & 15, 4) << 16 |
               expand((p >> 4) & 15, 4) << 8 | expand(p & 15, 4);
    /* Luminance is grey; an alpha-only texture samples as black with alpha. */
    case TexL8: return 0xff000000 | (p & 255) * 0x010101;
    case TexL16: return 0xff000000 | ((p >> 8) & 255) * 0x010101;   /* the top eight bits */
    case TexA8L8: return (p >> 8) << 24 | (p & 255) * 0x010101;
    case TexA8: return (p & 255) << 24;
    }
    return 0;
}

/* Read the level-zero texels into the scratch buffer, tightly packed. */
/* The texels of every level, tightly packed level after level. */
static ULONG level_side(ULONG size, ULONG level) { return size >> level ? size >> level : 1; }

/* A texture's planes in upload order: each face's levels (one face, or a
 * cube map's six), or each level's slices (a volume). */
static ULONG level_depth(const D7_TEXTURE *t, ULONG level)
{
    return t->depth >> level ? t->depth >> level : 1;
}

static ULONG plane_count(const D7_TEXTURE *t)
{
    ULONG level, n = 0;
    if (t->depth <= 1) return t->faces * t->levels;
    for (level = 0; level < t->levels; ++level) n += level_depth(t, level);
    return n;
}

static void plane_at(const D7_TEXTURE *t, ULONG i, ULONG *face, ULONG *level, ULONG *slice)
{
    if (t->depth <= 1) {
        *face = i / t->levels;
        *level = i % t->levels;
        *slice = 0;
        return;
    }
    *face = 0;
    for (*level = 0; *level + 1 < t->levels && i >= level_depth(t, *level); ++*level) i -= level_depth(t, *level);
    *slice = i;
}

static BOOL read_texels(D7_TEXTURE *t, ULONG *sum)
{
    ULONG bytes = 0, level, face, slice, plane, planes = plane_count(t), y, i, h = 0x811c9dc5;
    BYTE *out;
    for (plane = 0; plane < planes; ++plane) {
        plane_at(t, plane, &face, &level, &slice);
        bytes += rb_d7_row_bytes(t->format, level_side(t->width, level)) * rb_d7_rows(t->format, level_side(t->height, level));
    }
    if (bytes > texelScratchBytes) {
        if (texelScratch) EngFreeMem(texelScratch);
        texelScratch = EngAllocMem(0, bytes, 'r7bR');
        texelScratchBytes = texelScratch ? bytes : 0;
        if (!texelScratch) return FALSE;
    }
    out = texelScratch;
    __try {
        for (plane = 0; plane < planes; ++plane) {
                ULONG row, rows, pitch;
                const BYTE *in;
                plane_at(t, plane, &face, &level, &slice);
                row = rb_d7_row_bytes(t->format, level_side(t->width, level));
                rows = rb_d7_rows(t->format, level_side(t->height, level));
                pitch = t->levelPitch[face][level];
                /* A volume's slices follow each other, a slice pitch apart. */
                in = (const BYTE *)t->levelAddress[face][level] + slice * pitch * rows;
                if (pitch < row) return FALSE;
                EngProbeForRead((PVOID)in, pitch * (rows - 1) + row, 1);
                for (y = 0; y < rows; ++y, out += row) RtlCopyMemory(out, in + y * pitch, row);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    for (i = 0; i + 4 <= bytes; i += 4) h = (h ^ *(const ULONG *)(texelScratch + i)) * 0x01000193;
    *sum = h ^ bytes;
    return TRUE;
}

/* The colours of a DXT colour block: two RGB565 ends and two between them,
 * or one between and transparent black when the first end is not above the
 * second (DXT1 only: four colours selects the four-colour form). */
static void block_colors(const BYTE *b, BOOL four, ULONG *c)
{
    ULONG c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8, k, e[2][3];
    e[0][0] = expand(c0 >> 11, 5); e[0][1] = expand((c0 >> 5) & 63, 6); e[0][2] = expand(c0 & 31, 5);
    e[1][0] = expand(c1 >> 11, 5); e[1][1] = expand((c1 >> 5) & 63, 6); e[1][2] = expand(c1 & 31, 5);
    c[0] = 0xff000000 | e[0][0] << 16 | e[0][1] << 8 | e[0][2];
    c[1] = 0xff000000 | e[1][0] << 16 | e[1][1] << 8 | e[1][2];
    if (four || c0 > c1) {
        c[2] = c[3] = 0xff000000;
        for (k = 0; k < 3; ++k) {
            c[2] |= ((2 * e[0][k] + e[1][k]) / 3) << (16 - 8 * k);
            c[3] |= ((e[0][k] + 2 * e[1][k]) / 3) << (16 - 8 * k);
        }
    } else {
        c[2] = 0xff000000;
        for (k = 0; k < 3; ++k) c[2] |= ((e[0][k] + e[1][k]) / 2) << (16 - 8 * k);
        c[3] = 0;
    }
}

/* Decode one level of 4x4 blocks into width x height ARGB texels. */
static void decode_level(ULONG format, const BYTE *in, ULONG width, ULONG height, ULONG *out)
{
    ULONG bx, by, x, y, step = format == TexDxt1 ? 8 : 16;
    for (by = 0; by < (height + 3) / 4; ++by)
        for (bx = 0; bx < (width + 3) / 4; ++bx, in += step) {
            const BYTE *colour = format == TexDxt1 ? in : in + 8;
            ULONG c[4], a[8], bits = colour[4] | colour[5] << 8 | colour[6] << 16 | (ULONG)colour[7] << 24, k;
            block_colors(colour, format != TexDxt1, c);
            if (format == TexDxt5) {
                a[0] = in[0]; a[1] = in[1];
                if (a[0] > a[1]) for (k = 1; k < 7; ++k) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
                else {
                    for (k = 1; k < 5; ++k) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
                    a[6] = 0; a[7] = 255;
                }
            }
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x) {
                    ULONG i = 4 * y + x, texel = c[(bits >> (2 * i)) & 3];
                    if (bx * 4 + x >= width || by * 4 + y >= height) continue;
                    if (format == TexDxt3) texel = (texel & 0xffffff) | expand((in[i / 2] >> (4 * (i & 1))) & 15, 4) << 24;
                    else if (format == TexDxt5) {
                        ULONG at = 16 + 3 * i, index = ((in[at / 8] | in[at / 8 + 1] << 8) >> (at % 8)) & 7;
                        texel = (texel & 0xffffff) | a[index] << 24;
                    }
                    out[(by * 4 + y) * width + bx * 4 + x] = texel;
                }
        }
}

static BYTE *decodeScratch;
static ULONG decodeScratchBytes;

/* Every level of a compressed texture from the scratch buffer, decoded. */
static BOOL decode_texture(const D7_TEXTURE *t)
{
    ULONG bytes = 0, level, face, slice, plane, planes = plane_count(t);
    const BYTE *in = texelScratch;
    ULONG *out;
    for (plane = 0; plane < planes; ++plane) {
        plane_at(t, plane, &face, &level, &slice);
        bytes += level_side(t->width, level) * level_side(t->height, level) * 4;
    }
    if (bytes > decodeScratchBytes) {
        if (decodeScratch) EngFreeMem(decodeScratch);
        decodeScratch = EngAllocMem(0, bytes, 'x7bR');
        decodeScratchBytes = decodeScratch ? bytes : 0;
        if (!decodeScratch) return FALSE;
    }
    out = (ULONG *)decodeScratch;
    for (plane = 0; plane < planes; ++plane) {
            ULONG w, h;
            plane_at(t, plane, &face, &level, &slice);
            w = level_side(t->width, level);
            h = level_side(t->height, level);
            decode_level(t->format, in, w, h, out);
            in += rb_d7_row_bytes(t->format, w) * rb_d7_rows(t->format, h);
            out += w * h;
        }
    return TRUE;
}

/* Set once the miniport answers that the host cannot sample DXT blocks. */
static BOOL hostBlocksRefused;

/* Least-recently-used eviction. The miniport keeps host textures within a
 * fixed texel budget and slot count and refuses a new one past them. Then
 * the host copy of the texture drawn longest ago is freed, never one the
 * draw being built has chosen; it is uploaded again from its surface when
 * next drawn. Freeing goes through miniport(), which sends the queued draws
 * first, so no queued draw names a freed texture. */
static BOOL evict_one(DISPLAY *d)
{
    D7_TEXTURE *oldest = NULL;
    ULONG i;
    for (i = 0; textures && i < MaxTextures; ++i) {
        D7_TEXTURE *t = &textures[i];
        if (t->display != d || !t->host || t->image || t->rtCube || t->used == useClock) continue;
        if (!oldest || (LONG)(t->used - oldest->used) < 0) oldest = t;
    }
    if (!oldest) return FALSE;
    request(d, IOCTL_RB2K_D3D7_TEXTURE_FREE, &oldest->host, sizeof(oldest->host), NULL, 0);
    oldest->host = 0;
    ++d->d7draw.evictions;
    return TRUE;
}

/* The statuses a refusal for room arrives as, depending on how the video
 * port and GDI translate the miniport's ERROR_NOT_ENOUGH_MEMORY. */
static BOOL no_room(ULONG status)
{
    return status == ERROR_NOT_ENOUGH_MEMORY || status == ERROR_OUTOFMEMORY ||
           status == ERROR_NO_SYSTEM_RESOURCES || status == 0xc000009aUL || status == 0xc0000017UL;
}

/* One texture upload request. A new texture (packet id zero) refused for
 * room is retried after each eviction until it fits or nothing is left. */
static ULONG texture_send(DISPLAY *d, RB2K_D3D7_TEXTURE *packet, ULONG bytes, ULONG *newId)
{
    ULONG status, written, evicted = 0;
    for (;;) {
        status = miniport(d, IOCTL_RB2K_D3D7_TEXTURE, packet, bytes, newId, sizeof(*newId), &written);
        if (!status || packet->id) break;
        d->d7draw.roomStatus = status;
        if (!no_room(status) || evicted == MaxTextures || !evict_one(d)) break;
        ++evicted;
    }
    if (evicted) ++d->d7draw.evictedCreates;
    if (!status && written != sizeof(*newId)) status = ERROR_INVALID_DATA;
    return status;
}

/* A compressed texture's blocks as they are, level by level in whole block
 * rows. FALSE when they could not be sent; hostBlocksRefused tells a host
 * without the format from other failures. */
static BOOL upload_blocks(DISPLAY *d, D7_TEXTURE *t, ULONG targetId)
{
    RB2K_D3D7_TEXTURE *packet = (RB2K_D3D7_TEXTURE *)uploadScratch;
    const BYTE *src = texelScratch;
    ULONG id = t->host, level, face, total = 0, status;
    ULONG format = t->format == TexDxt1 ? RB2K_D7_BLOCKS_DXT1 : t->format == TexDxt3 ? RB2K_D7_BLOCKS_DXT3 :
                   RB2K_D7_BLOCKS_DXT5;
    for (face = 0; face < t->faces; ++face)
    for (level = 0; level < t->levels; ++level) {
        ULONG width = level_side(t->width, level), height = level_side(t->height, level);
        ULONG rowBytes = rb_d7_row_bytes(t->format, width), blockRows = rb_d7_rows(t->format, height);
        ULONG chunk = RB2K_D7_UPLOAD_BYTES / rowBytes, by;
        for (by = 0; by < blockRows; by += chunk) {
            ULONG rows = min(chunk, blockRows - by), newId = 0;
            RtlCopyMemory(packet + 1, src, rows * rowBytes);
            src += rows * rowBytes;
            packet->id = face || level || by ? id : t->host;
            packet->face = face;
            packet->faces = t->faces;
            packet->depth = 1;
            packet->target = targetId;
            packet->width = t->width;
            packet->height = t->height;
            packet->y = by * 4;
            packet->rows = min(rows * 4, height - by * 4);
            packet->levels = t->levels;
            packet->level = level;
            packet->format = format;
            status = texture_send(d, packet, sizeof(*packet) + rows * rowBytes, &newId);
            if (status) { d->d7draw.blockStatus = status; return FALSE; }
            if (!newId) { hostBlocksRefused = TRUE; return FALSE; }   /* the host cannot sample these */
            id = newId;
            total += rows * rowBytes;
        }
    }
    t->host = id;
    ++d->d7draw.uploads;
    ++d->d7draw.blockUploads;
    d->d7draw.uploadBytes += total;
    return TRUE;
}

static BOOL upload_texture(DISPLAY *d, D7_TEXTURE *t, ULONG targetId)
{
    RB2K_D3D7_TEXTURE *packet = (RB2K_D3D7_TEXTURE *)uploadScratch;
    ULONG id = t->host, tries, format = t->format, bpp, hostBpp, total = 0;
    ULONG *out = (ULONG *)(packet + 1);
    const BYTE *texels = texelScratch;
    if (!packet || t->width * 4 > RB2K_D7_UPLOAD_BYTES) return FALSE;
    /* DXT volumes are decoded here: the host blocks path has no slices. */
    if (compressed(format) && t->depth <= 1 && !hostBlocksRefused) {
        /* The host decodes them when it can: a reset releases the texture
         * like any other, so try once more with a new one. */
        if (upload_blocks(d, t, targetId)) return TRUE;
        if (!hostBlocksRefused && t->host) { t->host = 0; if (upload_blocks(d, t, targetId)) return TRUE; }
        if (!hostBlocksRefused) { ++d->d7draw.uploadFailures; return FALSE; }
        t->host = 0;
    }
    if (compressed(format)) {
        /* Decoded here: the host gets the texels as ARGB. */
        if (!decode_texture(t)) return FALSE;
        texels = decodeScratch;
        format = TexArgb8888;
    }
    bpp = rb_d7_texel_bytes(format);
    /* Signed texels go as they are; the rest as ARGB. */
    hostBpp = RB_TEX_SIGNED(format) ? bpp : 4;
    for (tries = 0; tries < 2; ++tries) {
        BOOL ok = TRUE;
        const BYTE *src = texels;
        ULONG level, face, slice, plane, planes = plane_count(t);
        total = 0;
        for (plane = 0; ok && plane < planes; ++plane) {
            ULONG width, height, chunk, y;
            plane_at(t, plane, &face, &level, &slice);
            width = level_side(t->width, level);
            height = level_side(t->height, level);
            chunk = RB2K_D7_UPLOAD_BYTES / (width * hostBpp);
            {
            for (y = 0; ok && y < height; y += chunk) {
                ULONG rows = min(chunk, height - y), x, r, newId = 0;
                if (RB_TEX_SIGNED(format)) {
                    RtlCopyMemory(out, src, rows * width * bpp);
                    src += rows * width * bpp;
                } else {
                    for (r = 0; r < rows; ++r, src += width * bpp)
                        for (x = 0; x < width; ++x)
                            out[r * width + x] = convert(format, bpp == 4 ? ((const ULONG *)src)[x] :
                                                         bpp == 2 ? ((const USHORT *)src)[x] : src[x]);
                }
                packet->id = plane || y ? id : t->host;
                packet->face = t->depth > 1 ? slice : face;
                packet->faces = t->faces;
                packet->depth = t->depth;
                packet->target = targetId;
                packet->width = t->width;
                packet->height = t->height;
                packet->y = y;
                packet->rows = rows;
                packet->levels = t->levels;
                packet->level = level;
                packet->format = format == TexV8U8 ? RB2K_D7_TEXELS_V8U8 : format == TexQ8W8V8U8 ? RB2K_D7_TEXELS_Q8W8V8U8 : 0;
                ok = !texture_send(d, packet, sizeof(*packet) + rows * width * hostBpp, &newId);
                /* Texture id zero: the host cannot sample these signed texels. */
                if (ok && !newId) ok = FALSE;
                if (ok) id = newId;
                total += rows * width * hostBpp;
            }
            }
        }
        if (ok) {
            t->host = id;
            ++d->d7draw.uploads;
            d->d7draw.uploadBytes += total;
            return TRUE;
        }
        /* A transport reset releases every host texture; make a new one. */
        if (!t->host) break;
        t->host = id = 0;
    }
    ++d->d7draw.uploadFailures;
    return FALSE;
}

/* The host texture for this draw, checked once a scene. The miniport
 * attaches a texture to the target it is uploaded for. */
/* A stage the shader samples with no texture bound reads (0,0,0,1): a 1x1 host texture of
 * that colour stands in. It is sent again into the same host texture once a scene epoch,
 * and made anew when a transport reset has released it. */
static ULONG blankHost, blankEpoch;
static ULONG blank_texture(DISPLAY *d, ULONG targetId)
{
    RB2K_D3D7_TEXTURE *packet = (RB2K_D3D7_TEXTURE *)uploadScratch;
    ULONG tries, newId;
    if (blankHost && blankEpoch == textureEpoch) return blankHost;
    if (!packet) return 0;
    for (tries = 0; tries < 2; ++tries) {
        RtlZeroMemory(packet, sizeof(*packet));
        packet->id = blankHost;
        packet->faces = packet->depth = packet->levels = 1;
        packet->target = targetId;
        packet->width = packet->height = packet->rows = 1;
        *(PULONG)(packet + 1) = 0xff000000;
        newId = 0;
        if (!texture_send(d, packet, sizeof(*packet) + sizeof(ULONG), &newId) && newId) {
            blankHost = newId;
            blankEpoch = textureEpoch;
            return blankHost;
        }
        if (!blankHost) break;
        blankHost = 0;
    }
    return 0;
}

/* Why a texture cannot be used for a draw (counted in d7draw.texSkips). */
static void texture_skip(D7_CONTEXT *c, const D7_TEXTURE *t, ULONG targetId)
{
    ULONG why = !t ? 0 : !t->format || t->owner != c->owner ? 1 : t->image && t->image == targetId ? 2 : 3;
    ++c->display->d7draw.texSkips[why];
}

/* The host cube texture of a cube map render target, with the faces drawn since the last call copied
 * into it; zero while one of its faces is the target. */
static ULONG cube_ready(D7_CONTEXT *c, D7_TEXTURE *t, ULONG targetId)
{
    RB2K_D7_CUBE_SYNC s;
    DD_SLOT *faces[6];
    ULONG k, id = 0, written;
    s.mask = 0;
    for (k = 0; k < 6; ++k) {
        faces[k] = target(c, t->cubeImages[k], FALSE);
        if (!faces[k] || faces[k]->id == targetId) return 0;
        if (!t->host || faces[k]->draws != t->cubeDraws[k]) s.mask |= 1UL << k;
        s.images[k] = faces[k]->id;
    }
    if (!s.mask) return t->host;
    s.texture = t->host;
    s.side = t->width;
    if (miniport(c->display, IOCTL_RB2K_D7_CUBE_SYNC, &s, sizeof(s), &id, sizeof(id), &written) ||
        written != sizeof(id) || !id) return 0;
    t->host = id;
    for (k = 0; k < 6; ++k) t->cubeDraws[k] = faces[k]->draws;
    return id;
}

static ULONG texture_ready(D7_CONTEXT *c, D7_TEXTURE *t, ULONG targetId)
{
    ULONG sum;
    t->used = useClock;
    if (t->rtCube) return cube_ready(c, t, targetId);
    /* A render target texture is sampled on the host, but never while it is the target. */
    if (t->image) return t->image == targetId ? 0 : RB2K_TEXTURE_IMAGE | t->image;
    /* The miniport attaches the host texture to each target that draws with it. */
    if (t->host && t->tracked && !t->dirty) { ++c->display->d7draw.checksSkipped; return t->host; }
    if (t->host && t->epoch == textureEpoch) return t->host;
    ++c->display->d7draw.checks;
    /* Cleared before reading, so a write during the read marks it again. */
    t->dirty = FALSE;
    {
        LONGLONG start, end;
        BOOL read;
        start = cycles();
        read = read_texels(t, &sum);
        end = cycles();
        c->display->d7draw.checkTicks += end - start;
        if (!read) { ++c->display->d7draw.checkFailures; return 0; }
    }
    t->epoch = textureEpoch;
    if (t->host && sum == t->sum) return t->host;
    {
        LONGLONG start, end;
        BOOL uploaded;
        start = cycles();
        uploaded = upload_texture(c->display, t, targetId);
        end = cycles();
        c->display->d7draw.uploadTicks += end - start;
        if (!uploaded) return 0;
    }
    t->sum = sum;
    return t->host;
}

/* ---- triangles ---- */

enum { List, Strip, Fan, LineList, LineStrip, Points };
/* The source of one primitive command's vertices. */
typedef struct {
    const BYTE *inline_;   /* immediate vertices in the command snapshot */
    ULONG first, primitives, kind;
    const WORD *indices;   /* relative to first, or NULL */
    const ULONG *indices32;   /* 32-bit indices of a Direct3D 8 or 9 draw, in place of indices */
    ULONG indexStride;     /* WORDs per triangle record for INDEXEDTRIANGLELIST */
    /* A DirectX 8 draw from stream zero: its data and size in the process,
     * the signed byte offset of vertex zero, the stride and vertex format. */
    BOOL stream;
    const BYTE *streamData;
    ULONG streamBytes, streamStride, fvf;
    LONG streamFirst;
} PRIMS;

/* A point draw's size and flags. Direct3D 8 and 9 draw points at
 * D3DRS_POINTSIZE within POINTSIZE_MIN and POINTSIZE_MAX and the reported
 * maximum, as sprites when POINTSPRITEENABLE is set; Direct3D 7 points are
 * one pixel. The sizes are compared as IEEE bits, which order positive
 * numbers as their values; a negative or not-a-number size counts as one.
 * Not done: POINTSCALEENABLE and sizes in fixed-function vertices. */
enum { RsPointSize = 154, RsPointSizeMin, RsPointSpriteEnable, RsPointSizeMax = 166 };
static ULONG point_bits(const D7_CONTEXT *c, ULONG state, ULONG otherwise)
{
    ULONG v = ((c->rsSet[state / 32] >> (state % 32)) & 1) ? c->rs[state] : otherwise;
    return v >= 0x7f800000UL ? otherwise : v;
}

static void point_state(const D7_CONTEXT *c, ULONG per, ULONG *size, ULONG *flags)
{
    ULONG s, lo, hi;
    *size = 0x3f800000UL;
    *flags = 0;
    if (per != 1 || !c->dx8) return;
    s = point_bits(c, RsPointSize, 0x3f800000UL);
    lo = point_bits(c, RsPointSizeMin, 0x3f800000UL);
    hi = min(point_bits(c, RsPointSizeMax, RB2K_POINT_SIZE_MAX), RB2K_POINT_SIZE_MAX);
    if (s < lo) s = lo;
    if (s > hi) s = hi;
    *size = s;
    if (((c->rsSet[RsPointSpriteEnable / 32] >> (RsPointSpriteEnable % 32)) & 1) && c->rs[RsPointSpriteEnable])
        *flags = RB2K_POINT_SPRITE;
}

/* Vertices per primitive: three for triangles, two for lines, one for points. */
static ULONG prim_per(const PRIMS *p) { return p->kind == Points ? 1 : p->kind == LineList || p->kind == LineStrip ? 2 : 3; }

static ULONG prim_vertex(const PRIMS *p, ULONG t, ULONG corner, ULONG *flat)
{
    ULONG n;
    switch (p->kind) {
    case LineList: n = t * 2 + corner; *flat = t * 2; break;
    case LineStrip: n = t + corner; *flat = t; break;
    case Points: n = t; *flat = t; break;
    case Strip:
        /* Odd strip triangles swap their first two vertices to keep the winding. */
        n = t + ((t & 1) && corner < 2 ? 1 - corner : corner);
        *flat = t;
        break;
    case Fan:
        n = corner ? t + corner : 0;
        *flat = t + 1;
        break;
    default:
        n = t * 3 + corner;
        *flat = t * 3;
        if (p->indexStride == 4) { n = t * 4 + corner; *flat = t * 4; }
        break;
    }
    if (p->indices) { *flat = p->indices[*flat]; n = p->indices[n]; }
    else if (p->indices32) { *flat = p->indices32[*flat]; n = p->indices32[n]; }
    return n;
}

static BOOL bad_position(const RB2K_D3D7_VERTEX *v)
{
    return (v->x & 0x7fffffff) >= 0x7f800000 || (v->y & 0x7fffffff) >= 0x7f800000 ||
           (v->z & 0x7fffffff) >= 0x7f800000 || !(v->rhw & 0x7fffffff) || (v->rhw & 0x7fffffff) >= 0x7f800000;
}

/* D3DRS_COLORWRITEENABLE (168) as the channels a draw does not write. Programs draw
 * stencil shadow volumes with it at zero; all channels are written until it is set. */
static ULONG no_write(const D7_CONTEXT *c)
{
    enum { ColorWriteEnable = 168 };
    if (!((c->rsSet[ColorWriteEnable / 32] >> (ColorWriteEnable % 32)) & 1)) return 0;
    return (~c->rs[ColorWriteEnable] & 15) << RB2K_BLEND_NO_WRITE_SHIFT;
}

static void skip(D7_CONTEXT *c, ULONG reason, ULONG primitives)
{
    c->display->d7draw.skipped[reason] += primitives;
    ++c->display->d7draw.skippedCalls;
}

/* A vertex skip at one of the checks, numbered in source order for d7draw. */
static void skip_vertex(D7_CONTEXT *c, ULONG primitives, ULONG site)
{
    RB2K_D7_DRAWSTATS *s = &c->display->d7draw;
    skip(c, RB2K_D7_SKIP_VERTEX, primitives);
    if (site < RB2K_D7_VERTEX_SITES) s->vertexSites[site] += primitives;
    s->vertexFvf = c->fvf;
    s->vertexStride = c->streamStride;
}

/* Diagnostics: count each distinct drawn state, and keep the last refused
 * vertex window with the reason. */
static void record_state(D7_CONTEXT *c, const RB2K_D3D7_DRAW *draw, ULONG fvf, ULONG format, ULONG primitives)
{
    RB2K_D7_DRAWSTATS *s = &c->display->d7draw;
    ULONG i, n = sizeof(s->states) / sizeof(s->states[0]);
    for (i = 0; i < n && s->states[i].calls; ++i) {
        if (s->states[i].colorStage == draw->colorStage && s->states[i].alphaStage == draw->alphaStage &&
            s->states[i].blend == draw->blend && s->states[i].alphaTest == draw->alphaTest &&
            s->states[i].depthState == draw->depthState && s->states[i].cull == draw->cull &&
            s->states[i].shade == c->shade && s->states[i].fvf == fvf && s->states[i].texFormat == format &&
            s->states[i].transform == draw->transform) break;
    }
    if (i == n) { ++s->stateOverflow; return; }
    s->states[i].colorStage = draw->colorStage;
    s->states[i].alphaStage = draw->alphaStage;
    s->states[i].blend = draw->blend;
    s->states[i].alphaTest = draw->alphaTest;
    s->states[i].depthState = draw->depthState;
    s->states[i].cull = draw->cull;
    s->states[i].shade = c->shade;
    s->states[i].fvf = fvf;
    s->states[i].texFormat = format;
    s->states[i].transform = draw->transform;
    ++s->states[i].calls;
    s->states[i].primitives += primitives;
}

static HRESULT refused(DISPLAY *d, ULONG site, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, const PRIMS *p,
                       ULONG low, ULONG high, ULONG stride)
{
    ULONG *r = d->d7draw.lastRange;
    PDD_SURFACE_GLOBAL g = a->lpDDVertex ? a->lpDDVertex->lpGbl : NULL;
    ++d->d7draw.failSites[site];
    r[0] = d->d7.lastCommand; r[1] = p->first; r[2] = low; r[3] = high;
    r[4] = a->dwVertexLength; r[5] = a->dwVertexOffset; r[6] = g ? g->dwLinearSize : 0;
    r[7] = a->dwFlags; r[8] = stride; r[9] = p->primitives;
    return DDERR_INVALIDPARAMS;
}

/* Queue one draw of count vertices; a full queue, or one for another
 * target, goes to the miniport first. */
static HRESULT queue_record(D7_CONTEXT *c, const void *record, ULONG bytes, ULONG kind, ULONG triangles)
{
    DISPLAY *d = c->display;
    ULONG target = *(const ULONG *)record;
    if (!drawQueue) {
        drawQueue = EngAllocMem(0, sizeof(ULONG) + RB2K_D7_DRAWS_BYTES, 'q7bR');
        if (!drawQueue) return DDERR_OUTOFMEMORY;
    }
    if (drawQueueBytes && (drawQueueDisplay != d || *(const ULONG *)drawQueue != target ||
                           drawQueueBytes + sizeof(ULONG) + bytes > sizeof(ULONG) + RB2K_D7_DRAWS_BYTES) &&
        flush_draws()) return DDERR_GENERIC;
    if (!drawQueueBytes) {
        *(ULONG *)drawQueue = target;
        drawQueueBytes = sizeof(ULONG);
        drawQueueDisplay = d;
    }
    *(ULONG *)(drawQueue + drawQueueBytes) = bytes | kind;
    RtlCopyMemory(drawQueue + drawQueueBytes + sizeof(ULONG), record, bytes);
    drawQueueBytes += sizeof(ULONG) + bytes;
    c->queued = TRUE;
    d->d7draw.drawn += triangles;
    ++useClock;
    ++d->d7draw.drawCalls;
    return DD_OK;
}

/* Vertices per primitive of a draw packet's RB2K_PRIM_*. */
#define PACKET_PER(primitive) ((primitive) == RB2K_PRIM_POINTS ? 1u : (primitive) == RB2K_PRIM_LINES ? 2u : 3u)

static HRESULT send_batch(D7_CONTEXT *c, RB2K_D3D7_DRAW *draw, ULONG count)
{
    draw->count = count;
    return queue_record(c, draw, FIELD_OFFSET(RB2K_D3D7_DRAW, vertices) + count * sizeof(draw->vertices[0]), 0,
                        count / PACKET_PER(draw->primitive));
}

static HRESULT send_batch8(D7_CONTEXT *c, RB2K_D3D8_DRAW *draw, ULONG count)
{
    draw->count = count;
    ++c->display->d7draw.psDraws;
    return queue_record(c, draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices) + sizeof(ULONG) *
                        (draw->transform == 2 ? 4 * draw->vertexConstants + count * draw->vertexWords :
                         count * RB2K_D8_VERTEX_WORDS(draw->coordSets)),
                        RB2K_D8_RECORD, count / PACKET_PER(draw->primitive));
}

/* Choose the draw's fog: table fog when a table mode is set, else fog
 * computed per vertex on the T&L device when a vertex mode is set, else the
 * factor in the specular alpha. Table fog uses w when the projection is
 * perspective, as Direct3D does on hardware that offers w-based fog. */
static void fog_draw(D7_CONTEXT *c, RB2K_D3D7_DRAW *draw, BOOL objectSpace)
{
    ULONG kind = RB2K_D7_FOG_ALPHA, mode = 0, flags = 0;
    if (c->fogTable >= D3DFOG_EXP && c->fogTable <= D3DFOG_LINEAR) {
        kind = RB2K_D7_FOG_TABLE;
        mode = c->fogTable;
        if (c->wNear != c->wFar) flags = RB2K_D7_FOG_W;
    } else if (objectSpace && c->fogVertex >= D3DFOG_EXP && c->fogVertex <= D3DFOG_LINEAR) {
        kind = RB2K_D7_FOG_VERTEX;
        mode = c->fogVertex;
        if (c->rangeFog) flags = RB2K_D7_FOG_RANGE;
    }
    draw->fog = kind | mode << RB2K_D7_FOG_MODE_SHIFT | flags;
    draw->fogColor = c->fogColor;
    draw->fogStart = c->fogStart;
    draw->fogEnd = c->fogEnd;
    draw->fogDensity = c->fogDensity;
}

/* Copy the enabled lights and the material for a lit draw. A light of
 * another type, or past the eighth, is left out and its draw counted. A
 * material source falls back to the material when the vertex lacks that
 * colour, as Direct3D does. */
static void light_draw(D7_CONTEXT *c, RB2K_D3D7_DRAW *draw, BOOL hasDiffuse, BOOL hasSpecular, ULONG primitives)
{
    ULONG k, flags = 0;
    draw->lighting = 1;
    for (k = 0; k < MaxLights; ++k) {
        if (!(c->lightOn & (1UL << k))) continue;
        if (c->lights[k][0] < D3DLIGHT_POINT || c->lights[k][0] > D3DLIGHT_DIRECTIONAL ||
            draw->lightCount == RB2K_D7_LIGHTS) {
            c->display->d7draw.ignored[RB2K_D7_IGNORED_LIGHTING] += primitives;
            continue;
        }
        RtlCopyMemory(draw->lights[draw->lightCount++], c->lights[k], sizeof(c->lights[k]));
    }
    for (k = 0; k < 4; ++k) {
        ULONG s = c->colorVertex ? c->sources[k] : D3DMCS_MATERIAL;
        if ((s == D3DMCS_COLOR1 && !hasDiffuse) || (s == D3DMCS_COLOR2 && !hasSpecular) || s > D3DMCS_COLOR2)
            s = D3DMCS_MATERIAL;
        flags |= s << (RB2K_D7_SOURCE_SHIFT + 2 * k);
    }
    if (c->normalize) flags |= RB2K_D7_NORMALIZE;
    if (c->localViewer) flags |= RB2K_D7_LOCAL_VIEWER;
    draw->lightFlags = flags;
    draw->ambient = c->ambient;
    RtlCopyMemory(draw->material, c->material, sizeof(draw->material));
}

/* The scissor test of a draw packet: on with D3DRS_SCISSORTESTENABLE on the Direct3D 9 interface,
 * the rectangle clamped to 0..8192. */
static void scissor_words(const D7_CONTEXT *c, ULONG *on, ULONG *rect)
{
    if (!c->dx9Ddi || !c->rs[174]) return;
    *on = 1;
    rect[0] = (ULONG)min(max(c->scissor.left, 0), 8192);
    rect[1] = (ULONG)min(max(c->scissor.top, 0), 8192);
    rect[2] = (ULONG)min(max(c->scissor.right, 0), 8192);
    rect[3] = (ULONG)min(max(c->scissor.bottom, 0), 8192);
}

static HRESULT shader_triangles(D7_CONTEXT *c, const PRIMS *p);

/* Whether the stages fit the two-stage combiner of the Direct3D 7 draws. */
static BOOL d3d7_stages(const D7_CONTEXT *c)
{
    ULONG color, alpha;
    BOOL texture;
    if (c->stages[0].colorOp != D3DTOP_DISABLE && c->stages[1].colorOp != D3DTOP_DISABLE &&
        c->stages[2].colorOp != D3DTOP_DISABLE) return FALSE;
    return stage_words(c, 0, &color, &alpha, &texture) != StageUnsupported &&
           (c->stages[0].colorOp == D3DTOP_DISABLE || stage_words(c, 1, &color, &alpha, &texture) != StageUnsupported);
}

static HRESULT triangles(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG stride, const PRIMS *p)
{
    ULONG per = prim_per(p);   /* vertices a primitive has: 3, 2 for lines, 1 for points */
    DISPLAY *d = c->display;
    DD_SLOT *s, *z;
    RB2K_D3D7_DRAW *draw = &c->draw;   /* too large for the kernel stack */
    D7_STAGE *st = &c->stages[0], *st1 = &c->stages[1];
    D7_TEXTURE *tex = NULL, *tex1 = NULL;
    ULONG fvf = p->stream ? p->fvf : a->dwVertexType, offset = 16, diffuse = 0, specular = 0, coord = 0, coordFloats = 0, normal = 0;
    ULONG coord1 = 0, coord1Floats = 0;
    ULONG sets, i, t, low = 0xffffffff, high = 0, address = 0, count = 0, flatSource = 0;
    ULONG limit = 0xffffffff;              /* highest index inside the runtime's window */
    ULONG position;
    BOOL texture, needTexture, needTexture1 = FALSE, objectSpace;
    HRESULT hr = render_surfaces(c, &s, &z);
    const BYTE *base = NULL;
    if (FAILED(hr)) return hr;
    if (hr != DD_OK) return DD_OK;      /* left out: see render_surfaces */
    if (!p->primitives) return DD_OK;
    if (c->fill != D3DFILL_SOLID) { skip(c, RB2K_D7_SKIP_FILL, p->primitives); return DD_OK; }
    /* Direct3D 8 stages beyond these two (a third stage, bump mapping) go to the
     * stage program of the shader path. */
    if (p->stream && !c->pixelShader && !d3d7_stages(c)) return shader_triangles(c, p);
    /* Two stages are drawn; a third one counts as the stage-one skip. */
    if (st->colorOp != D3DTOP_DISABLE && st1->colorOp != D3DTOP_DISABLE && c->stages[2].colorOp != D3DTOP_DISABLE) {
        skip(c, RB2K_D7_SKIP_STAGE1, p->primitives);
        return DD_OK;
    }
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D7_DRAW, vertices));
    draw->primitive = per == 3 ? RB2K_PRIM_TRIANGLES : per == 2 ? RB2K_PRIM_LINES : RB2K_PRIM_POINTS;
    point_state(c, per, &draw->pointSize, &draw->pointFlags);
    scissor_words(c, &draw->scissorOn, draw->scissor);
    if (stage_words(c, 0, &draw->colorStage, &draw->alphaStage, &needTexture) == StageUnsupported) {
        skip(c, RB2K_D7_SKIP_COMBINER, p->primitives);
        return DD_OK;
    }
    if (st->colorOp != D3DTOP_DISABLE) {
        ULONG color1, alpha1, state1 = stage_words(c, 1, &color1, &alpha1, &needTexture1);
        if (state1 == StageUnsupported) { skip(c, RB2K_D7_SKIP_COMBINER, p->primitives); return DD_OK; }
        if (state1 == StageOk) { draw->stage1Color = color1; draw->stage1Alpha = alpha1; }
        else needTexture1 = FALSE;
    }
    /* Vertex layout: a transformed position, or an object-space position
     * with any blend weights and normal (the T&L device), then the optional
     * fields in FVF order. */
    position = fvf & D3DFVF_POSITION_MASK;
    objectSpace = position != D3DFVF_XYZRHW;
    if (!position || (fvf & D3DFVF_RESERVED0) || (!objectSpace && (fvf & D3DFVF_NORMAL))) {
        skip_vertex(c, p->primitives, 1);
        return DD_OK;
    }
    /* Generated texture coordinates (site 18) and texture transforms (19). */
    if ((st->coordIndex >> 16) || (draw->stage1Color && (st1->coordIndex >> 16))) {
        skip_vertex(c, p->primitives, 18);
        return DD_OK;
    }
    if ((needTexture && (st->transform & 0xff)) || (draw->stage1Color && needTexture1 && (st1->transform & 0xff))) {
        /* The fixed-function program of the Direct3D 8 path applies texture matrices. */
        if (p->stream && !c->pixelShader) return shader_triangles(c, p);
        d->d7draw.transformFlags = (st->transform & 0xff) | (st1->transform & 0xff) << 8 | fvf >> 16 << 16;
        skip_vertex(c, p->primitives, 19);
        return DD_OK;
    }
    if (objectSpace) offset = 12 + (position >= D3DFVF_XYZB1 ? 4 * ((position - D3DFVF_XYZRHW) / 2) : 0);
    if (fvf & D3DFVF_NORMAL) { normal = offset; offset += 12; }
    if (fvf & D3DFVF_RESERVED1) offset += 4;
    if (fvf & D3DFVF_DIFFUSE) { diffuse = offset; offset += 4; }
    if (fvf & D3DFVF_SPECULAR) { specular = offset; offset += 4; }
    sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (i = 0; i < sets; ++i) {
        static const UCHAR floats[] = { 2, 3, 4, 1 };
        ULONG n = floats[(fvf >> (16 + 2*i)) & 3];
        if (i == (st->coordIndex & 0xffff)) { coord = offset; coordFloats = n; }
        if (i == (st1->coordIndex & 0xffff)) { coord1 = offset; coord1Floats = n; }
        offset += 4 * n;
    }
    if (offset > stride || stride > 256) {
        skip_vertex(c, p->primitives, 2);
        return DD_OK;
    }
    if (needTexture) {
        tex = find_texture(c->local, st->texture);
        if (!tex || !tex->format || tex->owner != c->owner || tex->faces != 1 || tex->depth > 1) {
            texture_skip(c, tex, s->id);
            skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives);
            return DD_OK;
        }
        if (tex->mipmaps && tex->levels == 1 && st->mipFilter != D3DTFP_NONE)
            d->d7draw.ignored[RB2K_D7_IGNORED_MIPMAP] += p->primitives;
        draw->texture = texture_ready(c, tex, s->id);
        if (!draw->texture) { texture_skip(c, tex, s->id); skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives); return DD_OK; }
    }
    if (needTexture1) {
        tex1 = find_texture(c->local, st1->texture);
        if (!tex1 || !tex1->format || tex1->owner != c->owner || tex1->faces != 1 || tex1->depth > 1) {
            skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives);
            return DD_OK;
        }
        if (tex1->mipmaps && tex1->levels == 1 && st1->mipFilter != D3DTFP_NONE)
            d->d7draw.ignored[RB2K_D7_IGNORED_MIPMAP] += p->primitives;
        draw->texture1 = texture_ready(c, tex1, s->id);
        if (!draw->texture1) { skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives); return DD_OK; }
    }
    texture = needTexture;
    if (c->fog) fog_draw(c, draw, objectSpace);
    if (c->colorKey) d->d7draw.ignored[RB2K_D7_IGNORED_COLORKEY] += p->primitives;

    draw->target = s->id;
    draw->depth = z ? z->id : 0;
    if (z && c->zEnable && c->zFunc >= D3DCMP_NEVER && c->zFunc <= D3DCMP_ALWAYS)
        draw->depthState = RB2K_Z_ENABLE | (c->zWrite ? RB2K_Z_WRITE : 0) | (c->zFunc - 1);
    stencil_state(c, z, &draw->stencil, &draw->stencilRef);
    if (c->alphaTest && c->alphaFunc >= D3DCMP_NEVER && c->alphaFunc <= D3DCMP_ALWAYS)
        draw->alphaTest = RB2K_D7_ON | (c->alphaFunc << 8) | (c->alphaRef & 255);
    if (c->blend && c->srcBlend >= D3DBLEND_ZERO && c->srcBlend <= D3DBLEND_BOTHINVSRCALPHA &&
        c->dstBlend >= D3DBLEND_ZERO && c->dstBlend <= D3DBLEND_BOTHINVSRCALPHA)
        draw->blend = RB2K_D7_ON | (c->srcBlend << 8) | c->dstBlend | (s->alpha ? RB2K_BLEND_DEST_ALPHA : 0);
    draw->blend |= no_write(c);
    draw->cull = c->cull >= D3DCULL_NONE && c->cull <= D3DCULL_CCW ? c->cull : D3DCULL_NONE;
    draw->specular = c->specular ? 1 : 0;
    draw->factor = c->factor;
    draw->sampler = (st->addressU >= 1 && st->addressU <= 4 ? st->addressU : 1) |
                   (st->addressV >= 1 && st->addressV <= 4 ? st->addressV : 1) << 4 |
                   (st->magFilter >= 1 && st->magFilter <= 5 ? st->magFilter : 1) << 8 |
                   (st->minFilter >= 1 && st->minFilter <= 3 ? st->minFilter : 1) << 12 |
                   (tex && tex->levels > 1 && st->mipFilter >= D3DTFP_POINT && st->mipFilter <= D3DTFP_LINEAR ?
                    st->mipFilter : D3DTFP_NONE) << 16;
    draw->border = st->border;
    draw->sampler1 = (st1->addressU >= 1 && st1->addressU <= 4 ? st1->addressU : 1) |
                     (st1->addressV >= 1 && st1->addressV <= 4 ? st1->addressV : 1) << 4 |
                     (st1->magFilter >= 1 && st1->magFilter <= 5 ? st1->magFilter : 1) << 8 |
                     (st1->minFilter >= 1 && st1->minFilter <= 3 ? st1->minFilter : 1) << 12 |
                     (tex1 && tex1->levels > 1 && st1->mipFilter >= D3DTFP_POINT && st1->mipFilter <= D3DTFP_LINEAR ?
                      st1->mipFilter : D3DTFP_NONE) << 16;
    draw->border1 = st1->border;
    if (objectSpace) {
        draw->transform = 1;
        if (c->lighting) light_draw(c, draw, diffuse != 0, specular != 0, p->primitives);
        draw->clipMask = c->clipPlanes & 63;   /* user clip planes, in world space */
        if (draw->clipMask) RtlCopyMemory(draw->clipPlanes, c->planes, sizeof(draw->clipPlanes));
        RtlCopyMemory(draw->matrices, c->matrices, sizeof(draw->matrices));
        RtlCopyMemory(draw->viewport, c->viewport, sizeof(c->viewport));
        draw->viewport[4] = c->zRange[0];
        draw->viewport[5] = c->zRange[1];
    }

    /* Check the whole vertex window before the first host draw. */
    for (t = 0; t < p->primitives; ++t) {
        for (i = 0; i < per; ++i) {
            ULONG v = prim_vertex(p, t, i, &flatSource);
            if (v < low) low = v;
            if (v > high) high = v;
        }
    }
    record_state(c, draw, fvf, tex ? tex->format : 0, p->primitives);
    if (!p->inline_ && !p->stream) {
        if (p->first < c->minStart) c->minStart = p->first;
        if (p->first + high > c->maxEnd) c->maxEnd = p->first + high;
    }
    if (p->inline_) base = p->inline_;
    else if (p->stream) {
        LONGLONG lo = p->streamFirst + (LONGLONG)low * stride, hi = p->streamFirst + (LONGLONG)high * stride + offset;
        if (lo < 0 || hi > p->streamBytes) {
            ++d->d7draw.beyondCalls;
            d->d7draw.beyondVertices += p->primitives;
            skip_vertex(c, p->primitives, 3);
            return DD_OK;
        }
        base = p->streamData + p->streamFirst;
    } else {
        PDD_SURFACE_GLOBAL g;
        ULONG window = a->dwVertexLength;      /* vertices valid from the offset */
        if (a->dwFlags & D3DNTHALDP2_USERMEMVERTICES) address = (ULONG)a->lpVertices;
        else {
            if (!a->lpDDVertex || !(a->lpDDVertex->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ||
                !(g = a->lpDDVertex->lpGbl)) return refused(d, RB2K_D7_FAIL_VBCAPS, a, p, low, high, stride);
            if (a->dwVertexOffset >= g->dwLinearSize)
                return refused(d, RB2K_D7_FAIL_LINEAR, a, p, low, high, stride);
            address = g->fpVidMem;
            /* Windows 2000's runtime can state more vertices than its buffer holds. */
            window = min(window, (g->dwLinearSize - a->dwVertexOffset) / stride);
        }
        if (!address) return refused(d, RB2K_D7_FAIL_ADDRESS, a, p, low, high, stride);
        /* With software vertex processing and a driver that never swaps
         * buffers, the runtime can index past the vertices it has written
         * (both NT5 runtimes, with Direct3D Quake 0.73). Draw the triangles
         * inside the window and count the rest as skipped. */
        if (p->first >= window || low > window - 1 - p->first) {
            ++d->d7draw.beyondCalls;
            d->d7draw.beyondVertices += p->primitives;
            skip_vertex(c, p->primitives, 4);
            return DD_OK;
        }
        if (high > window - 1 - p->first) {
            ++d->d7draw.beyondCalls;
            high = limit = window - 1 - p->first;
        }
        base = (const BYTE *)address + a->dwVertexOffset + p->first * stride;
    }
    __try {
        if (!p->inline_) EngProbeForRead((PVOID)(base + low * stride), (high - low) * stride + offset, 1);
        for (t = 0; t < p->primitives; ++t) {
            RB2K_D3D7_VERTEX *out = draw->vertices + count;
            ULONG flat;
            BOOL bad = FALSE;
            if (limit != 0xffffffff) {
                ULONG worst = 0;
                for (i = 0; i < per; ++i) worst = max(worst, prim_vertex(p, t, i, &flat));
                if (worst > limit || flat > limit) {
                    ++d->d7draw.beyondVertices;
                    skip_vertex(c, 1, 5);
                    continue;
                }
            }
            for (i = 0; i < per; ++i) {
                const BYTE *v = base + prim_vertex(p, t, i, &flat) * stride;
                RB2K_D3D7_VERTEX *o = out + i;
                RtlCopyMemory(o, v, objectSpace ? 12 : 16);
                if (objectSpace) o->rhw = 0x3f800000;
                if (normal) RtlCopyMemory(&o->nx, v + normal, 12);
                else o->nx = o->ny = o->nz = 0;
                o->diffuse = diffuse ? *(const ULONG *)(v + diffuse) : 0xffffffff;
                o->specular = specular ? *(const ULONG *)(v + specular) : 0;
                o->u = coordFloats ? *(const ULONG *)(v + coord) : 0;
                o->v = coordFloats > 1 ? *(const ULONG *)(v + coord + 4) : 0;
                o->u1 = coord1Floats ? *(const ULONG *)(v + coord1) : 0;
                o->v1 = coord1Floats > 1 ? *(const ULONG *)(v + coord1 + 4) : 0;
                bad |= bad_position(o);
            }
            if (c->shade == D3DSHADE_FLAT) {
                const BYTE *v = base + flat * stride;
                ULONG fd = diffuse ? *(const ULONG *)(v + diffuse) : 0xffffffff;
                ULONG fs = specular ? *(const ULONG *)(v + specular) : 0;
                for (i = 0; i < per; ++i) { out[i].diffuse = fd; out[i].specular = fs; }
            }
            if (bad) { ++d->d7draw.badPositions; skip_vertex(c, 1, 6); continue; }
            count += per;
            if (count == RB2K_D3D7_VERTICES || t + 1 == p->primitives) {
                hr = send_batch(c, draw, count);
                if (FAILED(hr)) break;
                count = 0;
            }
        }
        if (SUCCEEDED(hr) && count) hr = send_batch(c, draw, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) { hr = refused(d, RB2K_D7_FAIL_PROBE, a, p, low, high, stride); }
    return hr;
}

/* ---- Direct3D 8 pixel shaders ---- */

/* The stencil words of a draw (RB2K_STENCIL_ON); none without a stencil
 * buffer, where Direct3D's test always passes. */
static void stencil_state(const D7_CONTEXT *c, const DD_SLOT *z, ULONG *stencil, ULONG *ref)
{
    /* D3DSTENCILOP KEEP..DECR to Gallium's KEEP, ZERO, REPLACE, INCR (clamp),
     * DECR (clamp), INCR_WRAP, DECR_WRAP, INVERT. */
    static const UCHAR ops[9] = { 0, 0, 1, 2, 3, 4, 7, 5, 6 };
    *stencil = *ref = 0;
    if (!c->stencil || !z || !z->stencil || c->stencilFunc < D3DCMP_NEVER || c->stencilFunc > D3DCMP_ALWAYS ||
        c->stencilFail - 1 > 7 || c->stencilZFail - 1 > 7 || c->stencilPass - 1 > 7) return;
    *stencil = RB2K_STENCIL_ON | (c->stencilFunc - 1) | ops[c->stencilFail] << 4 | ops[c->stencilZFail] << 8 |
               ops[c->stencilPass] << 12;
    *ref = (c->stencilRef & 255) | (c->stencilMask & 255) << 8 | (c->stencilWriteMask & 255) << 16;
}

static D8_SHADER *find_shader(D7_CONTEXT *c, ULONG handle)
{
    ULONG i;
    if (!handle || !c->shaders) return NULL;
    if (c->shaderLast < c->shaderCount && c->shaders[c->shaderLast].handle == handle) return &c->shaders[c->shaderLast];
    for (i = 0; i < c->shaderCount; ++i)
        if (c->shaders[i].handle == handle) { c->shaderLast = i; return &c->shaders[i]; }
    return NULL;
}

/* A free record of the context's pixel shader table, which is made with the first shader;
 * NULL when the table is full. */
static D8_SHADER *new_shader(D7_CONTEXT *c)
{
    ULONG i;
    if (!c->shaders) c->shaders = EngAllocMem(FL_ZERO_MEMORY, MaxShaders * sizeof(*c->shaders), 's8bR');
    if (!c->shaders) return NULL;
    for (i = 0; i < c->shaderCount && c->shaders[i].handle; ++i) {}
    if (i == MaxShaders) return NULL;
    if (i == c->shaderCount) ++c->shaderCount;
    return &c->shaders[i];
}

static void release_shader(D7_CONTEXT *c, D8_SHADER *sh)
{
    ULONG k;
    for (k = 0; k < ShaderVariants; ++k)
        if (sh->variants[k].program)
            request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &sh->variants[k].program, sizeof(ULONG), NULL, 0);
    if (sh->tokens) EngFreeMem(sh->tokens);
    RtlZeroMemory(sh, sizeof(*sh));
}

/* Translate the shader with a fog setting and give it to the miniport;
 * zero when it cannot be translated or kept. */
static ULONG translate_program(D7_CONTEXT *c, D8_SHADER *sh, ULONG fog, RB_PS_INFO *info)
{
    ULONG n = 0, id = 0, written;
    info->reason = RbPsOverflow;
    info->detail = 0;
    if (programScratch && sh->tokens)
        n = rb_ps_translate((const unsigned int *)sh->tokens, sh->tokenCount, fog, programScratch->text,
                            sizeof(programScratch->text), info);
    if (!n) return 0;
    programScratch->samplers = info->samplers;
    if (miniport(c->display, IOCTL_RB2K_D3D8_PROGRAM, programScratch, sizeof(ULONG) + n, &id, sizeof(id),
                 &written) || written != sizeof(id)) {
        info->reason = RbPsReasons;
        return 0;
    }
    sh->samplers = info->samplers;
    sh->coords = info->coords;
    return id;
}

/* The program of a shader for one fog setting; a fifth setting replaces the
 * last kept one. */
static ULONG shader_program(D7_CONTEXT *c, D8_SHADER *sh, ULONG fog)
{
    RB_PS_INFO info;
    ULONG k;
    for (k = 0; k < ShaderVariants; ++k)
        if (sh->variants[k].program && sh->variants[k].fog == fog) return sh->variants[k].program;
    for (k = 0; k < ShaderVariants - 1 && sh->variants[k].program; ++k) {}
    if (sh->variants[k].program)
        request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &sh->variants[k].program, sizeof(ULONG), NULL, 0);
    sh->variants[k].fog = fog;
    sh->variants[k].program = translate_program(c, sh, fog, &info);
    if (!sh->variants[k].program) {
        ++c->display->d7draw.psFailed[info.reason < RbPsReasons ? info.reason : RbPsReasons];
        c->display->d7draw.psLastDetail = info.detail;
    }
    return sh->variants[k].program;
}

static void release_vshader(D7_CONTEXT *c, D8_VSHADER *vs)
{
    ULONG k;
    for (k = 0; k < 4; ++k)
        if (vs->program[k]) request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &vs->program[k], sizeof(ULONG), NULL, 0);
    for (k = 0; k < 4; ++k)
        if (vs->hostProgram[k]) request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &vs->hostProgram[k], sizeof(ULONG), NULL, 0);
    if (vs->decl) EngFreeMem(vs->decl);
    RtlZeroMemory(vs, sizeof(*vs));
}

static D8_VSHADER *find_vshader(D7_CONTEXT *c, ULONG handle)
{
    ULONG i;
    if (!handle || !c->vshaders) return NULL;
    if (c->vshaderLast < c->vshaderCount && c->vshaders[c->vshaderLast].handle == handle) return &c->vshaders[c->vshaderLast];
    for (i = 0; i < c->vshaderCount; ++i)
        if (c->vshaders[i].handle == handle) { c->vshaderLast = i; return &c->vshaders[i]; }
    return NULL;
}

/* As new_shader, for the vertex shader table. */
static D8_VSHADER *new_vshader(D7_CONTEXT *c)
{
    ULONG i;
    if (!c->vshaders) c->vshaders = EngAllocMem(FL_ZERO_MEMORY, MaxVertexShaders * sizeof(*c->vshaders), 'V8bR');
    if (!c->vshaders) return NULL;
    for (i = 0; i < c->vshaderCount && c->vshaders[i].handle; ++i) {}
    if (i == MaxVertexShaders) return NULL;
    if (i == c->vshaderCount) ++c->vshaderCount;
    return &c->vshaders[i];
}

static void release_shaders(D7_CONTEXT *c)
{
    ULONG i;
    for (i = 0; i < c->shaderCount; ++i) if (c->shaders[i].handle) release_shader(c, &c->shaders[i]);
    for (i = 0; i < c->vshaderCount; ++i) if (c->vshaders[i].handle) release_vshader(c, &c->vshaders[i]);
    if (c->shaders) EngFreeMem(c->shaders);
    if (c->vshaders) EngFreeMem(c->vshaders);
    c->shaders = NULL;
    c->vshaders = NULL;
    c->shaderCount = c->vshaderCount = 0;
    for (i = 0; i < StagePrograms; ++i)
        if (c->stagePrograms[i].program)
            request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &c->stagePrograms[i].program, sizeof(ULONG), NULL, 0);
    RtlZeroMemory(c->stagePrograms, sizeof(c->stagePrograms));
    release_blocks(c);
}

/* The pixel program for the current texture stages, specular and fog;
 * as gets its samplers and constant count for pixel_states. Zero when the
 * stages cannot be translated. */
/* byStage: for fixed-function draws, whose stage n reads its coordinates from
 * set n of the draw record, with the stages' texture transforms; otherwise
 * after a vertex shader, which outputs the sets the stages name. Direct3D does not transform the
 * coordinates of pretransformed vertices: only their projection (D3DTTFF_PROJECTED) divides, with an
 * identity matrix in the constants (shader_triangles). */
/* A stage that would sample a texture but has none bound: like the combiner of the Direct3D 7
 * draws (stage_words), the stage program ends before it, passing the current colour on. */
static BOOL stage_lacks_texture(const D7_CONTEXT *c, ULONG n)
{
    const D7_STAGE *s = &c->stages[n];
    BOOL ok = TRUE;
    ULONG c1 = stage_argument(s->colorArg1, n, &ok), c2 = stage_argument(s->colorArg2, n, &ok);
    ULONG a1 = stage_argument(s->alphaArg1, n, &ok), a2 = stage_argument(s->alphaArg2, n, &ok);
    return !s->texture && (samples_texture(s->colorOp, c1, c2) ||
                           (s->alphaOp != D3DTOP_DISABLE && samples_texture(s->alphaOp, a1, a2)));
}

static ULONG stage_program(D7_CONTEXT *c, ULONG fog, D8_SHADER *as, BOOL byStage, BOOL pretransformed)
{
    RB_STAGE_STATE st[TextureStages];
    ULONG key[StageKey], i, k, n = 0, id = 0, written, transforms = 0;
    D8_STAGE_PROGRAM *e;
    RB_PS_INFO info;
    RtlZeroMemory(key, sizeof(key));
    for (i = 0; i < TextureStages && c->stages[i].colorOp != D3DTOP_DISABLE && !stage_lacks_texture(c, i); ++i) {
        const D7_STAGE *s = &c->stages[i];
        st[i].colorOp = s->colorOp; st[i].colorArg1 = s->colorArg1; st[i].colorArg2 = s->colorArg2;
        st[i].alphaOp = s->alphaOp; st[i].alphaArg1 = s->alphaArg1; st[i].alphaArg2 = s->alphaArg2;
        st[i].coordIndex = byStage ? i : s->coordIndex;
        st[i].transform = byStage ? s->transform : 0;
        if (pretransformed && !(st[i].transform & RB_TTFF_PROJECTED)) st[i].transform = 0;
        if (st[i].transform & 0xff) transforms = 1;
        if (s->colorOp == D3DTOP_BUMPENVMAP || s->colorOp == D3DTOP_BUMPENVMAPLUMINANCE) transforms = 1;
        RtlCopyMemory(key + 8 * i, &st[i], 8 * sizeof(ULONG));
    }
    key[8 * 8] = c->specular ? 1 : 0;
    key[8 * 8 + 1] = fog;
    RtlZeroMemory(as, sizeof(*as));
    /* With texture matrices the program reads 32 constants (d3d8_ps.h). */
    as->constants = transforms ? RB_PS2_CONSTANTS : 8;
    for (k = 0; k < StagePrograms; ++k) {
        e = &c->stagePrograms[k];
        ULONG w;
        for (w = 0; w < StageKey && e->key[w] == key[w]; ++w) {}
        if (e->program && w == StageKey) {
            as->samplers = e->samplers;
            if (byStage) as->coords = as->samplers;   /* stage n reads set n */
            return e->program;
        }
    }
    if (programScratch)
        n = rb_stage_translate(st, i, c->specular ? 1 : 0, fog, programScratch->text, sizeof(programScratch->text), &info);
    if (n) {
        programScratch->samplers = info.samplers;
        if (miniport(c->display, IOCTL_RB2K_D3D8_PROGRAM, programScratch, sizeof(ULONG) + n, &id, sizeof(id),
                     &written) || written != sizeof(id)) id = 0;
    }
    if (!id) { ++c->display->d7draw.stageFailed; return 0; }
    e = &c->stagePrograms[c->stageNext];
    c->stageNext = (c->stageNext + 1) % StagePrograms;
    if (e->program) request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &e->program, sizeof(ULONG), NULL, 0);
    RtlCopyMemory(e->key, key, sizeof(key));
    e->program = id;
    e->samplers = as->samplers = info.samplers;
    if (byStage) as->coords = as->samplers;
    return id;
}

/* Keep a vertex shader's declaration and code; it is translated at its first draw. */
static void create_vshader(D7_CONTEXT *c, ULONG handle, const BYTE *decl, ULONG declBytes, ULONG codeBytes)
{
    D8_VSHADER *vs = find_vshader(c, handle);
    ULONG i;
    if (vs) release_vshader(c, vs);
    vs = handle && codeBytes && !((declBytes | codeBytes) % 4) ? new_vshader(c) : NULL;
    if (!vs) {
        ++c->display->d7draw.vsFailed[RbPsReasons];
        return;
    }
    vs->decl = EngAllocMem(0, declBytes + codeBytes, 'v8bR');
    if (!vs->decl) return;
    RtlCopyMemory(vs->decl, decl, declBytes + codeBytes);
    vs->handle = handle;
    vs->declCount = declBytes / 4;
    vs->tokens = vs->decl + vs->declCount;
    vs->tokenCount = codeBytes / 4;
    /* The streams named by STREAM tokens (stream 0 when there is none). */
    vs->streams = 0;
    for (i = 0; i < vs->declCount && vs->decl[i] != 0xffffffff; ++i)
        if (vs->decl[i] >> 29 == 1 && (vs->decl[i] & 0x1fffffff) < RB_VS_STREAMS) vs->streams |= 1UL << (vs->decl[i] & 0x1fffffff);
    if (!vs->streams) vs->streams = 1;
}

/* The miniport program of a vertex shader with or without fog in the
 * specular alpha, for streams placed at bases (words, see rb_vs_translate);
 * zero when it cannot be made. A program is made again when the bases change. */
static ULONG vertex_program(D7_CONTEXT *c, D8_VSHADER *vs, ULONG flags, const ULONG *bases)
{
    RB2K_D7_DRAWSTATS *st = &c->display->d7draw;
    ULONG k = (flags & RB_VS_FOG_ALPHA ? 1 : 0) | (flags & RB_VS_CLIP ? 2 : 0), n = 0, id = 0, written;
    RB_PS_INFO info;
    for (n = 0; n < RB_VS_STREAMS && vs->bases[k][n] == bases[n]; ++n) {}
    if (n < RB_VS_STREAMS) {
        if (vs->program[k]) request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &vs->program[k], sizeof(ULONG), NULL, 0);
        vs->program[k] = 0;
        vs->failed[k] = FALSE;
        RtlCopyMemory(vs->bases[k], bases, sizeof(vs->bases[k]));
    }
    n = 0;
    if (vs->program[k] || vs->failed[k]) return vs->program[k];
    info.reason = RbPsOverflow;
    info.detail = 0;
    if (programScratch)
        n = rb_vs_translate((const unsigned int *)vs->tokens, vs->tokenCount, (const unsigned int *)vs->decl,
                            vs->declCount, (const unsigned int *)bases, flags, programScratch->text, sizeof(programScratch->text), &info);
    if (n) {
        programScratch->samplers = 0;
        if (miniport(c->display, IOCTL_RB2K_D3D8_PROGRAM, programScratch, sizeof(ULONG) + n, &id, sizeof(id),
                     &written) || written != sizeof(id)) {
            id = 0;
            info.reason = RbPsReasons;
        }
    }
    if (!id) {
        vs->failed[k] = TRUE;
        ++st->vsFailed[info.reason < RbPsReasons ? info.reason : RbPsReasons];
        st->vsLastDetail = info.detail;
        return 0;
    }
    ++st->vsTranslated;
    vs->constants = info.constants;
    vs->words = info.coords;
    vs->pointSize = info.pointSize != 0;
    vs->program[k] = id;
    return id;
}

/* The miniport program of a vertex shader for draws from host buffers: its inputs are vertex
 * attributes (RB_VS_ATTRIBUTES), so it does not depend on where the streams lie. The elements
 * the inputs read go to the miniport with the text and stay here for the draws' checks. Zero
 * when it cannot be made. */
static ULONG host_program(D7_CONTEXT *c, D8_VSHADER *vs, ULONG flags)
{
    static const UCHAR size[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    RB2K_D7_DRAWSTATS *st = &c->display->d7draw;
    ULONG k = (flags & RB_VS_FOG_ALPHA ? 1 : 0) | (flags & RB_VS_CLIP ? 2 : 0), n = 0, id = 0, written, e;
    RB_PS_INFO info;
    if (vs->hostProgram[k] || vs->hostFailed[k]) return vs->hostProgram[k];
    info.reason = RbPsOverflow;
    info.detail = 0;
    if (vprogramScratch)
        n = rb_vs_translate((const unsigned int *)vs->tokens, vs->tokenCount, (const unsigned int *)vs->decl,
                            vs->declCount, NULL, flags | RB_VS_ATTRIBUTES, vprogramScratch->text,
                            sizeof(vprogramScratch->text), &info);
    if (n && info.elementCount && info.elementCount <= RB2K_D8_ELEMENTS) {
        vprogramScratch->elementCount = info.elementCount;
        RtlZeroMemory(vprogramScratch->elements, sizeof(vprogramScratch->elements));
        RtlCopyMemory(vprogramScratch->elements, info.elements, info.elementCount * sizeof(ULONG));
        if (miniport(c->display, IOCTL_RB2K_D3D8_VPROGRAM, vprogramScratch, FIELD_OFFSET(RB2K_D3D8_VPROGRAM, text) + n,
                     &id, sizeof(id), &written) || written != sizeof(id)) id = 0;
    }
    if (!id) {
        vs->hostFailed[k] = TRUE;
        /* Why, for d7draw (kept across mode changes): the translator's reason (the last index:
         * the miniport refused it, or it has no elements), its detail and what it had found, and
         * the declarations of the first four. */
        ++hostProgramFails.reasons[!n ? (info.reason < RbPsReasons ? info.reason : RbPsReasons) : RbPsReasons + 1];
        hostProgramFails.detail = info.detail;
        hostProgramFails.elements = info.elementCount;
        hostProgramFails.bytes = n;
        if (hostProgramFails.count < 4)
            RtlCopyMemory(hostProgramFails.decl[hostProgramFails.count], vs->decl, min(vs->declCount, 24UL) * sizeof(ULONG));
        ++hostProgramFails.count;
        return 0;
    }
    ++st->vsTranslated;
    vs->constants = info.constants;
    vs->pointSize = info.pointSize != 0;
    vs->elementCount = info.elementCount;
    RtlZeroMemory(vs->elements, sizeof(vs->elements));
    RtlCopyMemory(vs->elements, info.elements, info.elementCount * sizeof(ULONG));
    vs->hostStreams = 0;
    RtlZeroMemory(vs->extents, sizeof(vs->extents));
    for (e = 0; e < info.elementCount; ++e) {
        ULONG stream = RB_VS_ELEMENT_STREAM(info.elements[e]);
        ULONG end = RB_VS_ELEMENT_OFFSET(info.elements[e]) + size[RB_VS_ELEMENT_TYPE(info.elements[e]) & 7];
        vs->hostStreams |= 1UL << stream;
        if (end > vs->extents[stream]) vs->extents[stream] = end;
    }
    vs->hostProgram[k] = id;
    return id;
}
typedef char host_elements_match[RB_VS_ELEMENTS == RB2K_D8_ELEMENTS && RB_VS_STREAMS == RB2K_D8_STREAMS &&
                                 RB2K_D8_ELEMENT_TYPE(RB_VS_ELEMENT(5, 3, 7)) == 5 &&
                                 RB2K_D8_ELEMENT_STREAM(RB_VS_ELEMENT(5, 3, 7)) == 3 &&
                                 RB2K_D8_ELEMENT_OFFSET(RB_VS_ELEMENT(5, 3, 7)) == 7 ? 1 : -1];

/* Translate a pixel shader and give its text to the miniport. A shader that
 * cannot be translated keeps its handle; draws with it are counted and
 * skipped. */
static void create_shader(D7_CONTEXT *c, ULONG handle, const BYTE *code, ULONG bytes)
{
    RB2K_D7_DRAWSTATS *s = &c->display->d7draw;
    D8_SHADER *sh = find_shader(c, handle);
    RB_PS_INFO info;
    if (sh) release_shader(c, sh);
    if (s->psLogCount < 6) {
        ULONG *log = s->psLog[s->psLogCount++];
        log[0] = handle; log[1] = bytes;
        RtlCopyMemory(log + 2, code, min(bytes, 42UL * 4) & ~3UL);
    }
    sh = handle ? new_shader(c) : NULL;
    if (!sh) { ++s->psFailed[RbPsReasons]; return; }
    sh->handle = handle;
    /* The tokens stay for the fog variants made at draw time. */
    sh->tokenCount = bytes / 4;
    sh->constants = sh->tokenCount && *(const ULONG *)code == 0xffff0200 ? 32 : 8;
    sh->tokens = sh->tokenCount ? EngAllocMem(0, sh->tokenCount * sizeof(ULONG), 'k8bR') : NULL;
    if (sh->tokens) RtlCopyMemory(sh->tokens, code, sh->tokenCount * sizeof(ULONG));
    sh->variants[0].program = translate_program(c, sh, 0, &info);
    /* A version 1 program with texbem also reads the stages' bump constants. */
    if (info.constants > sh->constants) sh->constants = info.constants;
    if (sh->variants[0].program) ++s->psCreated;
    else {
        ++s->psFailed[info.reason < RbPsReasons ? info.reason : RbPsReasons];
        s->psLastDetail = info.detail;
    }
}

/* The stages among samplers whose texture is a cube map (volume: a volume). */
static ULONG cube_stages(D7_CONTEXT *c, ULONG samplers, BOOL volume)
{
    ULONG k, mask = 0;
    for (k = 0; k < RB2K_D8_STAGES; ++k)
        if ((samplers >> k) & 1) {
            D7_TEXTURE *tex = find_texture(c->local, c->stages[k].texture);
            if (tex && (volume ? tex->depth > 1 : tex->faces == 6)) mask |= 1UL << k;
        }
    return mask;
}

/* The program variant key of a version 1 shader: fog, cube and volume stages. */
static ULONG stage_kinds(D7_CONTEXT *c, ULONG samplers, ULONG fog)
{
    return fog | cube_stages(c, samplers, FALSE) << RB_PS_CUBE_SHIFT | cube_stages(c, samplers, TRUE) << RB_PS_VOLUME_SHIFT;
}

static ULONG stage_sampler(const D7_STAGE *st, const D7_TEXTURE *tex)
{
    return (st->addressU >= 1 && st->addressU <= 4 ? st->addressU : 1) |
           (st->addressV >= 1 && st->addressV <= 4 ? st->addressV : 1) << 4 |
           (st->magFilter >= 1 && st->magFilter <= 5 ? st->magFilter : 1) << 8 |
           (st->minFilter >= 1 && st->minFilter <= 3 ? st->minFilter : 1) << 12 |
           (tex && tex->levels > 1 && st->mipFilter >= D3DTFP_POINT && st->mipFilter <= D3DTFP_LINEAR ?
            st->mipFilter : D3DTFP_NONE) << 16;
}

/* The pixel shader half of a shader draw: textures, render states, the
 * program and its constants. FALSE when a texture is not ready; the draw is
 * then counted as skipped. */
static BOOL pixel_states(D7_CONTEXT *c, RB2K_D3D8_DRAW *draw, const D8_SHADER *sh, DD_SLOT *s, DD_SLOT *z,
                         ULONG program, ULONG fog, ULONG primitives)
{
    ULONG k;
    scissor_words(c, &draw->scissorOn, draw->scissor);
    for (k = 0; k < RB2K_D8_STAGES; ++k) {
        D7_STAGE *st = &c->stages[k];
        D7_TEXTURE *tex;
        if (!(sh->samplers & (1UL << k))) continue;
        tex = find_texture(c->local, st->texture);
        if (!st->texture) {
            /* No texture on a stage the shader samples: Direct3D reads (0,0,0,1) there. */
            if (!(draw->textures[k] = blank_texture(c->display, s->id))) {
                texture_skip(c, NULL, s->id);
                skip(c, RB2K_D7_SKIP_TEXTURE, primitives);
                return FALSE;
            }
            draw->samplers[k] = stage_sampler(st, NULL);
            draw->borders[k] = st->border;
            ++c->display->d7draw.blankSamples;
            continue;
        }
        if (!tex || !tex->format || tex->owner != c->owner || !(draw->textures[k] = texture_ready(c, tex, s->id))) {
            texture_skip(c, tex, s->id);
            skip(c, RB2K_D7_SKIP_TEXTURE, primitives);
            return FALSE;
        }
        draw->samplers[k] = stage_sampler(st, tex);
        draw->borders[k] = st->border;
    }
    draw->target = s->id;
    draw->depth = z ? z->id : 0;
    if (z && c->zEnable && c->zFunc >= D3DCMP_NEVER && c->zFunc <= D3DCMP_ALWAYS)
        draw->depthState = RB2K_Z_ENABLE | (c->zWrite ? RB2K_Z_WRITE : 0) | (c->zFunc - 1);
    stencil_state(c, z, &draw->stencil, &draw->stencilRef);
    if (c->alphaTest && c->alphaFunc >= D3DCMP_NEVER && c->alphaFunc <= D3DCMP_ALWAYS)
        draw->alphaTest = RB2K_D7_ON | (c->alphaFunc << 8) | (c->alphaRef & 255);
    if (c->blend && c->srcBlend >= D3DBLEND_ZERO && c->srcBlend <= D3DBLEND_BOTHINVSRCALPHA &&
        c->dstBlend >= D3DBLEND_ZERO && c->dstBlend <= D3DBLEND_BOTHINVSRCALPHA)
        draw->blend = RB2K_D7_ON | (c->srcBlend << 8) | c->dstBlend | (s->alpha ? RB2K_BLEND_DEST_ALPHA : 0);
    draw->blend |= no_write(c);
    draw->cull = c->cull >= D3DCULL_NONE && c->cull <= D3DCULL_CCW ? c->cull : D3DCULL_NONE;
    draw->program = program;
    draw->fog = fog;
    draw->fogColor = c->fogColor;
    draw->fogStart = c->fogStart;
    draw->fogEnd = c->fogEnd;
    draw->fogDensity = c->fogDensity;
    draw->constantCount = sh->constants;
    RtlCopyMemory(draw->constants, c->psConst, min(sh->constants, (ULONG)RB_PS2_CONSTANTS) * 16);
    if (sh->constants == RB_PS_BUMP_CONSTANTS) {
        /* Stage n's bump matrix, then its luminance scale and offset (d3d8_ps.h). */
        ULONG n;
        for (n = 0; n < 4; ++n) {
            draw->constants[RB_PS_CONSTANTS + n][0] = c->tss[n][D3DTSS_BUMPENVMAT00];
            draw->constants[RB_PS_CONSTANTS + n][1] = c->tss[n][D3DTSS_BUMPENVMAT01];
            draw->constants[RB_PS_CONSTANTS + n][2] = c->tss[n][D3DTSS_BUMPENVMAT10];
            draw->constants[RB_PS_CONSTANTS + n][3] = c->tss[n][D3DTSS_BUMPENVMAT11];
            draw->constants[RB_PS_CONSTANTS + 4 + n][0] = c->tss[n][D3DTSS_BUMPENVLSCALE];
            draw->constants[RB_PS_CONSTANTS + 4 + n][1] = c->tss[n][D3DTSS_BUMPENVLOFFSET];
            draw->constants[RB_PS_CONSTANTS + 4 + n][2] = draw->constants[RB_PS_CONSTANTS + 4 + n][3] = 0;
        }
    }
    return TRUE;
}

/* A draw with a pixel shader. The program replaces the texture stages; the
 * coordinates of stages 0..7 reach it as four components each, a missing
 * one being 0 or, for the fourth, 1. Only the sets up to the last one the
 * program reads travel with the vertices. Untransformed vertices are lit and
 * fog blends the shader's result as for the fixed-function draws. */
/* A bump stage's matrix and luminance scale and offset for the stage program
 * (d3d8_ps.h), in the place of its texture matrix. */
static void bump_constants(const D7_CONTEXT *c, RB2K_D3D8_DRAW *draw)
{
    ULONG k;
    for (k = 0; k < RB_STAGE_TRANSFORMS && k < TextureStages && c->stages[k].colorOp != D3DTOP_DISABLE; ++k) {
        if (c->stages[k].colorOp != D3DTOP_BUMPENVMAP && c->stages[k].colorOp != D3DTOP_BUMPENVMAPLUMINANCE) continue;
        draw->constants[8 + 4 * k][0] = c->tss[k][D3DTSS_BUMPENVMAT00];
        draw->constants[8 + 4 * k][1] = c->tss[k][D3DTSS_BUMPENVMAT01];
        draw->constants[8 + 4 * k][2] = c->tss[k][D3DTSS_BUMPENVMAT10];
        draw->constants[8 + 4 * k][3] = c->tss[k][D3DTSS_BUMPENVMAT11];
        draw->constants[9 + 4 * k][0] = c->tss[k][D3DTSS_BUMPENVLSCALE];
        draw->constants[9 + 4 * k][1] = c->tss[k][D3DTSS_BUMPENVLOFFSET];
        draw->constants[9 + 4 * k][2] = draw->constants[9 + 4 * k][3] = 0;
    }
}

static HRESULT shader_triangles(D7_CONTEXT *c, const PRIMS *p)
{
    ULONG per = prim_per(p);   /* vertices a primitive has: 3, 2 for lines, 1 for points */
    DISPLAY *d = c->display;
    RB2K_D3D8_DRAW *draw = &c->draw8;
    /* Without a pixel shader: fixed-function stages as a program, which also
     * applies the stages' texture matrices (from triangle_list). */
    D8_SHADER ff, *sh = c->pixelShader ? find_shader(c, c->pixelShader) : NULL;
    BOOL fixed = !c->pixelShader;
    DD_SLOT *s, *z;
    ULONG program = 0, fog = 0;
    ULONG fvf = p->fvf, stride = p->streamStride, offset = 16, diffuse = 0, specular = 0, normal = 0, position, sets, i, t, k;
    ULONG used = 0, words;
    ULONG coordOffset[RB2K_D8_STAGES], coordFloats[RB2K_D8_STAGES];
    ULONG low = 0xffffffff, high = 0, count = 0, flatSource = 0;
    BOOL objectSpace;
    const BYTE *base;
    LONGLONG lo, hi;
    HRESULT hr = render_surfaces(c, &s, &z);
    if (FAILED(hr)) return hr;
    if (hr != DD_OK) return DD_OK;      /* left out: see render_surfaces */
    if (!p->primitives) return DD_OK;
    if (c->fill != D3DFILL_SOLID) { skip(c, RB2K_D7_SKIP_FILL, p->primitives); return DD_OK; }
    position = fvf & D3DFVF_POSITION_MASK;
    objectSpace = position != D3DFVF_XYZRHW;
    /* Fog as for the fixed-function draws (fog_draw). */
    if (c->fog) {
        if (c->fogTable >= D3DFOG_EXP && c->fogTable <= D3DFOG_LINEAR)
            fog = RB2K_D7_FOG_TABLE | c->fogTable << RB2K_D7_FOG_MODE_SHIFT | (c->wNear != c->wFar ? RB2K_D7_FOG_W : 0);
        else if (objectSpace && c->fogVertex >= D3DFOG_EXP && c->fogVertex <= D3DFOG_LINEAR)
            fog = RB2K_D7_FOG_VERTEX | c->fogVertex << RB2K_D7_FOG_MODE_SHIFT | (c->rangeFog ? RB2K_D7_FOG_RANGE : 0);
        else fog = RB2K_D7_FOG_ALPHA;
    }
    if (sh) program = shader_program(c, sh, stage_kinds(c, sh->samplers, fog));
    else if (fixed) {
        program = stage_program(c, fog, &ff, TRUE, !objectSpace);
        if (program && (cube_stages(c, ff.samplers, FALSE) || cube_stages(c, ff.samplers, TRUE))) program = 0;
        sh = &ff;
    }
    if (!program) { skip(c, RB2K_D7_SKIP_COMBINER, p->primitives); return DD_OK; }
    if (!p->stream || !position || (fvf & D3DFVF_RESERVED0) || (!objectSpace && (fvf & D3DFVF_NORMAL))) {
        skip_vertex(c, p->primitives, 7);
        return DD_OK;
    }
    if (objectSpace) offset = 12 + (position >= D3DFVF_XYZB1 ? 4 * ((position - D3DFVF_XYZRHW) / 2) : 0);
    if (fvf & D3DFVF_NORMAL) { normal = offset; offset += 12; }
    if (fvf & D3DFVF_RESERVED1) offset += 4;
    if (fvf & D3DFVF_DIFFUSE) { diffuse = offset; offset += 4; }
    if (fvf & D3DFVF_SPECULAR) { specular = offset; offset += 4; }
    for (k = 0; k < RB2K_D8_STAGES; ++k) coordOffset[k] = coordFloats[k] = 0;
    sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (i = 0; i < sets; ++i) {
        static const UCHAR floats[] = { 2, 3, 4, 1 };
        ULONG n = floats[(fvf >> (16 + 2*i)) & 3];
        for (k = 0; k < RB2K_D8_STAGES; ++k)
            if ((sh->coords & (1UL << k)) && i == (c->stages[k].coordIndex & 0xffff)) {
                coordOffset[k] = offset;
                coordFloats[k] = n;
            }
        offset += 4 * n;
    }
    /* Generated coordinates are not applied yet, nor texture transforms with a
     * pixel shader (the fixed-function program applies them). */
    for (k = 0; k < RB2K_D8_STAGES; ++k)
        if ((sh->coords & (1UL << k)) && ((c->stages[k].coordIndex >> 16) || (!fixed && (c->stages[k].transform & 0xff))))
            offset = 0xffffffff;
    if (offset > stride || stride > 256) { skip_vertex(c, p->primitives, 8); return DD_OK; }
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices));
    draw->primitive = per == 3 ? RB2K_PRIM_TRIANGLES : per == 2 ? RB2K_PRIM_LINES : RB2K_PRIM_POINTS;
    point_state(c, per, &draw->pointSize, &draw->pointFlags);
    for (k = 0; k < RB2K_D8_STAGES; ++k) if (sh->coords & (1UL << k)) used = k + 1;
    draw->coordSets = used;
    words = RB2K_D8_VERTEX_WORDS(used);
    if (!pixel_states(c, draw, sh, s, z, program, fog, p->primitives)) return DD_OK;
    if (fixed) {
        draw->constants[0][0] = c->factor;   /* the texture factor, unpacked by the program */
        /* Stage k's texture matrix as its columns, CONST[8+4k] to [11+4k] (d3d8_ps.h); the identity for
         * pretransformed vertices, whose projection alone applies. */
        for (k = 0; k < RB_STAGE_TRANSFORMS && k < TextureStages; ++k) {
            ULONG col, row;
            if (!(c->stages[k].transform & 0xff)) continue;
            for (col = 0; col < 4; ++col)
                for (row = 0; row < 4; ++row) draw->constants[8 + 4 * k + col][row] = 
                    objectSpace ? c->texMatrices[k][4 * row + col] : col == row ? 0x3f800000 : 0;
        }
        bump_constants(c, draw);
    }
    if (objectSpace) {
        draw->transform = 1;
        RtlCopyMemory(draw->matrices, c->matrices, sizeof(draw->matrices));
        draw->clipMask = c->clipPlanes & 63;   /* user clip planes, in world space */
        if (draw->clipMask) RtlCopyMemory(draw->clipPlanes, c->planes, sizeof(draw->clipPlanes));
        RtlCopyMemory(draw->viewport, c->viewport, sizeof(c->viewport));
        draw->viewport[4] = c->zRange[0];
        draw->viewport[5] = c->zRange[1];
        if (c->lighting) {
            /* The fixed-function batch is free between calls: let light_draw
             * resolve the lights and material sources there, then copy them. */
            RB2K_D3D7_DRAW *lit = &c->draw;
            RtlZeroMemory(lit, FIELD_OFFSET(RB2K_D3D7_DRAW, vertices));
            light_draw(c, lit, diffuse != 0, specular != 0, p->primitives);
            draw->lighting = 1;
            draw->lightCount = lit->lightCount;
            draw->lightFlags = lit->lightFlags;
            draw->ambient = lit->ambient;
            draw->specular = c->specular ? 1 : 0;
            RtlCopyMemory(draw->material, lit->material, sizeof(draw->material));
            RtlCopyMemory(draw->lights, lit->lights, sizeof(draw->lights));
        }
    }
    for (t = 0; t < p->primitives; ++t)
        for (i = 0; i < per; ++i) {
            ULONG v = prim_vertex(p, t, i, &flatSource);
            if (v < low) low = v;
            if (v > high) high = v;
        }
    lo = p->streamFirst + (LONGLONG)low * stride;
    hi = p->streamFirst + (LONGLONG)high * stride + offset;
    if (lo < 0 || hi > p->streamBytes) {
        ++d->d7draw.beyondCalls;
        d->d7draw.beyondVertices += p->primitives;
        skip_vertex(c, p->primitives, 9);
        return DD_OK;
    }
    base = p->streamData + p->streamFirst;
    __try {
        EngProbeForRead((PVOID)(base + low * stride), (high - low) * stride + offset, 1);
        for (t = 0; t < p->primitives; ++t) {
            ULONG *out = draw->vertices + count * words;
            ULONG flat;
            BOOL bad = FALSE;
            for (i = 0; i < per; ++i) {
                const BYTE *v = base + prim_vertex(p, t, i, &flat) * stride;
                RB2K_D3D8_VERTEX *o = (RB2K_D3D8_VERTEX *)(out + i * words);
                ULONG j;
                RtlCopyMemory(o, v, objectSpace ? 12 : 16);
                if (objectSpace) o->rhw = 0x3f800000;
                if (normal) RtlCopyMemory(&o->nx, v + normal, 12);
                else o->nx = o->ny = o->nz = 0;
                o->diffuse = diffuse ? *(const ULONG *)(v + diffuse) : 0xffffffff;
                o->specular = specular ? *(const ULONG *)(v + specular) : 0;
                for (k = 0; k < used; ++k)
                    for (j = 0; j < 4; ++j)
                        o->tc[k][j] = j < coordFloats[k] ? *(const ULONG *)(v + coordOffset[k] + 4 * j) :
                                      /* Direct3D transforms (u, v, 1, 0): a one after the last coordinate. */
                                      (fixed && (c->stages[k].transform & 0xff)) ? (j == coordFloats[k] ? 0x3f800000 : 0) :
                                      j == 3 ? 0x3f800000 : 0;
                bad |= bad_position((const RB2K_D3D7_VERTEX *)o);
            }
            if (c->shade == D3DSHADE_FLAT) {
                const BYTE *v = base + flat * stride;
                ULONG fd = diffuse ? *(const ULONG *)(v + diffuse) : 0xffffffff;
                ULONG fs = specular ? *(const ULONG *)(v + specular) : 0;
                for (i = 0; i < per; ++i) {
                    RB2K_D3D8_VERTEX *o = (RB2K_D3D8_VERTEX *)(out + i * words);
                    o->diffuse = fd;
                    o->specular = fs;
                }
            }
            if (bad) { ++d->d7draw.badPositions; skip_vertex(c, 1, 10); continue; }
            count += per;
            if (count == RB2K_D8_VERTICES || t + 1 == p->primitives) {
                hr = send_batch8(c, draw, count);
                if (FAILED(hr)) break;
                count = 0;
            }
        }
        if (SUCCEEDED(hr) && count) hr = send_batch8(c, draw, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skip_vertex(c, p->primitives, 11);
        hr = DD_OK;
    }
    return hr;
}

/* A draw with a vertex shader and a pixel shader. The vertices travel as
 * the stream holds them, padded to whole constants after the vertex
 * shader's constants, and the vertex program fetches its inputs itself.
 * Fog comes from oFog (in the specular alpha) or, as table fog, from the
 * pixel depth. Without a pixel shader the texture stages become a pixel
 * program of their own. Flat shading is not applied. */
static HRESULT program_triangles(D7_CONTEXT *c, const PRIMS *p)
{
    ULONG per = prim_per(p);   /* vertices a primitive has: 3, 2 for lines, 1 for points */
    DISPLAY *d = c->display;
    RB2K_D3D8_DRAW *draw = &c->draw8;
    D8_VSHADER *vs = find_vshader(c, c->fvf);
    D8_SHADER *sh = find_shader(c, c->pixelShader), stages;
    DD_SLOT *s, *z;
    ULONG program = 0, vprogram = 0, fog = 0, flags = 0, stride = p->streamStride, words, i, t, count = 0;
    ULONG low = 0xffffffff, high = 0, flat, n, total = 0, mask = vs ? vs->streams : 1;
    ULONG *out;
    /* Each stream the declaration reads: its data from vertex zero on, its
     * size from there, stride, and the word of the record it starts at. */
    const BYTE *from[RB_VS_STREAMS];
    LONGLONG bytes[RB_VS_STREAMS];
    ULONG strides[RB_VS_STREAMS], bases[RB_VS_STREAMS];
    LONGLONG lo, hi;
    HRESULT hr = render_surfaces(c, &s, &z);
    if (FAILED(hr)) return hr;
    if (hr != DD_OK) return DD_OK;      /* left out: see render_surfaces */
    if (!p->primitives) return DD_OK;
    if (c->fill != D3DFILL_SOLID) { skip(c, RB2K_D7_SKIP_FILL, p->primitives); return DD_OK; }
    if (c->fog) {
        if (c->fogTable >= D3DFOG_EXP && c->fogTable <= D3DFOG_LINEAR)
            fog = RB2K_D7_FOG_TABLE | c->fogTable << RB2K_D7_FOG_MODE_SHIFT | (c->wNear != c->wFar ? RB2K_D7_FOG_W : 0);
        else { fog = RB2K_D7_FOG_ALPHA; flags = RB_VS_FOG_ALPHA; }
    }
    /* Stream 0 is the draw's (a buffer, or the program's memory for
     * DrawPrimitiveUP); the others are buffers, read from the same vertex on. */
    RtlZeroMemory(bases, sizeof(bases));
    for (n = 0; n < RB_VS_STREAMS; ++n) {
        if (!(mask & (1UL << n))) continue;
        if (!n) {
            from[0] = p->streamData + p->streamFirst;
            bytes[0] = (LONGLONG)p->streamBytes - p->streamFirst;
            strides[0] = stride;
        } else {
            D8_BUFFER *b = c->streamUser ? NULL : find_buffer(c->local, c->streamHandles[n]);
            LONGLONG first = stride ? (LONGLONG)(p->streamFirst / (LONG)stride) * c->streamStrides[n] : 0;
            first += c->streamOffsets[n];
            if (!b || b->owner != c->owner || !b->global->fpVidMem || first < 0) {
                skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
                return DD_OK;
            }
            from[n] = (const BYTE *)b->global->fpVidMem + first;
            bytes[n] = (LONGLONG)b->global->dwLinearSize - first;
            strides[n] = c->streamStrides[n];
        }
        if (!strides[n] || strides[n] % 4 || strides[n] > 256) { skip(c, RB2K_D7_SKIP_VERTEX, p->primitives); return DD_OK; }
        bases[n] = total / 4;
        total += strides[n];
    }
    /* Through the Direct3D 9 interface the integer and boolean constants come too (flowConstants). */
    if (vs) vprogram = vertex_program(c, vs, flags | (c->dx9Ddi ? RB_VS_FLOW : 0) | (c->clipPlanes & 63 ? RB_VS_CLIP : 0), bases);
    if (sh) program = shader_program(c, sh, stage_kinds(c, sh->samplers, fog));
    else {
        /* The fixed-function pixel stages after the vertex shader (which do
         * not sample cube maps). */
        program = stage_program(c, fog, &stages, FALSE, FALSE);
        if (program && (cube_stages(c, stages.samplers, FALSE) || cube_stages(c, stages.samplers, TRUE))) program = 0;
        sh = &stages;
        d->d7draw.vsStageDraws += p->primitives;
    }
    words = (total + 15) / 16 * 4;
    if (!vprogram || !program || !p->stream || words > RB2K_D8_VS_VERTEX_WORDS || vs->words > total / 4) {
        if (vprogram && program) skip_vertex(c, p->primitives, 17);
        else skip(c, RB2K_D7_SKIP_COMBINER, p->primitives);
        return DD_OK;
    }
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices));
    draw->primitive = per == 3 ? RB2K_PRIM_TRIANGLES : per == 2 ? RB2K_PRIM_LINES : RB2K_PRIM_POINTS;
    point_state(c, per, &draw->pointSize, &draw->pointFlags);
    if (per == 1 && vs->pointSize) draw->pointFlags |= RB2K_POINT_PER_VERTEX;
    if (!pixel_states(c, draw, sh, s, z, program, fog, p->primitives)) return DD_OK;
    if (sh == &stages) {
        draw->constants[0][0] = c->factor;   /* the texture factor, unpacked by the program */
        bump_constants(c, draw);
    }
    {
        RB2K_D7_DRAWSTATS *st = &d->d7draw;
        ULONG key[6], k, j;
        key[0] = c->fvf; key[1] = c->pixelShader; key[2] = draw->blend; key[3] = draw->alphaTest;
        key[4] = draw->fog; key[5] = draw->depthState;
        for (k = 0; k < st->comboCount; ++k) {
            for (j = 0; j < 6 && st->combos[k][j] == key[j]; ++j) {}
            if (j == 6) break;
        }
        if (k == st->comboCount && k < 24) { RtlCopyMemory(st->combos[k], key, sizeof(key)); st->combos[k][6] = 0; ++st->comboCount; }
        if (k < 24) ++st->combos[k][6];
    }
    draw->transform = 2;
    draw->vertexProgram = vprogram;
    draw->vertexConstants = vs->constants;
    /* User clip planes, in clip space with a vertex shader (RB_VS_CLIP). */
    draw->clipMask = c->clipPlanes & 63;
    if (draw->clipMask) RtlCopyMemory(draw->clipPlanes, c->planes, sizeof(draw->clipPlanes));
    if (c->dx9Ddi) {   /* i# and b# for static flow control (RB_VS_FLOW) */
        ULONG r;
        for (r = 0; r < 16; ++r) {
            draw->flowConstants[r][0] = rb_float_bits(c->vsInt[r][0]);
            draw->flowConstants[r][1] = rb_float_bits(c->vsInt[r][1]);
            draw->flowConstants[r][2] = rb_float_bits(c->vsInt[r][2]);
            draw->flowConstants[r][3] = (c->vsBool >> r) & 1 ? 0x3f800000 : 0;
        }
    }
    draw->vertexWords = words;
    RtlCopyMemory(draw->viewport, c->viewport, sizeof(c->viewport));
    draw->viewport[4] = c->zRange[0];
    draw->viewport[5] = c->zRange[1];
    RtlCopyMemory(draw->vertices, c->vsConst, vs->constants * 16);
    out = draw->vertices + 4 * vs->constants;
    for (t = 0; t < p->primitives; ++t)
        for (i = 0; i < per; ++i) {
            ULONG v = prim_vertex(p, t, i, &flat);
            if (v < low) low = v;
            if (v > high) high = v;
        }
    for (n = 0; n < RB_VS_STREAMS; ++n) {
        if (!(mask & (1UL << n))) continue;
        lo = (LONGLONG)low * strides[n] + (n ? 0 : p->streamFirst);
        hi = (LONGLONG)high * strides[n] + strides[n];
        if (lo < 0 || hi > bytes[n]) {
            ++d->d7draw.beyondCalls;
            d->d7draw.beyondVertices += p->primitives;
            skip_vertex(c, p->primitives, 12);
            return DD_OK;
        }
    }
    __try {
        for (n = 0; n < RB_VS_STREAMS; ++n)
            if (mask & (1UL << n)) EngProbeForRead((PVOID)(from[n] + low * strides[n]), (high - low + 1) * strides[n], 1);
        for (t = 0; t < p->primitives; ++t) {
            for (i = 0; i < per; ++i) {
                ULONG *o = out + (count + i) * words, v = prim_vertex(p, t, i, &flat);
                for (n = 0; n < RB_VS_STREAMS; ++n)
                    if (mask & (1UL << n)) RtlCopyMemory(o + bases[n], from[n] + v * strides[n], strides[n]);
                RtlZeroMemory((BYTE *)o + total, words * 4 - total);
            }
            count += per;
            if (count == RB2K_D8_VERTICES || t + 1 == p->primitives) {
                d->d7draw.vsDraws += count / per;
                hr = send_batch8(c, draw, count);
                if (FAILED(hr)) break;
                count = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skip_vertex(c, p->primitives, 13);
        hr = DD_OK;
    }
    return hr;
}

/* ---- The Direct3D 9 driver interface ----
 *
 * A DirectX 9 runtime on this interface creates vertex declarations
 * (D3DVERTEXELEMENT9) and shader functions separately and makes one of each
 * current; a declaration handle has bit 0 set, any other handle is a
 * fixed-function vertex format for stream 0. For a draw with a shader, the
 * pair becomes a Direct3D 8 vertex shader of the kind the rest of this file
 * draws: Direct3D 8 declaration tokens, which map each element to the input
 * register the shader's dcl statement gives its usage, and the code without
 * those statements. */
enum { MaxDecls = 256, MaxFuncs = 512, MaxPairs = 512, D9PairHandle = 0x40000001 };
struct D9_DECL { ULONG handle, count; RB_VERTEX_ELEMENT9 *elements; };
struct D9_FUNC { ULONG handle, count; ULONG *tokens; };
struct D9_PAIR { ULONG decl, func, vs; };

static BOOL d9_tables(D7_CONTEXT *c)
{
    if (!c->decls) c->decls = EngAllocMem(FL_ZERO_MEMORY, MaxDecls * sizeof(*c->decls), 'd9bR');
    if (!c->funcs) c->funcs = EngAllocMem(FL_ZERO_MEMORY, MaxFuncs * sizeof(*c->funcs), 'f9bR');
    if (!c->pairs) c->pairs = EngAllocMem(FL_ZERO_MEMORY, MaxPairs * sizeof(*c->pairs), 'p9bR');
    return c->decls && c->funcs && c->pairs;
}

/* Forget the shaders made from a declaration or function handle (0 matches none). */
static void d9_forget(D7_CONTEXT *c, ULONG decl, ULONG func)
{
    ULONG i;
    for (i = 0; c->pairs && i < MaxPairs; ++i) {
        struct D9_PAIR *q = &c->pairs[i];
        if (!q->vs || ((!decl || q->decl != decl) && (!func || q->func != func))) continue;
        {
            D8_VSHADER *vs = find_vshader(c, q->vs);
            if (vs) release_vshader(c, vs);
        }
        RtlZeroMemory(q, sizeof(*q));
    }
}

static struct D9_DECL *d9_decl(D7_CONTEXT *c, ULONG handle)
{
    ULONG i;
    for (i = 0; c->decls && handle && i < MaxDecls; ++i) if (c->decls[i].handle == handle) return &c->decls[i];
    return NULL;
}

static struct D9_FUNC *d9_func(D7_CONTEXT *c, ULONG handle)
{
    ULONG i;
    for (i = 0; c->funcs && handle && i < MaxFuncs; ++i) if (c->funcs[i].handle == handle) return &c->funcs[i];
    return NULL;
}

static void d9_delete_decl(D7_CONTEXT *c, ULONG handle)
{
    struct D9_DECL *e = d9_decl(c, handle);
    d9_forget(c, handle, 0);
    if (!e) return;
    if (e->elements) EngFreeMem(e->elements);
    RtlZeroMemory(e, sizeof(*e));
}

static void d9_delete_func(D7_CONTEXT *c, ULONG handle)
{
    struct D9_FUNC *f = d9_func(c, handle);
    d9_forget(c, 0, handle);
    if (!f) return;
    if (f->tokens) EngFreeMem(f->tokens);
    RtlZeroMemory(f, sizeof(*f));
}

static void d9_create_decl(D7_CONTEXT *c, ULONG handle, const RB_VERTEX_ELEMENT9 *elements, ULONG count)
{
    struct D9_DECL *e;
    ULONG i;
    if (!d9_tables(c) || !handle) return;
    d9_delete_decl(c, handle);
    for (i = 0; i < MaxDecls && c->decls[i].handle; ++i) {}
    if (i == MaxDecls || !count) return;
    e = &c->decls[i];
    e->elements = EngAllocMem(0, count * sizeof(*elements), 'e9bR');
    if (!e->elements) return;
    RtlCopyMemory(e->elements, elements, count * sizeof(*elements));
    e->handle = handle;
    e->count = count;
}

static void d9_create_func(D7_CONTEXT *c, ULONG handle, const ULONG *tokens, ULONG bytes)
{
    struct D9_FUNC *f;
    ULONG i;
    if (!d9_tables(c) || !handle) return;
    d9_delete_func(c, handle);
    for (i = 0; i < MaxFuncs && c->funcs[i].handle; ++i) {}
    if (i == MaxFuncs || bytes < 8 || bytes % 4) return;
    f = &c->funcs[i];
    f->tokens = EngAllocMem(0, bytes, 'c9bR');
    if (!f->tokens) return;
    RtlCopyMemory(f->tokens, tokens, bytes);
    f->handle = handle;
    f->count = bytes / 4;
}

/* The Direct3D 8 declaration and code for a declaration and a shader function, into out:
 * declaration tokens, then the code without its input dcl statements. Returns the words of
 * each, or FALSE when the pair has something Direct3D 8 cannot say. */
static BOOL d9_translate(const struct D9_DECL *dl, const struct D9_FUNC *fn, ULONG *out, ULONG room,
                         ULONG *declWords, ULONG *codeWords)
{
    static const UCHAR dwords[8] = { 1, 2, 3, 4, 1, 1, 1, 2 };   /* FLOAT1..4, D3DCOLOR, UBYTE4, SHORT2, SHORT4 */
    UCHAR reg[14][16];
    ULONG i = 1, n = 0, s, k, start;
    if (fn->count < 2 || (fn->tokens[0] & 0xffff0000) != 0xfffe0000 || (fn->tokens[0] & 0xffff) > 0x0200) return FALSE;
    RtlFillMemory(reg, sizeof(reg), 0xff);
    /* The input dcl statements. They stand before the first instruction, among comments and
     * the def, defi and defb statements: a compiled HLSL shader has its def statements first,
     * and stopping at them left every element of the declaration unread (3DMark05 then drew
     * most of its scenes from vertices at the origin). start: the first instruction. */
    while (i < fn->count) {
        ULONG t = fn->tokens[i], op = t & 0xffff;
        if (op == 0xfffe) { i += 1 + ((t >> 16) & 0x7fff); continue; }                /* comment */
        if (op == 81 || op == 48) { i += 6; continue; }                               /* def, defi: a register and four values */
        if (op == 47) { i += 3; continue; }                                           /* defb: a register and a value */
        if (op != 0x1f || i + 2 >= fn->count) break;                                  /* not a dcl */
        {
            ULONG usage = fn->tokens[i + 1] & 0x1f, index = (fn->tokens[i + 1] >> 16) & 0xf, dst = fn->tokens[i + 2];
            ULONG type = ((dst >> 28) & 7) | ((dst >> 8) & 0x18);
            if (type == 1 && usage < 14) reg[usage][index] = (UCHAR)(dst & 0x7ff);      /* D3DSPR_INPUT */
        }
        i += 3;
    }
    start = min(i, fn->count);
    /* Declaration tokens, stream by stream in offset order, with skips (at most 15 words each)
     * over gaps and over elements the shader does not read. */
    for (s = 0; s < RB_VS_STREAMS; ++s) {
        ULONG idx[64], m = 0, cursor = 0, j;
        for (k = 0; k < dl->count && dl->elements[k].stream != 0xff && m < 64; ++k) {
            if (dl->elements[k].stream != s) continue;
            for (j = m; j > 0 && dl->elements[idx[j - 1]].offset > dl->elements[k].offset; --j) idx[j] = idx[j - 1];
            idx[j] = k;
            ++m;
        }
        if (!m) continue;
        if (n + 1 >= room) return FALSE;
        out[n++] = (1UL << 29) | s;
        for (j = 0; j < m; ++j) {
            const RB_VERTEX_ELEMENT9 *e = &dl->elements[idx[j]];
            ULONG gap, r;
            if (e->type > 7 || e->method || e->offset % 4 || e->offset < cursor) return FALSE;
            for (gap = (e->offset - cursor) / 4; gap; gap -= min(gap, 15)) {
                if (n + 1 >= room) return FALSE;
                out[n++] = (2UL << 29) | (1UL << 28) | min(gap, 15) << 16;
            }
            if (n + 1 >= room) return FALSE;
            r = e->usage < 14 ? reg[e->usage][e->usageIndex & 15] : 0xff;
            out[n++] = r != 0xff ? (2UL << 29) | ((ULONG)e->type << 16) | r : (2UL << 29) | (1UL << 28) | ((ULONG)dwords[e->type] << 16);
            cursor = e->offset + 4 * dwords[e->type];
        }
    }
    if (n + 1 + fn->count > room) return FALSE;
    out[n++] = 0xffffffff;
    *declWords = n;
    out[n++] = fn->tokens[0];
    /* The statements before the first instruction without the dcl ones, then the instructions. */
    for (i = 1; i < start; ) {
        ULONG t = fn->tokens[i], op = t & 0xffff, words = op == 0xfffe ? 1 + ((t >> 16) & 0x7fff) : op == 47 || op == 0x1f ? 3 : 6;
        if (words > start - i) words = start - i;
        if (op != 0x1f) for (k = 0; k < words; ++k) out[n++] = fn->tokens[i + k];
        i += words;
    }
    for (i = start; i < fn->count; ++i) out[n++] = fn->tokens[i];
    *codeWords = n - *declWords;
    return TRUE;
}

/* The Direct3D 8 vertex shader handle for the current declaration and function, made on first
 * use; 0 when they cannot be drawn. */
static ULONG d9_vshader(D7_CONTEXT *c)
{
    struct D9_DECL *dl;
    struct D9_FUNC *fn;
    ULONG i, free = MaxPairs, declWords, codeWords, *buffer;
    /* The pair of the last draw: a pair lives only while its declaration and function do. */
    if (c->pairs && c->declHandle && c->funcHandle && c->pairs[c->pairLast].vs &&
        c->pairs[c->pairLast].decl == c->declHandle && c->pairs[c->pairLast].func == c->funcHandle)
        return c->pairs[c->pairLast].vs;
    dl = d9_decl(c, c->declHandle);
    fn = d9_func(c, c->funcHandle);
    if (!dl || !fn || !d9_tables(c)) return 0;
    for (i = 0; i < MaxPairs; ++i) {
        if (c->pairs[i].vs && c->pairs[i].decl == dl->handle && c->pairs[i].func == fn->handle) {
            c->pairLast = i;
            return c->pairs[i].vs;
        }
        if (!c->pairs[i].vs && free == MaxPairs) free = i;
    }
    if (free == MaxPairs) { d9_forget(c, c->pairs[0].decl, 0); free = 0; }
    buffer = EngAllocMem(0, (64 + fn->count + 2) * sizeof(ULONG), 't9bR');
    if (!buffer) return 0;
    if (d9_translate(dl, fn, buffer, 64 + fn->count + 2, &declWords, &codeWords)) {
        ULONG handle = D9PairHandle + 2 * free;
        create_vshader(c, handle, (const BYTE *)buffer, declWords * 4, codeWords * 4);
        if (find_vshader(c, handle)) {
            c->pairs[free].decl = dl->handle;
            c->pairs[free].func = fn->handle;
            c->pairs[free].vs = handle;
        }
    }
    EngFreeMem(buffer);
    return c->pairs[free].vs;
}

/* A declaration a fixed-function draw can read as a vertex format: stream 0 only, in the order
 * and sizes of the vertex format. 0 when it is not one. */
static ULONG d9_decl_fvf(const struct D9_DECL *dl)
{
    static const ULONG sizeCode[4] = { 3, 0, 1, 2 };   /* FLOAT1..4 as D3DFVF_TEXCOORDSIZE codes */
    ULONG fvf = 0, offset = 0, stage = 0, tex = 0, k;
    for (;;) {
        const RB_VERTEX_ELEMENT9 *e = NULL;
        for (k = 0; k < dl->count && dl->elements[k].stream != 0xff; ++k)
            if (dl->elements[k].offset == offset) e = &dl->elements[k];
        if (!e) break;
        if (e->stream || e->method) return 0;
        if (e->usage == 0 && !e->usageIndex && e->type == 2 && stage < 1) { fvf |= D3DFVF_XYZ; stage = 1; offset += 12; }
        else if (e->usage == 9 && !e->usageIndex && e->type == 3 && stage < 1) { fvf |= D3DFVF_XYZRHW; stage = 1; offset += 16; }
        else if (e->usage == 3 && !e->usageIndex && e->type == 2 && stage == 1 && (fvf & D3DFVF_XYZ)) { fvf |= D3DFVF_NORMAL; stage = 2; offset += 12; }
        else if (e->usage == 10 && e->usageIndex == 0 && e->type == 4 && stage >= 1 && stage < 3) { fvf |= D3DFVF_DIFFUSE; stage = 3; offset += 4; }
        else if (e->usage == 10 && e->usageIndex == 1 && e->type == 4 && stage >= 1 && stage < 4) { fvf |= D3DFVF_SPECULAR; stage = 4; offset += 4; }
        else if (e->usage == 5 && e->usageIndex == tex && e->type <= 3 && stage >= 1 && tex < 8) {
            fvf |= sizeCode[e->type] << (16 + 2 * tex);
            ++tex;
            stage = 5;
            offset += 4 * (e->type + 1);
        } else return 0;
    }
    for (k = 0; k < dl->count && dl->elements[k].stream != 0xff; ++k)
        if (dl->elements[k].offset >= offset || dl->elements[k].stream) return 0;   /* something left over */
    return stage ? fvf | (tex << D3DFVF_TEXCOUNT_SHIFT) : 0;
}

/* Before a draw on the Direct3D 9 interface: the vertex format, or the Direct3D 8 shader made
 * from the current declaration and function, as c->fvf. FALSE when the draw cannot be done. */
static BOOL d9_resolve(D7_CONTEXT *c)
{
    if (c->funcHandle) {
        ULONG vs = d9_vshader(c);
        if (!vs) { ++c->display->d7draw.d9Unresolved; return FALSE; }
        c->fvf = vs;
        return TRUE;
    }
    if (!(c->declHandle & 1)) { c->fvf = c->declHandle; return c->fvf != 0; }
    {
        struct D9_DECL *dl = d9_decl(c, c->declHandle);
        ULONG fvf = dl ? d9_decl_fvf(dl) : 0;
        if (!fvf) { ++c->display->d7draw.d9Unresolved; return FALSE; }
        c->fvf = fvf;
        return TRUE;
    }
}

/* A rectangle covering a whole target (an empty one stands for all of it). */
static BOOL whole_rect(const RECTL *r, const DD_SLOT *s)
{
    return r->right <= r->left || r->bottom <= r->top ||
           (!r->left && !r->top && r->right == (LONG)s->width && r->bottom == (LONG)s->height);
}

/* A rectangle of a target as the miniport takes it: an empty one stands for all of it. FALSE when it
 * does not lie inside the target. */
static BOOL target_rect(const RECTL *r, const DD_SLOT *s, RB2K_RECT *out)
{
    if (r->right <= r->left || r->bottom <= r->top) {
        out->left = out->top = 0; out->right = s->width; out->bottom = s->height;
        return TRUE;
    }
    if (r->left < 0 || r->top < 0 || r->right > (LONG)s->width || r->bottom > (LONG)s->height) return FALSE;
    out->left = r->left; out->top = r->top; out->right = r->right; out->bottom = r->bottom;
    return TRUE;
}

/* BLT and SURFACEBLT (StretchRect): a whole target into another of its size is a host copy; any
 * other pair of rectangles a host blit, linear when the runtime asks (DP2BLT_LINEAR, 2). Mip levels
 * of render targets are not offered, so a level other than 0 is counted in d9Ignored. */
static void d9_blt(D7_CONTEXT *c, const RB_DP2_BLT9 *b)
{
    DD_SLOT *src = target(c, b->source, TRUE), *dst = target(c, b->dest, TRUE);
    ULONG written, failed;
    if (!src || !dst || src == dst || src->depth16 || dst->depth16 || src->locked || dst->locked ||
        b->sourceLevel || b->destLevel) {
        ++c->display->d7draw.d9Ignored[RB2K_D9_IGNORED_BLT];
        return;
    }
    if (src->width == dst->width && src->height == dst->height &&
        whole_rect(&b->sourceRect, src) && whole_rect(&b->destRect, dst)) {
        RB2K_DD_COPY copy;
        copy.source = src->id;
        copy.destination = dst->id;
        failed = miniport(c->display, IOCTL_RB2K_DD_COPY, &copy, sizeof(copy), NULL, 0, &written);
    } else {
        RB2K_DD_STRETCH s;
        s.source = src->id;
        s.destination = dst->id;
        s.linear = (b->flags & 2) != 0;
        failed = !target_rect(&b->sourceRect, src, &s.from) || !target_rect(&b->destRect, dst, &s.to) ||
                 miniport(c->display, IOCTL_RB2K_DD_STRETCH, &s, sizeof(s), NULL, 0, &written);
    }
    if (failed) ++c->display->d7draw.d9Ignored[RB2K_D9_IGNORED_BLT];
    c->queued = TRUE;
}

/* COLORFILL: a whole target filled with a D3DCOLOR, as a clear of it. */
static void d9_fill(D7_CONTEXT *c, const RB_DP2_COLOR_FILL *f)
{
    DD_SLOT *s = target(c, f->surface, TRUE);
    RB2K_DD_FILL fill;
    ULONG written;
    if (!s || s->depth16 || s->locked || !whole_rect(&f->rect, s)) {
        ++c->display->d7draw.d9Ignored[RB2K_D9_IGNORED_FILL];
        return;
    }
    fill.id = s->id;
    fill.color = f->color;
    if (miniport(c->display, IOCTL_RB2K_D3D7_CLEAR, &fill, sizeof(fill), NULL, 0, &written))
        ++c->display->d7draw.d9Ignored[RB2K_D9_IGNORED_FILL];
    c->queued = TRUE;
}

static void d9_release(D7_CONTEXT *c)
{
    ULONG i;
    for (i = 0; c->decls && i < MaxDecls; ++i) if (c->decls[i].elements) EngFreeMem(c->decls[i].elements);
    for (i = 0; c->funcs && i < MaxFuncs; ++i) if (c->funcs[i].tokens) EngFreeMem(c->funcs[i].tokens);
    if (c->decls) EngFreeMem(c->decls);
    if (c->funcs) EngFreeMem(c->funcs);
    if (c->pairs) EngFreeMem(c->pairs);
    c->decls = NULL;
    c->funcs = NULL;
    c->pairs = NULL;
}

/* ---- Direct3D 8 streams ---- */

static void trace8(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG op, ULONG n, const BYTE *p, ULONG avail)
{
    RB2K_D7_DRAWSTATS *s = &c->display->d7draw;
    ULONG *t, k;
    if (s->d8TraceCount >= 64) return;
    t = s->d8Trace[s->d8TraceCount++];
    t[0] = op; t[1] = n; t[2] = a->dwFlags;
    for (k = 0; k < 4; ++k) t[3 + k] = 4 * (k + 1) <= avail ? ((const ULONG *)p)[k] : 0;
    if (op == RbDp2SetStreamSourceUm) {
        t[5] = (ULONG)a->lpVertices;
        t[6] = a->dwVertexLength;
    }
}

static BOOL index_scratch(ULONG bytes)
{
    if (bytes <= indexScratchCount) return TRUE;
    if (indexScratch) EngFreeMem(indexScratch);
    indexScratchCount = 0;
    indexScratch = EngAllocMem(0, bytes, 'i8bR');
    if (!indexScratch) return FALSE;
    indexScratchCount = bytes;
    return TRUE;
}

/* Stream zero: a registered buffer, or for DrawPrimitiveUP the user memory
 * the runtime passes as the command buffer's vertices. */
static BOOL stream_zero(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, PRIMS *p)
{
    if (c->streamUser) {
        if (a->dwFlags & D3DNTHALDP2_USERMEMVERTICES) {
            if (!a->lpVertices) return FALSE;
            p->streamData = (const BYTE *)a->lpVertices + a->dwVertexOffset;
            p->streamBytes = a->dwVertexLength * c->streamStride;
        } else {
            PDD_SURFACE_GLOBAL g = a->lpDDVertex ? a->lpDDVertex->lpGbl : NULL;
            if (!g || !g->fpVidMem || a->dwVertexOffset > g->dwLinearSize) return FALSE;
            p->streamData = (const BYTE *)g->fpVidMem + a->dwVertexOffset;
            p->streamBytes = g->dwLinearSize - a->dwVertexOffset;
        }
    } else {
        D8_BUFFER *b = find_buffer(c->local, c->streamHandle);
        if (!b || b->owner != c->owner || !b->global->fpVidMem) return FALSE;
        if (c->streamOffsets[0] > b->global->dwLinearSize) return FALSE;
        p->streamData = (const BYTE *)b->global->fpVidMem + c->streamOffsets[0];
        p->streamBytes = b->global->dwLinearSize - c->streamOffsets[0];
    }
    return TRUE;
}

/* ---- Draws from vertex and index buffers kept on the host (RippleHostBuffers) ----
 *
 * A draw with a vertex shader whose streams are registered buffers does not carry its vertices:
 * the host has the buffers' bytes, and the draw names host buffers, the first vertex or index
 * and the count. The vertex program reads vertex attributes.
 *
 * The program writes its buffers without telling the driver, so each draw looks at what it
 * reads. A buffer has a copy on the host, its mirror: the draw sums every BufferChunk bytes it
 * reads and uploads the chunks the host does not hold yet. The uploads go into the draw queue
 * before the draw.
 *
 * The host writes uploaded bytes into its buffer at once, not in order with the draws it was
 * sent before (virglrenderer maps the buffer unsynchronized, as Mesa's own guest driver expects
 * of it), so bytes that an earlier draw may still read are never written again. A chunk the
 * host holds that has changed is therefore not uploaded: the buffer counts as rewritten, and
 * for its next draws (rewrittenLeft of them, twice as many each time it happens) the bytes a
 * draw reads go to a stream arena instead: a host buffer that takes each draw's bytes after the
 * last one's and is replaced by a new one when it is full. After those draws the mirror is made
 * anew and tried again. Buffers a program fills once stay mirrored and cost a sum at each draw;
 * buffers it keeps refilling are streamed and not summed.
 *
 * An indexed draw's vertex range comes from its indices, which are scanned (the range the
 * program states is not relied on); a mirrored index buffer keeps its last scan while those
 * chunks do not change. */
enum { BufferChunk = 1024, UploadBytes = 32768, ArenaBytes = 8 << 20, RewrittenFirst = 64, RewrittenMost = 65536 };
enum { SyncFailed, SyncMirror, SyncStream };

/* The buffer record of stream n, or with n = RB_VS_STREAMS of the index buffer. */
static D8_BUFFER *stream_buffer(D7_CONTEXT *c, ULONG n)
{
    ULONG handle = n == RB_VS_STREAMS ? c->indexHandle : n ? c->streamHandles[n] : c->streamHandle;
    D8_BUFFER *b;
    if (c->bufferCacheEpoch != bufferEpoch) {
        RtlZeroMemory(c->bufferCache, sizeof(c->bufferCache));
        c->bufferCacheEpoch = bufferEpoch;
    }
    b = c->bufferCache[n];
    if (!b || b->handle != handle || b->local != c->local) b = c->bufferCache[n] = find_buffer(c->local, handle);
    return b && b->owner == c->owner && b->global->fpVidMem ? b : NULL;
}

/* The buffer's mirror on the host, of the kind the draw reads it as: made the first time, and
 * again when the buffer's size is another, when the transport was reset, or when the program
 * rewrote bytes the mirror holds and the draws streamed since then are over (pending draws may
 * still read the old mirror, so it is not written: a new one takes its place). */
static BOOL host_buffer(D7_CONTEXT *c, D8_BUFFER *b, ULONG kind)
{
    RB2K_D8_BUFFER_CREATE create;
    ULONG bytes = b->global->dwLinearSize, chunks = (bytes + BufferChunk - 1) / BufferChunk, written, id = 0;
    if (b->host && b->hostKind == kind && b->hostBytes == bytes && (!b->stale || b->rewrittenLeft)) return TRUE;
    if (!bytes || bytes > RB2K_D8_BUFFER_BYTES) return FALSE;
    if (b->host) request(c->display, IOCTL_RB2K_D8_BUFFER_FREE, &b->host, sizeof(b->host), NULL, 0);
    b->host = 0;
    if (b->sums && b->hostBytes != bytes) { EngFreeMem(b->sums); b->sums = NULL; }
    if (!b->sums) b->sums = EngAllocMem(0, chunks * 2 * sizeof(ULONG), 'h8bR');
    if (!b->sums) return FALSE;
    RtlZeroMemory(b->sums, chunks * 2 * sizeof(ULONG));
    b->scanCount = 0;
    b->checkedCall = 0;
    b->stale = FALSE;
    create.bytes = bytes;
    create.kind = kind;
    if (miniport(c->display, IOCTL_RB2K_D8_BUFFER_CREATE, &create, sizeof(create), &id, sizeof(id), &written) ||
        written != sizeof(id) || !id) return FALSE;
    b->host = id;
    b->hostKind = kind;
    b->hostBytes = bytes;
    host_log(c->display, RB2K_HOST_LOG_MADE, b->handle, id, bytes, kind, 0, 0, 0);
    ++c->display->d7draw.hostBuffers;
    return TRUE;
}

/* Two sums of a chunk. The first is never zero, so that (0, 0) says the host holds nothing. */
static void chunk_sums(const BYTE *p, ULONG bytes, ULONG *sum)
{
    const ULONG *w = (const ULONG *)p;
    ULONG a = 0x811c9dc5, b = bytes, n = bytes / 4, i;
    for (i = 0; i < n; ++i) {
        a = (a ^ w[i]) * 0x01000193;
        b = (((b ^ w[i]) << 13) | ((b ^ w[i]) >> 19)) * 0x9e3779b1;
    }
    for (i = n * 4; i < bytes; ++i) {
        a = (a ^ p[i]) * 0x01000193;
        b = (((b ^ p[i]) << 13) | ((b ^ p[i]) >> 19)) * 0x9e3779b1;
    }
    sum[0] = a ? a : 1;
    sum[1] = b;
}

/* An upload record into the draw queue for the draw's target: bytes at data (the program's
 * memory; the caller guards the read) for byte at of the miniport's buffer host. */
static BOOL queue_upload(D7_CONTEXT *c, ULONG target, ULONG host, ULONG at, const BYTE *data, ULONG bytes)
{
    DISPLAY *d = c->display;
    RB2K_D8_UPLOAD_HEADER *u;
    ULONG padded = (bytes + 3) & ~3UL, size = sizeof(*u) + padded;
    if (!drawQueue) {
        drawQueue = EngAllocMem(0, sizeof(ULONG) + RB2K_D7_DRAWS_BYTES, 'q7bR');
        if (!drawQueue) return FALSE;
    }
    if (drawQueueBytes && (drawQueueDisplay != d || *(const ULONG *)drawQueue != target ||
                           drawQueueBytes + sizeof(ULONG) + size > sizeof(ULONG) + RB2K_D7_DRAWS_BYTES) &&
        flush_draws()) return FALSE;
    if (!drawQueueBytes) {
        *(ULONG *)drawQueue = target;
        drawQueueBytes = sizeof(ULONG);
        drawQueueDisplay = d;
    }
    *(ULONG *)(drawQueue + drawQueueBytes) = size | RB2K_D8_UPLOAD;
    u = (RB2K_D8_UPLOAD_HEADER *)(drawQueue + drawQueueBytes + sizeof(ULONG));
    u->target = target;
    u->buffer = host;
    u->offset = at;
    u->bytes = bytes;
    *(ULONG *)((BYTE *)(u + 1) + padded - sizeof(ULONG)) = 0;
    RtlCopyMemory(u + 1, data, bytes);
    drawQueueBytes += sizeof(ULONG) + size;
    c->queued = TRUE;
    ++d->d7draw.hostUploads;
    d->d7draw.hostUploadBytes += bytes;
    host_log(d, RB2K_HOST_LOG_UPLOAD, host, at, bytes, target, *(const ULONG *)(u + 1), 0, 0);
    return TRUE;
}

/* The mirror of bytes lo to hi (not included) of a buffer, for a draw to target that reads them.
 * SyncMirror: the mirror holds them; chunks it did not hold were uploaded now (changed is set).
 * SyncStream: the draw's bytes are to be streamed, because the buffer's draws are for now, or
 * because a chunk the mirror holds has changed, which starts that. SyncFailed: the buffer
 * cannot be read or the queue failed; the mirror is then made anew before it is used again. */
static ULONG sync_buffer(D7_CONTEXT *c, ULONG target, D8_BUFFER *b, ULONG lo, ULONG hi, BOOL *changed)
{
    const BYTE *data = (const BYTE *)b->global->fpVidMem;
    ULONG first = lo / BufferChunk, last = (hi - 1) / BufferChunk, k, run = ~0UL, sum[2], result = SyncMirror;
    if (b->rewrittenLeft) {
        --b->rewrittenLeft;
        return SyncStream;
    }
    if (b->checkedCall == dp2Call && first >= b->checkedFirst && last <= b->checkedLast) {
        ++c->display->d7draw.hostSumSkips;      /* summed earlier in this call */
        return SyncMirror;
    }
    __try {
        EngProbeForRead((PVOID)(data + first * BufferChunk), min((last + 1) * BufferChunk, b->hostBytes) - first * BufferChunk, 1);
        /* One step past the last chunk closes a run that reaches it. */
        for (k = first; k <= last + 1 && result == SyncMirror; ++k) {
            BOOL fresh = FALSE;     /* a changed chunk the host does not hold yet */
            if (k <= last && !(b->checkedCall == dp2Call && k >= b->checkedFirst && k <= b->checkedLast)) {
                ULONG at = k * BufferChunk;
                chunk_sums(data + at, min((ULONG)BufferChunk, b->hostBytes - at), sum);
                ++c->display->d7draw.hostSummed;
                if (sum[0] != b->sums[2 * k] || sum[1] != b->sums[2 * k + 1]) {
                    if (b->sums[2 * k] || b->sums[2 * k + 1]) result = SyncStream;   /* held, and rewritten */
                    else {
                        fresh = TRUE;
                        b->sums[2 * k] = sum[0];
                        b->sums[2 * k + 1] = sum[1];
                        if (run == ~0UL) run = k;
                    }
                }
            }
            if (run != ~0UL && (!fresh || (k + 1 - run) * BufferChunk >= UploadBytes)) {
                ULONG from = run * BufferChunk, end = min((fresh ? k + 1 : k) * BufferChunk, b->hostBytes);
                if (!b->host || !queue_upload(c, target, b->host, from, data + from, end - from)) result = SyncFailed;
                run = ~0UL;
                *changed = TRUE;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = SyncFailed;
    }
    if (result != SyncMirror) b->checkedCall = 0;
    else if (b->checkedCall == dp2Call && first <= b->checkedLast + 1 && last + 1 >= b->checkedFirst) {
        if (first < b->checkedFirst) b->checkedFirst = first;
        if (last > b->checkedLast) b->checkedLast = last;
    } else {
        b->checkedCall = dp2Call;
        b->checkedFirst = first;
        b->checkedLast = last;
    }
    if (result == SyncStream) {
        /* Streamed from this draw on, for twice as many draws each time. */
        b->rewrittenSpan = b->rewrittenSpan ? min(b->rewrittenSpan * 2, (ULONG)RewrittenMost) : RewrittenFirst;
        b->rewrittenLeft = b->rewrittenSpan;
        b->stale = TRUE;
        b->scanCount = 0;
        ++c->display->d7draw.hostRewrites;
        host_log(c->display, RB2K_HOST_LOG_REWRITTEN, b->handle, b->host, lo, hi, b->rewrittenSpan, 0, 0);
    } else if (result == SyncFailed) {
        /* Sums may be ahead of what the host got: no chunk of this mirror is trusted again. */
        b->stale = TRUE;
        b->scanCount = 0;
    }
    return result;
}

/* Room for need more bytes in the stream arena of a kind: the arena is made, or replaced by a
 * new one when what is left of it is too little (draws sent before keep reading the old one,
 * which the miniport frees after them). A draw asks for all its bytes of a kind at once, before
 * it puts any there. */
static BOOL arena_room(D7_CONTEXT *c, ULONG kind, ULONG need)
{
    DISPLAY *d = c->display;
    RB2K_D8_BUFFER_CREATE create;
    ULONG written, id = 0;
    if (need > ArenaBytes) return FALSE;
    if (c->arenas[kind].host && need <= ArenaBytes - c->arenas[kind].used) return TRUE;
    if (c->arenas[kind].host) request(d, IOCTL_RB2K_D8_BUFFER_FREE, &c->arenas[kind].host, sizeof(ULONG), NULL, 0);
    c->arenas[kind].host = c->arenas[kind].used = 0;
    create.bytes = ArenaBytes;
    create.kind = kind;
    if (miniport(d, IOCTL_RB2K_D8_BUFFER_CREATE, &create, sizeof(create), &id, sizeof(id), &written) ||
        written != sizeof(id) || !id) return FALSE;
    c->arenas[kind].host = id;
    ++d->d7draw.hostArenas;
    return TRUE;
}

/* bytes at data (the program's memory) into the stream arena of a kind, after what it holds, at
 * a multiple of align. Returns where they are, or ~0 when the memory cannot be read or the
 * queue failed. */
static ULONG arena_put(D7_CONTEXT *c, ULONG target, ULONG kind, const BYTE *data, ULONG bytes, ULONG align)
{
    ULONG at = (c->arenas[kind].used + align - 1) / align * align, done, piece;
    BOOL ok = c->arenas[kind].host && at <= ArenaBytes && bytes <= ArenaBytes - at;
    if (!ok) return ~0UL;
    c->arenas[kind].used = at + bytes;
    __try {
        EngProbeForRead((PVOID)data, bytes, 1);
        for (done = 0; done < bytes && ok; done += piece) {
            piece = min(bytes - done, (ULONG)UploadBytes);
            /* A failed flush on the way drops the arena. */
            ok = c->arenas[kind].host && queue_upload(c, target, c->arenas[kind].host, at + done, data + done, piece);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = FALSE;
    }
    return ok && c->arenas[kind].host ? at : ~0UL;
}

/* bytes from byte from of a rewritten buffer into the context's stream arena, at a multiple of
 * align, or where they already are: bytes streamed earlier in this call of DrawPrimitives2 (the
 * same mesh drawn in another pass) are not sent again. ~0 when it fails (see arena_put). */
static ULONG stream_put(D7_CONTEXT *c, ULONG target, ULONG kind, D8_BUFFER *b, ULONG from, ULONG bytes, ULONG align)
{
    ULONG place;
    if (b->streamedCall == dp2Call && c->arenas[kind].host && b->streamedHost == c->arenas[kind].host &&
        from >= b->streamedFrom && bytes <= b->streamedBytes && from - b->streamedFrom <= b->streamedBytes - bytes &&
        !((from - b->streamedFrom) % align)) {
        ++c->display->d7draw.hostStreamHits;
        return b->streamedPlace + (from - b->streamedFrom);
    }
    place = arena_put(c, target, kind, (const BYTE *)b->global->fpVidMem + from, bytes, align);
    if (place != ~0UL) {
        b->streamedCall = dp2Call;
        b->streamedHost = c->arenas[kind].host;
        b->streamedFrom = from;
        b->streamedBytes = bytes;
        b->streamedPlace = place;
    }
    return place;
}

/* The lowest and highest vertex that count indices of width bytes at byte at of an index buffer
 * reach. keep: the buffer is mirrored, so the last scan serves the next draw of the same range
 * while its chunks stay (fresh: they changed just now). FALSE when the buffer cannot be read. */
static BOOL index_range(D7_CONTEXT *c, D8_BUFFER *b, ULONG at, ULONG count, ULONG width, BOOL fresh, BOOL keep,
                        ULONG *low, ULONG *high)
{
    ULONG lo = 0xffffffff, hi = 0, i;
    if (keep && !fresh && b->scanCount == count && b->scanAt == at && b->scanWidth == width) {
        *low = b->scanLow;
        *high = b->scanHigh;
        ++c->display->d7draw.hostScanHits;
        return TRUE;
    }
    b->scanCount = 0;
    __try {
        const BYTE *from = (const BYTE *)b->global->fpVidMem + at;
        EngProbeForRead((PVOID)from, count * width, 1);
        if (width == 2) {
            const WORD *x = (const WORD *)from;
            for (i = 0; i < count; ++i) { if (x[i] < lo) lo = x[i]; if (x[i] > hi) hi = x[i]; }
        } else {
            const ULONG *x = (const ULONG *)from;
            for (i = 0; i < count; ++i) { if (x[i] < lo) lo = x[i]; if (x[i] > hi) hi = x[i]; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    ++c->display->d7draw.hostScans;
    if (keep) { b->scanAt = at; b->scanCount = count; b->scanWidth = width; b->scanLow = lo; b->scanHigh = hi; }
    *low = lo;
    *high = hi;
    return TRUE;
}

/* One draw with a vertex shader from host buffers. first, count, indexed and indexByte are
 * stream_draw's. TRUE when the draw was taken: drawn, or skipped and counted, with its result in
 * hr. FALSE leaves it to the draws that carry their vertices (reason counted in hostLeft). */
static BOOL buffer_draw(D7_CONTEXT *c, ULONG type, LONGLONG first, ULONG count, BOOL indexed, ULONG indexByte, HRESULT *hr)
{
    DISPLAY *d = c->display;
    RB2K_D7_DRAWSTATS *st = &d->d7draw;
    RB2K_D3D8_DRAW *draw = &c->draw8;
    RB2K_D8_BUFFER_DRAW bd;
    D8_VSHADER *vs = find_vshader(c, c->fvf);
    D8_SHADER *sh = find_shader(c, c->pixelShader), stages;
    D8_BUFFER *vb[RB_VS_STREAMS], *ib = NULL;
    DD_SLOT *s, *z;
    ULONG per = type == RbPointList ? 1 : type == RbLineList || type == RbLineStrip ? 2 : 3;
    ULONG vertices = type == RbTriangleList ? 3 * count : type == RbLineList ? 2 * count : type == RbPointList ? count :
                     type == RbLineStrip ? count + 1 : count + 2;
    ULONG program = 0, vprogram, fog = 0, flags = 0, width = c->indexStride, n, low = 0, high = vertices - 1;
    ULONG stride0 = c->streamStride, strides[RB_VS_STREAMS], how[RB_VS_STREAMS], indexHow = SyncMirror;
    ULONG from[RB_VS_STREAMS], bytes[RB_VS_STREAMS], place[RB_VS_STREAMS], indexPlace = 0, needVertex = 0, needIndex = 0;
    /* offsets: where vertex zero of the draw is in each stream's buffer. */
    LONGLONG offsets[RB_VS_STREAMS], bias, rest, drawBias, lowest;
    BOOL changed = FALSE, rebase = FALSE;
    *hr = DD_OK;
    if (!vs || c->streamUser) { ++st->hostLeft[RB2K_HOST_LEFT_USER]; return FALSE; }
    /* Flat shading takes its colour from a triangle's first vertex; the host's draws are not set up for that. */
    if (c->shade == D3DSHADE_FLAT || c->fill != D3DFILL_SOLID || vertices > 0xffffff ||
        (indexed && width != 2 && width != 4)) { ++st->hostLeft[RB2K_HOST_LEFT_SHADE]; return FALSE; }
    *hr = render_surfaces(c, &s, &z);
    if (FAILED(*hr)) return TRUE;
    if (*hr != DD_OK) { *hr = DD_OK; return TRUE; }      /* left out: see render_surfaces */
    if (c->fog) {
        if (c->fogTable >= D3DFOG_EXP && c->fogTable <= D3DFOG_LINEAR)
            fog = RB2K_D7_FOG_TABLE | c->fogTable << RB2K_D7_FOG_MODE_SHIFT | (c->wNear != c->wFar ? RB2K_D7_FOG_W : 0);
        else { fog = RB2K_D7_FOG_ALPHA; flags = RB_VS_FOG_ALPHA; }
    }
    vprogram = host_program(c, vs, flags | (c->dx9Ddi ? RB_VS_FLOW : 0) | (c->clipPlanes & 63 ? RB_VS_CLIP : 0));
    if (sh) program = shader_program(c, sh, stage_kinds(c, sh->samplers, fog));
    else {
        program = stage_program(c, fog, &stages, FALSE, FALSE);
        if (program && (cube_stages(c, stages.samplers, FALSE) || cube_stages(c, stages.samplers, TRUE))) program = 0;
        sh = &stages;
    }
    if (!vprogram || !program) { ++st->hostLeft[RB2K_HOST_LEFT_PROGRAM]; return FALSE; }
    /* Vertex zero of the draw is bias whole vertices and rest bytes into stream zero; the other
     * streams are read from the same vertex on. A bias that is not negative goes into the
     * streams' offsets; a negative one (indexed draws only) stays with the draw. */
    bias = stride0 ? first / (LONGLONG)stride0 : 0;
    rest = first - bias * stride0;
    if (rest < 0) { --bias; rest += stride0; }
    if (bias < 0 && !indexed) { ++st->hostLeft[RB2K_HOST_LEFT_RANGE]; return FALSE; }
    drawBias = bias < 0 ? bias : 0;
    for (n = 0; n < RB_VS_STREAMS; ++n) {
        vb[n] = NULL;
        if (!(vs->hostStreams & (1UL << n))) continue;
        strides[n] = n ? c->streamStrides[n] : stride0;
        vb[n] = stream_buffer(c, n);
        if (!vb[n] || strides[n] > 255 || strides[n] % 4 || vs->extents[n] > (strides[n] ? strides[n] : 256)) {
            ++st->hostLeft[RB2K_HOST_LEFT_STREAM];
            return FALSE;
        }
        offsets[n] = (LONGLONG)c->streamOffsets[n] + (n ? 0 : rest) + (bias > 0 ? bias * strides[n] : 0);
        if (offsets[n] < 0 || offsets[n] > (LONGLONG)vb[n]->global->dwLinearSize) { ++st->hostLeft[RB2K_HOST_LEFT_RANGE]; return FALSE; }
        if (!host_buffer(c, vb[n], RB2K_D8_BUFFER_VERTEX)) { ++st->hostLeft[RB2K_HOST_LEFT_BUFFER]; return FALSE; }
    }
    if (indexed) {
        ib = stream_buffer(c, RB_VS_STREAMS);
        if (!ib || indexByte % width || indexByte > ib->global->dwLinearSize ||
            vertices > (ib->global->dwLinearSize - indexByte) / width) { ++st->hostLeft[RB2K_HOST_LEFT_INDEX]; return FALSE; }
        if (!host_buffer(c, ib, RB2K_D8_BUFFER_INDEX)) { ++st->hostLeft[RB2K_HOST_LEFT_BUFFER]; return FALSE; }
    }
    /* The record's states, as for the draws that carry their vertices (program_triangles). */
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices));
    draw->primitive = per == 3 ? RB2K_PRIM_TRIANGLES : per == 2 ? RB2K_PRIM_LINES : RB2K_PRIM_POINTS;
    point_state(c, per, &draw->pointSize, &draw->pointFlags);
    if (per == 1 && vs->pointSize) draw->pointFlags |= RB2K_POINT_PER_VERTEX;
    if (!pixel_states(c, draw, sh, s, z, program, fog, count)) return TRUE;
    if (sh == &stages) {
        draw->constants[0][0] = c->factor;
        bump_constants(c, draw);
        st->vsStageDraws += count;
    }
    draw->transform = 3;
    draw->vertexProgram = vprogram;
    draw->vertexConstants = vs->constants;
    draw->clipMask = c->clipPlanes & 63;
    if (draw->clipMask) RtlCopyMemory(draw->clipPlanes, c->planes, sizeof(draw->clipPlanes));
    if (c->dx9Ddi) {
        ULONG r;
        for (r = 0; r < 16; ++r) {
            draw->flowConstants[r][0] = rb_float_bits(c->vsInt[r][0]);
            draw->flowConstants[r][1] = rb_float_bits(c->vsInt[r][1]);
            draw->flowConstants[r][2] = rb_float_bits(c->vsInt[r][2]);
            draw->flowConstants[r][3] = (c->vsBool >> r) & 1 ? 0x3f800000 : 0;
        }
    }
    RtlCopyMemory(draw->viewport, c->viewport, sizeof(c->viewport));
    draw->viewport[4] = c->zRange[0];
    draw->viewport[5] = c->zRange[1];
    RtlCopyMemory(draw->vertices, c->vsConst, vs->constants * 16);
    /* The indices first: they say which vertices the draw reads. */
    if (indexed) {
        indexHow = sync_buffer(c, draw->target, ib, indexByte, indexByte + vertices * width, &changed);
        if (indexHow == SyncFailed ||
            !index_range(c, ib, indexByte, vertices, width, changed, indexHow == SyncMirror, &low, &high)) {
            skip_vertex(c, count, 16);
            return TRUE;
        }
        if (high > 0xffffff || (LONGLONG)low + drawBias < 0) { skip_vertex(c, count, 15); return TRUE; }
        if (indexHow == SyncStream) needIndex = vertices * width + width;
    }
    /* Each stream's bytes from the lowest vertex read to the highest, the last one as far as the
     * program reads it: in the mirror, or to be streamed. */
    lowest = (LONGLONG)low + drawBias;
    for (n = 0; n < RB_VS_STREAMS; ++n) {
        LONGLONG f, t;
        if (!vb[n]) continue;
        f = offsets[n] + lowest * strides[n];
        t = offsets[n] + ((LONGLONG)high + drawBias) * strides[n] + vs->extents[n];
        if (f < 0 || t > (LONGLONG)vb[n]->hostBytes) {
            ++st->beyondCalls;
            st->beyondVertices += count;
            skip_vertex(c, count, 12);
            return TRUE;
        }
        from[n] = (ULONG)f;
        bytes[n] = (ULONG)(t - f);
        how[n] = sync_buffer(c, draw->target, vb[n], from[n], (ULONG)t, &changed);
        if (how[n] == SyncFailed) { skip_vertex(c, count, 13); return TRUE; }
        if (how[n] == SyncStream) needVertex += (bytes[n] + 3) & ~3UL;
    }
    /* Room for all the streamed bytes before any are put: a new arena must not take the place of
     * one this draw already names. */
    if ((needVertex && !arena_room(c, RB2K_D8_BUFFER_VERTEX, needVertex)) ||
        (needIndex && !arena_room(c, RB2K_D8_BUFFER_INDEX, needIndex))) { ++st->hostLeft[RB2K_HOST_LEFT_BUFFER]; return FALSE; }
    if (indexHow == SyncStream) {
        indexPlace = stream_put(c, draw->target, RB2K_D8_BUFFER_INDEX, ib, indexByte, vertices * width, width);
        if (indexPlace == ~0UL) { skip_vertex(c, count, 13); return TRUE; }
    }
    for (n = 0; n < RB_VS_STREAMS; ++n) {
        if (!vb[n] || how[n] != SyncStream) continue;
        place[n] = stream_put(c, draw->target, RB2K_D8_BUFFER_VERTEX, vb[n], from[n], bytes[n], 4);
        if (place[n] == ~0UL) { skip_vertex(c, count, 13); return TRUE; }
        /* Streamed bytes start at the lowest vertex read. When they lie too near the arena's start
         * for vertex zero to be before them, the draw is rebased: its indices count from the
         * lowest vertex, and every stream's offset is that vertex's. */
        if ((LONGLONG)place[n] < lowest * strides[n]) rebase = TRUE;
        ++st->hostStreamed;
    }
    RtlZeroMemory(&bd, sizeof(bd));
    bd.mode = type == RbPointList ? 0 : type == RbLineList ? 1 : type;   /* strips, triangles and fans have Gallium's numbers */
    bd.count = vertices;
    for (n = 0; n < RB_VS_STREAMS; ++n) {
        if (!vb[n]) continue;
        bd.streams |= 1UL << n;
        bd.stream[n].stride = strides[n];
        if (how[n] == SyncStream) {
            bd.stream[n].buffer = c->arenas[RB2K_D8_BUFFER_VERTEX].host;
            bd.stream[n].offset = rebase ? place[n] : place[n] - (ULONG)(lowest * strides[n]);
        } else {
            bd.stream[n].buffer = vb[n]->host;
            bd.stream[n].offset = (ULONG)(offsets[n] + (rebase ? lowest * strides[n] : 0));
        }
        /* A failed flush on the way reset the transport and dropped the host's buffers: the draw is lost with them. */
        if (!bd.stream[n].buffer) { skip_vertex(c, count, 13); return TRUE; }
    }
    if (indexed) {
        /* The host starts at the index buffer's offset and has no first index apart from that (as
         * Mesa's own virgl driver sends it). */
        bd.indexBuffer = indexHow == SyncStream ? c->arenas[RB2K_D8_BUFFER_INDEX].host : ib->host;
        bd.indexOffset = indexHow == SyncStream ? indexPlace : indexByte;
        bd.indexSize = width;
        bd.bias = rebase ? -(LONG)low : (LONG)drawBias;
        bd.minIndex = low;
        bd.maxIndex = high;
        if (!bd.indexBuffer) { skip_vertex(c, count, 13); return TRUE; }
    }
    RtlCopyMemory(draw->vertices + 4 * vs->constants, &bd, sizeof(bd));
    draw->count = vertices;
    st->vsDraws += count;
    ++st->hostDraws;
    ++st->psDraws;
    host_log(d, RB2K_HOST_LOG_DRAW, bd.stream[0].buffer, bd.stream[0].offset, bd.indexBuffer, bd.indexOffset, vertices,
             low | high << 16, (ULONG)bd.bias);
    *hr = queue_record(c, draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices) + sizeof(ULONG) * 4 * vs->constants + sizeof(bd),
                       RB2K_D8_RECORD, count);
    return TRUE;
}

/* One DirectX 8 draw. first is the signed byte offset of vertex zero in
 * stream zero; an indexed draw reads 16-bit indices from indexByte on. */
static HRESULT stream_draw(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG type, LONGLONG first,
                           ULONG count, BOOL indexed, ULONG indexByte)
{
    PRIMS prims;
    RtlZeroMemory(&prims, sizeof(prims));
    prims.primitives = count;
    ++c->display->d7draw.d8StreamDraws;
    if (!count) return DD_OK;
    if (type < RbPointList || type > RbTriangleFan) { skip(c, RB2K_D7_SKIP_PRIMITIVE, count); return DD_OK; }
    prims.kind = type == RbPointList ? Points : type == RbLineList ? LineList : type == RbLineStrip ? LineStrip :
                 type == RbTriangleList ? List : type == RbTriangleStrip ? Strip : Fan;
    if (c->dx9Ddi && !d9_resolve(c)) { skip(c, RB2K_D7_SKIP_VERTEX, count); return DD_OK; }
    /* With a vertex shader and registered buffers the vertices stay on the host. */
    if ((c->fvf & 1) && c->display->hostBuffers && c->display->vertexShaders) {   /* (it counts the ones it leaves) */
        HRESULT hr;
        if (buffer_draw(c, type, first, count, indexed, indexByte, &hr)) return hr;
    }
    if (((c->fvf & 1) && !c->display->vertexShaders) || !c->streamStride || c->streamStride > 256 ||
        !stream_zero(c, a, &prims) ||
        first < -0x7fffffffLL || first > (LONGLONG)prims.streamBytes) {
        skip_vertex(c, count, 14);
        return DD_OK;
    }
    prims.stream = TRUE;
    prims.streamFirst = (LONG)first;
    prims.streamStride = c->streamStride;
    prims.fvf = c->fvf;
    if (indexed) {
        D8_BUFFER *b = find_buffer(c->local, c->indexHandle);
        ULONG indices = type == RbTriangleList ? 3 * count : type == RbLineList ? 2 * count :
                        type == RbPointList ? count : type == RbLineStrip ? count + 1 : count + 2, size;
        ULONG width = c->indexStride;   /* bytes an index: 2, or 4 for 32-bit index buffers */
        if (!b || b->owner != c->owner || (width != 2 && width != 4) || !b->global->fpVidMem || count > 0xffff ||
            indexByte > (size = b->global->dwLinearSize) || indices > (size - indexByte) / width) {
            skip_vertex(c, count, 15);
            return DD_OK;
        }
        if (!index_scratch(indices * width)) return DDERR_OUTOFMEMORY;
        __try {
            const BYTE *from = (const BYTE *)b->global->fpVidMem + indexByte;
            EngProbeForRead((PVOID)from, indices * width, 1);
            RtlCopyMemory(indexScratch, from, indices * width);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            skip_vertex(c, count, 16);
            return DD_OK;
        }
        if (width == 4) prims.indices32 = (const ULONG *)indexScratch;
        else prims.indices = (const WORD *)indexScratch;
    }
    if (c->fvf & 1) return program_triangles(c, &prims);
    if (c->pixelShader) return shader_triangles(c, &prims);
    return triangles(c, a, prims.streamStride, &prims);
}

/* A command that carries count records of size bytes, each followed by a
 * variable part whose length in bytes is the record word at index word. */
static ULONG variable_records(const BYTE *p, ULONG remaining, ULONG count, ULONG size, ULONG word, ULONG word2)
{
    ULONG used = 0, k;
    for (k = 0; k < count; ++k) {
        const ULONG *r = (const ULONG *)(p + used);
        ULONG extra;
        if (remaining - min(remaining, used) < size) return 0xffffffff;
        extra = r[word] + (word2 ? r[word2] : 0);
        if (extra < r[word] || extra > remaining - used - size) return 0xffffffff;
        used += size + extra;
    }
    return used;
}

/* The runtime's texture manager copies a system-memory texture into its
 * video-memory copy. Both live in this process's memory, so copy the top
 * level directly; the next draw compares and uploads it. A null source only
 * asks for the destination to be loaded, which the next draw does anyway. */
/* A box of a volume copied into another volume, at each level both have. */
/* A Direct3D 9 runtime describes a system-memory DXT texture by its bytes (a 4x4 DXT5 as 16x1), so
 * it registers with no format. Its blocks follow each other row by row, so a blit into a texture of
 * the format its FourCC names gives it that texture's size. */
static void adopt_blocks(D7_TEXTURE *src, const D7_TEXTURE *dst)
{
    ULONG face, level;
    if (!src || !dst || src->format || !src->blocks || src->blocks != dst->format || src->faces != dst->faces) return;
    src->format = dst->format;
    src->width = dst->width;
    src->height = dst->height;
    src->pitch = rb_d7_row_bytes(src->format, src->width);
    for (face = 0; face < src->faces; ++face)
        for (level = 0; level < src->levels; ++level)
            src->levelPitch[face][level] = rb_d7_row_bytes(src->format, max(1, src->width >> level));
}

static void volume_blt(D7_CONTEXT *c, const RB_DP2_VOLUME_BLT *b)
{
    DISPLAY *d = c->display;
    D7_TEXTURE *dst = find_texture(c->local, b->dest), *src = find_texture(c->local, b->source);
    ULONG left = b->box[0], top = b->box[1], right = b->box[2], bottom = b->box[3], front = b->box[4], back = b->box[5];
    ULONG bpp, level, levels;
    adopt_blocks(src, dst);
    if (!dst || !src || !dst->format || dst->format != src->format ||
        dst->depth < 2 || src->depth < 2 || dst->owner != c->owner || src->owner != c->owner) {
        ++d->d7draw.texBltFailures;
        return;
    }
    if (right <= left || bottom <= top || back <= front) {
        left = top = front = 0;
        right = src->width; bottom = src->height; back = src->depth;
    }
    if (right > src->width || bottom > src->height || back > src->depth ||
        b->x + right - left > dst->width || b->y + bottom - top > dst->height ||
        b->z + back - front > dst->depth) { ++d->d7draw.texBltFailures; return; }
    bpp = rb_d7_texel_bytes(src->format);
    levels = min(src->levels, dst->levels);
    if (src->width != dst->width || src->height != dst->height || src->depth != dst->depth) levels = 1;
    __try {
        for (level = 0; level < levels; ++level) {
            ULONG x0 = left >> level, y0 = top >> level, z0 = front >> level;
            ULONG x1 = max(x0 + 1, right >> level), y1 = max(y0 + 1, bottom >> level), z1 = max(z0 + 1, back >> level);
            ULONG dx = b->x >> level, dy = b->y >> level, dz = b->z >> level, z, y;
            ULONG rows = rb_d7_rows(src->format, level_side(src->height, level));
            ULONG sp = src->levelPitch[0][level], dp = dst->levelPitch[0][level];
            if (x1 > level_side(src->width, level) || y1 > level_side(src->height, level) || z1 > level_depth(src, level) ||
                dx + x1 - x0 > level_side(dst->width, level) || dy + y1 - y0 > level_side(dst->height, level) ||
                dz + z1 - z0 > level_depth(dst, level)) break;
            if (compressed(src->format)) {
                /* Blocks: whole slices at the same place, as the texture manager copies a volume. */
                ULONG row = rb_d7_row_bytes(src->format, level_side(src->width, level));
                if (x0 || y0 || dx || dy || x1 != level_side(src->width, level) || y1 != level_side(src->height, level))
                    break;
                for (z = 0; z < z1 - z0; ++z) {
                    const BYTE *from = (const BYTE *)src->levelAddress[0][level] + (z0 + z) * sp * rows;
                    BYTE *to = (BYTE *)dst->levelAddress[0][level] + (dz + z) * dp * rows;
                    EngProbeForRead((PVOID)from, (rows - 1) * sp + row, 1);
                    EngProbeForReadAndWrite(to, (rows - 1) * dp + row, 1);
                    for (y = 0; y < rows; ++y) RtlCopyMemory(to + y * dp, from + y * sp, row);
                }
                continue;
            }
            for (z = 0; z < z1 - z0; ++z) {
                const BYTE *from = (const BYTE *)src->levelAddress[0][level] + (z0 + z) * sp * rows + y0 * sp + x0 * bpp;
                BYTE *to = (BYTE *)dst->levelAddress[0][level] + (dz + z) * dp * rows + dy * dp + dx * bpp;
                EngProbeForRead((PVOID)from, (y1 - y0 - 1) * sp + (x1 - x0) * bpp, 1);
                EngProbeForReadAndWrite(to, (y1 - y0 - 1) * dp + (x1 - x0) * bpp, 1);
                for (y = 0; y < y1 - y0; ++y) RtlCopyMemory(to + y * dp, from + y * sp, (x1 - x0) * bpp);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++d->d7draw.texBltFailures; return; }
    dst->epoch = 0;
    dst->dirty = TRUE;
    ++d->d7draw.texBlts;
}

static void texture_blt(D7_CONTEXT *c, const D3DNTHAL_DP2TEXBLT *b)
{
    DISPLAY *d = c->display;
    D7_TEXTURE *dst, *src;
    LONG left = b->rSrc.left, top = b->rSrc.top, right = b->rSrc.right, bottom = b->rSrc.bottom;
    ULONG bpp, y, rows, bytes;
    ULONG srcFace = ~0UL, dstFace = ~0UL;   /* one face of a cube map, by that face's handle */
    if (!b->dwDDSrcSurface) return;
    dst = find_texture(c->local, b->dwDDDestSurface);
    src = find_texture(c->local, b->dwDDSrcSurface);
    if (!dst) dst = find_face(c->local, b->dwDDDestSurface, &dstFace);
    if (!src) src = find_face(c->local, b->dwDDSrcSurface, &srcFace);
    adopt_blocks(src, dst);
    if (!dst || !src || !dst->format || dst->format != src->format || dst->faces != src->faces || dst->image || src->image ||
        dst->rtCube || src->rtCube ||
        (srcFace == ~0UL) != (dstFace == ~0UL) ||
        dst->depth > 1 || src->depth > 1 ||
        dst->owner != c->owner || src->owner != c->owner) { ++d->d7draw.texBltFailures; return; }
    if (right <= left || bottom <= top) { left = top = 0; right = src->width; bottom = src->height; }
    if (left < 0 || top < 0 || (ULONG)right > src->width || (ULONG)bottom > src->height ||
        b->pDest.x < 0 || b->pDest.y < 0 || (ULONG)(b->pDest.x + right - left) > dst->width ||
        (ULONG)(b->pDest.y + bottom - top) > dst->height) { ++d->d7draw.texBltFailures; return; }
    bpp = rb_d7_texel_bytes(src->format);
    if (compressed(src->format) && (left % 4 || top % 4 || b->pDest.x % 4 || b->pDest.y % 4)) {
        ++d->d7draw.texBltFailures;   /* only whole blocks */
        return;
    }
    __try {
        /* Mipmapped textures copy the matching rectangle of each level both
         * have, cube maps of each face. */
        ULONG level, levels = min(src->levels, dst->levels), face, faces = srcFace == ~0UL ? src->faces : 1;
        if (src->width != dst->width || src->height != dst->height) levels = 1;
        for (face = 0; face < faces; ++face)
        for (level = 0; level < levels; ++level) {
            ULONG sf = srcFace == ~0UL ? face : srcFace, df = dstFace == ~0UL ? face : dstFace;
            ULONG x0 = (ULONG)left >> level, y0 = (ULONG)top >> level, dx = (ULONG)b->pDest.x >> level;
            ULONG dy = (ULONG)b->pDest.y >> level;
            ULONG x1 = max(x0 + 1, (ULONG)right >> level), y1 = max(y0 + 1, (ULONG)bottom >> level);
            const BYTE *from;
            BYTE *to;
            if (x1 > level_side(src->width, level) || y1 > level_side(src->height, level) ||
                dx + x1 - x0 > level_side(dst->width, level) || dy + y1 - y0 > level_side(dst->height, level)) break;
            rows = y1 - y0;
            bytes = (x1 - x0) * bpp;
            if (compressed(src->format)) {
                /* In 4x4 blocks: positions are whole blocks (levels below them start at 0). */
                x0 /= 4; y0 /= 4; dx /= 4; dy /= 4;
                rows = rb_d7_rows(src->format, y1 - y0 * 4);
                bytes = rb_d7_row_bytes(src->format, x1 - x0 * 4);
                bpp = src->format == TexDxt1 ? 8 : 16;
            }
            from = (const BYTE *)src->levelAddress[sf][level] + y0 * src->levelPitch[sf][level] + x0 * bpp;
            to = (BYTE *)dst->levelAddress[df][level] + dy * dst->levelPitch[df][level] + dx * bpp;
            EngProbeForRead((PVOID)from, (rows - 1) * src->levelPitch[sf][level] + bytes, 1);
            EngProbeForReadAndWrite(to, (rows - 1) * dst->levelPitch[df][level] + bytes, 1);
            for (y = 0; y < rows; ++y)
                RtlCopyMemory(to + y * dst->levelPitch[df][level], from + y * src->levelPitch[sf][level], bytes);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++d->d7draw.texBltFailures; return; }
    dst->epoch = 0;
    dst->dirty = TRUE;
    ++d->d7draw.texBlts;
}

static HRESULT clear(D7_CONTEXT *c, ULONG flags, ULONG color, ULONG depthBits, ULONG stencil, const RECT *rects,
                     ULONG count)
{
    DD_SLOT *s, *z;
    RB2K_DD_FILL f;
    ULONG written;
    BOOL small;
    HRESULT hr = target_surfaces(c, &s, &z, &small);
    if (FAILED(hr)) return hr;
    /* A depth buffer smaller than the target is not cleared with it: the colour clear goes
     * ahead alone, and the depth part counts as a surface refusal. */
    if (small && (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))) {
        ++c->display->d7draw.surfaceRefusals;
        flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    }
    if (!flags) return DD_OK;
    /* Without a stencil buffer a stencil clear has nothing to clear. */
    if ((flags & D3DCLEAR_STENCIL) && (!z || !z->stencil)) flags &= ~D3DCLEAR_STENCIL;
    if (!flags) return DD_OK;
    /* Part-target clears are not drawn yet. */
    if ((flags & ~(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL)) || ((flags & D3DCLEAR_ZBUFFER) && !z) ||
        count != 1 || !rects || rects->left || rects->top ||
        rects->right != (LONG)s->width || rects->bottom != (LONG)s->height) {
        ++c->display->d7draw.skipped[RB2K_D7_SKIP_CLEAR];
        return DD_OK;
    }
    if (z) {
        RB2K_D3D7_ZCLEAR both = {0};
        both.target = s->id;
        both.depth = z->id;
        both.flags = flags;
        both.color = color;
        both.depthBits = depthBits;
        both.stencil = stencil & 255;
        if (miniport(c->display, IOCTL_RB2K_D3D7_ZCLEAR, &both, sizeof(both), NULL, 0, &written))
            return DDERR_SURFACELOST;
    } else {
        f.id = s->id;
        f.color = color;
        if (miniport(c->display, IOCTL_RB2K_D3D7_CLEAR, &f, sizeof(f), NULL, 0, &written))
            return DDERR_SURFACELOST;
    }
    c->queued = TRUE;
    return DD_OK;
}

/* The command buffer is an NT system-memory execute surface. Snapshot a
 * bounded range before walking it so its lengths cannot change during use. */
static BOOL copy_commands(LPD3DNTHAL_DRAWPRIMITIVES2DATA a, BYTE *out, ULONG *origin)
{
    PDD_SURFACE_GLOBAL g;
    ULONG address, capacity;
    if (!a->lpDDCommands || !(a->lpDDCommands->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ||
        !(g = a->lpDDCommands->lpGbl) || !g->fpVidMem) return FALSE;
    capacity = g->dwLinearSize;
    if (a->dwCommandOffset > capacity || a->dwCommandLength > capacity-a->dwCommandOffset ||
        a->dwCommandOffset > 0xffffffffUL-g->fpVidMem) return FALSE;
    address = g->fpVidMem + a->dwCommandOffset;
    if (a->dwCommandLength > 0xffffffffUL-address) return FALSE;
    __try {
        EngProbeForRead((PVOID)address, a->dwCommandLength, 1);
        RtlCopyMemory(out, (PVOID)address, a->dwCommandLength);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    *origin = address;
    return TRUE;
}

/* Size in bytes of a command's records after the header, for the ones
 * this path consumes without drawing. Zero means variable or unknown. */
static ULONG fixed_records(ULONG op, ULONG n)
{
    switch (op) {
    case D3DNTDP2OP_POINTS: return n * sizeof(D3DNTHAL_DP2POINTS);
    case D3DNTDP2OP_INDEXEDLINELIST: return n * sizeof(D3DNTHAL_DP2INDEXEDLINELIST);
    case D3DNTDP2OP_LINELIST:
    case D3DNTDP2OP_LINESTRIP: return sizeof(D3DNTHAL_DP2STARTVERTEX);
    case D3DNTDP2OP_INDEXEDLINESTRIP: return sizeof(D3DNTHAL_DP2STARTVERTEX) + (n + 1) * sizeof(WORD);
    case D3DNTDP2OP_INDEXEDLINELIST2:
        return sizeof(D3DNTHAL_DP2STARTVERTEX) + n * sizeof(D3DNTHAL_DP2INDEXEDLINELIST);
    case D3DNTDP2OP_WINFO: return n * sizeof(D3DNTHAL_DP2WINFO);
    case D3DNTDP2OP_SETPALETTE: return n * sizeof(D3DNTHAL_DP2SETPALETTE);
    case D3DNTDP2OP_SETPRIORITY: return n * sizeof(D3DNTHAL_DP2SETPRIORITY);
    case D3DNTDP2OP_SETTEXLOD: return n * sizeof(D3DNTHAL_DP2SETTEXLOD);
    case D3DNTDP2OP_SETCLIPPLANE: return n * sizeof(D3DNTHAL_DP2SETCLIPPLANE);
    }
    return 0;
}

/* ---- state blocks ---- */

enum { NotState = 0xffffffff };

/* Bytes after the header of a state command, NotState for other commands. */
static ULONG state_bytes(ULONG op, ULONG n, const BYTE *p, ULONG remaining)
{
    ULONG used = 0, k;
    switch (op) {
    case D3DNTDP2OP_RENDERSTATE: return n * sizeof(D3DNTHAL_DP2RENDERSTATE);
    case D3DNTDP2OP_TEXTURESTAGESTATE: return n * sizeof(D3DNTHAL_DP2TEXTURESTAGESTATE);
    case D3DNTDP2OP_SETTRANSFORM: return n * sizeof(D3DNTHAL_DP2SETTRANSFORM);
    case D3DNTDP2OP_VIEWPORTINFO: return n * sizeof(D3DNTHAL_DP2VIEWPORTINFO);
    case D3DNTDP2OP_WINFO: return n * sizeof(D3DNTHAL_DP2WINFO);
    case D3DNTDP2OP_ZRANGE: return n * sizeof(D3DNTHAL_DP2ZRANGE);
    case D3DNTDP2OP_SETMATERIAL: return n * sizeof(D3DNTHAL_DP2SETMATERIAL);
    case D3DNTDP2OP_SETCLIPPLANE: return n * sizeof(D3DNTHAL_DP2SETCLIPPLANE);
    case D3DNTDP2OP_SETTEXLOD: return n * sizeof(D3DNTHAL_DP2SETTEXLOD);
    case RbDp2SetVertexShader: case RbDp2SetPixelShader: return n * sizeof(RB_DP2_HANDLE);
    case RbDp2SetStreamSource: return n * sizeof(RB_DP2_STREAM_SOURCE);
    case RbDp2SetIndices: return n * sizeof(RB_DP2_INDICES);
    case RbDp2SetStreamSource2: return n * sizeof(RB_DP2_STREAM_SOURCE2);
    case RbDp2SetVertexShaderDecl: case RbDp2SetVertexShaderFunc: return n * sizeof(RB_DP2_HANDLE);
    case RbDp2SetScissorRect: return n * sizeof(RECTL);
    case RbDp2SetVertexShaderConstI: case RbDp2SetPixelShaderConstI:
    case RbDp2SetVertexShaderConstB: case RbDp2SetPixelShaderConstB:
        for (k = 0; k < n; ++k) {
            const ULONG *r = (const ULONG *)(p + used);
            ULONG per = op == RbDp2SetVertexShaderConstI || op == RbDp2SetPixelShaderConstI ? 16 : 4;
            if (remaining - min(remaining, used) < 8) return remaining + 1;
            used += 8;
            if (r[1] > (remaining - used) / per) return remaining + 1;
            used += r[1] * per;
        }
        return used;
    case D3DNTDP2OP_SETLIGHT:
        for (k = 0; k < n; ++k) {
            const D3DNTHAL_DP2SETLIGHT *l = (const D3DNTHAL_DP2SETLIGHT *)(p + used);
            if (remaining - min(remaining, used) < sizeof(*l)) return remaining + 1;
            used += sizeof(*l) + (l->lightData == SetLightData ? sizeof(D3DLIGHT7) : 0);
        }
        return used;
    case RbDp2SetVertexShaderConst: case RbDp2SetPixelShaderConst:
        for (k = 0; k < n; ++k) {
            const RB_DP2_SHADER_CONST *r = (const RB_DP2_SHADER_CONST *)(p + used);
            if (remaining - min(remaining, used) < sizeof(*r)) return remaining + 1;
            used += sizeof(*r);
            if (r->count > (remaining - used) / 16) return remaining + 1;
            used += r->count * 16;
        }
        return used;
    }
    return NotState;
}

static D8_STATEBLOCK *find_block(D7_CONTEXT *c, ULONG handle, BOOL create)
{
    ULONG i, free = MaxStateBlocks;
    if (!c->blocks && create) {
        c->blocks = EngAllocMem(FL_ZERO_MEMORY, MaxStateBlocks * sizeof(D8_STATEBLOCK), 'b8bR');
    }
    if (!c->blocks) return NULL;
    for (i = 0; i < MaxStateBlocks; ++i) {
        if (c->blocks[i].handle == handle && handle) return &c->blocks[i];
        if (!c->blocks[i].handle && free == MaxStateBlocks) free = i;
    }
    if (!create || free == MaxStateBlocks || !handle) return NULL;
    c->blocks[free].handle = handle;
    c->blocks[free].bytes = 0;
    return &c->blocks[free];
}

static BOOL block_append(D8_STATEBLOCK *b, const void *data, ULONG bytes)
{
    if (b->bytes + bytes > b->capacity) {
        ULONG capacity = max(b->capacity * 2, b->bytes + bytes + 256);
        BYTE *grown = EngAllocMem(0, capacity, 'b8bR');
        if (!grown) return FALSE;
        if (b->data) { RtlCopyMemory(grown, b->data, b->bytes); EngFreeMem(b->data); }
        b->data = grown;
        b->capacity = capacity;
    }
    RtlCopyMemory(b->data + b->bytes, data, bytes);
    b->bytes += bytes;
    return TRUE;
}

static void record(D7_CONTEXT *c, const void *command, ULONG bytes)
{
    D8_STATEBLOCK *b = find_block(c, c->recording, FALSE);
    if (b && !block_append(b, command, bytes)) ++c->display->d7draw.blockFailures;
}

/* A command header and count records, appended to the block. */
static void block_command(D8_STATEBLOCK *b, ULONG op, ULONG count, const void *records, ULONG bytes)
{
    D3DNTHAL_DP2COMMAND h;
    if (!count) return;
    h.bCommand = (BYTE)op; h.bReserved = 0; h.wStateCount = (WORD)count;
    if (block_append(b, &h, sizeof(h))) block_append(b, records, bytes);
}

/* STATESETCREATE: a block holding the whole current state. */
static void create_block(D7_CONTEXT *c, D8_STATEBLOCK *b)
{
    /* The records are built on the heap: the kernel stack is small. */
    BYTE *scratch = EngAllocMem(0, sizeof(RB_DP2_SHADER_CONST) + sizeof(c->vsConst) + RenderStates * 8, 's8bR');
    D3DNTHAL_DP2RENDERSTATE *rs = (D3DNTHAL_DP2RENDERSTATE *)scratch;
    D3DNTHAL_DP2TEXTURESTAGESTATE ts[StageStates];
    D3DNTHAL_DP2SETTRANSFORM x;
    D3DNTHAL_DP2VIEWPORTINFO v;
    RB_DP2_HANDLE handle;
    ULONG i, k, n = 0;
    b->bytes = 0;
    if (!scratch) { ++c->display->d7draw.blockFailures; return; }
    for (i = 0; i < RenderStates; ++i)
        if ((c->rsSet[i / 32] >> (i % 32)) & 1) { rs[n].RenderState = (D3DRENDERSTATETYPE)i; rs[n].dwState = c->rs[i]; ++n; }
    block_command(b, D3DNTDP2OP_RENDERSTATE, n, rs, n * sizeof(rs[0]));
    for (k = 0; k < TextureStages; ++k) {
        for (i = 0, n = 0; i < StageStates; ++i)
            if ((c->tssSet[k] >> i) & 1) { ts[n].wStage = (WORD)k; ts[n].TSState = (WORD)i; ts[n].dwValue = c->tss[k][i]; ++n; }
        block_command(b, D3DNTDP2OP_TEXTURESTAGESTATE, n, ts, n * sizeof(ts[0]));
    }
    for (i = 0; i < 3; ++i) {
        x.xfrmType = (D3DTRANSFORMSTATETYPE)(i ? D3DTRANSFORMSTATE_WORLD + i : (c->dx8 ? 256 : D3DTRANSFORMSTATE_WORLD));
        RtlCopyMemory(&x.matrix, c->matrices[i], 64);
        block_command(b, D3DNTDP2OP_SETTRANSFORM, 1, &x, sizeof(x));
    }
    for (i = 0; i < TextureStages; ++i) {
        x.xfrmType = (D3DTRANSFORMSTATETYPE)(D3DTRANSFORMSTATE_TEXTURE0 + i);
        RtlCopyMemory(&x.matrix, c->texMatrices[i], 64);
        block_command(b, D3DNTDP2OP_SETTRANSFORM, 1, &x, sizeof(x));
    }
    v.dwX = c->viewport[0]; v.dwY = c->viewport[1]; v.dwWidth = c->viewport[2]; v.dwHeight = c->viewport[3];
    block_command(b, D3DNTDP2OP_VIEWPORTINFO, 1, &v, sizeof(v));
    block_command(b, D3DNTDP2OP_ZRANGE, 1, c->zRange, sizeof(c->zRange));
    block_command(b, D3DNTDP2OP_SETMATERIAL, 1, c->material, sizeof(c->material));
    for (i = 0; i < 6; ++i) {
        D3DNTHAL_DP2SETCLIPPLANE cp;
        cp.dwIndex = i;
        RtlCopyMemory(cp.plane, c->planes[i], 16);
        block_command(b, D3DNTDP2OP_SETCLIPPLANE, 1, &cp, sizeof(cp));
    }
    if (c->dx8) {
        BYTE *buffer = scratch;
        RB_DP2_SHADER_CONST *r = (RB_DP2_SHADER_CONST *)buffer;
        handle.handle = c->fvf;
        block_command(b, RbDp2SetVertexShader, 1, &handle, sizeof(handle));
        handle.handle = c->pixelShader;
        block_command(b, RbDp2SetPixelShader, 1, &handle, sizeof(handle));
        r->reg = 0; r->count = RB_VS_CONSTANTS;
        RtlCopyMemory(r + 1, c->vsConst, sizeof(c->vsConst));
        block_command(b, RbDp2SetVertexShaderConst, 1, buffer, sizeof(*r) + sizeof(c->vsConst));
        r->count = 32;
        RtlCopyMemory(r + 1, c->psConst, sizeof(c->psConst));
        block_command(b, RbDp2SetPixelShaderConst, 1, buffer, sizeof(*r) + sizeof(c->psConst));
    }
    EngFreeMem(scratch);
}

/* STATESETCAPTURE: the recorded states take their current values. */
static void capture_block(D7_CONTEXT *c, D8_STATEBLOCK *b)
{
    ULONG at = 0;
    while (at + sizeof(D3DNTHAL_DP2COMMAND) <= b->bytes) {
        D3DNTHAL_DP2COMMAND *h = (D3DNTHAL_DP2COMMAND *)(b->data + at);
        BYTE *p = (BYTE *)(h + 1);
        ULONG n = h->wStateCount, left = b->bytes - at - sizeof(*h), bytes = state_bytes(h->bCommand, n, p, left), k;
        if (bytes == NotState || bytes > left) break;
        switch (h->bCommand) {
        case D3DNTDP2OP_RENDERSTATE: {
            D3DNTHAL_DP2RENDERSTATE *r = (D3DNTHAL_DP2RENDERSTATE *)p;
            for (k = 0; k < n; ++k, ++r) if (r->RenderState < RenderStates) r->dwState = c->rs[r->RenderState];
            break;
        }
        case D3DNTDP2OP_TEXTURESTAGESTATE: {
            D3DNTHAL_DP2TEXTURESTAGESTATE *t = (D3DNTHAL_DP2TEXTURESTAGESTATE *)p;
            for (k = 0; k < n; ++k, ++t)
                if (t->wStage < TextureStages && t->TSState < StageStates) t->dwValue = c->tss[t->wStage][t->TSState];
            break;
        }
        case D3DNTDP2OP_SETTRANSFORM: {
            D3DNTHAL_DP2SETTRANSFORM *x = (D3DNTHAL_DP2SETTRANSFORM *)p;
            for (k = 0; k < n; ++k, ++x) {
                ULONG type = x->xfrmType == 256 ? D3DTRANSFORMSTATE_WORLD : x->xfrmType;
                if (type >= D3DTRANSFORMSTATE_WORLD && type <= D3DTRANSFORMSTATE_PROJECTION)
                    RtlCopyMemory(&x->matrix, c->matrices[type - D3DTRANSFORMSTATE_WORLD], 64);
                else if (type >= D3DTRANSFORMSTATE_TEXTURE0 && type < D3DTRANSFORMSTATE_TEXTURE0 + TextureStages)
                    RtlCopyMemory(&x->matrix, c->texMatrices[type - D3DTRANSFORMSTATE_TEXTURE0], 64);
            }
            break;
        }
        case D3DNTDP2OP_VIEWPORTINFO: {
            D3DNTHAL_DP2VIEWPORTINFO *v = (D3DNTHAL_DP2VIEWPORTINFO *)p;
            for (k = 0; k < n; ++k, ++v) {
                v->dwX = c->viewport[0]; v->dwY = c->viewport[1]; v->dwWidth = c->viewport[2]; v->dwHeight = c->viewport[3];
            }
            break;
        }
        case D3DNTDP2OP_ZRANGE: for (k = 0; k < n; ++k) RtlCopyMemory(p + 8 * k, c->zRange, 8); break;
        case D3DNTDP2OP_SETMATERIAL: for (k = 0; k < n; ++k) RtlCopyMemory(p + 68 * k, c->material, 68); break;
        case D3DNTDP2OP_SETCLIPPLANE:
            for (k = 0; k < n; ++k) {
                D3DNTHAL_DP2SETCLIPPLANE *cp = (D3DNTHAL_DP2SETCLIPPLANE *)p + k;
                if (cp->dwIndex < 6) RtlCopyMemory(cp->plane, c->planes[cp->dwIndex], 16);
            }
            break;
        case RbDp2SetVertexShader: case RbDp2SetPixelShader: {
            RB_DP2_HANDLE *v = (RB_DP2_HANDLE *)p;
            for (k = 0; k < n; ++k) v[k].handle = h->bCommand == RbDp2SetVertexShader ? c->fvf : c->pixelShader;
            break;
        }
        case RbDp2SetVertexShaderConst: case RbDp2SetPixelShaderConst: {
            ULONG used = 0, limit = h->bCommand == RbDp2SetVertexShaderConst ? RB_VS_CONSTANTS : 32;
            for (k = 0; k < n; ++k) {
                RB_DP2_SHADER_CONST *r = (RB_DP2_SHADER_CONST *)(p + used);
                ULONG j;
                used += sizeof(*r);
                for (j = 0; j < r->count && r->reg + j < limit; ++j)
                    RtlCopyMemory(p + used + 16 * j, h->bCommand == RbDp2SetVertexShaderConst ? c->vsConst[r->reg + j] :
                                  c->psConst[r->reg + j], 16);
                used += r->count * 16;
            }
            break;
        }
        }
        at += sizeof(*h) + bytes;
    }
}

static HRESULT commands(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG stride,
                        const BYTE *bytes, ULONG length, ULONG origin, ULONG *offset);

static HRESULT state_set(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG stride, ULONG operation, ULONG handle)
{
    D8_STATEBLOCK *b;
    ++c->display->d7draw.stateSets[operation < 6 ? operation : 6];
    switch (operation) {
    case 0:     /* D3DHAL_STATESETBEGIN */
        b = find_block(c, handle, TRUE);
        if (b) b->bytes = 0;
        c->recording = b ? handle : 0;
        break;
    case 1:     /* END */
        c->recording = 0;
        break;
    case 2:     /* DELETE */
        b = find_block(c, handle, FALSE);
        if (b) { if (b->data) EngFreeMem(b->data); RtlZeroMemory(b, sizeof(*b)); }
        break;
    case 3: {   /* EXECUTE */
        ULONG at = 0;
        HRESULT hr;
        b = find_block(c, handle, FALSE);
        if (!b || !b->bytes || c->replaying) break;
        c->replaying = TRUE;
        hr = commands(c, a, stride, b->data, b->bytes, 0, &at);
        c->replaying = FALSE;
        return hr;
    }
    case 4:     /* CAPTURE */
        b = find_block(c, handle, FALSE);
        if (b) capture_block(c, b);
        break;
    case 5:     /* CREATE (Direct3D 8) */
        b = find_block(c, handle, TRUE);
        if (b) create_block(c, b);
        break;
    }
    return DD_OK;
}

static void release_blocks(D7_CONTEXT *c)
{
    ULONG i;
    if (!c->blocks) return;
    for (i = 0; i < MaxStateBlocks; ++i) if (c->blocks[i].data) EngFreeMem(c->blocks[i].data);
    EngFreeMem(c->blocks);
    c->blocks = NULL;
    c->recording = 0;
}

static HRESULT commands(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG stride,
                        const BYTE *bytes, ULONG length, ULONG origin, ULONG *offset)
{
    while (*offset < length) {
        const D3DNTHAL_DP2COMMAND *h;
        const BYTE *p;
        ULONG n, used, remaining = length-*offset;
        HRESULT hr;
        PRIMS prims;
        if (remaining < sizeof(*h)) return DDERR_INVALIDPARAMS;
        h = (const D3DNTHAL_DP2COMMAND *)(bytes + *offset);
        c->display->d7.lastCommand = h->bCommand;
        if (h->bCommand < RB2K_D7_OPCODES) ++c->display->d7draw.opcodes[h->bCommand];
        n = h->wStateCount;
        p = (const BYTE *)(h + 1);
        remaining -= sizeof(*h);
        if (c->recording && h->bCommand != D3DNTDP2OP_STATESET) {
            /* Between STATESETBEGIN and END, state commands go into the block. */
            ULONG bytes = state_bytes(h->bCommand, n, p, remaining);
            if (bytes != NotState) {
                if (bytes > remaining) return DDERR_INVALIDPARAMS;
                record(c, h, sizeof(*h) + bytes);
                *offset += sizeof(*h) + bytes;
                continue;
            }
        }
        RtlZeroMemory(&prims, sizeof(prims));
        prims.primitives = n;
        switch (h->bCommand) {
        case D3DNTDP2OP_RENDERSTATE: {
            const D3DNTHAL_DP2RENDERSTATE *rs = (const D3DNTHAL_DP2RENDERSTATE *)p;
            used = n*sizeof(*rs);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++rs) {
                if (rs->RenderState < RenderStates && rs->RenderState != D3DRENDERSTATE_SCENECAPTURE) {
                    c->rs[rs->RenderState] = rs->dwState;
                    c->rsSet[rs->RenderState / 32] |= 1UL << (rs->RenderState % 32);
                }
                if (rs->RenderState == D3DRENDERSTATE_SCENECAPTURE && c->replaying) continue;
                if (rs->RenderState == D3DRENDERSTATE_SCENECAPTURE) {
                    c->inScene = rs->dwState != 0;
                    if (!c->inScene) {
                        ++textureEpoch;
                        if (FAILED(finish_scene(c))) return DDERR_SURFACELOST;
                    }
                } else render_state(c, rs->RenderState, rs->dwState);
            }
            break;
        }
        case D3DNTDP2OP_TRIANGLELIST:
        case D3DNTDP2OP_TRIANGLESTRIP:
        case D3DNTDP2OP_TRIANGLEFAN:
            used = sizeof(D3DNTHAL_DP2STARTVERTEX);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            prims.first = ((const D3DNTHAL_DP2STARTVERTEX *)p)->wVStart;
            prims.kind = h->bCommand == D3DNTDP2OP_TRIANGLELIST ? List :
                         h->bCommand == D3DNTDP2OP_TRIANGLESTRIP ? Strip : Fan;
            hr = triangles(c, a, stride, &prims);
            if (FAILED(hr)) return hr;
            break;
        case D3DNTDP2OP_INDEXEDTRIANGLELIST2:
        case D3DNTDP2OP_INDEXEDTRIANGLESTRIP:
        case D3DNTDP2OP_INDEXEDTRIANGLEFAN: {
            ULONG indices = h->bCommand == D3DNTDP2OP_INDEXEDTRIANGLELIST2 ? 3 * n : n + 2;
            used = sizeof(D3DNTHAL_DP2STARTVERTEX) + indices * sizeof(WORD);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            prims.first = ((const D3DNTHAL_DP2STARTVERTEX *)p)->wVStart;
            prims.indices = (const WORD *)(p + sizeof(D3DNTHAL_DP2STARTVERTEX));
            prims.kind = h->bCommand == D3DNTDP2OP_INDEXEDTRIANGLELIST2 ? List :
                         h->bCommand == D3DNTDP2OP_INDEXEDTRIANGLESTRIP ? Strip : Fan;
            if (n) {
                hr = triangles(c, a, stride, &prims);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case D3DNTDP2OP_POINTS: {
            /* Runs of points, each a first vertex and a count. */
            const D3DNTHAL_DP2POINTS *r = (const D3DNTHAL_DP2POINTS *)p;
            ULONG k;
            used = n * sizeof(*r);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (k = 0; k < n; ++k) {
                PRIMS q = prims;
                q.first = r[k].wVStart;
                q.primitives = r[k].wCount;
                q.kind = Points;
                if (q.primitives) { hr = triangles(c, a, stride, &q); if (FAILED(hr)) return hr; }
            }
            break;
        }
        case D3DNTDP2OP_LINELIST:
        case D3DNTDP2OP_LINESTRIP:
            used = sizeof(D3DNTHAL_DP2STARTVERTEX);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            prims.first = ((const D3DNTHAL_DP2STARTVERTEX *)p)->wVStart;
            prims.kind = h->bCommand == D3DNTDP2OP_LINELIST ? LineList : LineStrip;
            hr = triangles(c, a, stride, &prims);
            if (FAILED(hr)) return hr;
            break;
        case D3DNTDP2OP_INDEXEDLINELIST:
            /* Absolute indices, two a line. */
            used = n * sizeof(D3DNTHAL_DP2INDEXEDLINELIST);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            prims.indices = (const WORD *)p;
            prims.kind = LineList;
            hr = triangles(c, a, stride, &prims);
            if (FAILED(hr)) return hr;
            break;
        case D3DNTDP2OP_INDEXEDLINELIST2:
        case D3DNTDP2OP_INDEXEDLINESTRIP: {
            ULONG indices = h->bCommand == D3DNTDP2OP_INDEXEDLINELIST2 ? 2 * n : n + 1;
            used = sizeof(D3DNTHAL_DP2STARTVERTEX) + indices * sizeof(WORD);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            prims.first = ((const D3DNTHAL_DP2STARTVERTEX *)p)->wVStart;
            prims.indices = (const WORD *)(p + sizeof(D3DNTHAL_DP2STARTVERTEX));
            prims.kind = h->bCommand == D3DNTDP2OP_INDEXEDLINELIST2 ? LineList : LineStrip;
            if (n) { hr = triangles(c, a, stride, &prims); if (FAILED(hr)) return hr; }
            break;
        }
        case D3DNTDP2OP_INDEXEDTRIANGLELIST:
            /* Absolute indices with an edge-flag word per triangle. */
            used = n * sizeof(D3DNTHAL_DP2INDEXEDTRIANGLELIST);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            prims.indices = (const WORD *)p;
            prims.indexStride = 4;
            hr = triangles(c, a, stride, &prims);
            if (FAILED(hr)) return hr;
            break;
        case D3DNTDP2OP_TRIANGLEFAN_IMM:
        case D3DNTDP2OP_LINELIST_IMM: {
            /* Vertices follow in the command buffer at a DWORD-aligned address. */
            ULONG head = h->bCommand == D3DNTDP2OP_TRIANGLEFAN_IMM ? sizeof(D3DNTHAL_DP2TRIANGLEFAN_IMM) : 0;
            ULONG start = origin + *offset + sizeof(*h) + head, pad = (4 - (start & 3)) & 3;
            ULONG vertices = h->bCommand == D3DNTDP2OP_TRIANGLEFAN_IMM ? n + 2 : 2 * n;
            if (!stride || vertices > (remaining - min(remaining, head + pad)) / stride ||
                head + pad > remaining) {
                ++c->display->d7draw.failSites[RB2K_D7_FAIL_IMMEDIATE];
                return DDERR_INVALIDPARAMS;
            }
            used = head + pad + vertices * stride;
            prims.inline_ = p + head + pad;
            prims.kind = h->bCommand == D3DNTDP2OP_LINELIST_IMM ? LineList : Fan;
            hr = triangles(c, a, stride, &prims);
            if (FAILED(hr)) return hr;
            break;
        }
        case D3DNTDP2OP_TEXTURESTAGESTATE: {
            const D3DNTHAL_DP2TEXTURESTAGESTATE *st = (const D3DNTHAL_DP2TEXTURESTAGESTATE *)p;
            used = n*sizeof(*st);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++st) {
                ULONG value = st->dwValue;
                if (st->wStage < TextureStages && st->TSState < StageStates) {
                    c->tss[st->wStage][st->TSState] = value;
                    c->tssSet[st->wStage] |= 1UL << st->TSState;
                }
                /* With pixel shader 2.0 the Direct3D 9 runtime also sets samplers
                 * 8 to 15, which no draw here uses. */
                if (st->wStage >= TextureStages) continue;
                /* Direct3D 8 filters: mip NONE, POINT, LINEAR are 0..2 (Direct3D 7
                 * uses 1..3), and magnification ANISOTROPIC is 3. */
                if (c->dx8 && st->TSState == D3DTSS_MIPFILTER) ++value;
                else if (c->dx8 && st->TSState == D3DTSS_MAGFILTER && value == 3) value = D3DTFG_LINEAR;
                stage_state(&c->stages[st->wStage], st->TSState, value);
            }
            break;
        }
        case D3DNTDP2OP_SETTRANSFORM: {
            /* World, view and projection, and the stages' texture matrices. */
            const D3DNTHAL_DP2SETTRANSFORM *x = (const D3DNTHAL_DP2SETTRANSFORM *)p;
            used = n * sizeof(*x);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++x) {
                /* Direct3D 8 numbers the first world matrix 256. */
                ULONG type = x->xfrmType == 256 ? D3DTRANSFORMSTATE_WORLD : x->xfrmType;
                ++c->display->d7draw.transformTypes[x->xfrmType < 31 ? x->xfrmType : 31];
                if (type >= D3DTRANSFORMSTATE_WORLD && type <= D3DTRANSFORMSTATE_PROJECTION)
                    RtlCopyMemory(c->matrices[type - D3DTRANSFORMSTATE_WORLD], &x->matrix, 64);
                else if (type >= D3DTRANSFORMSTATE_TEXTURE0 && type < D3DTRANSFORMSTATE_TEXTURE0 + TextureStages)
                    RtlCopyMemory(c->texMatrices[type - D3DTRANSFORMSTATE_TEXTURE0], &x->matrix, 64);
            }
            break;
        }
        case D3DNTDP2OP_VIEWPORTINFO: {
            const D3DNTHAL_DP2VIEWPORTINFO *v = (const D3DNTHAL_DP2VIEWPORTINFO *)p;
            used = n * sizeof(*v);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            if (n) {
                v += n - 1;
                c->viewport[0] = v->dwX; c->viewport[1] = v->dwY;
                c->viewport[2] = v->dwWidth; c->viewport[3] = v->dwHeight;
            }
            break;
        }
        case D3DNTDP2OP_WINFO: {
            const D3DNTHAL_DP2WINFO *w = (const D3DNTHAL_DP2WINFO *)p;
            used = n * sizeof(*w);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            if (n) RtlCopyMemory(&c->wNear, w + n - 1, 2 * sizeof(ULONG));
            break;
        }
        case D3DNTDP2OP_ZRANGE: {
            const D3DNTHAL_DP2ZRANGE *z = (const D3DNTHAL_DP2ZRANGE *)p;
            used = n * sizeof(*z);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            if (n) RtlCopyMemory(c->zRange, z + n - 1, sizeof(c->zRange));
            break;
        }
        case D3DNTDP2OP_SETMATERIAL:
            used = n * sizeof(D3DNTHAL_DP2SETMATERIAL);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            if (n) RtlCopyMemory(c->material, (const D3DNTHAL_DP2SETMATERIAL *)p + n - 1, sizeof(c->material));
            break;
        case D3DNTDP2OP_SETCLIPPLANE: {
            const D3DNTHAL_DP2SETCLIPPLANE *cp = (const D3DNTHAL_DP2SETCLIPPLANE *)p;
            used = n * sizeof(*cp);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++cp) if (cp->dwIndex < 6) RtlCopyMemory(c->planes[cp->dwIndex], cp->plane, 16);
            break;
        }
        case D3DNTDP2OP_CREATELIGHT: used = n * sizeof(D3DNTHAL_DP2CREATELIGHT); break;
        case D3DNTDP2OP_STATESET: {
            const D3DNTHAL_DP2STATESET *ss = (const D3DNTHAL_DP2STATESET *)p;
            used = n * sizeof(*ss);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++ss) {
                hr = state_set(c, a, stride, ss->dwOperation, ss->dwParam);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case D3DNTDP2OP_SETLIGHT: {
            /* Enable, disable, or set the D3DLIGHT7 that follows the record.
             * Lights past MaxLights are not drawn. */
            ULONG k;
            used = 0;
            for (k = 0; k < n; ++k) {
                const D3DNTHAL_DP2SETLIGHT *l = (const D3DNTHAL_DP2SETLIGHT *)(p + used);
                ULONG index;
                if (remaining - min(remaining, used) < sizeof(*l)) return DDERR_INVALIDPARAMS;
                used += sizeof(*l);
                index = l->dwIndex;
                if (l->lightData == SetLightData) {
                    if (remaining - min(remaining, used) < sizeof(D3DLIGHT7)) return DDERR_INVALIDPARAMS;
                    if (index < MaxLights) RtlCopyMemory(c->lights[index], p + used, sizeof(c->lights[index]));
                    used += sizeof(D3DLIGHT7);
                } else if (index < MaxLights && l->lightData == SetLightEnable) c->lightOn |= 1UL << index;
                else if (index < MaxLights && l->lightData == SetLightDisable) c->lightOn &= ~(1UL << index);
                if (index >= MaxLights) ++c->display->d7draw.lightsBeyond;
            }
            break;
        }
        case D3DNTDP2OP_TEXBLT: {
            const D3DNTHAL_DP2TEXBLT *b = (const D3DNTHAL_DP2TEXBLT *)p;
            used = n * sizeof(*b);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++b) texture_blt(c, b);
            break;
        }
        case D3DNTDP2OP_UPDATEPALETTE: {
            const D3DNTHAL_DP2UPDATEPALETTE *u = (const D3DNTHAL_DP2UPDATEPALETTE *)p;
            if (remaining < sizeof(*u)) return DDERR_INVALIDPARAMS;
            used = sizeof(*u) + u->wNumEntries * sizeof(DWORD);
            break;
        }
        case D3DNTDP2OP_EXT: {
            const D3DNTHAL_DP2EXT *ext = (const D3DNTHAL_DP2EXT *)p;
            PVOID next = NULL;
            if (n != 1 || remaining < sizeof(*ext) ||
                ext->dwSize > remaining-sizeof(*ext)) return DDERR_INVALIDPARAMS;
            if (!c->display->parseD3DExtension) return D3DNTERR_COMMAND_UNPARSED;
            hr = c->display->parseD3DExtension((PVOID)h, &next);
            if (FAILED(hr)) return hr;
            if ((ULONG)next < (ULONG)p+sizeof(*ext) || (ULONG)next-(ULONG)p > remaining)
                return DDERR_INVALIDPARAMS;
            used = (ULONG)next-(ULONG)p;
            break;
        }
        case D3DNTDP2OP_SETRENDERTARGET: {
            const D3DNTHAL_DP2SETRENDERTARGET *set = (const D3DNTHAL_DP2SETRENDERTARGET *)p;
            DD_SLOT *s, *z;
            used = n*sizeof(*set);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++set) {
                s = target(c, set->hRenderTarget, TRUE);
                z = set->hZBuffer ? target(c, set->hZBuffer, TRUE) : NULL;
                if (!s) ++c->display->d7draw.targetFailures;
                if (!s || s->depth16 || (set->hZBuffer && (!z || !z->depth16 ||
                    z->width < s->width || z->height < s->height))) return DDERR_SURFACELOST;
                if ((s->id != c->target || (z ? z->id : 0) != c->depth) && FAILED(finish_scene(c)))
                    return DDERR_SURFACELOST;
                if (s->id != c->target) c->display->d7draw.targetLog[c->display->d7draw.targets++ & 7] = s->id;
                c->target = s->id;
                c->depth = z ? z->id : 0;
            }
            break;
        }
        case D3DNTDP2OP_CLEAR: {
            const D3DNTHAL_DP2CLEAR *f = (const D3DNTHAL_DP2CLEAR *)p;
            ULONG depthBits;
            ++c->display->d7.clearCalls;
            used = FIELD_OFFSET(D3DNTHAL_DP2CLEAR, Rects) + n*sizeof(RECT);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            RtlCopyMemory(&depthBits, &f->dvFillDepth, sizeof(depthBits));
            hr = clear(c, f->dwFlags, f->dwFillColor, depthBits, f->dwFillStencil, f->Rects, n);
            if (FAILED(hr)) return hr;
            break;
        }
        case RbDp2SetVertexShader:
        case RbDp2DeleteVertexShader:
        case RbDp2SetPixelShader:
        case RbDp2DeletePixelShader: {
            const RB_DP2_HANDLE *v = (const RB_DP2_HANDLE *)p;
            used = n * sizeof(*v);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            if (n && h->bCommand == RbDp2SetVertexShader) { c->fvf = v[n - 1].handle; c->dx8 = TRUE; }
            if (n && h->bCommand == RbDp2SetPixelShader) c->pixelShader = v[n - 1].handle;
            if (h->bCommand == RbDp2DeleteVertexShader) {
                ULONG k;
                for (k = 0; k < n; ++k) {
                    D8_VSHADER *vs = find_vshader(c, v[k].handle);
                    if (vs) release_vshader(c, vs);
                }
            }
            if (h->bCommand == RbDp2DeletePixelShader) {
                ULONG k;
                for (k = 0; k < n; ++k) {
                    D8_SHADER *sh = find_shader(c, v[k].handle);
                    if (sh) release_shader(c, sh);
                }
            }
            break;
        }
        case RbDp2SetStreamSource: {
            const RB_DP2_STREAM_SOURCE *s = (const RB_DP2_STREAM_SOURCE *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s)
                if (s->stream < RB_VS_STREAMS) {
                    c->streamHandles[s->stream] = s->handle;
                    c->streamStrides[s->stream] = s->stride;
                    if (!s->stream) { c->streamUser = FALSE; c->streamHandle = s->handle; c->streamStride = s->stride; }
                }
            break;
        }
        case RbDp2SetStreamSourceUm: {
            const RB_DP2_STREAM_SOURCE_UM *s = (const RB_DP2_STREAM_SOURCE_UM *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s)
                if (!s->stream) { c->streamUser = TRUE; c->streamHandle = 0; c->streamStride = s->stride; c->streamHandles[0] = 0; c->streamStrides[0] = s->stride; }
            break;
        }
        case RbDp2SetIndices: {
            const RB_DP2_INDICES *s = (const RB_DP2_INDICES *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            if (n) { c->indexHandle = s[n - 1].handle; c->indexStride = s[n - 1].stride; }
            break;
        }
        case RbDp2DrawPrimitive: {
            const RB_DP2_DRAW_PRIMITIVE *s = (const RB_DP2_DRAW_PRIMITIVE *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s) {
                hr = stream_draw(c, a, s->type, (LONGLONG)s->start * c->streamStride, s->count, FALSE, 0);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case RbDp2DrawPrimitive2: {
            const RB_DP2_DRAW_PRIMITIVE2 *s = (const RB_DP2_DRAW_PRIMITIVE2 *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s) {
                hr = stream_draw(c, a, s->type, s->firstOffset, s->count, FALSE, 0);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case RbDp2ClippedTriangleFan: {
            const RB_DP2_CLIPPED_FAN *s = (const RB_DP2_CLIPPED_FAN *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s) {
                hr = stream_draw(c, a, RbTriangleFan, s->firstOffset, s->count, FALSE, 0);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case RbDp2DrawIndexedPrimitive: {
            const RB_DP2_DRAW_INDEXED *s = (const RB_DP2_DRAW_INDEXED *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s) {
                hr = stream_draw(c, a, s->type, (LONGLONG)s->baseVertex * c->streamStride, s->count, TRUE,
                                 s->startIndex * c->indexStride);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case RbDp2DrawIndexedPrimitive2: {
            const RB_DP2_DRAW_INDEXED2 *s = (const RB_DP2_DRAW_INDEXED2 *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s) {
                hr = stream_draw(c, a, s->type, s->baseOffset, s->count, TRUE, s->startOffset);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case RbDp2CreateVertexShader: {
            ULONG k;
            used = variable_records(p, remaining, n, sizeof(RB_DP2_CREATE_VERTEX_SHADER), 1, 2);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (k = 0, used = 0; k < n; ++k) {
                const RB_DP2_CREATE_VERTEX_SHADER *r = (const RB_DP2_CREATE_VERTEX_SHADER *)(p + used);
                RB2K_D7_DRAWSTATS *st = &c->display->d7draw;
                {   /* the last four */
                    ULONG *log = st->vsLog[st->vsCreated % 4], words = (r->declBytes + r->codeBytes) / 4;
                    log[0] = r->handle; log[1] = r->declBytes; log[2] = r->codeBytes;
                    RtlCopyMemory(log + 3, p + used + sizeof(*r), min(words, 45UL) * 4);
                }
                ++st->vsCreated;
                create_vshader(c, r->handle, p + used + sizeof(*r), r->declBytes, r->codeBytes);
                used += sizeof(*r) + r->declBytes + r->codeBytes;
            }
            break;
        }
        case RbDp2CreatePixelShader: {
            ULONG k;
            used = variable_records(p, remaining, n, sizeof(RB_DP2_CREATE_PIXEL_SHADER), 1, 0);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (k = 0, used = 0; k < n; ++k) {
                const RB_DP2_CREATE_PIXEL_SHADER *r = (const RB_DP2_CREATE_PIXEL_SHADER *)(p + used);
                create_shader(c, r->handle, p + used + sizeof(*r), r->codeBytes);
                used += sizeof(*r) + r->codeBytes;
            }
            break;
        }
        case RbDp2SetVertexShaderConst:
        case RbDp2SetPixelShaderConst: {
            /* Each record is followed by count four-float constants. */
            ULONG k;
            used = 0;
            for (k = 0; k < n; ++k) {
                const RB_DP2_SHADER_CONST *r = (const RB_DP2_SHADER_CONST *)(p + used);
                if (remaining - min(remaining, used) < sizeof(*r)) return DDERR_INVALIDPARAMS;
                used += sizeof(*r);
                if (r->count > (remaining - used) / 16) return DDERR_INVALIDPARAMS;
                if (h->bCommand == RbDp2SetPixelShaderConst && r->reg < 32)
                    RtlCopyMemory(c->psConst[r->reg], p + used, min(r->count, 32 - r->reg) * 16);
                if (h->bCommand == RbDp2SetVertexShaderConst && r->reg < RB_VS_CONSTANTS)
                    RtlCopyMemory(c->vsConst[r->reg], p + used, min(r->count, RB_VS_CONSTANTS - r->reg) * 16);
                used += r->count * 16;
            }
            trace8(c, a, h->bCommand, n, p, remaining);
            break;
        }
        case RbDp2BufferBlt:
        case RbDp2VolumeBlt:
        case RbDp2MultiplyTransform:
        case RbDp2AddDirtyRect:
        case RbDp2AddDirtyBox:
            used = n * (h->bCommand == RbDp2BufferBlt ? sizeof(RB_DP2_BUFFER_BLT) :
                        h->bCommand == RbDp2VolumeBlt ? sizeof(RB_DP2_VOLUME_BLT) :
                        h->bCommand == RbDp2MultiplyTransform ? sizeof(RB_DP2_MULTIPLY_TRANSFORM) :
                        h->bCommand == RbDp2AddDirtyRect ? sizeof(RB_DP2_DIRTY_RECT) : sizeof(RB_DP2_DIRTY_BOX));
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            if (h->bCommand == RbDp2VolumeBlt) {
                const RB_DP2_VOLUME_BLT *b = (const RB_DP2_VOLUME_BLT *)p;
                ULONG k;
                for (k = 0; k < n; ++k) volume_blt(c, b + k);
            }
            break;
        /* ---- the Direct3D 9 interface ---- */
        case RbDp2SetStreamSource2: {
            const RB_DP2_STREAM_SOURCE2 *s = (const RB_DP2_STREAM_SOURCE2 *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++s)
                if (s->stream < RB_VS_STREAMS) {
                    c->streamHandles[s->stream] = s->handle;
                    c->streamStrides[s->stream] = s->stride;
                    c->streamOffsets[s->stream] = s->offset;
                    if (!s->stream) { c->streamUser = FALSE; c->streamHandle = s->handle; c->streamStride = s->stride; }
                }
            break;
        }
        case RbDp2SetScissorRect:
            used = n * sizeof(RECTL);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            if (n) RtlCopyMemory(&c->scissor, p + used - sizeof(RECTL), sizeof(RECTL));
            break;
        case RbDp2CreateVertexShaderDecl: case RbDp2CreateVertexShaderFunc: {
            ULONG k, per = h->bCommand == RbDp2CreateVertexShaderDecl ? sizeof(RB_VERTEX_ELEMENT9) : 1;
            used = 0;
            for (k = 0; k < n; ++k) {
                const ULONG *r = (const ULONG *)(p + used);
                if (remaining < used + 8 || r[1] > (remaining - used - 8) / per) return DDERR_INVALIDPARAMS;
                if (h->bCommand == RbDp2CreateVertexShaderDecl) d9_create_decl(c, r[0], (const RB_VERTEX_ELEMENT9 *)(r + 2), r[1]);
                else d9_create_func(c, r[0], r + 2, r[1]);
                used += 8 + r[1] * per;
            }
            break;
        }
        case RbDp2DeleteVertexShaderDecl: case RbDp2SetVertexShaderDecl:
        case RbDp2DeleteVertexShaderFunc: case RbDp2SetVertexShaderFunc: {
            const RB_DP2_HANDLE *s = (const RB_DP2_HANDLE *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++s)
                switch (h->bCommand) {
                case RbDp2DeleteVertexShaderDecl: d9_delete_decl(c, s->handle); break;
                case RbDp2SetVertexShaderDecl: c->declHandle = s->handle; break;
                case RbDp2DeleteVertexShaderFunc: d9_delete_func(c, s->handle); break;
                default: c->funcHandle = s->handle; break;
                }
            break;
        }
        case RbDp2SetRenderTarget2: case RbDp2SetDepthStencil: {
            ULONG size = h->bCommand == RbDp2SetRenderTarget2 ? sizeof(RB_DP2_RENDER_TARGET2) : sizeof(ULONG), k;
            used = n * size;
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (k = 0; k < n; ++k) {
                ULONG targetId = c->target, depthId = c->depth;
                if (h->bCommand == RbDp2SetRenderTarget2) {
                    const RB_DP2_RENDER_TARGET2 *t = (const RB_DP2_RENDER_TARGET2 *)(p + k * size);
                    DD_SLOT *s;
                    if (t->index) {                  /* one render target only */
                        if (t->handle) ++c->display->d7draw.d9Ignored[RB2K_D9_IGNORED_TARGETS];
                        continue;
                    }
                    s = target(c, t->handle, TRUE);
                    if (!s) ++c->display->d7draw.targetFailures;
                    if (!s || s->depth16) return DDERR_SURFACELOST;
                    targetId = s->id;
                    /* Setting a render target resets the scissor rectangle to all of it. */
                    c->scissor.left = c->scissor.top = 0;
                    c->scissor.right = s->width;
                    c->scissor.bottom = s->height;
                } else {
                    ULONG handle = *(const ULONG *)(p + k * size);
                    DD_SLOT *z = handle ? target(c, handle, TRUE) : NULL;
                    if (handle && (!z || !z->depth16)) return DDERR_SURFACELOST;
                    depthId = z ? z->id : 0;
                }
                if ((targetId != c->target || depthId != c->depth) && FAILED(finish_scene(c))) return DDERR_SURFACELOST;
                if (targetId != c->target) c->display->d7draw.targetLog[c->display->d7draw.targets++ & 7] = targetId;
                c->target = targetId;
                c->depth = depthId;
            }
            break;
        }
        /* Accepted and counted, not carried out yet: integer and boolean constants (no flow
         * control), blits and fills, queries (none are offered), mipmap generation, instancing. */
        case RbDp2SetVertexShaderConstI: case RbDp2SetPixelShaderConstI:
        case RbDp2SetVertexShaderConstB: case RbDp2SetPixelShaderConstB: {
            ULONG k, per = h->bCommand == RbDp2SetVertexShaderConstI || h->bCommand == RbDp2SetPixelShaderConstI ? 16 : 4;
            used = 0;
            for (k = 0; k < n; ++k) {
                const ULONG *r = (const ULONG *)(p + used);
                if (remaining < used + 8 || r[1] > (remaining - used - 8) / per) return DDERR_INVALIDPARAMS;
                if (h->bCommand == RbDp2SetVertexShaderConstI) {
                    ULONG j;
                    for (j = 0; j < r[1] && r[0] + j < 16; ++j) RtlCopyMemory(c->vsInt[r[0] + j], r + 2 + 4 * j, 16);
                } else if (h->bCommand == RbDp2SetVertexShaderConstB) {
                    ULONG j;
                    for (j = 0; j < r[1] && r[0] + j < 16; ++j)
                        c->vsBool = r[2 + j] ? c->vsBool | 1UL << (r[0] + j) : c->vsBool & ~(1UL << (r[0] + j));
                } else ++c->display->d7draw.d9Ignored[RB2K_D9_IGNORED_CONSTANTS];   /* pixel shader ones */
                used += 8 + r[1] * per;
            }
            break;
        }
        case RbDp2Blt: case RbDp2SurfaceBlt: {
            const RB_DP2_BLT9 *b = (const RB_DP2_BLT9 *)p;
            used = n * sizeof(*b);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++b) d9_blt(c, b);
            break;
        }
        case RbDp2ColorFill: {
            const RB_DP2_COLOR_FILL *f = (const RB_DP2_COLOR_FILL *)p;
            used = n * sizeof(*f);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++f) d9_fill(c, f);
            break;
        }
        case RbDp2CreateQuery: case RbDp2DeleteQuery: case RbDp2IssueQuery:
        case RbDp2GenerateMipSubLevels: case RbDp2SetStreamSourceFreq:
            used = n * (h->bCommand == RbDp2DeleteQuery ? 4 : 8);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            c->display->d7draw.d9Ignored[h->bCommand == RbDp2GenerateMipSubLevels ? RB2K_D9_IGNORED_MIPS :
                                         h->bCommand == RbDp2SetStreamSourceFreq ? RB2K_D9_IGNORED_FREQUENCY :
                                         RB2K_D9_IGNORED_QUERY] += n;
            break;
        default:
            used = fixed_records(h->bCommand, n);
            if (!used && n) return D3DNTERR_COMMAND_UNPARSED;
            if (used > remaining) return DDERR_INVALIDPARAMS;
        }
        if (used > remaining) return DDERR_INVALIDPARAMS;
        *offset += sizeof(*h) + used;
    }
    return DD_OK;
}

/* Diagnostics: keep the last command buffers, up to 16 after the first draw
 * that indexes past the runtime's stated vertex window. */
static void record_history(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG flags, ULONG beyond)
{
    RB2K_D7_DRAWSTATS *s = &c->display->d7draw;
    PDD_SURFACE_GLOBAL g = !(flags & D3DNTHALDP2_USERMEMVERTICES) && a->lpDDVertex ? a->lpDDVertex->lpGbl : NULL;
    ULONG *h;
    if (s->historyStop && s->historyNext >= s->historyStop) return;
    h = s->history[s->historyNext++ % 32];
    h[0] = flags;
    h[1] = a->dwVertexOffset;
    h[2] = a->dwVertexLength;
    h[3] = (flags & D3DNTHALDP2_USERMEMVERTICES) ? 0 : (ULONG)a->lpDDVertex;
    h[4] = (flags & D3DNTHALDP2_USERMEMVERTICES) ? (ULONG)a->lpVertices : g ? (ULONG)g->fpVidMem : 0;
    h[5] = g ? g->dwLinearSize : 0;
    h[6] = (flags & D3DNTHALDP2_REQVERTEXBUFSIZE) ? a->dwReqVertexBufSize : 0;
    h[7] = c->minStart;
    h[8] = c->maxEnd;
    h[9] = a->ddrval;
    if (!s->historyStop && s->beyondCalls != beyond) s->historyStop = s->historyNext + 16;
}

static DWORD APIENTRY dp2(LPD3DNTHAL_DRAWPRIMITIVES2DATA a)
{
    D7_CONTEXT *c;
    BYTE *copy = NULL;
    ULONG stride = a->dwVertexSize; /* Shares its field with the return code. */
    ULONG origin = 0;
    ULONG entryFlags = a->dwFlags, beyond = 0;
    LONGLONG start, end;
    a->dwErrorOffset = 0;
    if (!++dp2Call) ++dp2Call;
    a->ddrval = DDERR_INVALIDOBJECT;
    start = cycles();
    EngAcquireSemaphore(registry);
    c = owned_context(a->dwhContext);
    if (c) {
        EngAcquireSemaphore(c->display->ddLock);
        ++c->display->d7.commandCalls;
        ++c->display->d7draw.dp2Calls;
        c->minStart = 0xffffffff;
        c->maxEnd = 0;
        beyond = c->display->d7draw.beyondCalls;
        a->ddrval = DDERR_INVALIDPARAMS;
        if (!a->dwCommandLength) a->ddrval = DD_OK;
        else if (a->dwCommandLength <= MaxCommandBytes) {
            LONGLONG copied;
            BOOL ok;
            copy = EngAllocMem(0, a->dwCommandLength, '72bR');
            ok = copy && copy_commands(a, copy, &origin);
            copied = cycles();
            c->display->d7draw.copyTicks += copied - start;
            if (!copy) a->ddrval = DDERR_OUTOFMEMORY;
            else if (ok) {
                ULONG sites = 0, i;
                for (i = 0; i < 8; ++i) sites += c->display->d7draw.failSites[i];
                a->ddrval = commands(c, a, stride, copy, a->dwCommandLength, origin, &a->dwErrorOffset);
                for (i = 0; i < 8; ++i) sites -= c->display->d7draw.failSites[i];
                if (a->ddrval == DDERR_INVALIDPARAMS && !sites)
                    ++c->display->d7draw.failSites[a->dwErrorOffset + 4 > a->dwCommandLength ?
                                                   RB2K_D7_FAIL_LENGTH : RB2K_D7_FAIL_OTHER];
            }
            else ++c->display->d7draw.failSites[RB2K_D7_FAIL_ADDRESS];
        } else {
            ++c->display->d7draw.failSites[RB2K_D7_FAIL_LENGTH];
            c->display->d7draw.lastRange[9] = a->dwCommandLength;
        }
        record_history(c, a, entryFlags, beyond);
        /* Runtimes may issue operations outside BeginScene/EndScene.
         * Complete that command buffer before returning to the caller. */
        if (!c->inScene) ++textureEpoch;
        if ((!c->inScene || FAILED(a->ddrval)) && FAILED(finish_scene(c)) && SUCCEEDED(a->ddrval))
            a->ddrval = DDERR_SURFACELOST;
        if (flush_draws() && SUCCEEDED(a->ddrval)) a->ddrval = DDERR_GENERIC;
        /* A DirectX 9 runtime reads dwErrorOffset after success as the size of the driver's
         * responses, which this driver does not write. */
        if (SUCCEEDED(a->ddrval) && c->dx9Ddi) a->dwErrorOffset = 0;
        if (FAILED(a->ddrval)) {
            c->display->d7.lastError = a->ddrval;
            ++c->display->d7draw.dp2Failures;
            c->display->d7draw.lastFailure = a->ddrval;
            c->display->d7draw.lastFailureOp = c->display->d7.lastCommand;
            c->display->d7.errorOffset = a->dwErrorOffset;
        }
        end = cycles();
        c->display->d7draw.dp2Ticks += end - start;
        EngReleaseSemaphore(c->display->ddLock);
    }
    if (copy) EngFreeMem(copy);
    /* This driver never swaps buffers. A swap flag left set tells the runtime
     * it was handed a fresh vertex buffer, and its next start vertices then
     * run past the end of the real one (as the 9x HAL also clears them). */
    a->dwFlags &= ~(D3DNTHALDP2_SWAPVERTEXBUFFER | D3DNTHALDP2_SWAPCOMMANDBUFFER);
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

/* The runtime reads DDSURFACEDESC records here, including on DirectX 7.
 * Every format is converted to B8G8R8A8 for the host at upload. */
#define TEXTURE_FORMAT(bits, r, g, b, a) { \
    .dwSize = sizeof(DDSURFACEDESC), .dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT, \
    .ddsCaps = { .dwCaps = DDSCAPS_TEXTURE }, \
    .ddpfPixelFormat = { .dwSize = sizeof(DDPIXELFORMAT), \
        .dwFlags = DDPF_RGB | ((a) ? DDPF_ALPHAPIXELS : 0), .dwRGBBitCount = (bits), \
        .dwRBitMask = (r), .dwGBitMask = (g), .dwBBitMask = (b), .dwRGBAlphaBitMask = (a) } }
static DDSURFACEDESC textureFormats[] = {
    TEXTURE_FORMAT(32, 0xff0000, 0xff00, 0xff, 0xff000000),
    TEXTURE_FORMAT(32, 0xff0000, 0xff00, 0xff, 0),
    TEXTURE_FORMAT(16, 0xf800, 0x07e0, 0x001f, 0),
    TEXTURE_FORMAT(16, 0x7c00, 0x03e0, 0x001f, 0x8000),
    TEXTURE_FORMAT(16, 0x0f00, 0x00f0, 0x000f, 0xf000),
    TEXTURE_FORMAT(16, 0x7c00, 0x03e0, 0x001f, 0)
};
#define TRI_CAPS { \
    .dwSize = sizeof(D3DPRIMCAPS), \
    .dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_MASKZ, \
    .dwRasterCaps = D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | \
                    D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG, \
    .dwAlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL | \
                      D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS, \
    .dwSrcBlendCaps = BLEND_CAPS, .dwDestBlendCaps = BLEND_CAPS, \
    .dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | \
                   D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | \
                   D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND, \
    .dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA, \
    .dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | \
                           D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR | \
                           D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR | \
                           D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | \
                           D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | \
                           D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR, \
    .dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA | \
                          D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_ADD, \
    .dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | \
                            D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV }
#define BLEND_CAPS (D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | \
    D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | \
    D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | \
    D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | \
    D3DPBLENDCAPS_BOTHINVSRCALPHA)
static const D3DNTHAL_GLOBALDRIVERDATA global = {
    .dwSize = sizeof(D3DNTHAL_GLOBALDRIVERDATA),
    .dwNumTextureFormats = sizeof(textureFormats) / sizeof(textureFormats[0]),
    .lpTextureFormats = textureFormats,
    .hwCaps = {
        .dwSize = sizeof(D3DNTHALDEVICEDESC_V1),
        .dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRICAPS | D3DDD_TRANSFORMCAPS |
                   D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING | D3DDD_LINECAPS | D3DDD_DEVICERENDERBITDEPTH,
        .dcmColorModel = D3DCOLOR_RGB,
        /* The host transforms, lights and clips object-space vertices, so
         * the runtime also offers a T&L device and hands over vertex
         * buffers as the program wrote them. */
        .dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                     D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                     D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_DRAWPRIMITIVES2 |
                     D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION |
                     D3DDEVCAPS_HWTRANSFORMANDLIGHT,
        .dtcTransformCaps = { .dwSize = sizeof(D3DTRANSFORMCAPS), .dwCaps = D3DTRANSFORMCAPS_CLIP },
        .bClipping = TRUE,
        .dlcLightingCaps = { .dwSize = sizeof(D3DLIGHTINGCAPS),
            .dwCaps = D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL,
            .dwLightingModel = D3DLIGHTINGMODEL_RGB, .dwNumLights = RB2K_D7_LIGHTS },
        .dpcLineCaps = TRI_CAPS,   /* lines are drawn by the same pipeline */
        .dpcTriCaps = TRI_CAPS,
        .dwDeviceRenderBitDepth = DDBD_32
    }
};
static const D3DNTHAL_CALLBACKS cb = {
    .dwSize = sizeof(D3DNTHAL_CALLBACKS), .ContextCreate = create_context,
    .ContextDestroy = destroy_context
};

void rb_d7_caps(DD_HALINFO *info, BOOL depth)
{
    static D3DNTHAL_GLOBALDRIVERDATA withDepth;
    if (!registry) return;
    info->ddCaps.dwCaps |= DDCAPS_3D;
    info->lpD3DGlobalDriverData = (PVOID)&global;
    if (depth) {
        EngAcquireSemaphore(registry);
        if (!withDepth.dwSize) {
            withDepth = global;
            withDepth.hwCaps.dwFlags |= D3DDD_DEVICEZBUFFERBITDEPTH;
            withDepth.hwCaps.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_32;
            withDepth.hwCaps.dpcTriCaps.dwRasterCaps |= D3DPRASTERCAPS_ZTEST;
            withDepth.hwCaps.dpcTriCaps.dwZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS |
                D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER |
                D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
            withDepth.hwCaps.dpcLineCaps.dwRasterCaps = withDepth.hwCaps.dpcTriCaps.dwRasterCaps;
            withDepth.hwCaps.dpcLineCaps.dwZCmpCaps = withDepth.hwCaps.dpcTriCaps.dwZCmpCaps;
        }
        EngReleaseSemaphore(registry);
        info->ddCaps.dwZBufferBitDepths = DDBD_16 | DDBD_32;
        info->lpD3DGlobalDriverData = &withDepth;
    }
    info->lpD3DHALCallbacks = (PVOID)&cb;
}

/* The Direct3D 8 interface describes the same device as the Direct3D 7
 * one, as the runtime itself translates it for a Direct3D 7 driver, with
 * one vertex stream. Pixel shaders are offered (see below); the runtime
 * runs vertex shaders in software. */
static const ULONG d8Formats[][2] = {
    { RbFmtX8R8G8B8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatOffscreenTarget | RbFormatSameTarget |
                     RbFormatDisplayMode | RbFormat3D },
    /* a back buffer with alpha over the X8R8G8B8 desktop, or a target */
    { RbFmtA8R8G8B8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatOffscreenTarget | RbFormatSameTargetUpToAlpha },
    { RbFmtR5G6B5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatDisplayMode },
    { RbFmtX1R5G5B5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA1R5G5B5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA4R4G4B4, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    /* DXT: also volumes, which are decoded here */
    { RbFmtDxt1, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtDxt2, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtDxt3, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtDxt4, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtDxt5, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    /* converted to 32 bits on upload */
    { RbFmtL8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtL16, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA8L8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    { RbFmtA8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture },
    /* bump maps, sampled as signed texels */
    { RbFmtV8U8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatBumpMap },
    { RbFmtQ8W8V8U8, RbFormatTexture | RbFormatCubeTexture | RbFormatVolumeTexture | RbFormatBumpMap },
    /* last: only with depth rendering */
    { RbFmtD16, RbFormatZStencil }, { RbFmtD24S8, RbFormatZStencil }, { RbFmtD24X8, RbFormatZStencil }
};

static ULONG d8_format_count(const DISPLAY *d)
{
    ULONG n = sizeof(d8Formats) / sizeof(d8Formats[0]);
    return d->z16Render ? n : n - 3;
}

/* With the Direct3D 9 driver interface: what the runtime requires of a DirectX 9 driver before
 * it offers its hardware device (the debug runtime names each check), in both caps records: 8
 * streams, separate fog in the vertex format, no line patterns, the scissor test, dynamic
 * textures, no power-of-two-only textures, UBYTE4 vertex data. */
static void d9_ddi_caps(RB_D3DCAPS8 *c)
{
    c->maxStreams = RB_VS_STREAMS;
    c->primitiveMiscCaps = (c->primitiveMiscCaps | RB_D3DPMISCCAPS_FOGINFVF) & ~RB_D3DPMISCCAPS_LINEPATTERNREP;
    c->rasterCaps = (c->rasterCaps | RB_D3DPRASTERCAPS_SCISSORTEST) & ~D3DPRASTERCAPS_PAT;
    c->caps2 |= RB_D3DCAPS2_DYNAMICTEXTURES;
    if (c->textureCaps & D3DPTEXTURECAPS_POW2) c->textureCaps |= D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
    c->vertexProcessingCaps &= ~RB_D3DVTXPCAPS_NO_VSDT_UBYTE4;
}

static void d8_caps(const DISPLAY *d, RB_D3DCAPS8 *c, const D3DNTHAL_D3DEXTENDEDCAPS *x)
{
    const D3DNTHALDEVICEDESC_V1 *hw = &global.hwCaps;
    const D3DPRIMCAPS *tri = &hw->dpcTriCaps;
    RtlZeroMemory(c, sizeof(*c));
    c->deviceType = 1;                  /* D3DDEVTYPE_HAL */
    /* D3DCAPS2_PRESENT_INTERVAL_IMMEDIATE (d3dhal.h, for drivers): a Direct3D 8 or 9 runtime derives
     * D3DPRESENT_INTERVAL_IMMEDIATE from it, and no present here waits for the vertical blank.
     * 3DMark05 refuses hardware without that interval. */
    c->caps2 = RB_D3DCAPS2_CANRENDERWINDOWED | 0x00400000;
    c->presentationIntervals = RB_D3DPRESENT_INTERVAL_IMMEDIATE | RB_D3DPRESENT_INTERVAL_ONE;
    /* Not texturing from system memory: only video-memory textures are registered
     * and uploaded, so the runtime is to keep a video-memory copy of each. */
    c->devCaps = hw->dwDevCaps & ~(D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_TEXTURESYSTEMMEMORY);
    c->primitiveMiscCaps = tri->dwMiscCaps | 0x80;   /* D3DPMISCCAPS_COLORWRITEENABLE */
    c->rasterCaps = tri->dwRasterCaps & ~D3DPRASTERCAPS_SUBPIXEL;
    c->srcBlendCaps = tri->dwSrcBlendCaps;
    c->destBlendCaps = tri->dwDestBlendCaps;
    c->alphaCmpCaps = tri->dwAlphaCmpCaps;
    c->shadeCaps = tri->dwShadeCaps;
    c->textureCaps = tri->dwTextureCaps | 0x4000;     /* D3DPTEXTURECAPS_MIPMAP */
    /* The stage programs divide per pixel for D3DTTFF_PROJECTED; without this cap the runtime
     * hands pretransformed vertices over as COUNT3 without the flag. */
    c->textureCaps |= 0x400;             /* D3DPTEXTURECAPS_PROJECTED */
    c->textureFilterCaps = tri->dwTextureFilterCaps;
    /* Cube maps, sampled by pixel shaders (the fixed-function stages do not). */
    c->textureCaps |= 0x800 | 0x10000;   /* D3DPTEXTURECAPS_CUBEMAP, MIPCUBEMAP */
    c->cubeTextureFilterCaps = c->textureFilterCaps;
    /* Volumes too, also only for pixel shaders. */
    c->textureCaps |= 0x2000 | 0x8000;   /* D3DPTEXTURECAPS_VOLUMEMAP, MIPVOLUMEMAP */
    c->volumeTextureFilterCaps = c->textureFilterCaps;
    c->maxVolumeExtent = RB2K_D7_VOLUME_SIDE;
    c->textureAddressCaps = tri->dwTextureAddressCaps;
    c->volumeTextureAddressCaps = c->textureAddressCaps;
    c->stencilCaps = d->z24s8 ? 0xff : 0;
    if (d->z16Render) {
        c->rasterCaps |= D3DPRASTERCAPS_ZTEST;
        c->zCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                      D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    }
    c->maxTextureWidth = c->maxTextureHeight = RB2K_D7_TEXTURE_SIDE;
    c->maxTextureRepeat = x->dwMaxTextureRepeat;
    c->maxTextureAspectRatio = x->dwMaxTextureAspectRatio;
    c->maxAnisotropy = 1;
    c->maxVertexW = 0x501502f9;                         /* 1e10 */
    c->guardBandLeft = c->guardBandTop = 0xc5800000;    /* -4096 */
    c->guardBandRight = c->guardBandBottom = 0x457ff000; /* 4095 */
    c->fvfCaps = x->dwFVFCaps;
    c->textureOpCaps = x->dwTextureOpCaps;
    c->maxTextureBlendStages = x->wMaxTextureBlendStages;
    c->maxSimultaneousTextures = x->wMaxSimultaneousTextures;
    c->vertexProcessingCaps = x->dwVertexProcessingCaps & ~D3DVTXPCAPS_VERTEXFOG;
    c->maxActiveLights = x->dwMaxActiveLights;
    c->maxUserClipPlanes = x->wMaxUserClipPlanes;   /* in world space for object-space draws, in clip space for vertex shaders */
    c->maxPointSize = RB2K_POINT_SIZE_MAX;              /* 64 */
    c->maxPrimitiveCount = 0xffff;
    c->maxVertexIndex = 0xffffff;   /* past 0xffff: 32-bit index buffers */
    /* More streams only for vertex shaders, whose declarations say where each is read. */
    c->maxStreams = d->vertexShaders ? RB_VS_STREAMS : 1;
    c->maxStreamStride = 255;
    c->vertexShaderVersion = 0xfffe0000;            /* version 0.0 */
    if (d->vertexShaders) {
        c->vertexShaderVersion = d->d7draw.d8Version >= 0x900 ? 0xfffe0200 : 0xfffe0101;
        c->maxVertexShaderConst = d->d7draw.d8Version >= 0x900 ? 256 : 96;
    }
    /* 1.3 for Direct3D 8; shader model 2 when the runtime that asks has
     * announced itself as Direct3D 9, which uses this record for drivers of
     * the Direct3D 8 interface. */
    c->pixelShaderVersion = d->d7draw.d8Version >= 0x900 ? 0xffff0200 : 0xffff0104;   /* 1.4 for Direct3D 8 programs */
    c->maxSimultaneousTextures = max(stage_limit(d), 6UL);   /* pixel shader 1.4 samples six */
    /* The fixed-function stages run as a stage program (d3d8_stages_valid), which also
     * does bump mapping. */
    c->maxTextureBlendStages = stage_limit(d);
    c->textureOpCaps |= D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE;
    c->maxPixelShaderValue = 0x3f800000;            /* 1 */
    if (d->d3d9Ddi && d->d7draw.d8Version >= 0x900) d9_ddi_caps(c);
}

/* D3DCAPS9 for a runtime that has announced Direct3D 9: the Direct3D 8 record and the parts of the
 * DirectX 9 one that the runtime otherwise leaves at zero, among them the shader model 2 details
 * (the minimums of versions 2.0, which the translators meet) that programs such as 3DMark05 test.
 * One render target; of the DirectX 9 vertex types, the vertex shaders read UBYTE4. */
static void d9_caps(const DISPLAY *d, RB_D3DCAPS9 *c, const D3DNTHAL_D3DEXTENDEDCAPS *x)
{
    RtlZeroMemory(c, sizeof(*c));
    d8_caps(d, &c->caps8, x);
    c->numberOfAdaptersInGroup = 1;
    c->numSimultaneousRTs = 1;
    /* SETSTREAMSOURCE2 offsets are applied; the runtime requires this of a DirectX 9 driver. */
    if (d->d3d9Ddi) c->devCaps2 |= RB_D3DDEVCAPS2_STREAMOFFSET;
    c->stretchRectFilterCaps = RB_D3DPTFILTERCAPS_MINFPOINT | RB_D3DPTFILTERCAPS_MAGFPOINT;
    /* The Direct3D 9 interface blits any rectangles on the host, point-sampled or linear. */
    if (d->d3d9Ddi) c->stretchRectFilterCaps |= RB_D3DPTFILTERCAPS_MINFLINEAR | RB_D3DPTFILTERCAPS_MAGFLINEAR;
    if (c->caps8.vertexShaderVersion >= 0xfffe0200) {
        c->declTypes = RB_D3DDTCAPS_UBYTE4;
        c->vs20NumTemps = 12;
        c->vs20StaticFlowControlDepth = 1;
        c->maxVShaderInstructionsExecuted = 65535;
    }
    if (c->caps8.pixelShaderVersion >= 0xffff0200) {
        c->ps20NumTemps = 12;
        c->ps20NumInstructionSlots = 96;
        c->maxPShaderInstructionsExecuted = 96;
    }
}

/* GetDriverInfo2 requests arrive with the stereo-mode GUID. The size of the
 * reply buffer is the one in the request; the outer size is not used. */
static BOOL d8_info(DISPLAY *d, PDD_GETDRIVERINFODATA a, const D3DNTHAL_D3DEXTENDEDCAPS *x)
{
    RB_GDI2_HEADER *h = a->lpvData;
    ULONG size, room;
    if (!h || h->magic != RB_GDI2_MAGIC) return FALSE;
    room = h->expectedSize;
    EngAcquireSemaphore(d->ddLock);
    ++d->d7draw.d8Requests[h->type < 31 ? h->type : 31];
    if (d->d7draw.d8LogCount < 24) {
        d->d7draw.d8Log[d->d7draw.d8LogCount][0] = h->type;
        d->d7draw.d8Log[d->d7draw.d8LogCount++][1] = room;
    }
    EngReleaseSemaphore(d->ddLock);
    switch (h->type) {
    case RbGdi2Caps8: {
        RB_D3DCAPS8 caps;
        d8_caps(d, &caps, x);
        size = sizeof(caps);
        size = min(size, room);
        RtlCopyMemory(h, &caps, size);
        break;
    }
    case RbGdi2Caps9: {
        RB_D3DCAPS9 caps;
        if (!d->d3d9Ddi || d->d7draw.d8Version < 0x900) goto unavailable;
        d9_caps(d, &caps, x);
        size = sizeof(caps);
        size = min(size, room);
        RtlCopyMemory(h, &caps, size);
        break;
    }
    case RbGdi2QueryCount: {
        /* The Direct3D 9 interface: no query types (the runtime then issues none). */
        ULONG *count = (ULONG *)(h + 1);
        if (!d->d3d9Ddi || room < sizeof(*h) + sizeof(ULONG)) goto unavailable;
        *count = 0;
        size = sizeof(*h) + sizeof(ULONG);
        break;
    }
    case RbGdi2DdiVersion: {
        /* The Direct3D 9 interface for a DirectX 9 runtime, when it is switched on. */
        RB_GDI2_DDI_VERSION *v = (RB_GDI2_DDI_VERSION *)h;
        if (!d->d3d9Ddi || room < sizeof(*v)) goto unavailable;
        v->ddiVersion = v->dxVersion >= 9 ? RB_DX9_DDI_VERSION : 0;
        size = sizeof(*v);
        break;
    }
    case RbGdi2FormatCount: {
        RB_GDI2_FORMAT_COUNT *f = (RB_GDI2_FORMAT_COUNT *)h;
        if (room < sizeof(*f)) goto unavailable;
        f->count = d8_format_count(d);
        f->reserved = 0;
        size = sizeof(*f);
        break;
    }
    case RbGdi2Format: {
        RB_GDI2_FORMAT *f = (RB_GDI2_FORMAT *)h;
        ULONG index = f->index;
        if (room < sizeof(*f) || index >= d8_format_count(d)) goto unavailable;
        RtlZeroMemory(&f->format, sizeof(f->format));
        f->format.dwSize = sizeof(f->format);
        f->format.dwFlags = RB_DDPF_D3DFORMAT;
        f->format.dwFourCC = d8Formats[index][0];
        f->format.dwRBitMask = d8Formats[index][1];     /* the operations */
        size = sizeof(*f);
        break;
    }
    case RbGdi2DxVersion: {
        RB_GDI2_DX_VERSION *v = (RB_GDI2_DX_VERSION *)h;
        if (room < sizeof(*v)) goto unavailable;
        d->d7draw.d8Version = v->version;
        size = sizeof(*v);
        break;
    }
    default:
        goto unavailable;
    }
    a->dwActualSize = size;
    a->ddRVal = DD_OK;
    return TRUE;
unavailable:
    /* Requests this driver does not answer, such as most of the Direct3D 9 ones. */
    a->dwActualSize = 0;
    a->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    return TRUE;
}

BOOL rb_d7_info(PDD_GETDRIVERINFODATA a)
{
    static const GUID stereo = {0xf828169c,0xa8e8,0x11d2,{0xa1,0xf2,0x00,0xa0,0xc9,0x83,0xea,0xf6}};
    static const GUID g3 = {0xddf41230,0xec0a,0x11d0,{0xa9,0xb6,0x00,0xaa,0x00,0xc0,0x99,0x3e}};
    static const GUID gm = {0x406b2f00,0x3e5a,0x11d1,{0xb6,0x40,0x00,0xaa,0x00,0xa1,0xf9,0x6a}};
    static const GUID parser = {0x2e04ffa0,0x98e4,0x11d1,{0x8c,0xe1,0x00,0xa0,0xc9,0x06,0x29,0xa8}};
    static const GUID depth = {0x93869880,0x36cf,0x11d1,{0x9b,0x1b,0x00,0xaa,0x00,0xbb,0xb8,0xae}};
    static const GUID extended = {0x7de41f80,0x9d93,0x11d0,{0x89,0xab,0x00,0xa0,0xc9,0x05,0x41,0x29}};
    /* Return the colour-only descriptor when paired with an older miniport. */
    static const DWORD depthFormats = 0;
    /* 16 bits of depth, and 24 with 8 of stencil. */
    static const struct { DWORD count; DDPIXELFORMAT format[2]; } z16 = {
        2, { { .dwSize = sizeof(DDPIXELFORMAT), .dwFlags = DDPF_ZBUFFER,
               .dwZBufferBitDepth = 16, .dwZBitMask = 0xffff },
             { .dwSize = sizeof(DDPIXELFORMAT), .dwFlags = DDPF_ZBUFFER | DDPF_STENCILBUFFER,
               .dwZBufferBitDepth = 32, .dwStencilBitDepth = 8, .dwZBitMask = 0xffffff00,
               .dwStencilBitMask = 0xff } }
    };
    static const D3DNTHAL_CALLBACKS3 c3 = {
        .dwSize = sizeof(c3),
        .dwFlags = D3DNTHAL3_CB32_DRAWPRIMITIVES2 | D3DNTHAL3_CB32_VALIDATETEXTURESTAGESTATE,
        .ValidateTextureStageState = validate_stages, .DrawPrimitives2 = dp2
    };
    static const DD_MISCELLANEOUS2CALLBACKS misc = {
        .dwSize = sizeof(misc),
        .dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE |
                   DDHAL_MISC2CB32_DESTROYDDLOCAL,
        .CreateSurfaceEx = register_surface, .GetDriverState = get_driver_state,
        .DestroyDDLocal = destroy_local
    };
    /* The guard band lets the runtime pass screen-edge triangles unclipped;
     * the host clips them. The floats are written as IEEE bit patterns. */
    static const D3DNTHAL_D3DEXTENDEDCAPS extendedCaps = {
        .dwSize = sizeof(D3DNTHAL_D3DEXTENDEDCAPS),
        .dwMinTextureWidth = 1, .dwMinTextureHeight = 1,
        .dwMaxTextureWidth = RB2K_D7_TEXTURE_SIDE, .dwMaxTextureHeight = RB2K_D7_TEXTURE_SIDE,
        .dwMaxTextureRepeat = 2048, .dwMaxTextureAspectRatio = RB2K_D7_TEXTURE_SIDE, .dwMaxAnisotropy = 1,
        .dvGuardBandLeft = -4096.0f, .dvGuardBandTop = -4096.0f,
        .dvGuardBandRight = 4095.0f, .dvGuardBandBottom = 4095.0f,
        .dwFVFCaps = 8,
        .dwStencilCaps = 0xff,  /* KEEP, ZERO, REPLACE, INCRSAT, DECRSAT, INVERT, INCR, DECR */
        .dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 |
            D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
            D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X |
            D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
            D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
            D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_DOTPRODUCT3,
        .wMaxTextureBlendStages = 2, .wMaxSimultaneousTextures = 2, .wMaxVertexBlendMatrices = 1,
        .dvMaxVertexW = 1.0e10f, .dwMaxActiveLights = RB2K_D7_LIGHTS, .wMaxUserClipPlanes = 6,
        .dwVertexProcessingCaps = D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                                  D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_VERTEXFOG
    };
    D3DNTHAL_D3DEXTENDEDCAPS caps;
    const void *source;
    DISPLAY *d = (DISPLAY *)a->dhpdev;
    ULONG size, i;
    if (!registry) return FALSE;
    EngAcquireSemaphore(d->ddLock);
    for (i = 0; i < d->d7.queryCount; ++i)
        if (d->d7.queryIds[i] == a->guidInfo.Data1) break;
    if (i == d->d7.queryCount && i < RB2K_D3D7_QUERY_LIMIT)
        d->d7.queryIds[d->d7.queryCount++] = a->guidInfo.Data1;
    /* For this GUID the graphics engine passes its own function, not an
     * output buffer. Acknowledge it without writing through lpvData. */
    if (IsEqualGUID(&a->guidInfo, &parser)) {
        d->parseD3DExtension = (PFND3DNTPARSEUNKNOWNCOMMAND)a->lpvData;
        a->dwActualSize = 0;
        a->ddRVal = a->lpvData ? DD_OK : DDERR_CURRENTLYNOTAVAIL;
        EngReleaseSemaphore(d->ddLock);
        return TRUE;
    }
    EngReleaseSemaphore(d->ddLock);
    if (IsEqualGUID(&a->guidInfo, &stereo)) return d->d3d8 && d8_info(d, a, &extendedCaps);
    if (IsEqualGUID(&a->guidInfo, &g3)) { source = &c3; size = sizeof(c3); }
    else if (IsEqualGUID(&a->guidInfo, &gm)) { source = &misc; size = sizeof(misc); }
    else if (IsEqualGUID(&a->guidInfo, &depth)) {
        if (d->z16Render) { source = &z16; size = sizeof(z16); }
        else { source = &depthFormats; size = sizeof(depthFormats); }
    }
    else if (IsEqualGUID(&a->guidInfo, &extended)) {
        /* DX6 and DX7 request different lengths of this record. */
        caps = extendedCaps;
        caps.dwSize = min(a->dwExpectedSize, sizeof(caps));
        source = &caps;
        size = caps.dwSize;
    }
    else return FALSE;
    {
        EngAcquireSemaphore(d->ddLock);
        if (source == &c3) ++d->d7.callbackQueries;
        else if (source == &misc) ++d->d7.surfaceQueries;
        EngReleaseSemaphore(d->ddLock);
    }
    if (a->dwExpectedSize && !a->lpvData) return FALSE;
    if (a->dwExpectedSize) RtlCopyMemory(a->lpvData, source, min(size, a->dwExpectedSize));
    a->dwActualSize = size;
    a->ddRVal = DD_OK;
    return TRUE;
}

ULONG rb_d7_stats(DISPLAY *d, ULONG bytes, PVOID out)
{
    ULONG i;
    RB2K_D3D7_DIAGNOSTICS s;
    if (!registry || !d->ddLock || (bytes != sizeof(s) && bytes != sizeof(s.totals) &&
        bytes != FIELD_OFFSET(RB2K_D3D7_DIAGNOSTICS, halInfoCalls) &&
        bytes != FIELD_OFFSET(RB2K_D3D7_DIAGNOSTICS, queryCount))) return 0;
    EngAcquireSemaphore(registry);
    EngAcquireSemaphore(d->ddLock);
    s = d->d7;
    if (d->d7.nativeEnabled) {
        RB2K_D3D7_COMPLETED completed;
        ULONG written = 0;
        if (EngDeviceIoControl(d->miniport, IOCTL_RB2K_D3D7_COMPLETED, NULL, 0,
                               &completed, sizeof(completed), &written) || written != sizeof(completed)) {
            EngReleaseSemaphore(d->ddLock);
            EngReleaseSemaphore(registry);
            return 0;
        }
        s.totals.clears = completed.clears;
        s.totals.triangles = completed.triangles;
    }
    s.totals.version = RB2K_D3D7_STATS_VERSION;
    for (i = 0; i < MaxContexts; ++i) if (contexts[i].display == d) ++s.totals.contexts;
    for (i = 0; i < RB2K_DD_SLOTS; ++i) if (d->slots[i].d3dHandle) ++s.totals.targets;
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    RtlCopyMemory(out, &s, bytes);
    return 1;
}

ULONG rb_d7_drawstats(DISPLAY *d, ULONG bytes, PVOID out)
{
    if (!registry || !d->ddLock || bytes != sizeof(RB2K_D7_DRAWSTATS)) return 0;
    EngAcquireSemaphore(registry);
    EngAcquireSemaphore(d->ddLock);
    d->d7draw.version = RB2K_D7_DRAWSTATS_VERSION;
    d->d7draw.frequency = cycle_rate();
    d->d7draw.codeAt = (ULONG)(ULONG_PTR)rb_d7_drawstats;
    RtlCopyMemory(d->d7draw.ddModes, rb_dd_mode_stats, sizeof(rb_dd_mode_stats));
    RtlCopyMemory(out, &d->d7draw, bytes);
    {
        RB2K_D7_DRAWSTATS *o = out;
        ULONG i;
        o->hostDraws += hostGone.draws; o->hostBuffers += hostGone.buffers; o->hostSummed += hostGone.summed;
        o->hostUploads += hostGone.uploads; o->hostUploadBytes += hostGone.uploadBytes; o->hostScans += hostGone.scans;
        o->hostScanHits += hostGone.scanHits; o->hostRewrites += hostGone.rewrites; o->hostStreamed += hostGone.streamed;
        o->hostArenas += hostGone.arenas; o->hostSumSkips += hostGone.sumSkips; o->hostStreamHits += hostGone.streamHits;
        for (i = 0; i < 8; ++i) o->hostLeft[i] += hostGone.left[i];
        o->hostProgramFails = hostProgramFails.count;
        RtlCopyMemory(o->hostProgramReasons, hostProgramFails.reasons, sizeof(o->hostProgramReasons));
        o->hostProgramDetail = hostProgramFails.detail;
        o->hostProgramElements = hostProgramFails.elements;
        o->hostProgramBytes = hostProgramFails.bytes;
        RtlCopyMemory(o->hostProgramDecl, hostProgramFails.decl, sizeof(o->hostProgramDecl));
    }
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    return 1;
}
