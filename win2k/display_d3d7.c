/* Stock NT5 Direct3D 7 callbacks. An offscreen DirectDraw surface owns the
 * host image. The runtime's surface numbers are local to a DirectDraw object.
 * Numeric context handles never expose display-driver addresses. */
#include "display_private.h"
#include <d3dnthal.h>
#include "rb2kd3d7.h"

/* These Direct3D 7 HRESULTs are declared only in the user-side d3d.h. */
#ifndef D3DERR_UNSUPPORTEDCOLOROPERATION
#define D3DERR_UNSUPPORTEDCOLOROPERATION MAKE_DDHRESULT(2073)
#define D3DERR_UNSUPPORTEDALPHAOPERATION MAKE_DDHRESULT(2075)
#endif

enum { MaxContexts = 16, MaxCommandBytes = 65536, TextureStages = 8 };
typedef struct {
    ULONG texture, colorOp, colorArg1, alphaOp, alphaArg1;
} D7_STAGE;
typedef struct {
    ULONG id, target;
    DISPLAY *display;
    PDD_DIRECTDRAW_LOCAL local;
    HANDLE owner;
    ULONG cull, shade, fill, drawFeatures;
    D7_STAGE stages[TextureStages];
} D7_CONTEXT;

static HSEMAPHORE registry;
static D7_CONTEXT contexts[MaxContexts];
static ULONG lastContext;

/* A context-only callback takes the registry first, then the display lock.
 * DrvDisableDirectDraw drops the display lock before retiring the contexts. */
void rb_d7_init(void)
{
    if (!registry) registry = EngCreateSemaphore();
}

static D7_CONTEXT *owned_context(ULONG id)
{
    ULONG i;
    for (i = 0; id && i < MaxContexts; ++i)
        if (contexts[i].id == id && contexts[i].owner == EngGetCurrentProcessId())
            return &contexts[i];
    return NULL;
}

void rb_d7_disable(DISPLAY *d)
{
    ULONG i;
    if (!registry) return;
    EngAcquireSemaphore(registry);
    for (i = 0; i < MaxContexts; ++i)
        if (contexts[i].display == d) RtlZeroMemory(&contexts[i], sizeof(contexts[i]));
    EngReleaseSemaphore(registry);
}

static DWORD APIENTRY register_surface(PDD_CREATESURFACEEXDATA a)
{
    PDD_SURFACE_LOCAL l = a->lpDDSLcl;
    DISPLAY *d = (DISPLAY *)a->lpDDLcl->lpGbl->dhpdev;
    DD_SLOT *slot;
    a->ddRVal = DD_OK;
    if (!l || !l->lpGbl || !(l->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) ||
        (l->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) return DDHAL_DRIVER_HANDLED;
    EngAcquireSemaphore(d->ddLock);
    ++d->d7.surfaceCalls;
    slot = rb_dd_slot(d, l->lpGbl);
    if (!d->ddEnabled || !slot || slot->lost || slot->owner != EngGetCurrentProcessId())
        a->ddRVal = DDERR_SURFACELOST;
    else if (a->dwFlags || !l->lpSurfMore || !l->lpSurfMore->dwSurfaceHandle ||
             (slot->local && slot->local != a->lpDDLcl))
        a->ddRVal = DDERR_UNSUPPORTED;
    else {
        slot->local = a->lpDDLcl;
        slot->d3dHandle = l->lpSurfMore->dwSurfaceHandle;
    }
    EngReleaseSemaphore(d->ddLock);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY destroy_local(PDD_DESTROYDDLOCALDATA a)
{
    DISPLAY *d = (DISPLAY *)a->pDDLcl->lpGbl->dhpdev;
    ULONG i;
    EngAcquireSemaphore(registry);
    EngAcquireSemaphore(d->ddLock);
    for (i = 0; i < MaxContexts; ++i)
        if (contexts[i].display == d && contexts[i].local == a->pDDLcl)
            RtlZeroMemory(&contexts[i], sizeof(contexts[i]));
    for (i = 0; i < RB2K_DD_SLOTS; ++i) {
        DD_SLOT *s = &d->slots[i];
        if (s->local == a->pDDLcl) { s->local = NULL; s->d3dHandle = 0; }
    }
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    a->ddRVal = DD_OK;
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

static DWORD APIENTRY create_context(LPD3DNTHAL_CONTEXTCREATEDATA a)
{
    PDD_DIRECTDRAW_LOCAL local = a->lpDDLcl;
    ULONG i;
    DISPLAY *d;
    DD_SLOT *s;
    a->dwhContext = 0;
    a->ddrval = DDERR_UNSUPPORTED;
    if (!local || !local->lpGbl || !a->lpDDSLcl || !a->lpDDSLcl->lpGbl ||
        !(a->lpDDSLcl->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) || a->lpDDSZLcl)
        return DDHAL_DRIVER_HANDLED;
    d = (DISPLAY *)local->lpGbl->dhpdev;
    EngAcquireSemaphore(registry);
    EngAcquireSemaphore(d->ddLock);
    s = rb_dd_slot(d, a->lpDDSLcl->lpGbl);
    ++d->d7.contextCalls;
    if (!d->ddEnabled || !s || s->lost || s->owner != EngGetCurrentProcessId() ||
        (s->local && s->local != local)) a->ddrval = DDERR_SURFACELOST;
    else {
        a->ddrval = DDERR_OUTOFMEMORY;
        for (i = 0; lastContext != 0xffffffffUL && i < MaxContexts; ++i) {
            D7_CONTEXT *c = &contexts[i];
            ULONG stage;
            if (c->id) continue;
            c->display = d;
            c->local = local;
            c->owner = s->owner;
            c->target = s->id;
            c->cull = D3DCULL_CCW;
            c->shade = D3DSHADE_GOURAUD;
            c->fill = D3DFILL_SOLID;
            c->id = ++lastContext;
            for (stage = 0; stage < TextureStages; ++stage) {
                c->stages[stage].colorOp = stage ? D3DTOP_DISABLE : D3DTOP_MODULATE;
                c->stages[stage].alphaOp = stage ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
                c->stages[stage].colorArg1 = c->stages[stage].alphaArg1 = D3DTA_TEXTURE;
            }
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
    if (c) RtlZeroMemory(c, sizeof(*c));
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

/* Stage zero starts with interpolated diffuse colour. This first path can
 * pass it through unchanged. An active sampler or blend requires more of
 * the fixed-function pipeline than this device currently implements. */
static BOOL diffuse_argument(ULONG arg)
{
    return arg == D3DTA_DIFFUSE || arg == D3DTA_CURRENT;
}

static HRESULT texture_passes(const D7_CONTEXT *c)
{
    ULONG i;
    for (i = 0; i < TextureStages; ++i) {
        const D7_STAGE *s = &c->stages[i];
        if (s->colorOp == D3DTOP_DISABLE || (!s->texture && s->colorArg1 == D3DTA_TEXTURE))
            break;
        if (s->colorOp != D3DTOP_SELECTARG1 || !diffuse_argument(s->colorArg1))
            return D3DERR_UNSUPPORTEDCOLOROPERATION;
        /* With an XRGB target and blending and alpha testing off, a missing
         * alpha texture cannot affect an output colour channel. */
        if (s->alphaOp != D3DTOP_SELECTARG1 || (!diffuse_argument(s->alphaArg1) &&
            (s->texture || s->alphaArg1 != D3DTA_TEXTURE)))
            return D3DERR_UNSUPPORTEDALPHAOPERATION;
    }
    return DD_OK;
}

static DWORD APIENTRY validate_stages(LPD3DNTHAL_VALIDATETEXTURESTAGESTATEDATA a)
{
    D7_CONTEXT *c;
    EngAcquireSemaphore(registry);
    c = owned_context(a->dwhContext);
    a->ddrval = !c ? DDERR_INVALIDOBJECT : a->dwFlags ? DDERR_INVALIDPARAMS : texture_passes(c);
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

/* The first rasterizer accepts solid, untextured Gouraud triangles. Keep
 * state that changes their observable colour or coverage across DP2 calls. */
static void render_state(D7_CONTEXT *c, ULONG state, ULONG value)
{
    ULONG feature;
    switch (state) {
    case D3DRENDERSTATE_CULLMODE: c->cull = value; return;
    case D3DRENDERSTATE_SHADEMODE: c->shade = value; return;
    case D3DRENDERSTATE_FILLMODE: c->fill = value; return;
    case D3DRENDERSTATE_ZENABLE: feature = 1; break;
    case D3DRENDERSTATE_ALPHATESTENABLE: feature = 2; break;
    case D3DRENDERSTATE_ALPHABLENDENABLE: feature = 4; break;
    case D3DRENDERSTATE_FOGENABLE: feature = 8; break;
    case D3DRENDERSTATE_STENCILENABLE: feature = 16; break;
    case D3DRENDERSTATE_SPECULARENABLE: feature = 32; break;
    case D3DRENDERSTATE_CLIPPLANEENABLE: feature = 64; break;
    default: return;
    }
    if (value) c->drawFeatures |= feature;
    else c->drawFeatures &= ~feature;
}

static HRESULT triangle_list(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a,
                             ULONG stride, ULONG first, ULONG primitives)
{
    DD_SLOT *s = target(c, c->target, FALSE);
    RB2K_D3D7_VERTEX *vertices;
    RB2K_D3D7_BATCH batch;
    PDD_SURFACE_GLOBAL g;
    ULONG address, offset, bytes, i, written, count = 3*primitives;
    HRESULT hr;
    if (!s) return DDERR_SURFACELOST;
    if (s->locked) return DDERR_SURFACEBUSY;
    if (!count) return DD_OK;
    if (c->drawFeatures || c->shade != D3DSHADE_GOURAUD || c->fill != D3DFILL_SOLID ||
        c->cull != D3DCULL_NONE || (a->dwVertexType & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW ||
        !(a->dwVertexType & D3DFVF_DIFFUSE) || (a->dwVertexType & (D3DFVF_RESERVED0 | D3DFVF_RESERVED1 |
                                                                   D3DFVF_NORMAL))) return DDERR_UNSUPPORTED;
    hr = texture_passes(c);
    if (FAILED(hr)) return hr;
    if (stride < sizeof(*vertices) || stride > 256 || first > a->dwVertexLength ||
        count > a->dwVertexLength-first || first > (0xffffffffUL-a->dwVertexOffset)/stride ||
        count > MaxCommandBytes/stride) return DDERR_INVALIDPARAMS;
    offset = a->dwVertexOffset+first*stride;
    bytes = count*stride;
    if (bytes > 0xffffffffUL-offset) return DDERR_INVALIDPARAMS;
    if (a->dwFlags & D3DNTHALDP2_USERMEMVERTICES) address = (ULONG)a->lpVertices;
    else {
        if (!a->lpDDVertex || !(a->lpDDVertex->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ||
            !(g = a->lpDDVertex->lpGbl) || offset+bytes > g->dwLinearSize) return DDERR_INVALIDPARAMS;
        address = g->fpVidMem;
    }
    if (!address || offset+bytes > 0xffffffffUL-address) return DDERR_INVALIDPARAMS;
    vertices = EngAllocMem(0, count*sizeof(*vertices), 'v7bR');
    if (!vertices) return DDERR_OUTOFMEMORY;
    /* Snapshot and check the whole list before the first host draw. The
     * driver only reads the transformed position and packed diffuse colour. */
    hr = DD_OK;
    __try {
        const BYTE *v = (const BYTE *)(address+offset);
        EngProbeForRead((PVOID)v, bytes, 1);
        for (i = 0; i < count; ++i, v += stride) {
            RB2K_D3D7_VERTEX *d = &vertices[i];
            RtlCopyMemory(d, v, sizeof(*d));
            if ((d->x & 0x7fffffff) >= 0x7f800000 || (d->y & 0x7fffffff) >= 0x7f800000 ||
                (d->z & 0x7fffffff) >= 0x7f800000 || !d->rhw || d->rhw >= 0x7f800000) {
                hr = DDERR_INVALIDPARAMS;
                break;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { hr = DDERR_INVALIDPARAMS; }

    batch.target = s->id;
    for (i = 0; SUCCEEDED(hr) && i < count; i += batch.count) {
        batch.count = min(count-i,RB2K_D3D7_VERTICES);
        RtlCopyMemory(batch.vertices, vertices+i, batch.count*sizeof(*vertices));
        if (EngDeviceIoControl(c->display->miniport, IOCTL_RB2K_D3D7_TRIANGLES, &batch,
                FIELD_OFFSET(RB2K_D3D7_BATCH,vertices)+batch.count*sizeof(*vertices),
                NULL, 0, &written)) hr = DDERR_GENERIC;
        else c->display->d7.totals.triangles += batch.count/3;
    }
    EngFreeMem(vertices);
    return hr;
}

static HRESULT clear(D7_CONTEXT *c, ULONG flags, ULONG color, const RECT *rects, ULONG count)
{
    DD_SLOT *s = target(c, c->target, FALSE);
    RB2K_DD_FILL f;
    ULONG written;
    if (!s) return DDERR_SURFACELOST;
    if (s->locked) return DDERR_SURFACEBUSY;
    if (flags != D3DCLEAR_TARGET || count != 1 || !rects || rects->left || rects->top ||
        rects->right != (LONG)s->width || rects->bottom != (LONG)s->height)
        return DDERR_UNSUPPORTED;
    f.id = s->id;
    f.color = color;
    if (EngDeviceIoControl(c->display->miniport, IOCTL_RB2K_D3D7_CLEAR,
                           &f, sizeof(f), NULL, 0, &written)) return DDERR_SURFACELOST;
    ++c->display->d7.totals.clears;
    return DD_OK;
}

/* The command buffer is an NT system-memory execute surface. Snapshot a
 * bounded range before walking it so its lengths cannot change during use. */
static BOOL copy_commands(LPD3DNTHAL_DRAWPRIMITIVES2DATA a, BYTE *out)
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
    return TRUE;
}

/* The 9x DP2 wire layout is identical here. Consume only complete records
 * from the snapshot. State packets cannot affect a target-only clear. */
static HRESULT commands(D7_CONTEXT *c, LPD3DNTHAL_DRAWPRIMITIVES2DATA a, ULONG stride,
                        const BYTE *bytes, ULONG length, ULONG *offset)
{
    while (*offset < length) {
        const D3DNTHAL_DP2COMMAND *h;
        const BYTE *p;
        ULONG n, used, remaining = length-*offset;
        if (remaining < sizeof(*h)) return DDERR_INVALIDPARAMS;
        h = (const D3DNTHAL_DP2COMMAND *)(bytes + *offset);
        c->display->d7.lastCommand = h->bCommand;
        n = h->wStateCount;
        p = (const BYTE *)(h + 1);
        remaining -= sizeof(*h);
        switch (h->bCommand) {
        case D3DNTDP2OP_RENDERSTATE: {
            const D3DNTHAL_DP2RENDERSTATE *rs = (const D3DNTHAL_DP2RENDERSTATE *)p;
            used = n*sizeof(*rs);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++rs) render_state(c, rs->RenderState, rs->dwState);
            break;
        }
        case D3DNTDP2OP_TRIANGLELIST: {
            const D3DNTHAL_DP2TRIANGLELIST *list = (const D3DNTHAL_DP2TRIANGLELIST *)p;
            HRESULT hr;
            used = sizeof(*list);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            hr = triangle_list(c, a, stride, list->wVStart, n);
            if (FAILED(hr)) return hr;
            break;
        }
        case D3DNTDP2OP_TEXTURESTAGESTATE: {
            const D3DNTHAL_DP2TEXTURESTAGESTATE *st = (const D3DNTHAL_DP2TEXTURESTAGESTATE *)p;
            used = n*sizeof(*st);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++st) {
                D7_STAGE *s;
                if (st->wStage >= TextureStages) return DDERR_INVALIDPARAMS;
                s = &c->stages[st->wStage];
                switch (st->TSState) {
                case D3DTSS_TEXTUREMAP: s->texture = st->dwValue; break;
                case D3DTSS_COLOROP: s->colorOp = st->dwValue; break;
                case D3DTSS_COLORARG1: s->colorArg1 = st->dwValue; break;
                case D3DTSS_ALPHAOP: s->alphaOp = st->dwValue; break;
                case D3DTSS_ALPHAARG1: s->alphaArg1 = st->dwValue; break;
                }
            }
            break;
        }
        case D3DNTDP2OP_VIEWPORTINFO: used = n*sizeof(D3DNTHAL_DP2VIEWPORTINFO); break;
        case D3DNTDP2OP_WINFO: used = n*sizeof(D3DNTHAL_DP2WINFO); break;
        case D3DNTDP2OP_ZRANGE: used = n*sizeof(D3DNTHAL_DP2ZRANGE); break;
        case D3DNTDP2OP_EXT: {
            const D3DNTHAL_DP2EXT *ext = (const D3DNTHAL_DP2EXT *)p;
            PVOID next = NULL;
            HRESULT hr;
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
            DD_SLOT *s;
            used = n*sizeof(*set);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            for (; n; --n, ++set) {
                if (set->hZBuffer) return DDERR_UNSUPPORTED;
                s = target(c, set->hRenderTarget, TRUE);
                if (!s) return DDERR_SURFACELOST;
                c->target = s->id;
            }
            break;
        }
        case D3DNTDP2OP_CLEAR: {
            const D3DNTHAL_DP2CLEAR *f = (const D3DNTHAL_DP2CLEAR *)p;
            HRESULT hr;
            ++c->display->d7.clearCalls;
            used = FIELD_OFFSET(D3DNTHAL_DP2CLEAR, Rects) + n*sizeof(RECT);
            if (used > remaining) return DDERR_INVALIDPARAMS;
            hr = clear(c, f->dwFlags, f->dwFillColor, f->Rects, n);
            if (FAILED(hr)) return hr;
            break;
        }
        default: return D3DNTERR_COMMAND_UNPARSED;
        }
        if (used > remaining) return DDERR_INVALIDPARAMS;
        *offset += sizeof(*h) + used;
    }
    return DD_OK;
}

static DWORD APIENTRY dp2(LPD3DNTHAL_DRAWPRIMITIVES2DATA a)
{
    D7_CONTEXT *c;
    BYTE *copy = NULL;
    ULONG stride = a->dwVertexSize; /* Shares its field with the return code. */
    a->dwErrorOffset = 0;
    a->ddrval = DDERR_INVALIDOBJECT;
    EngAcquireSemaphore(registry);
    c = owned_context(a->dwhContext);
    if (c) {
        EngAcquireSemaphore(c->display->ddLock);
        ++c->display->d7.commandCalls;
        a->ddrval = DDERR_INVALIDPARAMS;
        if (!a->dwCommandLength) a->ddrval = DD_OK;
        else if (a->dwCommandLength <= MaxCommandBytes) {
            copy = EngAllocMem(0, a->dwCommandLength, '72bR');
            if (!copy) a->ddrval = DDERR_OUTOFMEMORY;
            else if (copy_commands(a, copy)) {
                a->ddrval = commands(c, a, stride, copy, a->dwCommandLength, &a->dwErrorOffset);
            }
        }
        if (FAILED(a->ddrval)) {
            c->display->d7.lastError = a->ddrval;
            c->display->d7.errorOffset = a->dwErrorOffset;
        }
        EngReleaseSemaphore(c->display->ddLock);
    }
    if (copy) EngFreeMem(copy);
    EngReleaseSemaphore(registry);
    return DDHAL_DRIVER_HANDLED;
}

/* The runtime reads DDSURFACEDESC records here, including on DirectX 7.
 * The associated texture has a single XRGB level and a host sampler bind. */
static DDSURFACEDESC textureFormat = {
    .dwSize = sizeof(DDSURFACEDESC), .dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT,
    .ddsCaps = { .dwCaps = DDSCAPS_TEXTURE },
    .ddpfPixelFormat = {
        .dwSize = sizeof(DDPIXELFORMAT), .dwFlags = DDPF_RGB, .dwRGBBitCount = 32,
        .dwRBitMask = 0xff0000, .dwGBitMask = 0xff00, .dwBBitMask = 0xff
    }
};
static const D3DNTHAL_GLOBALDRIVERDATA global = {
    .dwSize = sizeof(D3DNTHAL_GLOBALDRIVERDATA),
    .dwNumTextureFormats = 1, .lpTextureFormats = &textureFormat,
    .hwCaps = {
        .dwSize = sizeof(D3DNTHALDEVICEDESC_V1),
        .dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRICAPS |
                   D3DDD_LINECAPS | D3DDD_DEVICERENDERBITDEPTH,
        .dcmColorModel = D3DCOLOR_RGB,
        .dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                     D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_DRAWPRIMITIVES2 |
                     D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION,
        .dtcTransformCaps = { .dwSize = sizeof(D3DTRANSFORMCAPS) },
        .dlcLightingCaps = { .dwSize = sizeof(D3DLIGHTINGCAPS) },
        .dpcLineCaps = { .dwSize = sizeof(D3DPRIMCAPS) },
        .dpcTriCaps = { .dwSize = sizeof(D3DPRIMCAPS), .dwMiscCaps = D3DPMISCCAPS_CULLNONE,
                        .dwShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB },
        .dwDeviceRenderBitDepth = DDBD_32
    }
};
static const D3DNTHAL_CALLBACKS cb = {
    .dwSize = sizeof(D3DNTHAL_CALLBACKS), .ContextCreate = create_context,
    .ContextDestroy = destroy_context
};

void rb_d7_caps(DD_HALINFO *info)
{
    if (!registry) return;
    info->ddCaps.dwCaps |= DDCAPS_3D;
    info->lpD3DGlobalDriverData = (PVOID)&global;
    info->lpD3DHALCallbacks = (PVOID)&cb;
}

BOOL rb_d7_info(PDD_GETDRIVERINFODATA a)
{
    static const GUID g3 = {0xddf41230,0xec0a,0x11d0,{0xa9,0xb6,0x00,0xaa,0x00,0xc0,0x99,0x3e}};
    static const GUID gm = {0x406b2f00,0x3e5a,0x11d1,{0xb6,0x40,0x00,0xaa,0x00,0xa1,0xf9,0x6a}};
    static const GUID parser = {0x2e04ffa0,0x98e4,0x11d1,{0x8c,0xe1,0x00,0xa0,0xc9,0x06,0x29,0xa8}};
    static const GUID depth = {0x93869880,0x36cf,0x11d1,{0x9b,0x1b,0x00,0xaa,0x00,0xbb,0xb8,0xae}};
    static const GUID extended = {0x7de41f80,0x9d93,0x11d0,{0x89,0xab,0x00,0xa0,0xc9,0x05,0x41,0x29}};
    /* GUID_ZPixelFormats returns a DWORD count followed by that many
     * DDPIXELFORMATs. The initial colour-only pipeline has no depth surface. */
    static const DWORD depthFormats = 0;
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
    D3DNTHAL_D3DEXTENDEDCAPS caps = {0};
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
    else if (IsEqualGUID(&a->guidInfo, &depth)) { source = &depthFormats; size = sizeof(depthFormats); }
    else if (IsEqualGUID(&a->guidInfo, &extended)) {
        /* DX6 and DX7 request different lengths of this record. The HAL
         * currently accepts XRGB storage and a single diffuse stage. */
        caps.dwSize = min(a->dwExpectedSize, sizeof(caps));
        caps.dwMinTextureWidth = caps.dwMinTextureHeight = 1;
        caps.dwMaxTextureWidth = RB2K_DD_MAX_WIDTH;
        caps.dwMaxTextureHeight = RB2K_DD_MAX_HEIGHT;
        caps.dwMaxTextureAspectRatio = RB2K_DD_MAX_WIDTH;
        caps.dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1;
        caps.wMaxTextureBlendStages = 1;
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
    s.totals.version = RB2K_D3D7_STATS_VERSION;
    for (i = 0; i < MaxContexts; ++i) if (contexts[i].display == d) ++s.totals.contexts;
    for (i = 0; i < RB2K_DD_SLOTS; ++i) if (d->slots[i].d3dHandle) ++s.totals.targets;
    EngReleaseSemaphore(d->ddLock);
    EngReleaseSemaphore(registry);
    RtlCopyMemory(out, &s, bytes);
    return 1;
}
