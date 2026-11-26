#ifndef RB2K_GPU_TRANSPORT_H
#define RB2K_GPU_TRANSPORT_H

/* Include miniport.h and video.h before this header. */
#include "rb2kprobe.h"
#include "ddsurface.h"
#include "rb2kcursor.h"
#include "rb2kd3d7.h"

/* Host state objects one colour context has created for Direct3D draws.
 * Each table maps a validated key to a fixed object handle range. */
enum { RB2K_D7_DSA_KEYS = 16, RB2K_D7_BLEND_KEYS = 8, RB2K_D7_SAMPLER_KEYS = 8,
       RB2K_D7_SHADER_KEYS = 16, RB2K_D8_SAMPLER_KEYS = 16 };
typedef struct {
    ULONG keys[RB2K_D7_SHADER_KEYS > RB2K_D7_DSA_KEYS ? RB2K_D7_SHADER_KEYS : RB2K_D7_DSA_KEYS][6];
    ULONG used, next, bound;
} RB2K_D7_CACHE;
typedef struct {
    BOOLEAN pipeline;
    ULONG vertexShaders, boundVertex;   /* bit and value: 1 transformed input, 2 object space */
    ULONG rasters, boundRaster, clipRaster, viewTexture, viewTexture1;
    ULONG pointSize, pointFlags;        /* the point rasterizer's, once pointMade */
    BOOLEAN pointMade;
    /* Samplers and views bound together from slot zero; the renderer drops
     * the slots past the last set. boundViews counts views, boundView0 is the
     * view handle in slot zero. */
    ULONG boundSampler[2], boundSamplers, boundViews, boundView0;
    RB2K_D7_CACHE dsa, blend, sampler, sampler1, shader;   /* sampler1: texture stage one */
    /* Pixel shader draws: the texture in each stage's view, and their samplers. */
    ULONG psViews[RB2K_D8_STAGES];
    RB2K_D7_CACHE psSampler;
    RB2K_D7_CACHE vsProgram;            /* vertex programs by id */
    ULONG stencilRef;                   /* the reference value sent, plus 0x100 once sent */
} RB2K_D7_STATE;

/* Attachment to the colour contexts of images, one bit per slot of
 * RB2K_GPU.images. Destroying an image clears its bit everywhere, since the
 * next image in that slot has a context of its own. */
#define RB2K_SLOT_WORDS (RB2K_DD_SLOTS / 32)
typedef struct RB2K_IMAGE {
    ULONG id, width, height;
    BOOLEAN defined, hasSurface, uniform;
    ULONG color;
    BOOLEAN frameFailed;
    BOOLEAN depth16;
    BOOLEAN stencil8;       /* a depth image of 24 bits with 8 of stencil (with depth16 set) */
    struct RB2K_IMAGE *depthBuffer, *depthOwner;
    ULONG sampledBy[RB2K_SLOT_WORDS];   /* targets that sample this image, a bit per image slot */
    RB2K_D7_STATE d7;
} RB2K_IMAGE;

/* A Direct3D texture is a host resource without a context of its own. It
 * is attached to each colour context that samples it. */
typedef struct {
    ULONG id, width, height, levels, bytes;   /* bytes: counted against RB2K_D7_TEXEL_BUDGET */
    ULONG format;                               /* host format; 0 for B8G8R8A8 */
    ULONG faces;                                /* 1, or 6 for a cube map */
    ULONG depth;                                /* a volume's slices; 1 otherwise */
    ULONG attached[RB2K_SLOT_WORDS];    /* the colour contexts it is attached to, a bit per image slot */
} RB2K_TEXTURE;
#define RB2K_D7_TEXEL_BUDGET (64UL << 20)
typedef struct {
    RB2K_IMAGE image;
    RB2K_RECT gpuPixels, gdiWrites[3];
    ULONG writeCount, writeTags[3];
    RB2K_RECT clip[RB2K_GL_MAX_RECTS];
    ULONG clipCount;
    BOOLEAN visible, inlineUploads;
    /* A program's full-screen image on the display instead of this one
     * (RB2K_RVD_OP_SCANOUT), until RB2K_RVD_OP_DESKTOP or until it goes. */
    ULONG scanout;
} RB2K_GL_DESKTOP;

typedef struct {
    PULONG pixels;
    ULONG width, height, pitchPixels, bytes, bitsPerPixel;
} RB2K_PRIMARY;

#define RB2K_SEGMENT_BYTES 65536UL
#define RB2K_MAX_SEGMENTS ((RB2K_DD_MAX_BYTES + RB2K_SEGMENT_BYTES - 1) / RB2K_SEGMENT_BYTES)
typedef struct {
    PUCHAR data;
    PHYSICAL_ADDRESS bus;
    ULONG allocated;
} RB2K_DMA_SEGMENT;

enum { RB2K_D7_FLIGHT_SLOTS = 2 };
typedef struct {
    RB2K_IMAGE *image;
    RB2K_D3D7_COMPLETED work;
    ULONG fence;   /* zero: an unfenced packet, done once the host has read it */
} RB2K_D7_SUBMISSION;

/* Device-readable data chained after a request (at most RB2K_GPU_MAX_EXTENTS). */
#define RB2K_GPU_MAX_EXTENTS 13
typedef struct {
    PHYSICAL_ADDRESS bus;
    ULONG length;
} RB2K_GPU_EXTENT;

/* OpenGL (the RIPPLEVD interface, gpu_rvd.c) has its own staging, written and
 * read by the display DLL under its lock between requests. */
#define RB2K_RVD_SEGMENTS 48
#define RB2K_INLINE_PACKET_BYTES 2048
/* Native Direct3D 7 command words gathered into one virgl submission. */
/* A scene packet: 256 KB, as large as Mesa's own virgl command buffers. Each
 * submit costs a fence, a host batch and a round trip; with draws carrying their
 * vertices as constants, 64 KB held only about ten draws. */
#define RB2K_D7_FRAME_WORDS ((256UL * 1024 - 128) / sizeof(ULONG))

typedef struct {
    PVOID extension;
    PUCHAR common, notifyBase;
    PUSHORT doorbell;
    ULONG notifySize, multiplier;
    PUCHAR dma;
    PHYSICAL_ADDRESS bus;
    ULONG allocated;
    USHORT index, usedIndex;
    USHORT cursorIndex;
    PUSHORT cursorDoorbell;
    ULONG features, cursorResource;
    BOOLEAN cursorEnabled, cursorValid, cursorVisible;
    RB2K_CURSOR_SHAPE cursorShape;
    RB2K_CURSOR_STATS cursorStats;
    LONG busy, barrier;
    BOOLEAN owned, ready, enabledMaster, tested;
    ULONG nextId, fence, imageCount, hostFills, readbacks, uploads, hostCopies;
    BOOLEAN ddGpuSurfaces, nativeD3D7, plainSurfaces, d3d8, vertexShaders, d3d9Ddi;
    /* DXT blocks as they are (RippleHostDxt), and the formats the host samples:
     * bit n for upload format n (RB2K_D7_BLOCKS_DXT*, RB2K_D7_TEXELS_*), known after the first ask. */
    BOOLEAN hostDxt, hostBlocksKnown;
    ULONG hostBlocks;
    struct {
        RB2K_IMAGE *target;
        ULONG count;
        RB2K_D3D7_COMPLETED pending;
        ULONG words[RB2K_D7_FRAME_WORDS];
    } d7Frame;
    RB2K_D3D7_COMPLETED d7Completed;
    RB2K_D7_SUBMISSION d7Submitted[RB2K_D7_FLIGHT_SLOTS];
    RB2K_DMA_SEGMENT d7Packets[RB2K_D7_FLIGHT_SLOTS];   /* one buffer per packet slot */
    RB2K_D7_FLIGHT d7Flight;
    ULONG d7Next;
    BOOLEAN d7WaitCurrent;
    RB2K_D7_PROFILE d7Profile;
    RB2K_D7_TIME *d7Measure; /* set only during a serialized native operation */
    BOOLEAN glEnabled;
    ULONG presents, presentReadbacks, presentBytes;
    RB2K_PRIMARY primary;
    RB2K_GL_DESKTOP glDesktop;
    RB2K_GL_STATS glStats;
    RB2K_GL_TIMING glTiming;
    RB2K_GL_PROFILE glProfile;
    /* Private to the synchronous miniport request. Keep the maximum copy
     * batch off the NT kernel stack. */
    union { ULONGLONG aligned; UCHAR bytes[RB2K_INLINE_PACKET_BYTES]; } copyPacket;
    RB2K_IMAGE images[RB2K_DD_SLOTS];
    RB2K_TEXTURE textures[RB2K_D7_TEXTURES];
    ULONG textureCount, textureBytes;
    RB2K_DMA_SEGMENT staging[RB2K_MAX_SEGMENTS];
    /* The image whose whole readback staging holds for further presents;
     * zero once staging or any image may have changed. */
    ULONG presentCache;
    ULONG imageSamples;                 /* draws that sampled a render target texture */
    RB2K_DMA_SEGMENT rvdStaging[RB2K_RVD_SEGMENTS];
    ULONG rvdContexts;
    /* Direct3D 8 pixel shader programs as TGSI text; an id is a serial
     * number and the slot, so a freed id is never reused. */
    struct { ULONG id, samplers, bytes; char text[RB2K_D8_PROGRAM_BYTES]; } programs[RB2K_D8_PROGRAMS];
    ULONG programSerial;
    ULONG d8Step;       /* the step of a pixel shader draw being queued, for failures */
} RB2K_GPU;

void rb_gpu_find(RB2K_GPU *gpu, PVOID extension, ULONG slot,
                 PVIDEO_ACCESS_RANGE registers, PVIDEO_PORT_CONFIG_INFO config);
void rb_gpu_probe(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
BOOLEAN rb_gl_record_time(RB2K_GPU *gpu, ULONG operation, LONGLONG start, BOOLEAN ok);
VP_STATUS rb_gpu_submit(RB2K_GPU *gpu, const void *request, ULONG bytes,
                        void *reply, ULONG capacity, PULONG received);
VP_STATUS rb_gpu_submit_extents(RB2K_GPU *gpu, const void *request, ULONG bytes,
                                const RB2K_GPU_EXTENT *extents, ULONG count,
                                void *reply, ULONG capacity, PULONG received);
VP_STATUS rb_gpu_submit_scene(RB2K_GPU *gpu, const ULONG *words, ULONG count, const RB2K_D3D7_COMPLETED *work,
                              BOOLEAN fenced);
VP_STATUS rb_gpu_poll_scenes(RB2K_GPU *gpu);
VP_STATUS rb_gpu_wait_image(RB2K_GPU *gpu, const RB2K_IMAGE *image);
VP_STATUS rb_gpu_reserve_staging(RB2K_GPU *gpu, ULONG bytes);
VP_STATUS rb_gpu_start(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
#define RB2K_CURSOR_DMA_PIXELS 12288
VP_STATUS rb_gpu_cursor_command(RB2K_GPU *gpu, const void *data, ULONG bytes);
VP_STATUS rb_gpu_cursor_pixels(RB2K_GPU *gpu);
VP_STATUS rb_cursor_restore(RB2K_GPU *gpu);
void rb_cursor_forget(RB2K_GPU *gpu);
VP_STATUS rb_cursor_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet);
VP_STATUS rb_gpu_stop(RB2K_GPU *gpu);
void rb_gpu_pixel(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
BOOLEAN rb_gpu_create_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                            const RB2K_IMAGE *image, const char *name, ULONG nameBytes, BOOLEAN scanout);
/* IEEE single encodings of channel/255 for channel 0 to 255, rounded to nearest (rb_gpu_channel). */
static const ULONG rbChannelBits[256] = {
    0x00000000UL, 0x3b808081UL, 0x3c008081UL, 0x3c40c0c1UL, 0x3c808081UL, 0x3ca0a0a1UL,
    0x3cc0c0c1UL, 0x3ce0e0e1UL, 0x3d008081UL, 0x3d109091UL, 0x3d20a0a1UL, 0x3d30b0b1UL,
    0x3d40c0c1UL, 0x3d50d0d1UL, 0x3d60e0e1UL, 0x3d70f0f1UL, 0x3d808081UL, 0x3d888889UL,
    0x3d909091UL, 0x3d989899UL, 0x3da0a0a1UL, 0x3da8a8a9UL, 0x3db0b0b1UL, 0x3db8b8b9UL,
    0x3dc0c0c1UL, 0x3dc8c8c9UL, 0x3dd0d0d1UL, 0x3dd8d8d9UL, 0x3de0e0e1UL, 0x3de8e8e9UL,
    0x3df0f0f1UL, 0x3df8f8f9UL, 0x3e008081UL, 0x3e048485UL, 0x3e088889UL, 0x3e0c8c8dUL,
    0x3e109091UL, 0x3e149495UL, 0x3e189899UL, 0x3e1c9c9dUL, 0x3e20a0a1UL, 0x3e24a4a5UL,
    0x3e28a8a9UL, 0x3e2cacadUL, 0x3e30b0b1UL, 0x3e34b4b5UL, 0x3e38b8b9UL, 0x3e3cbcbdUL,
    0x3e40c0c1UL, 0x3e44c4c5UL, 0x3e48c8c9UL, 0x3e4ccccdUL, 0x3e50d0d1UL, 0x3e54d4d5UL,
    0x3e58d8d9UL, 0x3e5cdcddUL, 0x3e60e0e1UL, 0x3e64e4e5UL, 0x3e68e8e9UL, 0x3e6cecedUL,
    0x3e70f0f1UL, 0x3e74f4f5UL, 0x3e78f8f9UL, 0x3e7cfcfdUL, 0x3e808081UL, 0x3e828283UL,
    0x3e848485UL, 0x3e868687UL, 0x3e888889UL, 0x3e8a8a8bUL, 0x3e8c8c8dUL, 0x3e8e8e8fUL,
    0x3e909091UL, 0x3e929293UL, 0x3e949495UL, 0x3e969697UL, 0x3e989899UL, 0x3e9a9a9bUL,
    0x3e9c9c9dUL, 0x3e9e9e9fUL, 0x3ea0a0a1UL, 0x3ea2a2a3UL, 0x3ea4a4a5UL, 0x3ea6a6a7UL,
    0x3ea8a8a9UL, 0x3eaaaaabUL, 0x3eacacadUL, 0x3eaeaeafUL, 0x3eb0b0b1UL, 0x3eb2b2b3UL,
    0x3eb4b4b5UL, 0x3eb6b6b7UL, 0x3eb8b8b9UL, 0x3ebababbUL, 0x3ebcbcbdUL, 0x3ebebebfUL,
    0x3ec0c0c1UL, 0x3ec2c2c3UL, 0x3ec4c4c5UL, 0x3ec6c6c7UL, 0x3ec8c8c9UL, 0x3ecacacbUL,
    0x3ecccccdUL, 0x3ecececfUL, 0x3ed0d0d1UL, 0x3ed2d2d3UL, 0x3ed4d4d5UL, 0x3ed6d6d7UL,
    0x3ed8d8d9UL, 0x3edadadbUL, 0x3edcdcddUL, 0x3edededfUL, 0x3ee0e0e1UL, 0x3ee2e2e3UL,
    0x3ee4e4e5UL, 0x3ee6e6e7UL, 0x3ee8e8e9UL, 0x3eeaeaebUL, 0x3eececedUL, 0x3eeeeeefUL,
    0x3ef0f0f1UL, 0x3ef2f2f3UL, 0x3ef4f4f5UL, 0x3ef6f6f7UL, 0x3ef8f8f9UL, 0x3efafafbUL,
    0x3efcfcfdUL, 0x3efefeffUL, 0x3f008081UL, 0x3f018182UL, 0x3f028283UL, 0x3f038384UL,
    0x3f048485UL, 0x3f058586UL, 0x3f068687UL, 0x3f078788UL, 0x3f088889UL, 0x3f09898aUL,
    0x3f0a8a8bUL, 0x3f0b8b8cUL, 0x3f0c8c8dUL, 0x3f0d8d8eUL, 0x3f0e8e8fUL, 0x3f0f8f90UL,
    0x3f109091UL, 0x3f119192UL, 0x3f129293UL, 0x3f139394UL, 0x3f149495UL, 0x3f159596UL,
    0x3f169697UL, 0x3f179798UL, 0x3f189899UL, 0x3f19999aUL, 0x3f1a9a9bUL, 0x3f1b9b9cUL,
    0x3f1c9c9dUL, 0x3f1d9d9eUL, 0x3f1e9e9fUL, 0x3f1f9fa0UL, 0x3f20a0a1UL, 0x3f21a1a2UL,
    0x3f22a2a3UL, 0x3f23a3a4UL, 0x3f24a4a5UL, 0x3f25a5a6UL, 0x3f26a6a7UL, 0x3f27a7a8UL,
    0x3f28a8a9UL, 0x3f29a9aaUL, 0x3f2aaaabUL, 0x3f2babacUL, 0x3f2cacadUL, 0x3f2dadaeUL,
    0x3f2eaeafUL, 0x3f2fafb0UL, 0x3f30b0b1UL, 0x3f31b1b2UL, 0x3f32b2b3UL, 0x3f33b3b4UL,
    0x3f34b4b5UL, 0x3f35b5b6UL, 0x3f36b6b7UL, 0x3f37b7b8UL, 0x3f38b8b9UL, 0x3f39b9baUL,
    0x3f3ababbUL, 0x3f3bbbbcUL, 0x3f3cbcbdUL, 0x3f3dbdbeUL, 0x3f3ebebfUL, 0x3f3fbfc0UL,
    0x3f40c0c1UL, 0x3f41c1c2UL, 0x3f42c2c3UL, 0x3f43c3c4UL, 0x3f44c4c5UL, 0x3f45c5c6UL,
    0x3f46c6c7UL, 0x3f47c7c8UL, 0x3f48c8c9UL, 0x3f49c9caUL, 0x3f4acacbUL, 0x3f4bcbccUL,
    0x3f4ccccdUL, 0x3f4dcdceUL, 0x3f4ececfUL, 0x3f4fcfd0UL, 0x3f50d0d1UL, 0x3f51d1d2UL,
    0x3f52d2d3UL, 0x3f53d3d4UL, 0x3f54d4d5UL, 0x3f55d5d6UL, 0x3f56d6d7UL, 0x3f57d7d8UL,
    0x3f58d8d9UL, 0x3f59d9daUL, 0x3f5adadbUL, 0x3f5bdbdcUL, 0x3f5cdcddUL, 0x3f5ddddeUL,
    0x3f5ededfUL, 0x3f5fdfe0UL, 0x3f60e0e1UL, 0x3f61e1e2UL, 0x3f62e2e3UL, 0x3f63e3e4UL,
    0x3f64e4e5UL, 0x3f65e5e6UL, 0x3f66e6e7UL, 0x3f67e7e8UL, 0x3f68e8e9UL, 0x3f69e9eaUL,
    0x3f6aeaebUL, 0x3f6bebecUL, 0x3f6cecedUL, 0x3f6dedeeUL, 0x3f6eeeefUL, 0x3f6feff0UL,
    0x3f70f0f1UL, 0x3f71f1f2UL, 0x3f72f2f3UL, 0x3f73f3f4UL, 0x3f74f4f5UL, 0x3f75f5f6UL,
    0x3f76f6f7UL, 0x3f77f7f8UL, 0x3f78f8f9UL, 0x3f79f9faUL, 0x3f7afafbUL, 0x3f7bfbfcUL,
    0x3f7cfcfdUL, 0x3f7dfdfeUL, 0x3f7efeffUL, 0x3f800000UL
};
/* Every vertex colour goes through here, eight channels a vertex: a table, not a call. */
#define rb_gpu_channel(channel) (rbChannelBits[(channel) & 255])
ULONG *rb_gpu_frame_reserve(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image, ULONG count);
BOOLEAN rb_gpu_frame_words(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                           const ULONG *words, ULONG count);
BOOLEAN rb_gpu_frame_flush(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
BOOLEAN rb_gpu_d7_draw(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                       const RB2K_D3D7_DRAW *draw, const RB2K_TEXTURE *texture, const RB2K_TEXTURE *texture1);
BOOLEAN rb_gpu_d7_valid(const RB2K_D3D7_DRAW *draw);
BOOLEAN rb_gpu_d8_valid(const RB2K_D3D8_DRAW *draw);
BOOLEAN rb_gpu_d8_draw(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image, const RB2K_D3D8_DRAW *draw,
                       const char *program, ULONG programBytes, const char *vertexProgram, ULONG vertexBytes,
                       RB2K_TEXTURE *const *textures);
BOOLEAN rb_gpu_d7_samples(const RB2K_D3D7_DRAW *draw, ULONG stage);
RB2K_TEXTURE *rb_gpu_texture(RB2K_GPU *gpu, ULONG id);
ULONG rb_gpu_host_blocks(RB2K_GPU *gpu);
BOOLEAN rb_gpu_texture_create(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture);
BOOLEAN rb_gpu_copy_layer(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG image, ULONG texture, ULONG layer, ULONG side);
BOOLEAN rb_gpu_texture_attach(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture,
                              const RB2K_IMAGE *image);
BOOLEAN rb_gpu_image_attach(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *source, const RB2K_IMAGE *target);
BOOLEAN rb_gpu_texture_upload(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, const RB2K_TEXTURE *texture,
                              const RB2K_IMAGE *image, ULONG level, ULONG face, ULONG y, ULONG rows, PVOID texels);
BOOLEAN rb_gpu_texture_free(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture);
void rb_gpu_texture_forget(RB2K_GPU *gpu);
void rb_gpu_slot_forget(RB2K_GPU *gpu, const RB2K_IMAGE *image);
BOOLEAN rb_gpu_prepare_target(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image);
BOOLEAN rb_gpu_depth_target(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *color, RB2K_IMAGE *depth);
BOOLEAN rb_gpu_clear_buffers(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                             ULONG flags, ULONG color, ULONG depthBits, ULONG stencil);
BOOLEAN rb_gpu_sync_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image);
BOOLEAN rb_gpu_order_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image);
enum { RB2K_D7_COLOR_VIEW = 1, RB2K_D7_DEPTH_VIEW = 8 };
BOOLEAN rb_gpu_draw_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id,
                          ULONG color, BOOLEAN createSurface, RB2K_IMAGE *frameTarget, ULONG depthView);
BOOLEAN rb_gpu_transfer_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                              const RB2K_IMAGE *image, PVOID pixels, BOOLEAN readback);
BOOLEAN rb_gpu_transfer_box(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG resource,
                            ULONG x, ULONG y, ULONG width, ULONG height, PVOID upload);
BOOLEAN rb_gpu_copy_rects(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG destination,
                          ULONG source, const RB2K_RECT *rects, ULONG count,
                          LONG originX, LONG originY, RB2K_GL_PROFILE *profile);
BOOLEAN rb_gpu_blit_rect(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG destination, ULONG source,
                         const RB2K_RECT *to, const RB2K_RECT *from, BOOLEAN linear);
BOOLEAN rb_gpu_primary_transfer(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                                const RB2K_RECT *rect, BOOLEAN readback);
VP_STATUS rb_gpu_inline_uploads(RB2K_GPU *gpu, const RB2K_RECT *rects, ULONG count);
BOOLEAN rb_gpu_show_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG resource, ULONG width, ULONG height,
                          BOOLEAN select);
BOOLEAN rb_gpu_show_desktop(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, BOOLEAN select);
VP_STATUS rb_gl_present(RB2K_GPU *gpu, const RB2K_GL_FRAME *frame);
VP_STATUS rb_gl_access(RB2K_GPU *gpu, const RB2K_RECT *rect, ULONG access, ULONG tag);
VP_STATUS rb_gl_flush(RB2K_GPU *gpu);
VP_STATUS rb_gl_scanout(RB2K_GPU *gpu, ULONG resource, ULONG width, ULONG height);
VP_STATUS rb_gl_desktop(RB2K_GPU *gpu);
VP_STATUS rb_gl_finish(RB2K_GPU *gpu);
BOOLEAN rb_gpu_destroy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id);
VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet);
VP_STATUS rb_gpu_surface_reset(RB2K_GPU *gpu);
VP_STATUS rb_gpu_rvd_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet);
void rb_gpu_rvd_sweep(RB2K_GPU *gpu);
#endif
