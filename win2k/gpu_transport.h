#ifndef RB2K_GPU_TRANSPORT_H
#define RB2K_GPU_TRANSPORT_H

/* Include miniport.h and video.h before this header. */
#include "rb2kprobe.h"
#include "ddsurface.h"

typedef struct {
    ULONG id, width, height;
    BOOLEAN defined, hasSurface;
} RB2K_IMAGE;

#define RB2K_SEGMENT_BYTES 65536UL
#define RB2K_MAX_SEGMENTS ((RB2K_DD_MAX_BYTES + RB2K_SEGMENT_BYTES - 1) / RB2K_SEGMENT_BYTES)
typedef struct {
    PUCHAR data;
    PHYSICAL_ADDRESS bus;
    ULONG allocated;
} RB2K_DMA_SEGMENT;

typedef struct {
    PVOID extension;
    PUCHAR common, notifyBase;
    PUSHORT doorbell;
    ULONG notifySize, multiplier;
    PUCHAR dma;
    PHYSICAL_ADDRESS bus;
    ULONG allocated;
    USHORT index;
    LONG busy, barrier;
    BOOLEAN owned, ready, enabledMaster, tested;
    ULONG nextId, fence, imageCount, hostFills, readbacks, uploads, hostCopies;
    RB2K_IMAGE images[RB2K_DD_SLOTS];
    RB2K_DMA_SEGMENT staging[RB2K_MAX_SEGMENTS];
} RB2K_GPU;

void rb_gpu_find(RB2K_GPU *gpu, PVOID extension, ULONG slot,
                 PVIDEO_ACCESS_RANGE registers, PVIDEO_PORT_CONFIG_INFO config);
void rb_gpu_probe(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
VP_STATUS rb_gpu_submit(RB2K_GPU *gpu, const void *request, ULONG bytes,
                        void *reply, ULONG capacity, PULONG received);
VP_STATUS rb_gpu_reserve_staging(RB2K_GPU *gpu, ULONG bytes);
VP_STATUS rb_gpu_start(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
VP_STATUS rb_gpu_stop(RB2K_GPU *gpu);
void rb_gpu_pixel(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
BOOLEAN rb_gpu_create_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                            const RB2K_IMAGE *image, const char *name, ULONG nameBytes);
BOOLEAN rb_gpu_draw_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id,
                          ULONG color, BOOLEAN createSurface);
BOOLEAN rb_gpu_transfer_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                              const RB2K_IMAGE *image, PVOID pixels, BOOLEAN readback);
BOOLEAN rb_gpu_copy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                          const RB2K_IMAGE *source, const RB2K_IMAGE *destination);
BOOLEAN rb_gpu_destroy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id);
VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet);
VP_STATUS rb_gpu_surface_reset(RB2K_GPU *gpu);
#endif
