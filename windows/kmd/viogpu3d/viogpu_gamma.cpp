/*
 * SPDX-License-Identifier: MIT
 *
 * The display's gamma ramp, applied by the driver: see viogpu_gamma.h. The commands are
 * virgl's, laid out as in virglrenderer's virgl_protocol.h. Written for moose-ripple.
 */
#include "viogpu_gamma.h"
#include "viogpu_adapter.h"
#include "viogpu_allocation.h"
#include "baseobj.h"
#include "virgl_hw.h"
#include "trace.h"

// Not paged: the ramp is copied under a spin lock.

// The names of the objects in the gamma context; virgl keeps one name space for all kinds.
enum
{
    H_BLEND = 1,
    H_DSA,
    H_RASTERIZER,
    H_VERTEX_SHADER,
    H_FRAGMENT_SHADER,
    H_ELEMENTS,
    H_SAMPLER,
    H_TABLE_VIEW,
    H_SURFACE,
    H_SOURCE_VIEW,
};

// Floating-point numbers are written as their bits throughout: 0x3f000000 is 0.5,
// 0x3f800000 is 1, 0xbf800000 is -1. Kernel code does no floating-point arithmetic.

// A rectangle over the whole target; the texture coordinate follows from the position.
static const char VertexShader[] = "VERT\n"
                                   "DCL IN[0]\n"
                                   "DCL OUT[0], POSITION\n"
                                   "DCL OUT[1], GENERIC[0]\n"
                                   "IMM[0] FLT32 {0x3f000000, 0x3f000000, 0x00000000, 0x3f800000}\n"
                                   "  0: MOV OUT[0], IN[0]\n"
                                   "  1: MAD OUT[1], IN[0], IMM[0].xxxx, IMM[0].xxxx\n"
                                   "  2: END\n";

// Each colour channel of the surface looks itself up in its own channel of the 256-entry
// table (sampler 1). 0x3f7f0000 is 255/256 and 0x3b000000 is 1/512: they take a channel value
// to the middle of its table entry.
static const char FragmentShader[] = "FRAG\n"
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

// A command buffer being filled, in 32-bit words
class Words
{
  public:
    Words(UINT *words, UINT room) : m_words(words), m_count(0), m_room(room)
    {
    }
    void Put(UINT value)
    {
        if (m_count < m_room)
        {
            m_words[m_count] = value;
        }
        m_count++;
    }
    void Zeros(UINT count)
    {
        while (count--)
        {
            Put(0);
        }
    }
    void Command(UINT command, UINT object, UINT length)
    {
        Put(VIRGL_CMD0(command, object, length));
    }
    void Bind(UINT object, UINT handle)
    {
        Command(VIRGL_CCMD_BIND_OBJECT, object, 1);
        Put(handle);
    }
    // A shader as text: name, stage, the text's length with its end, room for its tokens, no
    // stream output, the text
    void Shader(UINT handle, UINT stage, const char *text, UINT bytes)
    {
        const UINT textWords = (bytes + 3) / 4;
        Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + textWords);
        Put(handle);
        Put(stage);
        Put(bytes);
        Put(300);
        Put(0);
        if (m_count + textWords <= m_room)
        {
            RtlZeroMemory(&m_words[m_count], textWords * 4);
            RtlCopyMemory(&m_words[m_count], text, bytes);
        }
        m_count += textWords;
    }
    // A view of a whole 2D texture
    void View(UINT handle, UINT resource, UINT format)
    {
        Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
        Put(handle);
        Put(resource);
        Put(format);
        Put(0);                                // first and last layer
        Put(0);                                // first and last level
        Put(0 | (1 << 3) | (2 << 6) | (3 << 9)); // the channels as they are
    }
    void Views(UINT first, UINT second)
    {
        Command(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 4);
        Put(VIRGL_SHADER_FRAGMENT);
        Put(0);
        Put(first);
        Put(second);
    }
    // The start of a write into a resource: one row of WIDTH units at the origin
    void Write(UINT resource, UINT width, UINT dataWords)
    {
        Command(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, 11 + dataWords);
        Put(resource);
        Put(0); // level
        Put(2); // usage: write
        Put(0); // stride
        Put(0); // layer stride
        Put(0); // x, y, z
        Put(0);
        Put(0);
        Put(width);
        Put(1);
        Put(1);
    }
    UINT Count()
    {
        return m_count;
    }
    BOOLEAN Fits()
    {
        return m_count <= m_room;
    }

  private:
    UINT *m_words;
    UINT m_count;
    UINT m_room;
};

// The bits of VALUE / 2 as a 32-bit float
static UINT HalfAsFloat(UINT value)
{
    if (value == 0)
    {
        return 0;
    }
    UINT top = 31;
    while (!(value >> top))
    {
        top--;
    }
    UINT mantissa = top <= 23 ? value << (23 - top) : value >> (top - 23);
    return ((top - 1 + 127) << 23) | (mantissa & 0x7fffff);
}

void VioGpuGamma::Init(VioGpuAdapter *adapter)
{
    m_pAdapter = adapter;
    KeInitializeSpinLock(&m_lock);
    for (UINT channel = 0; channel < 3; channel++)
    {
        for (UINT i = 0; i < 256; i++)
        {
            m_table[channel][i] = (UCHAR)i;
        }
    }
    m_identity = TRUE;
    m_dirty = FALSE;
    m_ctx = m_shadow = m_lut = m_vertices = 0;
    m_width = m_height = m_format = 0;
    m_tableSent = FALSE;
    m_showing = FALSE;
}

void VioGpuGamma::SetRamp(const USHORT *red, const USHORT *green, const USHORT *blue)
{
    const USHORT *ramps[3] = {red, green, blue};
    UCHAR table[3][256];
    BOOLEAN identity = TRUE;
    for (UINT channel = 0; channel < 3; channel++)
    {
        for (UINT i = 0; i < 256; i++)
        {
            table[channel][i] = ramps[channel] ? (UCHAR)(ramps[channel][i] >> 8) : (UCHAR)i;
            if (table[channel][i] != i)
            {
                identity = FALSE;
            }
        }
    }

    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    if (RtlCompareMemory(m_table, table, sizeof(table)) != sizeof(table))
    {
        RtlCopyMemory(m_table, table, sizeof(table));
        m_identity = identity;
        m_dirty = TRUE;
    }
    KeReleaseSpinLock(&m_lock, irql);
    DbgPrint(TRACE_LEVEL_INFORMATION, ("gamma ramp: top %u %u %u\n", table[0][255], table[1][255], table[2][255]));
}

BOOLEAN VioGpuGamma::Active()
{
    return !m_identity;
}

BOOLEAN VioGpuGamma::Changed()
{
    return m_dirty;
}

// Hand a command buffer to the host. The queue frees it once the host has run it.
void VioGpuGamma::Submit(UINT *words, UINT count)
{
    m_pAdapter->ctrlQueue.SubmitCommand(words, count * 4, m_ctx, NULL, NULL, FALSE);
}

BOOLEAN VioGpuGamma::Create(UINT width, UINT height, UINT format)
{
    CtrlQueue &queue = m_pAdapter->ctrlQueue;
    const UINT room = 512;
    UINT *words = (UINT *)new (NonPagedPoolNx) BYTE[room * 4];
    if (words == NULL)
    {
        return FALSE;
    }

    // A context of the same kind the user-mode driver's are (viogpu_device.cpp)
    UINT kind = 0;
    if (virtio_is_feature_enabled(m_pAdapter->m_u64HostFeatures, 4))
    {
        kind = (m_pAdapter->m_supportedCapsetIDs & (1ull << 2)) ? 2 : 1;
    }
    m_ctx = m_pAdapter->ctxIdr.GetId();
    queue.CreateCtx(m_ctx, kind);

    // The image that goes on screen, made like the surface it stands in for
    VIOGPU_RESOURCE_OPTIONS options;
    RtlZeroMemory(&options, sizeof(options));
    options.target = VIRGL_TARGET_TEXTURE_2D;
    options.format = format;
    options.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT;
    options.width = width;
    options.height = height;
    options.depth = 1;
    options.array_size = 1;
    m_shadow = m_pAdapter->resourceIdr.GetId();
    queue.CreateResource3D(m_shadow, &options);
    queue.CtxResource(true, m_ctx, m_shadow);

    // The table: 256 entries, a row of red, green, blue, alpha bytes
    options.format = VIRGL_FORMAT_R8G8B8A8_UNORM;
    options.bind = VIRGL_BIND_SAMPLER_VIEW;
    options.width = 256;
    options.height = 1;
    m_lut = m_pAdapter->resourceIdr.GetId();
    queue.CreateResource3D(m_lut, &options);
    queue.CtxResource(true, m_ctx, m_lut);

    // Four corners, two floats each
    options.target = VIRGL_TARGET_BUFFER;
    options.format = VIRGL_FORMAT_R8_UNORM;
    options.bind = VIRGL_BIND_VERTEX_BUFFER;
    options.width = 32;
    m_vertices = m_pAdapter->resourceIdr.GetId();
    queue.CreateResource3D(m_vertices, &options);
    queue.CtxResource(true, m_ctx, m_vertices);

    Words w(words, room);
    // Every colour channel written, nothing blended
    w.Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
    w.Put(H_BLEND);
    w.Put(0);
    w.Put(0);
    w.Put(0xfu << 27);
    w.Zeros(7);
    // No depth, stencil or alpha test
    w.Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5);
    w.Put(H_DSA);
    w.Zeros(4);
    // Filled, not culled, not scissored; depth clip, pixel centres at halves, bottom edge rule
    w.Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9);
    w.Put(H_RASTERIZER);
    w.Put((1u << 1) | (1u << 29) | (1u << 30));
    w.Put(0x3f800000); // point size
    w.Put(0);
    w.Put(0);
    w.Put(0x3f800000); // line width
    w.Zeros(3);
    w.Shader(H_VERTEX_SHADER, VIRGL_SHADER_VERTEX, VertexShader, sizeof(VertexShader));
    w.Shader(H_FRAGMENT_SHADER, VIRGL_SHADER_FRAGMENT, FragmentShader, sizeof(FragmentShader));
    // One vertex element: two floats from buffer 0
    w.Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 5);
    w.Put(H_ELEMENTS);
    w.Put(0);
    w.Put(0);
    w.Put(0);
    w.Put(VIRGL_FORMAT_R32G32_FLOAT);
    // Nearest texel, clamped to the edge (2), no mip levels (2)
    w.Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_STATE, 9);
    w.Put(H_SAMPLER);
    w.Put(2 | (2 << 3) | (2 << 6) | (2 << 11));
    w.Zeros(7);
    w.Command(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    w.Put(H_SURFACE);
    w.Put(m_shadow);
    w.Put(format);
    w.Put(0); // level
    w.Put(0); // layers

    w.Bind(VIRGL_OBJECT_BLEND, H_BLEND);
    w.Bind(VIRGL_OBJECT_DSA, H_DSA);
    w.Bind(VIRGL_OBJECT_RASTERIZER, H_RASTERIZER);
    w.Bind(VIRGL_OBJECT_VERTEX_ELEMENTS, H_ELEMENTS);
    w.Command(VIRGL_CCMD_BIND_SHADER, 0, 2);
    w.Put(H_VERTEX_SHADER);
    w.Put(VIRGL_SHADER_VERTEX);
    w.Command(VIRGL_CCMD_BIND_SHADER, 0, 2);
    w.Put(H_FRAGMENT_SHADER);
    w.Put(VIRGL_SHADER_FRAGMENT);
    w.Command(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 4);
    w.Put(VIRGL_SHADER_FRAGMENT);
    w.Put(0);
    w.Put(H_SAMPLER);
    w.Put(H_SAMPLER);
    w.Command(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    w.Put(1);
    w.Put(0);
    w.Put(H_SURFACE);
    // The viewport: the whole image, depth 0 to 1
    w.Command(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
    w.Put(0);
    w.Put(HalfAsFloat(width));
    w.Put(HalfAsFloat(height));
    w.Put(0x3f000000);
    w.Put(HalfAsFloat(width));
    w.Put(HalfAsFloat(height));
    w.Put(0x3f000000);
    w.Command(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
    w.Put(8);
    w.Put(0);
    w.Put(m_vertices);
    // The corners as a strip: (-1,-1) (1,-1) (-1,1) (1,1)
    w.Write(m_vertices, 32, 8);
    w.Put(0xbf800000);
    w.Put(0xbf800000);
    w.Put(0x3f800000);
    w.Put(0xbf800000);
    w.Put(0xbf800000);
    w.Put(0x3f800000);
    w.Put(0x3f800000);
    w.Put(0x3f800000);

    if (!w.Fits())
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("gamma: the setup commands need %u words\n", w.Count()));
        delete[] (BYTE *)words;
        Destroy();
        return FALSE;
    }
    Submit(words, w.Count());

    m_width = width;
    m_height = height;
    m_format = format;
    m_tableSent = FALSE;
    m_showing = FALSE;
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("gamma: context %u, image %u (%ux%u, format %u), table %u\n", m_ctx, m_shadow, width, height, format, m_lut));
    return TRUE;
}

void VioGpuGamma::UploadTable(const UCHAR *table)
{
    const UINT room = 12 + 256;
    UINT *words = (UINT *)new (NonPagedPoolNx) BYTE[room * 4];
    if (words == NULL)
    {
        return;
    }
    Words w(words, room);
    w.Write(m_lut, 256, 256);
    for (UINT i = 0; i < 256; i++)
    {
        w.Put(table[i] | (table[256 + i] << 8) | (table[512 + i] << 16) | 0xff000000);
    }
    Submit(words, w.Count());
    m_tableSent = TRUE;
}

void VioGpuGamma::Show(VioGpuAllocation *source)
{
    CtrlQueue &queue = m_pAdapter->ctrlQueue;
    const UINT width = source->GetWidth(), height = source->GetHeight(), format = source->GetFormat();
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
    {
        return;
    }
    if (m_ctx == 0 || width != m_width || height != m_height || format != m_format)
    {
        Destroy();
        if (!Create(width, height, format))
        {
            return;
        }
    }

    UCHAR table[3][256];
    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    BOOLEAN upload = m_dirty;
    m_dirty = FALSE;
    RtlCopyMemory(table, m_table, sizeof(table));
    KeReleaseSpinLock(&m_lock, irql);
    if (upload || !m_tableSent)
    {
        UploadTable(&table[0][0]);
    }

    // Both views are made anew for every draw. The host binds a view to its texture unit only
    // when the view in that slot changed, and binds a newly set view on whichever unit was
    // used last; a table view kept from draw to draw lost its unit to the surface that way.
    const UINT room = 48;
    UINT *words = (UINT *)new (NonPagedPoolNx) BYTE[room * 4];
    if (words == NULL)
    {
        return;
    }
    Words w(words, room);
    w.View(H_SOURCE_VIEW, source->GetId(), format);
    w.View(H_TABLE_VIEW, m_lut, VIRGL_FORMAT_R8G8B8A8_UNORM);
    w.Views(H_SOURCE_VIEW, H_TABLE_VIEW);
    w.Command(VIRGL_CCMD_DRAW_VBO, 0, 12);
    w.Put(0); // first vertex
    w.Put(4); // count
    w.Put(VIRGL_PRIM_TRIANGLE_STRIP);
    w.Put(0); // not indexed
    w.Put(1); // one instance
    w.Zeros(5);
    w.Put(3); // highest index
    w.Put(0);
    w.Views(0, 0);
    w.Command(VIRGL_CCMD_DESTROY_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 1);
    w.Put(H_SOURCE_VIEW);
    w.Command(VIRGL_CCMD_DESTROY_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 1);
    w.Put(H_TABLE_VIEW);
    if (!w.Fits())
    {
        delete[] (BYTE *)words;
        return;
    }

    // The surface belongs to the contexts that draw it; this one borrows it for the draw.
    queue.CtxResource(true, m_ctx, source->GetId());
    Submit(words, w.Count());
    queue.CtxResource(false, m_ctx, source->GetId());

    if (!m_showing)
    {
        queue.SetScanout(0, m_shadow, width, height, 0, 0);
        m_showing = TRUE;
    }
    queue.ResFlush(m_shadow, width, height, 0, 0);
}

void VioGpuGamma::Destroy()
{
    if (m_ctx == 0)
    {
        return;
    }
    CtrlQueue &queue = m_pAdapter->ctrlQueue;
    queue.DestroyCtx(m_ctx);
    m_pAdapter->ctxIdr.PutId(m_ctx);
    const UINT resources[3] = {m_shadow, m_lut, m_vertices};
    for (UINT i = 0; i < 3; i++)
    {
        if (resources[i] != 0)
        {
            queue.DestroyResource(resources[i]);
            m_pAdapter->resourceIdr.PutId(resources[i]);
        }
    }
    m_ctx = m_shadow = m_lut = m_vertices = 0;
    m_width = m_height = m_format = 0;
    m_tableSent = FALSE;
    m_showing = FALSE;
}
