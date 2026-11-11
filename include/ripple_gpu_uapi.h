/*
 * ripple_gpu_uapi.h: the user-client API between user space (Mesa winsys, test tools) and a
 * moose-ripple GPU kext. GPU-neutral by design (docs/decisions/0005): the kernel provides
 * device info, capability blobs, contexts, mappable buffers, command submission, transfers
 * and fences; what a command stream, a capability blob or a buffer's creation parameters
 * mean is up to the backend named in ripple_gpu_info.backend. Documented in docs/kext-uapi.md.
 *
 * Plain C, shared by the kext and user space. All structures are naturally aligned and
 * identical for i386 and x86_64 user space.
 */
#ifndef RIPPLE_GPU_UAPI_H
#define RIPPLE_GPU_UAPI_H

#include <stdint.h>

/* IOServiceOpen() type for the GPU client on the RippleAccelerator service. Not 0..4, which
 * Apple's graphics stack uses for IOAccelerator clients (surface, GL, 2D, ...). */
#define RIPPLE_GPU_CLIENT_TYPE      0x52504731u     /* 'RPG1' */
#define RIPPLE_GPU_SERVICE_CLASS    "RippleAccelerator"
#define RIPPLE_GPU_API_VERSION      1

/* Backends (ripple_gpu_info.backend). Each defines its command stream, capability blobs
 * and buffer parameters. */
#define RIPPLE_GPU_BACKEND_VIRGL    1               /* QEMU virtio-gpu with virglrenderer */

/* External method selectors (IOConnectCallMethod). */
enum ripple_gpu_method {
    RIPPLE_GPU_GET_INFO        = 0, /* out struct: ripple_gpu_info */
    RIPPLE_GPU_GET_CAPS        = 1, /* in scalars: caps id, version; out struct: the blob */
    RIPPLE_GPU_CONTEXT_CREATE  = 2, /* in scalar: flags; out scalar: context handle */
    RIPPLE_GPU_CONTEXT_DESTROY = 3, /* in scalar: context handle */
    RIPPLE_GPU_BUFFER_CREATE   = 4, /* in struct: ripple_gpu_buffer_create; out scalars: handle, size */
    RIPPLE_GPU_BUFFER_DESTROY  = 5, /* in scalar: buffer handle */
    RIPPLE_GPU_SUBMIT          = 6, /* in struct: ripple_gpu_submit + handles + commands; out scalar: fence */
    RIPPLE_GPU_TRANSFER        = 7, /* in struct: ripple_gpu_transfer; out scalar: fence */
    RIPPLE_GPU_WAIT            = 8, /* in scalars: fence, timeout ms; out scalar: 1 signalled, 0 not */
    RIPPLE_GPU_DISPLAY         = 9, /* in scalars: buffer (0 = back to the console), x, y, w, h */
    RIPPLE_GPU_METHOD_COUNT
};

struct ripple_gpu_info {
    uint32_t api_version;           /* RIPPLE_GPU_API_VERSION */
    uint32_t backend;               /* RIPPLE_GPU_BACKEND_* */
    uint32_t num_caps;              /* capability blobs available */
    uint32_t caps_ids[8];           /* their ids (backend-defined) */
    uint32_t caps_versions[8];      /* highest version of each */
    char     name[48];              /* human-readable device name */
};

/* Buffer: GPU-side object with a guest-memory backing the client can map with
 * IOConnectMapMemory(connection, handle, ...). `params` are backend-defined. */
struct ripple_gpu_buffer_create {
    uint64_t size;                  /* bytes of backing memory (page-rounded by the kernel) */
    uint32_t flags;                 /* none defined yet; 0 */
    uint32_t params_size;           /* bytes used in params */
    uint8_t  params[64];
};

/* Submit: followed in the same input by num_handles uint32 buffer handles (buffers the
 * commands use), then cmd_size bytes of backend-defined commands (a multiple of 4). */
struct ripple_gpu_submit {
    uint32_t context;
    uint32_t num_handles;
    uint32_t cmd_size;
    uint32_t flags;                 /* none defined yet; 0 */
};

#define RIPPLE_GPU_TO_DEVICE        0   /* backing memory -> GPU object */
#define RIPPLE_GPU_FROM_DEVICE      1   /* GPU object -> backing memory */

struct ripple_gpu_transfer {
    uint32_t context;
    uint32_t buffer;
    uint32_t direction;             /* RIPPLE_GPU_TO_DEVICE or RIPPLE_GPU_FROM_DEVICE */
    uint32_t level;
    uint32_t x, y, z, width, height, depth;
    uint32_t stride, layer_stride;  /* layout in the backing memory; 0 = backend default */
    uint64_t offset;                /* byte offset in the backing memory */
};

/* ---- Backend: virgl ------------------------------------------------------------------------
 * Command stream: virgl protocol (third_party/virgl/virgl_protocol.h).
 * Capability blobs: virgl capsets (id 1 = virgl_caps_v1, id 2 = virgl_caps_v2).
 * Buffer params: struct ripple_virgl_buffer_params (a virgl resource). */
struct ripple_virgl_buffer_params {
    uint32_t target;                /* pipe_texture_target: 0 buffer, 2 2D texture, ... */
    uint32_t format;                /* virgl_formats */
    uint32_t bind;                  /* VIRGL_BIND_* */
    uint32_t width, height, depth, array_size, last_level, nr_samples, flags;
};

#endif
