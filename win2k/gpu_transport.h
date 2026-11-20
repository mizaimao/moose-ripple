#ifndef RB2K_GPU_TRANSPORT_H
#define RB2K_GPU_TRANSPORT_H

/* Include miniport.h and video.h before this header. */
#include "rb2kprobe.h"

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
    BOOLEAN owned, ready, enabledMaster;
} RB2K_GPU;

void rb_gpu_find(RB2K_GPU *gpu, PVOID extension, ULONG slot,
                 PVIDEO_ACCESS_RANGE registers, PVIDEO_PORT_CONFIG_INFO config);
void rb_gpu_probe(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
VP_STATUS rb_gpu_submit(RB2K_GPU *gpu, const void *request, ULONG bytes,
                        void *reply, ULONG capacity, PULONG received);
ULONG *rb_gpu_backing(RB2K_GPU *gpu, PHYSICAL_ADDRESS *bus);
void rb_gpu_pixel(RB2K_GPU *gpu, RB2K_GPU_RESULT *result);
#endif
