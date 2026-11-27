/*
 * The moose-ripple mini-VDD's interface for user-mode drivers (ripplevd.vxd):
 * DeviceIoControl on \\.\RIPPLEVD. docs/win9x/vxd-interface.md describes it.
 *
 * Shared by the VxD (Visual C++ 6), the test program and Mesa's winsys (clang): plain C89,
 * 32-bit only, no other headers needed.
 */
#ifndef RVD_H
#define RVD_H

#define RVD_DEVICE_NAME         "\\\\.\\RIPPLEVD"
#define RVD_INTERFACE_VERSION   2

typedef unsigned long rvd_u32;

/* DeviceIoControl codes. "in" is the input buffer, "out" the output buffer. */
#define RVD_GET_INFO            1   /* out: struct rvd_info */
#define RVD_GET_CAPS            2   /* in: rvd_u32 id, version; out: the capability blob */
#define RVD_CONTEXT_CREATE      3   /* in: a name (bytes, optional); out: rvd_u32 context */
#define RVD_CONTEXT_DESTROY     4   /* in: rvd_u32 context. Its buffers go with it. */
#define RVD_BUFFER_CREATE       5   /* in: struct rvd_buffer_create; out: struct rvd_buffer */
#define RVD_BUFFER_DESTROY      6   /* in: rvd_u32 handle */
#define RVD_SUBMIT              7   /* in: struct rvd_submit, then the commands */
#define RVD_TRANSFER            8   /* in: struct rvd_transfer */
#define RVD_HEAP                9   /* in: rvd_u32 bytes wanted; out: struct rvd_heap */
#define RVD_SET_TIMEOUT         10  /* in: rvd_u32 ms a request may take (0: the default, 10 s);
                                       out (optional): rvd_u32 contexts and buffers held for
                                       the host. For tests; ends with the program that set it */
#define RVD_TIMEOUT_AT_ONCE     0xFFFFFFFFUL    /* give up on a request as soon as it is sent,
                                                   and hold the device stalled until reset */
/* Optional RVD_SET_TIMEOUT test form. Keep a device handle open while testing. */
struct rvd_upload_test { rvd_u32 ms, action; }; /* ms must be zero; output is a status word */
#define RVD_UPLOAD_RELEASE   0
#define RVD_UPLOAD_ARM       1
#define RVD_UPLOAD_QUERY     2
#define RVD_UPLOAD_TIMEOUT   3
#define RVD_UPLOAD_ARMED     1
#define RVD_UPLOAD_HELD      2
#define RVD_UPLOAD_TIMED_OUT 4
#define RVD_UPLOAD_REPLIED   8
#define RVD_UPLOAD_SUPPORTED 0x80000000UL /* older VxDs return only a small zombie count */
#define RVD_GET_STATS           11  /* out: struct rvd_stats. No GPU needed */
#define RVD_PRESENT             12  /* in: struct rvd_present; publish the latest GPU image */
#define RVD_DESKTOP             13  /* in: offset or struct rvd_desktop; no input: out u32 takeover,
                                       and, with room for a second u32, the flags it knows */
#define RVD_PRESENT_WINDOW      14  /* in: struct rvd_window, then its rectangles: copy on the host */

#define RVD_DESKTOP_FLIP        1   /* select the CPU page now; its display update may coalesce */
#define RVD_DESKTOP_HEAP        2   /* the page lies in the RVD_HEAP block, offset bytes from its
                                       start (a back buffer the card's memory had no room for);
                                       the display leaves VGA for it if it had not yet */
struct rvd_desktop {
    rvd_u32 offset, flags;
};

struct rvd_present {
    rvd_u32 context, buffer;    /* its scanout resource; updates to the same one coalesce */
    rvd_u32 width, height;      /* must match the current display mode and resource */
};

/* An OpenGL window's frame copied into the desktop's image on the host, inside the given
   screen rectangles (GDI's visible region of the window). ERROR_NOT_READY: show it with GDI. */
#define RVD_WINDOW_RECTS        16
struct rvd_rect {
    rvd_u32 left, top, right, bottom;   /* screen pixels, signed */
};
struct rvd_window {
    rvd_u32 context, buffer;    /* the window's image; its top-left pixel is the client area's */
    rvd_u32 width, height;      /* of that image */
    rvd_u32 x, y;               /* the client area's top-left on the screen, signed */
    rvd_u32 format;             /* the image's virgl format */
    rvd_u32 count;              /* rectangles that follow, 1 to RVD_WINDOW_RECTS */
};

#define RVD_MAX_CAPS            8

/* What the VxD holds now, for tests that look for memory a long session loses. */
struct rvd_stats {
    rvd_u32 contexts;           /* contexts in use */
    rvd_u32 buffers;            /* buffers in use */
    rvd_u32 buffer_pages;       /* 4 KB pages of locked memory behind them */
    rvd_u32 zombies;            /* contexts and buffers held for a host that stopped answering */
    rvd_u32 heap_bytes;         /* the DirectDraw heap (RVD_HEAP); 0 until it is made */
};

struct rvd_info {
    rvd_u32 version;            /* RVD_INTERFACE_VERSION */
    rvd_u32 virgl;              /* 1: the host renders 3D (virtio-gpu with virgl) */
    rvd_u32 num_caps;           /* capability sets the host offers */
    rvd_u32 caps_ids[RVD_MAX_CAPS];
    rvd_u32 caps_versions[RVD_MAX_CAPS];
    rvd_u32 caps_sizes[RVD_MAX_CAPS];   /* bytes */
    rvd_u32 display_w, display_h;       /* the host's idea of the display size */
    rvd_u32 max_submit;         /* most command bytes in one RVD_SUBMIT */
    rvd_u32 max_buffer;         /* most bytes of memory behind one buffer */
};

/* A buffer is a host GPU object (a virgl resource: texture, vertex buffer, ...) together with
   guest memory behind it, which transfers copy to and from. The parameters are virgl's
   (pipe_texture_target, virgl_formats, VIRGL_BIND_*). */
struct rvd_buffer_create {
    rvd_u32 context;            /* the buffer belongs to this context */
    rvd_u32 size;               /* bytes of memory behind it (rounded up to pages) */
    rvd_u32 target, format, bind;
    rvd_u32 width, height, depth, array_size, last_level, nr_samples, flags;
};

struct rvd_buffer {
    rvd_u32 handle;             /* the virgl resource ID, for the command stream */
    rvd_u32 size;               /* bytes of memory, page-rounded */
    void   *address;            /* the memory: valid in every process, until BUFFER_DESTROY */
};

struct rvd_submit {
    rvd_u32 context;
    rvd_u32 bytes;              /* of virgl commands following this structure */
};

#define RVD_TO_HOST     0       /* the buffer's memory -> the GPU object */
#define RVD_FROM_HOST   1       /* the GPU object -> the buffer's memory */

/* Memory for the display driver's DirectDraw surfaces that are never shown on the screen
   (textures, z-buffers): one block, made at the first call (the size asked for, or less when
   memory is short) and returned again by later calls; valid in every process until Windows
   ends. Not tied to the GPU: no device needed. */
struct rvd_heap {
    void   *address;            /* 0: none */
    rvd_u32 size;               /* bytes */
};
struct rvd_transfer {
    rvd_u32 context, buffer;
    rvd_u32 direction;          /* RVD_TO_HOST or RVD_FROM_HOST */
    rvd_u32 level;
    rvd_u32 x, y, z, width, height, depth;
    rvd_u32 stride, layer_stride;       /* layout in the memory; 0 = the host's default */
    rvd_u32 offset;             /* byte offset in the buffer's memory */
};

#endif
