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
       MaxBuffers = 1024, MaxShaders = 32, MaxVertexShaders = 32 };
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
 * programs without and with oFog in the specular alpha (zero until made;
 * failed when it cannot be translated). */
typedef struct {
    ULONG handle, declCount, tokenCount, constants, words;
    ULONG *decl, *tokens;
    ULONG program[2];
    BOOL failed[2];
} D8_VSHADER;
/* A pixel program made from the texture stage states, for draws with a
 * vertex shader and no pixel shader: the states it was made from (seven
 * words a stage, then specular and fog), its program and what it samples. */
enum { StagePrograms = 8, StageKey = 7 * 8 + 2 };
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
    BOOL dx8, streamUser;
    ULONG fvf, pixelShader, streamHandle, streamStride, indexHandle, indexStride;
    BOOL inScene, queued;
    ULONG minStart, maxEnd;     /* diagnostics for one command buffer */
    RB2K_D3D7_DRAW draw;        /* the batch being filled */
    D8_SHADER shaders[MaxShaders];
    ULONG psConst[32][4];       /* pixel shader constants c0..c31 */
    D8_VSHADER vshaders[MaxVertexShaders];
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
    FLATPTR levelAddress[6][RB2K_D7_LEVELS];
    /* Video-memory textures are written only through DirectDraw locks and
     * blits, which mark them dirty; others are checked once a scene. */
    BOOL tracked, dirty;
    ULONG host, sum, epoch;
    ULONG targets[RB2K_DD_SLOTS];
} D7_TEXTURE;

/* A Direct3D 8 vertex or index buffer registered through CreateSurfaceEx.
 * The runtime keeps both in the owning process's memory; the address and
 * size are read from the surface at each draw. */
typedef struct {
    PDD_DIRECTDRAW_LOCAL local;
    PDD_SURFACE_GLOBAL global;
    DISPLAY *display;
    HANDLE owner;
    ULONG handle;
} D8_BUFFER;

static void release_shaders(D7_CONTEXT *c);
static void release_blocks(D7_CONTEXT *c);
static void stencil_state(const D7_CONTEXT *c, const DD_SLOT *z, ULONG *stencil, ULONG *ref);
static HSEMAPHORE registry;
static D8_BUFFER *buffers;
static WORD *indexScratch;      /* indices copied out of the process before use */
static RB2K_D3D8_PROGRAM *programScratch;   /* a translated pixel shader on its way */
static ULONG indexScratchCount;
static D7_CONTEXT contexts[MaxContexts];
static ULONG lastContext;
static D7_TEXTURE *textures;
static ULONG textureEpoch = 1;
static BYTE *texelScratch, *uploadScratch;
static ULONG texelScratchBytes;

/* A context-only callback takes the registry first, then the display lock.
 * DrvDisableDirectDraw drops the display lock before retiring the contexts. */
void rb_d7_init(void)
{
    if (!registry) registry = EngCreateSemaphore();
    if (!textures) textures = EngAllocMem(FL_ZERO_MEMORY, MaxTextures * sizeof(*textures), 't7bR');
    if (!buffers) buffers = EngAllocMem(FL_ZERO_MEMORY, MaxBuffers * sizeof(*buffers), 'b8bR');
    if (!programScratch) programScratch = EngAllocMem(0, sizeof(*programScratch), 's8bR');
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

static ULONG flush_draws(void)
{
    DISPLAY *d = drawQueueDisplay;
    ULONG written = 0, error;
    LONGLONG start, end;
    if (!drawQueueBytes) return 0;
    EngQueryPerformanceCounter(&start);
    error = EngDeviceIoControl(d->miniport, IOCTL_RB2K_D3D7_DRAWS, drawQueue, drawQueueBytes, NULL, 0, &written);
    EngQueryPerformanceCounter(&end);
    d->d7draw.drawTicks += end - start;
    ++d->d7draw.drawRequests;
    drawQueueBytes = 0;
    if (error) {
        ULONG i;
        ++d->d7draw.drawFailures;
        d->d7draw.lastDrawError = error;
        /* A transport reset releases the host textures: upload them again. */
        for (i = 0; textures && i < MaxTextures; ++i)
            if (textures[i].display == d) {
                textures[i].host = 0;
                RtlZeroMemory(textures[i].targets, sizeof(textures[i].targets));
            }
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

void rb_d7_disable(DISPLAY *d)
{
    ULONG i;
    if (!registry) return;
    EngAcquireSemaphore(registry);
    for (i = 0; i < MaxContexts; ++i)
        if (contexts[i].display == d) {
            release_shaders(&contexts[i]);
            RtlZeroMemory(&contexts[i], sizeof(contexts[i]));
        }
    /* Every DirectDraw object of this display is going away. */
    for (i = 0; textures && i < MaxTextures; ++i)
        if (textures[i].display == d) release_texture(&textures[i], TRUE);
    for (i = 0; buffers && i < MaxBuffers; ++i)
        if (buffers[i].display == d) RtlZeroMemory(&buffers[i], sizeof(buffers[i]));
    EngReleaseSemaphore(registry);
}

static D7_TEXTURE *find_texture(PDD_DIRECTDRAW_LOCAL local, ULONG handle)
{
    ULONG i;
    for (i = 0; textures && handle && i < MaxTextures; ++i)
        if (textures[i].handle == handle && textures[i].local == local) return &textures[i];
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
        case RbFmtDxt1: return TexDxt1;
        case RbFmtDxt2: case RbFmtDxt3: return TexDxt3;
        case RbFmtDxt4: case RbFmtDxt5: return TexDxt5;
        }
        return TexNone;
    }
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
    return format == TexArgb8888 || format == TexXrgb8888 ? 4 : 2;
}

static BOOL compressed(ULONG format) { return format >= TexDxt1 && format <= TexDxt5; }

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
    if (!g->fpVidMem) return;
    for (i = 0; i < MaxTextures && textures[i].handle; ++i) {}
    if (i == MaxTextures) return;
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
    if (t->mipmaps) ++d->d7draw.regMipmaps;
    t->tracked = !(l->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY);
    t->dirty = TRUE;
    t->faces = 1;
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
            levels = face_levels(t, k, top[k], TRUE, &stop);
            if (levels < t->levels) t->levels = levels;
        }
        if (t->width != t->height) t->format = TexNone;
        ++d->d7draw.cubeTextures;
    } else {
        ULONG stop;
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
    ++d->d7draw.registered[t->format < 7 ? t->format : 7];   /* 7: compressed */
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
    if (b) RtlZeroMemory(b, sizeof(*b));
    if (!l->lpGbl->fpVidMem) return;
    for (i = 0; i < MaxBuffers && buffers[i].handle; ++i) {}
    if (i == MaxBuffers) return;
    b = &buffers[i];
    b->local = local;
    b->global = l->lpGbl;
    b->display = d;
    b->owner = EngGetCurrentProcessId();
    b->handle = handle;
    ++d->d7draw.d8Buffers;
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
    if ((l->ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)) == DDSCAPS_TEXTURE) {
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
            release_shaders(&contexts[i]);
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
        if (buffers[i].local == a->pDDLcl) RtlZeroMemory(&buffers[i], sizeof(buffers[i]));
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
        z->width != s->width || z->height != s->height))) a->ddrval = DDERR_SURFACELOST;
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
            c->viewport[2] = s->width;
            c->viewport[3] = s->height;
            c->zRange[1] = 0x3f800000;
            default_stages(c);
            /* The Direct3D 8 and 9 runtimes pass 4 here; Direct3D 7 passes 3. */
            c->dx8 = kind >= 4;
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
        release_shaders(c);
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
    if (!ok || colorOp < D3DTOP_SELECTARG1 || colorOp > D3DTOP_BLENDCURRENTALPHA ||
        alphaOp < D3DTOP_SELECTARG1 || alphaOp > D3DTOP_BLENDCURRENTALPHA) return StageUnsupported;
    *color = colorOp | (c1 << 8) | (c2 << 16);
    *alpha = alphaOp | (a1 << 8) | (a2 << 16);
    *texture = samples_texture(colorOp, c1, c2) || samples_texture(alphaOp, a1, a2);
    return StageOk;
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

static HRESULT render_surfaces(D7_CONTEXT *c, DD_SLOT **color, DD_SLOT **depth)
{
    DD_SLOT *s = target(c, c->target, FALSE);
    DD_SLOT *z = c->depth ? target(c, c->depth, FALSE) : NULL;
    if (!s || s->depth16 || (c->depth && (!z || !z->depth16 ||
        z->width != s->width || z->height != s->height))) return DDERR_SURFACELOST;
    if (s->locked || (z && z->locked)) return DDERR_SURFACEBUSY;
    *color = s;
    *depth = z;
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
    }
    return 0;
}

/* Read the level-zero texels into the scratch buffer, tightly packed. */
/* The texels of every level, tightly packed level after level. */
static ULONG level_side(ULONG size, ULONG level) { return size >> level ? size >> level : 1; }

static BOOL read_texels(D7_TEXTURE *t, ULONG *sum)
{
    ULONG bytes = 0, level, face, y, i, h = 0x811c9dc5;
    BYTE *out;
    for (level = 0; level < t->levels; ++level)
        bytes += rb_d7_row_bytes(t->format, level_side(t->width, level)) * rb_d7_rows(t->format, level_side(t->height, level));
    bytes *= t->faces;
    if (bytes > texelScratchBytes) {
        if (texelScratch) EngFreeMem(texelScratch);
        texelScratch = EngAllocMem(0, bytes, 'r7bR');
        texelScratchBytes = texelScratch ? bytes : 0;
        if (!texelScratch) return FALSE;
    }
    out = texelScratch;
    __try {
        for (face = 0; face < t->faces; ++face)
            for (level = 0; level < t->levels; ++level) {
                ULONG row = rb_d7_row_bytes(t->format, level_side(t->width, level));
                ULONG rows = rb_d7_rows(t->format, level_side(t->height, level)), pitch = t->levelPitch[face][level];
                const BYTE *in = (const BYTE *)t->levelAddress[face][level];
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
    ULONG bytes = 0, level, face;
    const BYTE *in = texelScratch;
    ULONG *out;
    for (level = 0; level < t->levels; ++level) bytes += level_side(t->width, level) * level_side(t->height, level) * 4;
    bytes *= t->faces;
    if (bytes > decodeScratchBytes) {
        if (decodeScratch) EngFreeMem(decodeScratch);
        decodeScratch = EngAllocMem(0, bytes, 'x7bR');
        decodeScratchBytes = decodeScratch ? bytes : 0;
        if (!decodeScratch) return FALSE;
    }
    out = (ULONG *)decodeScratch;
    for (face = 0; face < t->faces; ++face)
        for (level = 0; level < t->levels; ++level) {
            ULONG w = level_side(t->width, level), h = level_side(t->height, level);
            decode_level(t->format, in, w, h, out);
            in += rb_d7_row_bytes(t->format, w) * rb_d7_rows(t->format, h);
            out += w * h;
        }
    return TRUE;
}

/* Set once the miniport answers that the host cannot sample DXT blocks. */
static BOOL hostBlocksRefused;

/* A compressed texture's blocks as they are, level by level in whole block
 * rows. FALSE when they could not be sent; hostBlocksRefused tells a host
 * without the format from other failures. */
static BOOL upload_blocks(DISPLAY *d, D7_TEXTURE *t, ULONG targetId)
{
    RB2K_D3D7_TEXTURE *packet = (RB2K_D3D7_TEXTURE *)uploadScratch;
    const BYTE *src = texelScratch;
    ULONG id = t->host, level, face, total = 0, written, status;
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
            packet->target = targetId;
            packet->width = t->width;
            packet->height = t->height;
            packet->y = by * 4;
            packet->rows = min(rows * 4, height - by * 4);
            packet->levels = t->levels;
            packet->level = level;
            packet->format = format;
            status = miniport(d, IOCTL_RB2K_D3D7_TEXTURE, packet, sizeof(*packet) + rows * rowBytes, &newId,
                              sizeof(newId), &written);
            if (status) d->d7draw.blockStatus = status;
            if (status || written != sizeof(newId)) return FALSE;
            if (!newId) { hostBlocksRefused = TRUE; return FALSE; }   /* the host cannot sample these */
            id = newId;
            total += rows * rowBytes;
        }
    }
    if (t->host != id) RtlZeroMemory(t->targets, sizeof(t->targets));
    t->host = id;
    ++d->d7draw.uploads;
    ++d->d7draw.blockUploads;
    d->d7draw.uploadBytes += total;
    return TRUE;
}

static BOOL upload_texture(DISPLAY *d, D7_TEXTURE *t, ULONG targetId)
{
    RB2K_D3D7_TEXTURE *packet = (RB2K_D3D7_TEXTURE *)uploadScratch;
    ULONG id = t->host, tries, format = t->format, bpp, total = 0;
    ULONG *out = (ULONG *)(packet + 1);
    const BYTE *texels = texelScratch;
    if (!packet || t->width * 4 > RB2K_D7_UPLOAD_BYTES) return FALSE;
    if (compressed(format) && !hostBlocksRefused) {
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
    for (tries = 0; tries < 2; ++tries) {
        BOOL ok = TRUE;
        const BYTE *src = texels;
        ULONG level;
        total = 0;
        ULONG face;
        for (face = 0; ok && face < t->faces; ++face)
        for (level = 0; ok && level < t->levels; ++level) {
            ULONG width = level_side(t->width, level), height = level_side(t->height, level);
            ULONG chunk = RB2K_D7_UPLOAD_BYTES / (width * 4), y;
            for (y = 0; ok && y < height; y += chunk) {
                ULONG rows = min(chunk, height - y), x, r, newId = 0;
                for (r = 0; r < rows; ++r, src += width * bpp)
                    for (x = 0; x < width; ++x)
                        out[r * width + x] = convert(format, bpp == 4 ? ((const ULONG *)src)[x] :
                                                                         ((const USHORT *)src)[x]);
                packet->id = face || level || y ? id : t->host;
                packet->face = face;
                packet->faces = t->faces;
                packet->target = targetId;
                packet->width = t->width;
                packet->height = t->height;
                packet->y = y;
                packet->rows = rows;
                packet->levels = t->levels;
                packet->level = level;
                packet->format = 0;
                ok = request(d, IOCTL_RB2K_D3D7_TEXTURE, packet, sizeof(*packet) + rows * width * 4,
                             &newId, sizeof(newId));
                if (ok) id = newId;
                total += rows * width * 4;
            }
        }
        if (ok) {
            if (t->host != id) RtlZeroMemory(t->targets, sizeof(t->targets));
            t->host = id;
            ++d->d7draw.uploads;
            d->d7draw.uploadBytes += total;
            return TRUE;
        }
        /* A transport reset releases every host texture; make a new one. */
        if (!t->host) break;
        t->host = id = 0;
        RtlZeroMemory(t->targets, sizeof(t->targets));
    }
    ++d->d7draw.uploadFailures;
    return FALSE;
}

/* The host texture for this draw, checked once a scene. The miniport
 * attaches a texture to the target it is uploaded for. */
static ULONG texture_ready(D7_CONTEXT *c, D7_TEXTURE *t, ULONG targetId)
{
    ULONG sum, i;
    BOOL attached = FALSE;
    for (i = 0; i < RB2K_DD_SLOTS; ++i) if (t->targets[i] == targetId) attached = TRUE;
    if (t->host && attached && t->tracked && !t->dirty) { ++c->display->d7draw.checksSkipped; return t->host; }
    if (t->host && attached && t->epoch == textureEpoch) return t->host;
    ++c->display->d7draw.checks;
    /* Cleared before reading, so a write during the read marks it again. */
    t->dirty = FALSE;
    {
        LONGLONG start, end;
        BOOL read;
        EngQueryPerformanceCounter(&start);
        read = read_texels(t, &sum);
        EngQueryPerformanceCounter(&end);
        c->display->d7draw.checkTicks += end - start;
        if (!read) { ++c->display->d7draw.checkFailures; return 0; }
    }
    t->epoch = textureEpoch;
    if (t->host && attached && sum == t->sum) return t->host;
    {
        LONGLONG start, end;
        BOOL uploaded;
        EngQueryPerformanceCounter(&start);
        uploaded = upload_texture(c->display, t, targetId);
        EngQueryPerformanceCounter(&end);
        c->display->d7draw.uploadTicks += end - start;
        if (!uploaded) return 0;
    }
    t->sum = sum;
    for (i = 0; i < RB2K_DD_SLOTS && t->targets[i] != targetId; ++i) {}
    if (i == RB2K_DD_SLOTS) {
        /* Reuse the entry of a target that no longer exists. */
        for (i = 0; i < RB2K_DD_SLOTS && t->targets[i] && target(c, t->targets[i], FALSE); ++i) {}
        if (i < RB2K_DD_SLOTS) t->targets[i] = targetId;
    }
    return t->host;
}

/* ---- triangles ---- */

enum { List, Strip, Fan };
/* The source of one primitive command's vertices. */
typedef struct {
    const BYTE *inline_;   /* immediate vertices in the command snapshot */
    ULONG first, primitives, kind;
    const WORD *indices;   /* relative to first, or NULL */
    ULONG indexStride;     /* WORDs per triangle record for INDEXEDTRIANGLELIST */
    /* A DirectX 8 draw from stream zero: its data and size in the process,
     * the signed byte offset of vertex zero, the stride and vertex format. */
    BOOL stream;
    const BYTE *streamData;
    ULONG streamBytes, streamStride, fvf;
    LONG streamFirst;
} PRIMS;

static ULONG prim_vertex(const PRIMS *p, ULONG t, ULONG corner, ULONG *flat)
{
    ULONG n;
    switch (p->kind) {
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
    return n;
}

static BOOL bad_position(const RB2K_D3D7_VERTEX *v)
{
    return (v->x & 0x7fffffff) >= 0x7f800000 || (v->y & 0x7fffffff) >= 0x7f800000 ||
           (v->z & 0x7fffffff) >= 0x7f800000 || !(v->rhw & 0x7fffffff) || (v->rhw & 0x7fffffff) >= 0x7f800000;
}

static void skip(D7_CONTEXT *c, ULONG reason, ULONG primitives)
{
    c->display->d7draw.skipped[reason] += primitives;
    ++c->display->d7draw.skippedCalls;
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
    ++d->d7draw.drawCalls;
    return DD_OK;
}

static HRESULT send_batch(D7_CONTEXT *c, RB2K_D3D7_DRAW *draw, ULONG count)
{
    draw->count = count;
    return queue_record(c, draw, FIELD_OFFSET(RB2K_D3D7_DRAW, vertices) + count * sizeof(draw->vertices[0]), 0,
                        count / 3);
}

static HRESULT send_batch8(D7_CONTEXT *c, RB2K_D3D8_DRAW *draw, ULONG count)
{
    draw->count = count;
    ++c->display->d7draw.psDraws;
    return queue_record(c, draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices) + sizeof(ULONG) *
                        (draw->transform == 2 ? 4 * draw->vertexConstants + count * draw->vertexWords :
                         count * RB2K_D8_VERTEX_WORDS(draw->coordSets)),
                        RB2K_D8_RECORD, count / 3);
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

static HRESULT triangles(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG stride, const PRIMS *p)
{
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
    if (!p->primitives) return DD_OK;
    if (c->fill != D3DFILL_SOLID) { skip(c, RB2K_D7_SKIP_FILL, p->primitives); return DD_OK; }
    /* Two stages are drawn; a third one counts as the stage-one skip. */
    if (st->colorOp != D3DTOP_DISABLE && st1->colorOp != D3DTOP_DISABLE && c->stages[2].colorOp != D3DTOP_DISABLE) {
        skip(c, RB2K_D7_SKIP_STAGE1, p->primitives);
        return DD_OK;
    }
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D7_DRAW, vertices));
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
    if (!position || (fvf & D3DFVF_RESERVED0) || (!objectSpace && (fvf & D3DFVF_NORMAL)) ||
        (st->coordIndex >> 16) || (needTexture && (st->transform & 0xff)) ||
        (draw->stage1Color && ((st1->coordIndex >> 16) || (needTexture1 && (st1->transform & 0xff))))) {
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
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
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
        return DD_OK;
    }
    if (needTexture) {
        tex = find_texture(c->local, st->texture);
        if (!tex || !tex->format || tex->owner != c->owner || tex->faces != 1) {
            skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives);
            return DD_OK;
        }
        if (tex->mipmaps && tex->levels == 1 && st->mipFilter != D3DTFP_NONE)
            d->d7draw.ignored[RB2K_D7_IGNORED_MIPMAP] += p->primitives;
        draw->texture = texture_ready(c, tex, s->id);
        if (!draw->texture) { skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives); return DD_OK; }
    }
    if (needTexture1) {
        tex1 = find_texture(c->local, st1->texture);
        if (!tex1 || !tex1->format || tex1->owner != c->owner || tex1->faces != 1) {
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
    if (c->clipPlanes) d->d7draw.ignored[RB2K_D7_IGNORED_CLIPPLANE] += p->primitives;

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
        RtlCopyMemory(draw->matrices, c->matrices, sizeof(draw->matrices));
        RtlCopyMemory(draw->viewport, c->viewport, sizeof(c->viewport));
        draw->viewport[4] = c->zRange[0];
        draw->viewport[5] = c->zRange[1];
    }

    /* Check the whole vertex window before the first host draw. */
    for (t = 0; t < p->primitives; ++t) {
        for (i = 0; i < 3; ++i) {
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
            skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
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
            skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
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
                for (i = 0; i < 3; ++i) worst = max(worst, prim_vertex(p, t, i, &flat));
                if (worst > limit || flat > limit) {
                    ++d->d7draw.beyondVertices;
                    skip(c, RB2K_D7_SKIP_VERTEX, 1);
                    continue;
                }
            }
            for (i = 0; i < 3; ++i) {
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
                for (i = 0; i < 3; ++i) { out[i].diffuse = fd; out[i].specular = fs; }
            }
            if (bad) { ++d->d7draw.badPositions; skip(c, RB2K_D7_SKIP_VERTEX, 1); continue; }
            count += 3;
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
    for (i = 0; handle && i < MaxShaders; ++i)
        if (c->shaders[i].handle == handle) return &c->shaders[i];
    return NULL;
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
    for (k = 0; k < 2; ++k)
        if (vs->program[k]) request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &vs->program[k], sizeof(ULONG), NULL, 0);
    if (vs->decl) EngFreeMem(vs->decl);
    RtlZeroMemory(vs, sizeof(*vs));
}

static D8_VSHADER *find_vshader(D7_CONTEXT *c, ULONG handle)
{
    ULONG i;
    for (i = 0; handle && i < MaxVertexShaders; ++i)
        if (c->vshaders[i].handle == handle) return &c->vshaders[i];
    return NULL;
}

static void release_shaders(D7_CONTEXT *c)
{
    ULONG i;
    for (i = 0; i < MaxShaders; ++i) if (c->shaders[i].handle) release_shader(c, &c->shaders[i]);
    for (i = 0; i < MaxVertexShaders; ++i) if (c->vshaders[i].handle) release_vshader(c, &c->vshaders[i]);
    for (i = 0; i < StagePrograms; ++i)
        if (c->stagePrograms[i].program)
            request(c->display, IOCTL_RB2K_D3D8_PROGRAM_FREE, &c->stagePrograms[i].program, sizeof(ULONG), NULL, 0);
    RtlZeroMemory(c->stagePrograms, sizeof(c->stagePrograms));
    release_blocks(c);
}

/* The pixel program for the current texture stages, specular and fog;
 * as gets its samplers and constant count for pixel_states. Zero when the
 * stages cannot be translated. */
static ULONG stage_program(D7_CONTEXT *c, ULONG fog, D8_SHADER *as)
{
    RB_STAGE_STATE st[TextureStages];
    ULONG key[StageKey], i, k, n = 0, id = 0, written;
    D8_STAGE_PROGRAM *e;
    RB_PS_INFO info;
    RtlZeroMemory(key, sizeof(key));
    for (i = 0; i < TextureStages && c->stages[i].colorOp != D3DTOP_DISABLE; ++i) {
        const D7_STAGE *s = &c->stages[i];
        st[i].colorOp = s->colorOp; st[i].colorArg1 = s->colorArg1; st[i].colorArg2 = s->colorArg2;
        st[i].alphaOp = s->alphaOp; st[i].alphaArg1 = s->alphaArg1; st[i].alphaArg2 = s->alphaArg2;
        st[i].coordIndex = s->coordIndex;
        RtlCopyMemory(key + 7 * i, &st[i], 7 * sizeof(ULONG));
    }
    key[7 * 8] = c->specular ? 1 : 0;
    key[7 * 8 + 1] = fog;
    RtlZeroMemory(as, sizeof(*as));
    as->constants = 8;
    for (k = 0; k < StagePrograms; ++k) {
        e = &c->stagePrograms[k];
        ULONG w;
        for (w = 0; w < StageKey && e->key[w] == key[w]; ++w) {}
        if (e->program && w == StageKey) {
            as->samplers = e->samplers;
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
    return id;
}

/* Keep a vertex shader's declaration and code; it is translated at its first draw. */
static void create_vshader(D7_CONTEXT *c, ULONG handle, const BYTE *decl, ULONG declBytes, ULONG codeBytes)
{
    D8_VSHADER *vs = find_vshader(c, handle);
    ULONG i;
    if (vs) release_vshader(c, vs);
    for (i = 0; i < MaxVertexShaders && c->vshaders[i].handle; ++i) {}
    if (!handle || i == MaxVertexShaders || !codeBytes || (declBytes | codeBytes) % 4) {
        ++c->display->d7draw.vsFailed[RbPsReasons];
        return;
    }
    vs = &c->vshaders[i];
    vs->decl = EngAllocMem(0, declBytes + codeBytes, 'v8bR');
    if (!vs->decl) return;
    RtlCopyMemory(vs->decl, decl, declBytes + codeBytes);
    vs->handle = handle;
    vs->declCount = declBytes / 4;
    vs->tokens = vs->decl + vs->declCount;
    vs->tokenCount = codeBytes / 4;
}

/* The miniport program of a vertex shader with or without fog in the
 * specular alpha; zero when it cannot be made. */
static ULONG vertex_program(D7_CONTEXT *c, D8_VSHADER *vs, ULONG flags)
{
    RB2K_D7_DRAWSTATS *st = &c->display->d7draw;
    ULONG k = flags & RB_VS_FOG_ALPHA ? 1 : 0, n = 0, id = 0, written;
    RB_PS_INFO info;
    if (vs->program[k] || vs->failed[k]) return vs->program[k];
    info.reason = RbPsOverflow;
    info.detail = 0;
    if (programScratch)
        n = rb_vs_translate((const unsigned int *)vs->tokens, vs->tokenCount, (const unsigned int *)vs->decl,
                            vs->declCount, flags, programScratch->text, sizeof(programScratch->text), &info);
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
    vs->program[k] = id;
    return id;
}

/* Translate a pixel shader and give its text to the miniport. A shader that
 * cannot be translated keeps its handle; draws with it are counted and
 * skipped. */
static void create_shader(D7_CONTEXT *c, ULONG handle, const BYTE *code, ULONG bytes)
{
    RB2K_D7_DRAWSTATS *s = &c->display->d7draw;
    D8_SHADER *sh = find_shader(c, handle);
    RB_PS_INFO info;
    ULONG i;
    if (sh) release_shader(c, sh);
    if (s->psLogCount < 6) {
        ULONG *log = s->psLog[s->psLogCount++];
        log[0] = handle; log[1] = bytes;
        RtlCopyMemory(log + 2, code, min(bytes, 42UL * 4) & ~3UL);
    }
    for (i = 0; i < MaxShaders && c->shaders[i].handle; ++i) {}
    if (!handle || i == MaxShaders) { ++s->psFailed[RbPsReasons]; return; }
    sh = &c->shaders[i];
    sh->handle = handle;
    /* The tokens stay for the fog variants made at draw time. */
    sh->tokenCount = bytes / 4;
    sh->constants = sh->tokenCount && *(const ULONG *)code == 0xffff0200 ? 32 : 8;
    sh->tokens = sh->tokenCount ? EngAllocMem(0, sh->tokenCount * sizeof(ULONG), 'k8bR') : NULL;
    if (sh->tokens) RtlCopyMemory(sh->tokens, code, sh->tokenCount * sizeof(ULONG));
    sh->variants[0].program = translate_program(c, sh, 0, &info);
    if (sh->variants[0].program) ++s->psCreated;
    else {
        ++s->psFailed[info.reason < RbPsReasons ? info.reason : RbPsReasons];
        s->psLastDetail = info.detail;
    }
}

/* The stages among samplers whose texture is a cube map. */
static ULONG cube_stages(D7_CONTEXT *c, ULONG samplers)
{
    ULONG k, mask = 0;
    for (k = 0; k < RB2K_D8_STAGES; ++k)
        if ((samplers >> k) & 1) {
            D7_TEXTURE *tex = find_texture(c->local, c->stages[k].texture);
            if (tex && tex->faces == 6) mask |= 1UL << k;
        }
    return mask;
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
    for (k = 0; k < RB2K_D8_STAGES; ++k) {
        D7_STAGE *st = &c->stages[k];
        D7_TEXTURE *tex;
        if (!(sh->samplers & (1UL << k))) continue;
        tex = find_texture(c->local, st->texture);
        if (!tex || !tex->format || tex->owner != c->owner || !(draw->textures[k] = texture_ready(c, tex, s->id))) {
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
    draw->cull = c->cull >= D3DCULL_NONE && c->cull <= D3DCULL_CCW ? c->cull : D3DCULL_NONE;
    draw->program = program;
    draw->fog = fog;
    draw->fogColor = c->fogColor;
    draw->fogStart = c->fogStart;
    draw->fogEnd = c->fogEnd;
    draw->fogDensity = c->fogDensity;
    draw->constantCount = sh->constants;
    RtlCopyMemory(draw->constants, c->psConst, sh->constants * 16);
    return TRUE;
}

/* A draw with a pixel shader. The program replaces the texture stages; the
 * coordinates of stages 0..7 reach it as four components each, a missing
 * one being 0 or, for the fourth, 1. Only the sets up to the last one the
 * program reads travel with the vertices. Untransformed vertices are lit and
 * fog blends the shader's result as for the fixed-function draws. */
static HRESULT shader_triangles(D7_CONTEXT *c, const PRIMS *p)
{
    DISPLAY *d = c->display;
    RB2K_D3D8_DRAW *draw = &c->draw8;
    D8_SHADER *sh = find_shader(c, c->pixelShader);
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
    if (sh) program = shader_program(c, sh, fog | cube_stages(c, sh->samplers) << RB_PS_CUBE_SHIFT);
    if (!program) { skip(c, RB2K_D7_SKIP_COMBINER, p->primitives); return DD_OK; }
    if (!p->stream || !position || (fvf & D3DFVF_RESERVED0) || (!objectSpace && (fvf & D3DFVF_NORMAL))) {
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
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
    /* Generated coordinates and texture transforms are not applied yet. */
    for (k = 0; k < RB2K_D8_STAGES; ++k)
        if ((sh->coords & (1UL << k)) && ((c->stages[k].coordIndex >> 16) || (c->stages[k].transform & 0xff)))
            offset = 0xffffffff;
    if (offset > stride || stride > 256) { skip(c, RB2K_D7_SKIP_VERTEX, p->primitives); return DD_OK; }
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices));
    for (k = 0; k < RB2K_D8_STAGES; ++k) if (sh->coords & (1UL << k)) used = k + 1;
    draw->coordSets = used;
    words = RB2K_D8_VERTEX_WORDS(used);
    if (!pixel_states(c, draw, sh, s, z, program, fog, p->primitives)) return DD_OK;
    if (objectSpace) {
        draw->transform = 1;
        RtlCopyMemory(draw->matrices, c->matrices, sizeof(draw->matrices));
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
        for (i = 0; i < 3; ++i) {
            ULONG v = prim_vertex(p, t, i, &flatSource);
            if (v < low) low = v;
            if (v > high) high = v;
        }
    lo = p->streamFirst + (LONGLONG)low * stride;
    hi = p->streamFirst + (LONGLONG)high * stride + offset;
    if (lo < 0 || hi > p->streamBytes) {
        ++d->d7draw.beyondCalls;
        d->d7draw.beyondVertices += p->primitives;
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
        return DD_OK;
    }
    base = p->streamData + p->streamFirst;
    __try {
        EngProbeForRead((PVOID)(base + low * stride), (high - low) * stride + offset, 1);
        for (t = 0; t < p->primitives; ++t) {
            ULONG *out = draw->vertices + count * words;
            ULONG flat;
            BOOL bad = FALSE;
            for (i = 0; i < 3; ++i) {
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
                                      j == 3 ? 0x3f800000 : 0;
                bad |= bad_position((const RB2K_D3D7_VERTEX *)o);
            }
            if (c->shade == D3DSHADE_FLAT) {
                const BYTE *v = base + flat * stride;
                ULONG fd = diffuse ? *(const ULONG *)(v + diffuse) : 0xffffffff;
                ULONG fs = specular ? *(const ULONG *)(v + specular) : 0;
                for (i = 0; i < 3; ++i) {
                    RB2K_D3D8_VERTEX *o = (RB2K_D3D8_VERTEX *)(out + i * words);
                    o->diffuse = fd;
                    o->specular = fs;
                }
            }
            if (bad) { ++d->d7draw.badPositions; skip(c, RB2K_D7_SKIP_VERTEX, 1); continue; }
            count += 3;
            if (count == RB2K_D8_VERTICES || t + 1 == p->primitives) {
                hr = send_batch8(c, draw, count);
                if (FAILED(hr)) break;
                count = 0;
            }
        }
        if (SUCCEEDED(hr) && count) hr = send_batch8(c, draw, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
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
    DISPLAY *d = c->display;
    RB2K_D3D8_DRAW *draw = &c->draw8;
    D8_VSHADER *vs = find_vshader(c, c->fvf);
    D8_SHADER *sh = find_shader(c, c->pixelShader), stages;
    DD_SLOT *s, *z;
    ULONG program = 0, vprogram = 0, fog = 0, flags = 0, stride = p->streamStride, words, i, t, count = 0;
    ULONG low = 0xffffffff, high = 0, flat;
    ULONG *out;
    const BYTE *base;
    LONGLONG lo, hi;
    HRESULT hr = render_surfaces(c, &s, &z);
    if (FAILED(hr)) return hr;
    if (!p->primitives) return DD_OK;
    if (c->fill != D3DFILL_SOLID) { skip(c, RB2K_D7_SKIP_FILL, p->primitives); return DD_OK; }
    if (c->fog) {
        if (c->fogTable >= D3DFOG_EXP && c->fogTable <= D3DFOG_LINEAR)
            fog = RB2K_D7_FOG_TABLE | c->fogTable << RB2K_D7_FOG_MODE_SHIFT | (c->wNear != c->wFar ? RB2K_D7_FOG_W : 0);
        else { fog = RB2K_D7_FOG_ALPHA; flags = RB_VS_FOG_ALPHA; }
    }
    if (vs) vprogram = vertex_program(c, vs, flags);
    if (sh) program = shader_program(c, sh, fog | cube_stages(c, sh->samplers) << RB_PS_CUBE_SHIFT);
    else {
        /* The fixed-function pixel stages after the vertex shader (which do
         * not sample cube maps). */
        program = stage_program(c, fog, &stages);
        if (program && cube_stages(c, stages.samplers)) program = 0;
        sh = &stages;
        d->d7draw.vsStageDraws += p->primitives;
    }
    words = (stride + 15) / 16 * 4;
    if (!vprogram || !program || !p->stream || stride % 4 || words > RB2K_D8_VS_VERTEX_WORDS || vs->words > stride / 4) {
        skip(c, vprogram && program ? RB2K_D7_SKIP_VERTEX : RB2K_D7_SKIP_COMBINER, p->primitives);
        return DD_OK;
    }
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D8_DRAW, vertices));
    if (!pixel_states(c, draw, sh, s, z, program, fog, p->primitives)) return DD_OK;
    if (sh == &stages) draw->constants[0][0] = c->factor;   /* the texture factor, unpacked by the program */
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
    draw->vertexWords = words;
    RtlCopyMemory(draw->viewport, c->viewport, sizeof(c->viewport));
    draw->viewport[4] = c->zRange[0];
    draw->viewport[5] = c->zRange[1];
    RtlCopyMemory(draw->vertices, c->vsConst, vs->constants * 16);
    out = draw->vertices + 4 * vs->constants;
    for (t = 0; t < p->primitives; ++t)
        for (i = 0; i < 3; ++i) {
            ULONG v = prim_vertex(p, t, i, &flat);
            if (v < low) low = v;
            if (v > high) high = v;
        }
    lo = p->streamFirst + (LONGLONG)low * stride;
    hi = p->streamFirst + (LONGLONG)high * stride + stride;
    if (lo < 0 || hi > p->streamBytes) {
        ++d->d7draw.beyondCalls;
        d->d7draw.beyondVertices += p->primitives;
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
        return DD_OK;
    }
    base = p->streamData + p->streamFirst;
    __try {
        EngProbeForRead((PVOID)(base + low * stride), (high - low + 1) * stride, 1);
        for (t = 0; t < p->primitives; ++t) {
            for (i = 0; i < 3; ++i) {
                ULONG *o = out + (count + i) * words;
                RtlCopyMemory(o, base + prim_vertex(p, t, i, &flat) * stride, stride);
                RtlZeroMemory((BYTE *)o + stride, words * 4 - stride);
            }
            count += 3;
            if (count == RB2K_D8_VERTICES || t + 1 == p->primitives) {
                d->d7draw.vsDraws += count / 3;
                hr = send_batch8(c, draw, count);
                if (FAILED(hr)) break;
                count = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
        hr = DD_OK;
    }
    return hr;
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

static BOOL index_scratch(ULONG count)
{
    if (count <= indexScratchCount) return TRUE;
    if (indexScratch) EngFreeMem(indexScratch);
    indexScratchCount = 0;
    indexScratch = EngAllocMem(0, count * sizeof(WORD), 'i8bR');
    if (!indexScratch) return FALSE;
    indexScratchCount = count;
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
        p->streamData = (const BYTE *)b->global->fpVidMem;
        p->streamBytes = b->global->dwLinearSize;
    }
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
    if (type < RbTriangleList || type > RbTriangleFan) { skip(c, RB2K_D7_SKIP_PRIMITIVE, count); return DD_OK; }
    prims.kind = type == RbTriangleList ? List : type == RbTriangleStrip ? Strip : Fan;
    if (((c->fvf & 1) && !c->display->vertexShaders) || !c->streamStride || c->streamStride > 256 ||
        !stream_zero(c, a, &prims) ||
        first < -0x7fffffffLL || first > (LONGLONG)prims.streamBytes) {
        skip(c, RB2K_D7_SKIP_VERTEX, count);
        return DD_OK;
    }
    prims.stream = TRUE;
    prims.streamFirst = (LONG)first;
    prims.streamStride = c->streamStride;
    prims.fvf = c->fvf;
    if (indexed) {
        D8_BUFFER *b = find_buffer(c->local, c->indexHandle);
        ULONG indices = type == RbTriangleList ? 3 * count : count + 2, size;
        if (!b || b->owner != c->owner || c->indexStride != 2 || !b->global->fpVidMem || count > 0xffff ||
            indexByte > (size = b->global->dwLinearSize) || indices > (size - indexByte) / 2) {
            skip(c, RB2K_D7_SKIP_VERTEX, count);
            return DD_OK;
        }
        if (!index_scratch(indices)) return DDERR_OUTOFMEMORY;
        __try {
            const BYTE *from = (const BYTE *)b->global->fpVidMem + indexByte;
            EngProbeForRead((PVOID)from, indices * sizeof(WORD), 1);
            RtlCopyMemory(indexScratch, from, indices * sizeof(WORD));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            skip(c, RB2K_D7_SKIP_VERTEX, count);
            return DD_OK;
        }
        prims.indices = indexScratch;
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
static void texture_blt(D7_CONTEXT *c, const D3DNTHAL_DP2TEXBLT *b)
{
    DISPLAY *d = c->display;
    D7_TEXTURE *dst, *src;
    LONG left = b->rSrc.left, top = b->rSrc.top, right = b->rSrc.right, bottom = b->rSrc.bottom;
    ULONG bpp, y, rows, bytes;
    if (!b->dwDDSrcSurface) return;
    dst = find_texture(c->local, b->dwDDDestSurface);
    src = find_texture(c->local, b->dwDDSrcSurface);
    if (!dst || !src || !dst->format || dst->format != src->format || dst->faces != src->faces ||
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
        ULONG level, levels = min(src->levels, dst->levels), face;
        if (src->width != dst->width || src->height != dst->height) levels = 1;
        for (face = 0; face < src->faces; ++face)
        for (level = 0; level < levels; ++level) {
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
            from = (const BYTE *)src->levelAddress[face][level] + y0 * src->levelPitch[face][level] + x0 * bpp;
            to = (BYTE *)dst->levelAddress[face][level] + dy * dst->levelPitch[face][level] + dx * bpp;
            EngProbeForRead((PVOID)from, (rows - 1) * src->levelPitch[face][level] + bytes, 1);
            EngProbeForReadAndWrite(to, (rows - 1) * dst->levelPitch[face][level] + bytes, 1);
            for (y = 0; y < rows; ++y)
                RtlCopyMemory(to + y * dst->levelPitch[face][level], from + y * src->levelPitch[face][level], bytes);
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
    HRESULT hr = render_surfaces(c, &s, &z);
    if (FAILED(hr)) return hr;
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
    v.dwX = c->viewport[0]; v.dwY = c->viewport[1]; v.dwWidth = c->viewport[2]; v.dwHeight = c->viewport[3];
    block_command(b, D3DNTDP2OP_VIEWPORTINFO, 1, &v, sizeof(v));
    block_command(b, D3DNTDP2OP_ZRANGE, 1, c->zRange, sizeof(c->zRange));
    block_command(b, D3DNTDP2OP_SETMATERIAL, 1, c->material, sizeof(c->material));
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
            if (h->bCommand == D3DNTDP2OP_LINELIST_IMM) { skip(c, RB2K_D7_SKIP_PRIMITIVE, n); break; }
            prims.inline_ = p + head + pad;
            prims.kind = Fan;
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
            /* World, view and projection; texture matrices are not applied. */
            const D3DNTHAL_DP2SETTRANSFORM *x = (const D3DNTHAL_DP2SETTRANSFORM *)p;
            used = n * sizeof(*x);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++x) {
                /* Direct3D 8 numbers the first world matrix 256. */
                ULONG type = x->xfrmType == 256 ? D3DTRANSFORMSTATE_WORLD : x->xfrmType;
                ++c->display->d7draw.transformTypes[x->xfrmType < 31 ? x->xfrmType : 31];
                if (type >= D3DTRANSFORMSTATE_WORLD && type <= D3DTRANSFORMSTATE_PROJECTION)
                    RtlCopyMemory(c->matrices[type - D3DTRANSFORMSTATE_WORLD], &x->matrix, 64);
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
                    z->width != s->width || z->height != s->height))) return DDERR_SURFACELOST;
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
                if (!s->stream) { c->streamUser = FALSE; c->streamHandle = s->handle; c->streamStride = s->stride; }
            break;
        }
        case RbDp2SetStreamSourceUm: {
            const RB_DP2_STREAM_SOURCE_UM *s = (const RB_DP2_STREAM_SOURCE_UM *)p;
            used = n * sizeof(*s);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            trace8(c, a, h->bCommand, n, p, remaining);
            for (; n; --n, ++s)
                if (!s->stream) { c->streamUser = TRUE; c->streamHandle = 0; c->streamStride = s->stride; }
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
            break;
        default:
            used = fixed_records(h->bCommand, n);
            if (!used && n) return D3DNTERR_COMMAND_UNPARSED;
            if (used > remaining) return DDERR_INVALIDPARAMS;
            if (h->bCommand == D3DNTDP2OP_POINTS || h->bCommand == D3DNTDP2OP_INDEXEDLINELIST ||
                h->bCommand == D3DNTDP2OP_LINELIST || h->bCommand == D3DNTDP2OP_LINESTRIP ||
                h->bCommand == D3DNTDP2OP_INDEXEDLINESTRIP || h->bCommand == D3DNTDP2OP_INDEXEDLINELIST2)
                skip(c, RB2K_D7_SKIP_PRIMITIVE, n);
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
    a->ddrval = DDERR_INVALIDOBJECT;
    EngQueryPerformanceCounter(&start);
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
            EngQueryPerformanceCounter(&copied);
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
        if (FAILED(a->ddrval)) {
            c->display->d7.lastError = a->ddrval;
            ++c->display->d7draw.dp2Failures;
            c->display->d7draw.lastFailure = a->ddrval;
            c->display->d7draw.lastFailureOp = c->display->d7.lastCommand;
            c->display->d7.errorOffset = a->dwErrorOffset;
        }
        EngQueryPerformanceCounter(&end);
        c->display->d7draw.dp2Ticks += end - start;
        if (!c->display->d7draw.frequency) EngQueryPerformanceFrequency(&c->display->d7draw.frequency);
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
        .dpcLineCaps = { .dwSize = sizeof(D3DPRIMCAPS) },
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
    { RbFmtX8R8G8B8, RbFormatTexture | RbFormatCubeTexture | RbFormatOffscreenTarget | RbFormatSameTarget |
                     RbFormatDisplayMode | RbFormat3D },
    /* a back buffer with alpha over the X8R8G8B8 desktop, or a target */
    { RbFmtA8R8G8B8, RbFormatTexture | RbFormatCubeTexture | RbFormatOffscreenTarget | RbFormatSameTargetUpToAlpha },
    { RbFmtR5G6B5, RbFormatTexture | RbFormatCubeTexture | RbFormatDisplayMode },
    { RbFmtX1R5G5B5, RbFormatTexture | RbFormatCubeTexture },
    { RbFmtA1R5G5B5, RbFormatTexture | RbFormatCubeTexture },
    { RbFmtA4R4G4B4, RbFormatTexture | RbFormatCubeTexture },
    { RbFmtDxt1, RbFormatTexture | RbFormatCubeTexture }, { RbFmtDxt2, RbFormatTexture | RbFormatCubeTexture }, { RbFmtDxt3, RbFormatTexture | RbFormatCubeTexture },
    { RbFmtDxt4, RbFormatTexture | RbFormatCubeTexture }, { RbFmtDxt5, RbFormatTexture | RbFormatCubeTexture },
    /* last: only with depth rendering */
    { RbFmtD16, RbFormatZStencil }, { RbFmtD24S8, RbFormatZStencil }, { RbFmtD24X8, RbFormatZStencil }
};

static ULONG d8_format_count(const DISPLAY *d)
{
    ULONG n = sizeof(d8Formats) / sizeof(d8Formats[0]);
    return d->z16Render ? n : n - 3;
}

static void d8_caps(const DISPLAY *d, RB_D3DCAPS8 *c, const D3DNTHAL_D3DEXTENDEDCAPS *x)
{
    const D3DNTHALDEVICEDESC_V1 *hw = &global.hwCaps;
    const D3DPRIMCAPS *tri = &hw->dpcTriCaps;
    RtlZeroMemory(c, sizeof(*c));
    c->deviceType = 1;                  /* D3DDEVTYPE_HAL */
    c->caps2 = RB_D3DCAPS2_CANRENDERWINDOWED;
    c->presentationIntervals = RB_D3DPRESENT_INTERVAL_IMMEDIATE | RB_D3DPRESENT_INTERVAL_ONE;
    c->devCaps = hw->dwDevCaps & ~D3DDEVCAPS_FLOATTLVERTEX;
    c->primitiveMiscCaps = tri->dwMiscCaps;
    c->rasterCaps = tri->dwRasterCaps & ~D3DPRASTERCAPS_SUBPIXEL;
    c->srcBlendCaps = tri->dwSrcBlendCaps;
    c->destBlendCaps = tri->dwDestBlendCaps;
    c->alphaCmpCaps = tri->dwAlphaCmpCaps;
    c->shadeCaps = tri->dwShadeCaps;
    c->textureCaps = tri->dwTextureCaps | 0x4000;     /* D3DPTEXTURECAPS_MIPMAP */
    c->textureFilterCaps = tri->dwTextureFilterCaps;
    /* Cube maps, sampled by pixel shaders (the fixed-function stages do not). */
    c->textureCaps |= 0x800 | 0x10000;   /* D3DPTEXTURECAPS_CUBEMAP, MIPCUBEMAP */
    c->cubeTextureFilterCaps = c->textureFilterCaps;
    c->textureAddressCaps = tri->dwTextureAddressCaps;
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
    c->maxPointSize = 0x3f800000;                       /* 1 */
    c->maxPrimitiveCount = 0xffff;
    c->maxVertexIndex = 0xffff;
    c->maxStreams = 1;
    c->maxStreamStride = 255;
    c->vertexShaderVersion = 0xfffe0000;            /* version 0.0 */
    if (d->vertexShaders) {
        c->vertexShaderVersion = d->d7draw.d8Version >= 0x900 ? 0xfffe0200 : 0xfffe0101;
        c->maxVertexShaderConst = d->d7draw.d8Version >= 0x900 ? 256 : 96;
    }
    /* 1.3 for Direct3D 8; shader model 2 when the runtime that asks has
     * announced itself as Direct3D 9, which uses this record for drivers of
     * the Direct3D 8 interface. */
    c->pixelShaderVersion = d->d7draw.d8Version >= 0x900 ? 0xffff0200 : 0xffff0103;
    c->maxSimultaneousTextures = RB_PS_COORD_SETS;  /* t0..t3 of version 1 shaders */
    c->maxPixelShaderValue = 0x3f800000;            /* 1 */
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
    /* Requests this driver does not answer, such as the Direct3D 9 ones. */
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
            D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA,
        .wMaxTextureBlendStages = 2, .wMaxSimultaneousTextures = 2, .wMaxVertexBlendMatrices = 1,
        .dvMaxVertexW = 1.0e10f, .dwMaxActiveLights = RB2K_D7_LIGHTS,
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
    RtlCopyMemory(d->d7draw.ddModes, rb_dd_mode_stats, sizeof(rb_dd_mode_stats));
    RtlCopyMemory(out, &d->d7draw, bytes);
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    return 1;
}
