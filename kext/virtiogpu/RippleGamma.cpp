/*
 * RippleGamma: see RippleGamma.h. The commands are virgl's, laid out as in virglrenderer's
 * virgl_protocol.h (third_party/virgl); the numbers used here are repeated below so that
 * kernel code does not pull that header in.
 */
#include "RippleGamma.h"

#include <IOKit/IOLib.h>

#define LOG(fmt, ...) IOLog("RippleGamma: " fmt "\n", ##__VA_ARGS__)

enum {
    // commands
    kCreateObject = 1, kBindObject = 2, kDestroyObject = 3, kSetViewportState = 4,
    kSetFramebufferState = 5, kSetVertexBuffers = 6, kDrawVBO = 8, kResourceInlineWrite = 9,
    kSetSamplerViews = 10, kBindSamplerStates = 18, kBindShader = 31,
    // objects
    kBlend = 1, kRasterizer = 2, kDSA = 3, kShader = 4, kVertexElements = 5, kSamplerView = 6,
    kSamplerState = 7, kSurface = 8,
    // formats, binds, targets, shader stages, primitives, resource flags
    kFormatR32G32Float = 29, kFormatR8 = 64, kFormatRGBA8 = 67,
    kBindRenderTarget = 1 << 1, kBindSamplerView = 1 << 3, kBindVertexBuffer = 1 << 4,
    kBindDisplayTarget = 1 << 7, kBindScanout = 1 << 18,
    kTargetBuffer = 0, kTargetTexture2D = 2,
    kVertex = 0, kFragment = 1,
    kTriangleStrip = 5,
    kFlagTopFirst = 1 << 0,
    // the names of the objects in the gamma context (virgl keeps one name space for all kinds)
    kHBlend = 1, kHDSA, kHRasterizer, kHVertexShader, kHFragmentShader, kHElements, kHSampler,
    kHSurface, kHSourceView, kHTableView,
    kCmdBytes = 16384,
};

// Floating-point numbers are written as their bits throughout: 0x3f000000 is 0.5, 0x3f800000
// is 1, 0xbf800000 is -1. Kernel code does no floating-point arithmetic.

// A rectangle over the whole image; each corner carries the place in the source it shows.
static const char kVertexShader[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: MOV OUT[1], IN[1]\n"
    "  2: END\n";

// Each colour channel of the source looks itself up in its own channel of the 256-entry table
// (sampler 1). 0x3f7f0000 is 255/256 and 0x3b000000 is 1/512: they take a channel value to
// the middle of its table entry.
static const char kFragmentShader[] =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SAMP[1]\n"
    "DCL TEMP[0..2]\n"
    "IMM[0] FLT32 {0x3f7f0000, 0x3b000000, 0x3f000000, 0x3f800000}\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: MAD TEMP[1].xyz, TEMP[0].xyzz, IMM[0].xxxx, IMM[0].yyyy\n"
    "  2: MOV TEMP[1].w, IMM[0].zzzz\n"
    "  3: TEX TEMP[2].x, TEMP[1].xwww, SAMP[1], 2D\n"
    "  4: TEX TEMP[2].y, TEMP[1].ywww, SAMP[1], 2D\n"
    "  5: TEX TEMP[2].z, TEMP[1].zwww, SAMP[1], 2D\n"
    "  6: MOV TEMP[2].w, IMM[0].wwww\n"
    "  7: MOV OUT[0], TEMP[2]\n"
    "  8: END\n";

namespace {

// A command buffer being filled, in 32-bit words
struct Words {
    uint32_t *w;
    uint32_t  n, room;

    void put(uint32_t v) { if (n < room) w[n] = v; n++; }
    void zeros(uint32_t count) { while (count--) put(0); }
    void command(uint32_t cmd, uint32_t object, uint32_t length) { put(cmd | (object << 8) | (length << 16)); }
    void bind(uint32_t object, uint32_t handle) { command(kBindObject, object, 1); put(handle); }
    void destroy(uint32_t object, uint32_t handle) { command(kDestroyObject, object, 1); put(handle); }
    // A shader as text: name, stage, the text's length with its end, room for its tokens, no
    // stream output, the text
    void shader(uint32_t handle, uint32_t stage, const char *text, uint32_t bytes)
    {
        const uint32_t textWords = (bytes + 3) / 4;
        command(kCreateObject, kShader, 5 + textWords);
        put(handle); put(stage); put(bytes); put(300); put(0);
        if (n + textWords <= room) {
            bzero(&w[n], textWords * 4);
            memcpy(&w[n], text, bytes);
        }
        n += textWords;
    }
    // A view of a whole 2D texture, its channels as they are
    void view(uint32_t handle, uint32_t resource, uint32_t format)
    {
        command(kCreateObject, kSamplerView, 6);
        put(handle); put(resource); put(format); put(0); put(0);
        put(0 | (1 << 3) | (2 << 6) | (3 << 9));
    }
    void views(uint32_t first, uint32_t second)
    {
        command(kSetSamplerViews, 0, 4);
        put(kFragment); put(0); put(first); put(second);
    }
    // The start of a write into a resource: one row of `width` units at the origin
    void write(uint32_t resource, uint32_t width, uint32_t dataWords)
    {
        command(kResourceInlineWrite, 0, 11 + dataWords);
        put(resource);
        put(0);                 // level
        put(2);                 // usage: write
        zeros(5);               // stride, layer stride, x, y, z
        put(width); put(1); put(1);
    }
    bool fits() const { return n <= room; }
};

// The bits of num / den as a 32-bit float (0 <= num / den < 2^31)
uint32_t ratioBits(uint64_t num, uint64_t den)
{
    if (!num || !den)
        return 0;
    uint64_t q = (num << 32) / den;            // 32 binary places
    if (!q)
        return 0;
    int top = 63;
    while (!(q >> top))
        top--;
    uint32_t mantissa = (uint32_t)(top >= 23 ? q >> (top - 23) : q << (23 - top)) & 0x7fffff;
    return ((uint32_t)(top - 32 + 127) << 23) | mantissa;
}

}   // namespace

bool RippleGamma::init(VirtioGPUDevice *dev)
{
    fDev = dev;
    for (unsigned c = 0; c < 3; c++)
        for (unsigned i = 0; i < 256; i++)
            fTable[c][i] = (uint8_t)i;
    fIdentity = true;
    fCmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
               kIODirectionInOut | kIOMemoryPhysicallyContiguous, kCmdBytes, 0xFFFFFFFFFFFFF000ULL);
    if (!fCmd || fCmd->prepare() != kIOReturnSuccess) {
        if (fCmd)
            fCmd->release();
        fCmd = 0;
        return false;
    }
    fCmdPA = fCmd->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
    return true;
}

void RippleGamma::free()
{
    destroy();
    if (fCmd) {
        fCmd->complete();
        fCmd->release();
        fCmd = 0;
    }
}

bool RippleGamma::setTable(uint32_t channelCount, uint32_t dataCount, uint32_t dataWidth,
                           const void *data)
{
    if (!data || (channelCount != 1 && channelCount != 3) || dataCount < 2 || dataCount > 65536 ||
        dataWidth < 1 || dataWidth > 16)
        return false;
    uint8_t table[3][256];
    bool identity = true;
    for (unsigned c = 0; c < 3; c++) {
        const uint32_t base = (channelCount == 3 ? c : 0) * dataCount;
        for (unsigned i = 0; i < 256; i++) {
            // the entry for input i / 255, and its top eight bits
            const uint32_t e = base + (uint32_t)(((uint64_t)i * (dataCount - 1) + 127) / 255);
            const uint32_t v = dataWidth <= 8 ? ((const uint8_t *)data)[e] : ((const uint16_t *)data)[e];
            table[c][i] = (uint8_t)(dataWidth >= 8 ? v >> (dataWidth - 8) : v << (8 - dataWidth));
            if (table[c][i] != i)
                identity = false;
        }
    }
    if (memcmp(table, fTable, sizeof(table)) == 0)
        return false;
    memcpy(fTable, table, sizeof(table));
    fIdentity = identity;
    fDirty = true;
    return true;
}

bool RippleGamma::create(uint32_t width, uint32_t height, uint32_t format, bool top)
{
    fCtx = fDev->allocContextId();
    if (!fDev->ctxCreate(fCtx, "ripple-gamma")) {
        fCtx = 0;
        return false;
    }
    // The image that goes on screen, made like what it stands in for; the table, 256 entries
    // of red, green, blue, alpha bytes; four corners of four floats each.
    fImage = fDev->allocResourceId();
    fTableRes = fDev->allocResourceId();
    fVertices = fDev->allocResourceId();
    bool ok =
        fDev->resourceCreate3D(fImage, kTargetTexture2D, format,
                               kBindRenderTarget | kBindSamplerView | kBindDisplayTarget | kBindScanout,
                               width, height, 1, 1, 0, 0, top ? kFlagTopFirst : 0) &&
        fDev->ctxAttachResource(fCtx, fImage) &&
        fDev->resourceCreate3D(fTableRes, kTargetTexture2D, kFormatRGBA8, kBindSamplerView,
                               256, 1, 1, 1, 0, 0, 0) &&
        fDev->ctxAttachResource(fCtx, fTableRes) &&
        fDev->resourceCreate3D(fVertices, kTargetBuffer, kFormatR8, kBindVertexBuffer,
                               64, 1, 1, 1, 0, 0, 0) &&
        fDev->ctxAttachResource(fCtx, fVertices);

    Words w = { (uint32_t *)fCmd->getBytesNoCopy(), 0, kCmdBytes / 4 };
    // Every colour channel written, nothing blended
    w.command(kCreateObject, kBlend, 11);
    w.put(kHBlend); w.put(0); w.put(0); w.put(0xfu << 27); w.zeros(7);
    // No depth, stencil or alpha test
    w.command(kCreateObject, kDSA, 5);
    w.put(kHDSA); w.zeros(4);
    // Filled, not culled, not scissored; depth clip, pixel centres at halves, bottom edge rule
    w.command(kCreateObject, kRasterizer, 9);
    w.put(kHRasterizer); w.put((1u << 1) | (1u << 29) | (1u << 30));
    w.put(0x3f800000); w.put(0); w.put(0); w.put(0x3f800000); w.zeros(3);
    w.shader(kHVertexShader, kVertex, kVertexShader, sizeof(kVertexShader));
    w.shader(kHFragmentShader, kFragment, kFragmentShader, sizeof(kFragmentShader));
    // Two vertex elements from buffer 0: position, then the place in the source
    w.command(kCreateObject, kVertexElements, 9);
    w.put(kHElements);
    w.put(0); w.put(0); w.put(0); w.put(kFormatR32G32Float);
    w.put(8); w.put(0); w.put(0); w.put(kFormatR32G32Float);
    // Nearest texel, clamped to the edge (2), no mip levels (2)
    w.command(kCreateObject, kSamplerState, 9);
    w.put(kHSampler); w.put(2 | (2 << 3) | (2 << 6) | (2 << 11)); w.zeros(7);
    w.command(kCreateObject, kSurface, 5);
    w.put(kHSurface); w.put(fImage); w.put(format); w.put(0); w.put(0);

    w.bind(kBlend, kHBlend);
    w.bind(kDSA, kHDSA);
    w.bind(kRasterizer, kHRasterizer);
    w.bind(kVertexElements, kHElements);
    w.command(kBindShader, 0, 2); w.put(kHVertexShader); w.put(kVertex);
    w.command(kBindShader, 0, 2); w.put(kHFragmentShader); w.put(kFragment);
    w.command(kBindSamplerStates, 0, 4);
    w.put(kFragment); w.put(0); w.put(kHSampler); w.put(kHSampler);
    w.command(kSetFramebufferState, 0, 3);
    w.put(1); w.put(0); w.put(kHSurface);
    // The viewport: the whole image, depth 0 to 1
    w.command(kSetViewportState, 0, 7);
    w.put(0);
    w.put(ratioBits(width, 2)); w.put(ratioBits(height, 2)); w.put(0x3f000000);
    w.put(ratioBits(width, 2)); w.put(ratioBits(height, 2)); w.put(0x3f000000);
    w.command(kSetVertexBuffers, 0, 3);
    w.put(16); w.put(0); w.put(fVertices);

    ok = ok && w.fits() && fDev->submit3D(fCtx, fCmdPA, w.n * 4, NULL);
    if (!ok) {
        LOG("could not set up the drawing on the host");
        destroy();
        return false;
    }
    fWidth = width;
    fHeight = height;
    fFormat = format;
    fTop = top;
    fTableSent = false;
    return true;
}

uint32_t RippleGamma::draw(uint32_t src, uint32_t srcWidth, uint32_t srcHeight, uint32_t format,
                           bool top, const VirtioGPUDevice::Rect &r)
{
    if (!fDev || !fCmd || !fDev->hasVirgl() || !src || !r.width || !r.height ||
        (uint64_t)r.x + r.width > srcWidth || (uint64_t)r.y + r.height > srcHeight)
        return 0;
    if (!fCtx || fWidth != r.width || fHeight != r.height || fFormat != format || fTop != top) {
        destroy();
        if (!create(r.width, r.height, format, top))
            return 0;
    }

    Words w = { (uint32_t *)fCmd->getBytesNoCopy(), 0, kCmdBytes / 4 };
    if (fDirty || !fTableSent) {
        w.write(fTableRes, 256, 256);
        for (unsigned i = 0; i < 256; i++)
            w.put(fTable[0][i] | (fTable[1][i] << 8) | (fTable[2][i] << 16) | 0xff000000u);
    }
    // The corners as a strip: (-1,-1) (1,-1) (-1,1) (1,1), each with its place in the source as
    // a fraction of the source's size. The host keeps an image's first stored row at y = -1 and
    // at coordinate 0, whichever way up the image is meant; a region is given from the top.
    const uint32_t first = top ? r.y : srcHeight - r.y - r.height;
    const uint32_t u0 = ratioBits(r.x, srcWidth), u1 = ratioBits(r.x + r.width, srcWidth);
    const uint32_t v0 = ratioBits(first, srcHeight), v1 = ratioBits(first + r.height, srcHeight);
    w.write(fVertices, 64, 16);
    w.put(0xbf800000); w.put(0xbf800000); w.put(u0); w.put(v0);
    w.put(0x3f800000); w.put(0xbf800000); w.put(u1); w.put(v0);
    w.put(0xbf800000); w.put(0x3f800000); w.put(u0); w.put(v1);
    w.put(0x3f800000); w.put(0x3f800000); w.put(u1); w.put(v1);
    // Both views are made anew for every draw. The host binds a view to its texture unit only
    // when the view in that slot changed, and binds a newly set view on whichever unit was
    // used last; a table view kept from draw to draw loses its unit to the source that way.
    w.view(kHSourceView, src, format);
    w.view(kHTableView, fTableRes, kFormatRGBA8);
    w.views(kHSourceView, kHTableView);
    w.command(kDrawVBO, 0, 12);
    w.put(0);                   // first vertex
    w.put(4);                   // count
    w.put(kTriangleStrip);
    w.put(0);                   // not indexed
    w.put(1);                   // one instance
    w.zeros(5);
    w.put(3);                   // highest index
    w.put(0);
    w.views(0, 0);
    w.destroy(kSamplerView, kHSourceView);
    w.destroy(kSamplerView, kHTableView);
    if (!w.fits())
        return 0;

    // The source belongs to whoever draws it; this context borrows it for the draw.
    fDev->ctxAttachResource(fCtx, src);
    bool ok = fDev->submit3D(fCtx, fCmdPA, w.n * 4, NULL);
    fDev->ctxDetachResource(fCtx, src);
    if (!ok)
        return 0;
    fDirty = false;
    fTableSent = true;
    return fImage;
}

void RippleGamma::destroy()
{
    if (!fCtx)
        return;
    fDev->ctxDestroy(fCtx);
    if (fImage)    fDev->resourceUnref(fImage);
    if (fTableRes) fDev->resourceUnref(fTableRes);
    if (fVertices) fDev->resourceUnref(fVertices);
    fCtx = fImage = fTableRes = fVertices = 0;
    fWidth = fHeight = fFormat = 0;
    fTableSent = false;
}
