#ifndef RB2K_GPU_TRANSPORT_H
#define RB2K_GPU_TRANSPORT_H

/* Include miniport.h and video.h before this header. */
#include "rb2kprobe.h"
#include "ddsurface.h"

typedef struct {
    ULONG id, width, height;
    BOOLEAN defined, hasSurface;
} RB2K_IMAGE;

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
    ULONG nextId, fence, imageCount, hostFills, readbacks, uploads;
    RB2K_IMAGE images[RB2K_DD_SLOTS];
} RB2K_GPU;

void rb_gpu_find(RB2K_GPU *gpu, PVOID extension, ULONG slot,
                 PVIDEO_ACCESS_RANGE registers, PVIDEO_PORT_CONFIG_INFO config);
void rb_gpu_probe(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
VP_STATUS rb_gpu_submit(RB2K_GPU *gpu, const void *request, ULONG bytes,
                        void *reply, ULONG capacity, PULONG received);
ULONG *rb_gpu_backing(RB2K_GPU *gpu, ULONG slot, PHYSICAL_ADDRESS *bus);
VP_STATUS rb_gpu_start(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
VP_STATUS rb_gpu_stop(RB2K_GPU *gpu);
void rb_gpu_pixel(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
BOOLEAN rb_gpu_create_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                            const RB2K_IMAGE *image, ULONG slot, const char *name, ULONG nameBytes);
BOOLEAN rb_gpu_draw_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id,
                          ULONG color, BOOLEAN createSurface);
BOOLEAN rb_gpu_transfer_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result,
                              const RB2K_IMAGE *image, BOOLEAN readback);
BOOLEAN rb_gpu_destroy_image(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, ULONG id);
VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet);
VP_STATUS rb_gpu_surface_reset(RB2K_GPU *gpu);
#endif
