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

/* These Direct3D 7 HRESULTs are declared only in the user-side d3d.h. */
#ifndef D3DERR_UNSUPPORTEDCOLOROPERATION
#define D3DERR_UNSUPPORTEDCOLOROPERATION MAKE_DDHRESULT(2073)
#define D3DERR_UNSUPPORTEDALPHAOPERATION MAKE_DDHRESULT(2075)
#define D3DERR_UNSUPPORTEDCOLORARG MAKE_DDHRESULT(2074)
#define D3DERR_TOOMANYOPERATIONS MAKE_DDHRESULT(2077)
#endif

enum { MaxContexts = 16, MaxCommandBytes = 1 << 20, TextureStages = 8, MaxTextures = 1024, MaxLights = 32 };
/* DP2 SETLIGHT operations (D3DHAL_SETLIGHT_*). */
enum { SetLightEnable, SetLightDisable, SetLightData };
typedef char light_record[sizeof(D3DLIGHT7) == 26 * sizeof(ULONG) ? 1 : -1];
typedef char material_record[sizeof(D3DNTHAL_DP2SETMATERIAL) == 17 * sizeof(ULONG) ? 1 : -1];
typedef struct {
    ULONG texture, colorOp, colorArg1, colorArg2, alphaOp, alphaArg1, alphaArg2;
    ULONG coordIndex, addressU, addressV, magFilter, minFilter, mipFilter, border, transform;
} D7_STAGE;
typedef struct {
    ULONG id, target, depth;
    DISPLAY *display;
    PDD_DIRECTDRAW_LOCAL local;
    HANDLE owner;
    ULONG cull, shade, fill;
    ULONG zEnable, zWrite, zFunc;
    ULONG alphaTest, alphaFunc, alphaRef, blend, srcBlend, dstBlend;
    ULONG fog, specular, stencil, clipPlanes, colorKey, factor, lighting;
    ULONG matrices[3][16];      /* world, view, projection */
    ULONG viewport[4], zRange[2];
    /* Lighting: D3DRENDERSTATE_AMBIENT, COLORVERTEX, LOCALVIEWER and
     * NORMALIZENORMALS, the diffuse, specular, ambient and emissive material
     * sources, the D3DMATERIAL7, and lights by index with their D3DLIGHT7. */
    ULONG ambient, colorVertex, localViewer, normalize, sources[4];
    ULONG material[17];
    ULONG lightOn;
    ULONG lights[MaxLights][26];
    BOOL inScene, queued;
    ULONG minStart, maxEnd;     /* diagnostics for one command buffer */
    RB2K_D3D7_DRAW draw;        /* the batch being filled */
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
    ULONG host, sum, epoch;
    ULONG targets[RB2K_DD_SLOTS];
} D7_TEXTURE;

static HSEMAPHORE registry;
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

static BOOL request(DISPLAY *d, ULONG code, PVOID in, ULONG inSize, PVOID out, ULONG outSize)
{
    ULONG written = 0;
    return !EngDeviceIoControl(d->miniport, code, in, inSize, out, outSize, &written) &&
           written == outSize;
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
        if (contexts[i].display == d) RtlZeroMemory(&contexts[i], sizeof(contexts[i]));
    /* Every DirectDraw object of this display is going away. */
    for (i = 0; textures && i < MaxTextures; ++i)
        if (textures[i].display == d) release_texture(&textures[i], TRUE);
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

/* Record or forget a texture. System memory belongs to the application;
 * a video-memory texture is runtime-allocated user memory (display_ddraw.c).
 * The runtime calls this again with a zero address when a system-memory
 * surface is destroyed, and DestroySurface reports video-memory ones. */
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
    if (l->lpSurfMore && (l->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_TEXTUREMANAGE)) ++d->d7draw.regManaged;
    if (!t->width || t->width > RB2K_D7_TEXTURE_SIDE || !t->height || t->height > RB2K_D7_TEXTURE_SIDE ||
        t->pitch < t->width * rb_d7_texel_bytes(t->format))
        t->format = TexNone;
    ++d->d7draw.textures;
    ++d->d7draw.registered[t->format];
    if (!(l->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) ++d->d7draw.videoTextures;
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

static DWORD APIENTRY register_surface(PDD_CREATESURFACEEXDATA a)
{
    PDD_SURFACE_LOCAL l = a->lpDDSLcl;
    DISPLAY *d = (DISPLAY *)a->lpDDLcl->lpGbl->dhpdev;
    DD_SLOT *slot;
    a->ddRVal = DD_OK;
    if (!l || !l->lpGbl) return DDHAL_DRIVER_HANDLED;
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
    /* DirectDraw registers a 3D-tagged window primary before it will blit
     * to it. Its storage is the scanout mapping. Only offscreen targets
     * need a host image and a Direct3D surface handle. */
    if (!slot && d->nativeD3D7 && rb_dd_mapped_primary(d, l))
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
    return EngDeviceIoControl(c->display->miniport, IOCTL_RB2K_D3D7_FLUSH, &c->target,
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
            RtlZeroMemory(&contexts[i], sizeof(contexts[i]));
        }
    }
    for (i = 0; i < RB2K_DD_SLOTS; ++i) {
        DD_SLOT *s = &d->slots[i];
        if (s->local == a->pDDLcl) { s->local = NULL; s->d3dHandle = 0; }
    }
    for (i = 0; textures && i < MaxTextures; ++i)
        if (textures[i].local == a->pDDLcl) release_texture(&textures[i], TRUE);
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
    ULONG i;
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
            c->lighting = TRUE;
            c->colorVertex = c->localViewer = TRUE;
            c->sources[0] = D3DMCS_COLOR1;
            c->sources[1] = D3DMCS_COLOR2;
            for (m = 0; m < 3; ++m)
                c->matrices[m][0] = c->matrices[m][5] = c->matrices[m][10] = c->matrices[m][15] = 0x3f800000;
            c->viewport[2] = s->width;
            c->viewport[3] = s->height;
            c->zRange[1] = 0x3f800000;
            default_stages(c);
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
        RtlZeroMemory(c, sizeof(*c));
        EngReleaseSemaphore(d->ddLock);
    }
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

/* ---- texture stage zero, reduced to what the miniport draws ---- */

enum { StageOk, StageDisabled, StageUnsupported };

/* Stage zero's current colour is the diffuse colour. */
static ULONG stage_argument(ULONG arg, BOOL *ok)
{
    ULONG select = arg & D3DTA_SELECTMASK;
    if (arg & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE) ||
        select > D3DTA_SPECULAR) *ok = FALSE;
    if (select == D3DTA_CURRENT) arg = (arg & ~D3DTA_SELECTMASK) | D3DTA_DIFFUSE;
    return arg;
}

static BOOL samples_texture(ULONG op, ULONG arg1, ULONG arg2)
{
    if (op == D3DTOP_BLENDTEXTUREALPHA || op == D3DTOP_BLENDTEXTUREALPHAPM) return TRUE;
    return (op != D3DTOP_SELECTARG2 && (arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE) ||
           (op != D3DTOP_SELECTARG1 && (arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE);
}

/* Fill the colour and alpha stage words. A stage that reads a missing texture
 * passes the diffuse colour through, as the 9x HAL and the RGB device do. */
static ULONG stage_words(const D7_CONTEXT *c, ULONG *color, ULONG *alpha, BOOL *texture)
{
    const D7_STAGE *s = &c->stages[0];
    ULONG colorOp = s->colorOp, alphaOp = s->alphaOp;
    BOOL ok = TRUE;
    ULONG c1 = stage_argument(s->colorArg1, &ok), c2 = stage_argument(s->colorArg2, &ok);
    ULONG a1 = stage_argument(s->alphaArg1, &ok), a2 = stage_argument(s->alphaArg2, &ok);
    *texture = FALSE;
    if (colorOp == D3DTOP_DISABLE ||
        ((samples_texture(colorOp, c1, c2) || (alphaOp != D3DTOP_DISABLE && samples_texture(alphaOp, a1, a2))) &&
         !s->texture)) {
        *color = *alpha = D3DTOP_SELECTARG1 | (D3DTA_DIFFUSE << 8);
        return StageDisabled;
    }
    if (alphaOp == D3DTOP_DISABLE) { alphaOp = D3DTOP_SELECTARG1; a1 = D3DTA_DIFFUSE; }
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
    else if (c->stages[1].colorOp != D3DTOP_DISABLE) a->ddrval = D3DERR_TOOMANYOPERATIONS;
    else a->ddrval = stage_words(c, &color, &alpha, &texture) == StageUnsupported ?
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
    case D3DRENDERSTATE_SPECULARENABLE: c->specular = value; break;
    case D3DRENDERSTATE_STENCILENABLE: c->stencil = value; break;
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
static BOOL read_texels(D7_TEXTURE *t, ULONG *sum)
{
    ULONG row = t->width * rb_d7_texel_bytes(t->format), bytes = row * t->height, y, i, h = 0x811c9dc5;
    if (bytes > texelScratchBytes) {
        if (texelScratch) EngFreeMem(texelScratch);
        texelScratch = EngAllocMem(0, bytes, 'r7bR');
        texelScratchBytes = texelScratch ? bytes : 0;
        if (!texelScratch) return FALSE;
    }
    __try {
        EngProbeForRead((PVOID)t->address, t->pitch * (t->height - 1) + row, 1);
        for (y = 0; y < t->height; ++y)
            RtlCopyMemory(texelScratch + y * row, (const BYTE *)t->address + y * t->pitch, row);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    for (i = 0; i + 4 <= bytes; i += 4) h = (h ^ *(const ULONG *)(texelScratch + i)) * 0x01000193;
    *sum = h ^ bytes;
    return TRUE;
}

static BOOL upload_texture(DISPLAY *d, D7_TEXTURE *t, ULONG targetId)
{
    RB2K_D3D7_TEXTURE *packet = (RB2K_D3D7_TEXTURE *)uploadScratch;
    ULONG chunk = RB2K_D7_UPLOAD_BYTES / (t->width * 4), y, id = t->host, tries;
    ULONG *out = (ULONG *)(packet + 1), bpp = rb_d7_texel_bytes(t->format);
    if (!packet || !chunk) return FALSE;
    for (tries = 0; tries < 2; ++tries) {
        BOOL ok = TRUE;
        for (y = 0; ok && y < t->height; y += chunk) {
            ULONG rows = min(chunk, t->height - y), x, r, newId = 0;
            for (r = 0; r < rows; ++r) {
                const BYTE *src = texelScratch + (y + r) * t->width * bpp;
                for (x = 0; x < t->width; ++x)
                    out[r * t->width + x] = convert(t->format, bpp == 4 ? ((const ULONG *)src)[x] :
                                                                     ((const USHORT *)src)[x]);
            }
            packet->id = y ? id : t->host;
            packet->target = targetId;
            packet->width = t->width;
            packet->height = t->height;
            packet->y = y;
            packet->rows = rows;
            ok = request(d, IOCTL_RB2K_D3D7_TEXTURE, packet, sizeof(*packet) + rows * t->width * 4,
                         &newId, sizeof(newId));
            if (ok) id = newId;
        }
        if (ok) {
            if (t->host != id) RtlZeroMemory(t->targets, sizeof(t->targets));
            t->host = id;
            ++d->d7draw.uploads;
            d->d7draw.uploadBytes += t->width * t->height * 4;
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
    if (t->host && attached && t->epoch == textureEpoch) return t->host;
    ++c->display->d7draw.checks;
    if (!read_texels(t, &sum)) { ++c->display->d7draw.checkFailures; return 0; }
    t->epoch = textureEpoch;
    if (t->host && attached && sum == t->sum) return t->host;
    if (!upload_texture(c->display, t, targetId)) return 0;
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

static HRESULT send_batch(D7_CONTEXT *c, RB2K_D3D7_DRAW *draw, ULONG count, D7_TEXTURE *tex)
{
    DISPLAY *d = c->display;
    ULONG written = 0, error;
    draw->count = count;
    error = EngDeviceIoControl(d->miniport, IOCTL_RB2K_D3D7_DRAW, draw,
                               FIELD_OFFSET(RB2K_D3D7_DRAW, vertices) + count * sizeof(draw->vertices[0]),
                               NULL, 0, &written);
    if (error) {
        ++d->d7draw.drawFailures;
        d->d7draw.lastDrawError = error;
        d->d7draw.lastDrawState[0] = draw->colorStage;
        d->d7draw.lastDrawState[1] = draw->alphaStage;
        d->d7draw.lastDrawState[2] = draw->blend;
        d->d7draw.lastDrawState[3] = draw->alphaTest;
        d->d7draw.lastDrawState[4] = draw->depthState;
        d->d7draw.lastDrawState[5] = draw->texture;
        d->d7draw.lastDrawState[6] = draw->sampler;
        d->d7draw.lastDrawState[7] = draw->depth;
        /* The texture may have gone with a transport reset. */
        if (tex) { tex->host = 0; RtlZeroMemory(tex->targets, sizeof(tex->targets)); }
        return DDERR_GENERIC;
    }
    c->queued = TRUE;
    d->d7draw.drawn += count / 3;
    ++d->d7draw.drawCalls;
    return DD_OK;
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
    D7_STAGE *st = &c->stages[0];
    D7_TEXTURE *tex = NULL;
    ULONG fvf = a->dwVertexType, offset = 16, diffuse = 0, specular = 0, coord = 0, coordFloats = 0, normal = 0;
    ULONG sets, i, t, low = 0xffffffff, high = 0, address = 0, count = 0, flatSource = 0;
    ULONG limit = 0xffffffff;              /* highest index inside the runtime's window */
    ULONG position;
    BOOL texture, needTexture, objectSpace;
    HRESULT hr = render_surfaces(c, &s, &z);
    const BYTE *base = NULL;
    if (FAILED(hr)) return hr;
    if (!p->primitives) return DD_OK;
    if (c->fill != D3DFILL_SOLID) { skip(c, RB2K_D7_SKIP_FILL, p->primitives); return DD_OK; }
    if (c->stencil) { skip(c, RB2K_D7_SKIP_STENCIL, p->primitives); return DD_OK; }
    if (c->stages[1].colorOp != D3DTOP_DISABLE) { skip(c, RB2K_D7_SKIP_STAGE1, p->primitives); return DD_OK; }
    RtlZeroMemory(draw, FIELD_OFFSET(RB2K_D3D7_DRAW, vertices));
    if (stage_words(c, &draw->colorStage, &draw->alphaStage, &needTexture) == StageUnsupported) {
        skip(c, RB2K_D7_SKIP_COMBINER, p->primitives);
        return DD_OK;
    }
    /* Vertex layout: a transformed position, or an object-space position
     * with any blend weights and normal (the T&L device), then the optional
     * fields in FVF order. */
    position = fvf & D3DFVF_POSITION_MASK;
    objectSpace = position != D3DFVF_XYZRHW;
    if (!position || (fvf & D3DFVF_RESERVED0) || (!objectSpace && (fvf & D3DFVF_NORMAL)) ||
        (st->coordIndex >> 16) || (needTexture && (st->transform & 0xff))) {
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
        offset += 4 * n;
    }
    if (offset > stride || stride > 256) {
        skip(c, RB2K_D7_SKIP_VERTEX, p->primitives);
        return DD_OK;
    }
    if (needTexture) {
        tex = find_texture(c->local, st->texture);
        if (!tex || !tex->format || tex->owner != c->owner) {
            skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives);
            return DD_OK;
        }
        if (tex->mipmaps && st->mipFilter != D3DTFP_NONE) d->d7draw.ignored[RB2K_D7_IGNORED_MIPMAP] += p->primitives;
        draw->texture = texture_ready(c, tex, s->id);
        if (!draw->texture) { skip(c, RB2K_D7_SKIP_TEXTURE, p->primitives); return DD_OK; }
    }
    texture = needTexture;
    if (c->fog) d->d7draw.ignored[RB2K_D7_IGNORED_FOG] += p->primitives;
    if (c->colorKey) d->d7draw.ignored[RB2K_D7_IGNORED_COLORKEY] += p->primitives;
    if (c->clipPlanes) d->d7draw.ignored[RB2K_D7_IGNORED_CLIPPLANE] += p->primitives;

    draw->target = s->id;
    draw->depth = z ? z->id : 0;
    if (z && c->zEnable && c->zFunc >= D3DCMP_NEVER && c->zFunc <= D3DCMP_ALWAYS)
        draw->depthState = RB2K_Z_ENABLE | (c->zWrite ? RB2K_Z_WRITE : 0) | (c->zFunc - 1);
    if (c->alphaTest && c->alphaFunc >= D3DCMP_NEVER && c->alphaFunc <= D3DCMP_ALWAYS)
        draw->alphaTest = RB2K_D7_ON | (c->alphaFunc << 8) | (c->alphaRef & 255);
    if (c->blend && c->srcBlend >= D3DBLEND_ZERO && c->srcBlend <= D3DBLEND_BOTHINVSRCALPHA &&
        c->dstBlend >= D3DBLEND_ZERO && c->dstBlend <= D3DBLEND_BOTHINVSRCALPHA)
        draw->blend = RB2K_D7_ON | (c->srcBlend << 8) | c->dstBlend;
    draw->cull = c->cull >= D3DCULL_NONE && c->cull <= D3DCULL_CCW ? c->cull : D3DCULL_NONE;
    draw->specular = c->specular ? 1 : 0;
    draw->factor = c->factor;
    draw->sampler = (st->addressU >= 1 && st->addressU <= 4 ? st->addressU : 1) |
                   (st->addressV >= 1 && st->addressV <= 4 ? st->addressV : 1) << 4 |
                   (st->magFilter >= 1 && st->magFilter <= 5 ? st->magFilter : 1) << 8 |
                   (st->minFilter >= 1 && st->minFilter <= 3 ? st->minFilter : 1) << 12;
    draw->border = st->border;
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
    if (!p->inline_) {
        if (p->first < c->minStart) c->minStart = p->first;
        if (p->first + high > c->maxEnd) c->maxEnd = p->first + high;
    }
    if (p->inline_) base = p->inline_;
    else {
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
                hr = send_batch(c, draw, count, tex);
                if (FAILED(hr)) break;
                count = 0;
            }
        }
        if (SUCCEEDED(hr) && count) hr = send_batch(c, draw, count, tex);
    } __except (EXCEPTION_EXECUTE_HANDLER) { hr = refused(d, RB2K_D7_FAIL_PROBE, a, p, low, high, stride); }
    return hr;
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
    if (!dst || !src || !dst->format || dst->format != src->format ||
        dst->owner != c->owner || src->owner != c->owner) { ++d->d7draw.texBltFailures; return; }
    if (right <= left || bottom <= top) { left = top = 0; right = src->width; bottom = src->height; }
    if (left < 0 || top < 0 || (ULONG)right > src->width || (ULONG)bottom > src->height ||
        b->pDest.x < 0 || b->pDest.y < 0 || (ULONG)(b->pDest.x + right - left) > dst->width ||
        (ULONG)(b->pDest.y + bottom - top) > dst->height) { ++d->d7draw.texBltFailures; return; }
    bpp = rb_d7_texel_bytes(src->format);
    rows = bottom - top;
    bytes = (right - left) * bpp;
    __try {
        const BYTE *from = (const BYTE *)src->address + top * src->pitch + left * bpp;
        BYTE *to = (BYTE *)dst->address + b->pDest.y * dst->pitch + b->pDest.x * bpp;
        EngProbeForRead((PVOID)from, (rows - 1) * src->pitch + bytes, 1);
        EngProbeForReadAndWrite(to, (rows - 1) * dst->pitch + bytes, 1);
        for (y = 0; y < rows; ++y) RtlCopyMemory(to + y * dst->pitch, from + y * src->pitch, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++d->d7draw.texBltFailures; return; }
    dst->epoch = 0;
    ++d->d7draw.texBlts;
}

static HRESULT clear(D7_CONTEXT *c, ULONG flags, ULONG color, ULONG depthBits, const RECT *rects, ULONG count)
{
    DD_SLOT *s, *z;
    RB2K_DD_FILL f;
    ULONG written;
    HRESULT hr = render_surfaces(c, &s, &z);
    if (FAILED(hr)) return hr;
    if (!flags) return DD_OK;
    /* Stencil and part-target clears are not drawn yet. */
    if ((flags & ~(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER)) || ((flags & D3DCLEAR_ZBUFFER) && !z) ||
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
        if (EngDeviceIoControl(c->display->miniport, IOCTL_RB2K_D3D7_ZCLEAR,
                               &both, sizeof(both), NULL, 0, &written)) return DDERR_SURFACELOST;
    } else {
        f.id = s->id;
        f.color = color;
        if (EngDeviceIoControl(c->display->miniport, IOCTL_RB2K_D3D7_CLEAR,
                               &f, sizeof(f), NULL, 0, &written)) return DDERR_SURFACELOST;
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
    case D3DNTDP2OP_STATESET: return n * sizeof(D3DNTHAL_DP2STATESET);
    case D3DNTDP2OP_SETPRIORITY: return n * sizeof(D3DNTHAL_DP2SETPRIORITY);
    case D3DNTDP2OP_SETTEXLOD: return n * sizeof(D3DNTHAL_DP2SETTEXLOD);
    case D3DNTDP2OP_SETCLIPPLANE: return n * sizeof(D3DNTHAL_DP2SETCLIPPLANE);
    }
    return 0;
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
        RtlZeroMemory(&prims, sizeof(prims));
        prims.primitives = n;
        switch (h->bCommand) {
        case D3DNTDP2OP_RENDERSTATE: {
            const D3DNTHAL_DP2RENDERSTATE *rs = (const D3DNTHAL_DP2RENDERSTATE *)p;
            used = n*sizeof(*rs);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++rs) {
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
                if (st->wStage >= TextureStages) return DDERR_INVALIDPARAMS;
                stage_state(&c->stages[st->wStage], st->TSState, st->dwValue);
            }
            break;
        }
        case D3DNTDP2OP_SETTRANSFORM: {
            /* World, view and projection; texture matrices are not applied. */
            const D3DNTHAL_DP2SETTRANSFORM *x = (const D3DNTHAL_DP2SETTRANSFORM *)p;
            used = n * sizeof(*x);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++x) {
                ++c->display->d7draw.transformTypes[x->xfrmType < 31 ? x->xfrmType : 31];
                if (x->xfrmType >= D3DTRANSFORMSTATE_WORLD && x->xfrmType <= D3DTRANSFORMSTATE_PROJECTION)
                    RtlCopyMemory(c->matrices[x->xfrmType - D3DTRANSFORMSTATE_WORLD], &x->matrix, 64);
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
                if (!s || s->depth16 || (set->hZBuffer && (!z || !z->depth16 ||
                    z->width != s->width || z->height != s->height))) return DDERR_SURFACELOST;
                if ((s->id != c->target || (z ? z->id : 0) != c->depth) && FAILED(finish_scene(c)))
                    return DDERR_SURFACELOST;
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
            hr = clear(c, f->dwFlags, f->dwFillColor, depthBits, f->Rects, n);
            if (FAILED(hr)) return hr;
            break;
        }
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
    a->dwErrorOffset = 0;
    a->ddrval = DDERR_INVALIDOBJECT;
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
            copy = EngAllocMem(0, a->dwCommandLength, '72bR');
            if (!copy) a->ddrval = DDERR_OUTOFMEMORY;
            else if (copy_commands(a, copy, &origin)) {
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
        if (FAILED(a->ddrval)) {
            c->display->d7.lastError = a->ddrval;
            ++c->display->d7draw.dp2Failures;
            c->display->d7draw.lastFailure = a->ddrval;
            c->display->d7draw.lastFailureOp = c->display->d7.lastCommand;
            c->display->d7.errorOffset = a->dwErrorOffset;
        }
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
    .dwRasterCaps = D3DPRASTERCAPS_SUBPIXEL, \
    .dwAlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL | \
                      D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS, \
    .dwSrcBlendCaps = BLEND_CAPS, .dwDestBlendCaps = BLEND_CAPS, \
    .dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | \
                   D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | \
                   D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND, \
    .dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA, \
    .dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | \
                           D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | \
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
            withDepth.hwCaps.dwDeviceZBufferBitDepth = DDBD_16;
            withDepth.hwCaps.dpcTriCaps.dwRasterCaps |= D3DPRASTERCAPS_ZTEST;
            withDepth.hwCaps.dpcTriCaps.dwZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS |
                D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER |
                D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
        }
        EngReleaseSemaphore(registry);
        info->ddCaps.dwZBufferBitDepths = DDBD_16;
        info->lpD3DGlobalDriverData = &withDepth;
    }
    info->lpD3DHALCallbacks = (PVOID)&cb;
}

BOOL rb_d7_info(PDD_GETDRIVERINFODATA a)
{
    static const GUID g3 = {0xddf41230,0xec0a,0x11d0,{0xa9,0xb6,0x00,0xaa,0x00,0xc0,0x99,0x3e}};
    static const GUID gm = {0x406b2f00,0x3e5a,0x11d1,{0xb6,0x40,0x00,0xaa,0x00,0xa1,0xf9,0x6a}};
    static const GUID parser = {0x2e04ffa0,0x98e4,0x11d1,{0x8c,0xe1,0x00,0xa0,0xc9,0x06,0x29,0xa8}};
    static const GUID depth = {0x93869880,0x36cf,0x11d1,{0x9b,0x1b,0x00,0xaa,0x00,0xbb,0xb8,0xae}};
    static const GUID extended = {0x7de41f80,0x9d93,0x11d0,{0x89,0xab,0x00,0xa0,0xc9,0x05,0x41,0x29}};
    /* Return the colour-only descriptor when paired with an older miniport. */
    static const DWORD depthFormats = 0;
    static const struct { DWORD count; DDPIXELFORMAT format; } z16 = {
        1, { .dwSize = sizeof(DDPIXELFORMAT), .dwFlags = DDPF_ZBUFFER,
             .dwZBufferBitDepth = 16, .dwZBitMask = 0xffff }
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
        .dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 |
            D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
            D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X |
            D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
            D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
            D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA,
        .wMaxTextureBlendStages = 1, .wMaxSimultaneousTextures = 1, .wMaxVertexBlendMatrices = 1,
        .dvMaxVertexW = 1.0e10f, .dwMaxActiveLights = RB2K_D7_LIGHTS,
        .dwVertexProcessingCaps = D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                                  D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER
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
    RtlCopyMemory(out, &d->d7draw, bytes);
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    return 1;
}
