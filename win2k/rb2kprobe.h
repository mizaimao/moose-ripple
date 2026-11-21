/* Display defaults and diagnostic interface shared with the guest tools. */
#ifndef RB2K_PROBE_H
#define RB2K_PROBE_H

#define RB2K_DEFAULT_WIDTH 1024UL
#define RB2K_DEFAULT_HEIGHT 768UL

enum RB2K_DDRAW_TEST { RB2K_DDRAW_FILL, RB2K_DDRAW_COPY, RB2K_DDRAW_WINDOW,
                      RB2K_DDRAW_FLIP, RB2K_DDRAW_PAIR, RB2K_DDRAW_FLIP16 };

#define RB2K_GPU_VERSION 1
#define RB2K_GPU_ESCAPE 0x52420001
#define RB2K_DD_STATS_ESCAPE 0x52420002
#define RB2K_DD_SCANOUT_ESCAPE 0x52420003
/* Current Bochs framebuffer layout and verified page changes. Pitch and page
 * size are bytes; yOffset is the hardware scanout origin in pixels. */
typedef struct {
    ULONG pageBytes, pageCount, visiblePage, flips, width, height, pitch, yOffset;
} RB2K_DD_FLIP_INFO;
#define IOCTL_RB2K_GPU_PROBE CTL_CODE(FILE_DEVICE_VIDEO, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum {
    RB2K_STAGE_TRANSPORT = 1,
    RB2K_STAGE_CONTEXT,
    RB2K_STAGE_RESOURCE,
    RB2K_STAGE_BACKING,
    RB2K_STAGE_ATTACH,
    RB2K_STAGE_CLEAR,
    RB2K_STAGE_TRANSFER,
    RB2K_STAGE_COMPARE,
    RB2K_STAGE_CLEANUP,
    RB2K_STAGE_DONE
};

typedef struct {
    ULONG version;
    ULONG stage;
    ULONG status;
    ULONG response;
    ULONG expected;
    ULONG observed;
    ULONG features;
} RB2K_GPU_RESULT;

typedef struct {
    ULONG hostFills;
    ULONG readbacks;
    ULONG uploads;
} RB2K_DD_STATS;

typedef struct {
    RB2K_DD_STATS transfers;
    ULONG hostCopies;
} RB2K_DD_COPY_STATS;

typedef struct {
    RB2K_DD_COPY_STATS offscreen;
    ULONG presents, presentReadbacks, presentBytes;
} RB2K_DD_PRESENT_STATS;

#endif
