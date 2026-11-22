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
       RB2K_D7_SHADER_KEYS = 16 };
typedef struct {
    ULONG keys[RB2K_D7_SHADER_KEYS > RB2K_D7_DSA_KEYS ? RB2K_D7_SHADER_KEYS : RB2K_D7_DSA_KEYS][3];
    ULONG used, next, bound;
} RB2K_D7_CACHE;
typedef struct {
    BOOLEAN pipeline;
    ULONG vertexShaders, boundVertex;   /* bit and value: 1 transformed input, 2 object space */
    ULONG rasters, boundRaster, viewTexture;
    RB2K_D7_CACHE dsa, blend, sampler, shader;
} RB2K_D7_STATE;

typedef struct RB2K_IMAGE {
    ULONG id, width, height;
    BOOLEAN defined, hasSurface, uniform;
    ULONG color;
    BOOLEAN frameFailed;
    BOOLEAN depth16;
    struct RB2K_IMAGE *depthBuffer, *depthOwner;
    RB2K_D7_STATE d7;
} RB2K_IMAGE;

/* A Direct3D texture is a host resource without a context of its own. It
 * is attached to each colour context that samples it. */
typedef struct {
    ULONG id, width, height;
    ULONG contexts[RB2K_DD_SLOTS];
} RB2K_TEXTURE;
#define RB2K_D7_TEXEL_BUDGET (64UL << 20)
typedef struct {
    RB2K_IMAGE image;
    RB2K_RECT gpuPixels, gdiWrites[3];
    ULONG writeCount, writeTags[3];
    RB2K_RECT clip[RB2K_GL_MAX_RECTS];
    ULONG clipCount;
    BOOLEAN visible, inlineUploads;
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
    ULONG fence;
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
#define RB2K_D7_FRAME_WORDS ((64UL * 1024 - 128) / sizeof(ULONG))

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
    BOOLEAN ddGpuSurfaces, nativeD3D7;
    struct {
        RB2K_IMAGE *target;
        ULONG count;
        RB2K_D3D7_COMPLETED pending;
        ULONG words[RB2K_D7_FRAME_WORDS];
    } d7Frame;
    RB2K_D3D7_COMPLETED d7Completed;
    RB2K_D7_SUBMISSION d7Submitted[RB2K_D7_FLIGHT_SLOTS];
    RB2K_DMA_SEGMENT d7Packets;
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
    RB2K_DMA_SEGMENT rvdStaging[RB2K_RVD_SEGMENTS];
    ULONG rvdContexts;
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
VP_STATUS rb_gpu_submit_scene(RB2K_GPU *gpu, const ULONG *words, ULONG count, const RB2K_D3D7_COMPLETED *work);
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
ULONG rb_gpu_channel(ULONG channel);
ULONG *rb_gpu_frame_reserve(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image, ULONG count);
BOOLEAN rb_gpu_frame_words(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                           const ULONG *words, ULONG count);
BOOLEAN rb_gpu_frame_flush(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
BOOLEAN rb_gpu_d7_draw(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                       const RB2K_D3D7_DRAW *draw, const RB2K_TEXTURE *texture);
BOOLEAN rb_gpu_d7_valid(const RB2K_D3D7_DRAW *draw);
BOOLEAN rb_gpu_d7_samples(const RB2K_D3D7_DRAW *draw);
RB2K_TEXTURE *rb_gpu_texture(RB2K_GPU *gpu, ULONG id);
BOOLEAN rb_gpu_texture_create(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture);
BOOLEAN rb_gpu_texture_attach(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture,
                              const RB2K_IMAGE *image);
BOOLEAN rb_gpu_texture_upload(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, const RB2K_TEXTURE *texture,
                              const RB2K_IMAGE *image, ULONG y, ULONG rows, PVOID texels);
BOOLEAN rb_gpu_texture_free(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_TEXTURE *texture);
void rb_gpu_texture_forget(RB2K_GPU *gpu);
BOOLEAN rb_gpu_prepare_target(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image);
BOOLEAN rb_gpu_depth_target(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *color, RB2K_IMAGE *depth);
BOOLEAN rb_gpu_clear_buffers(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                             ULONG flags, ULONG color, ULONG depthBits);
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
BOOLEAN rb_gpu_primary_transfer(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                                const RB2K_RECT *rect, BOOLEAN readback);
VP_STATUS rb_gpu_inline_uploads(RB2K_GPU *gpu, const RB2K_RECT *rects, ULONG count);
BOOLEAN rb_gpu_show_desktop(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, BOOLEAN select);
VP_STATUS rb_gl_present(RB2K_GPU *gpu, const RB2K_GL_FRAME *frame);
VP_STATUS rb_gl_access(RB2K_GPU *gpu, const RB2K_RECT *rect, ULONG access, ULONG tag);
VP_STATUS rb_gl_flush(RB2K_GPU *gpu);
VP_STATUS rb_gl_finish(RB2K_GPU *gpu);
BOOLEAN rb_gpu_destroy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id);
VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet);
VP_STATUS rb_gpu_surface_reset(RB2K_GPU *gpu);
VP_STATUS rb_gpu_rvd_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet);
void rb_gpu_rvd_sweep(RB2K_GPU *gpu);
#endif
