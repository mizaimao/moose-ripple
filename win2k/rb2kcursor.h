/* Private NT5 display/miniport interface for the virtio pointer plane. */
#ifndef RB2K_CURSOR_H
#define RB2K_CURSOR_H
#define RB2K_CURSOR_EDGE 64
#define RB2K_CURSOR_PIXELS (RB2K_CURSOR_EDGE * RB2K_CURSOR_EDGE)
#define RB2K_CURSOR_ESCAPE 0x52420018
#define RB2K_CURSOR_SOURCE_ESCAPE 0x52420019
#define IOCTL_RB2K_CURSOR CTL_CODE(FILE_DEVICE_VIDEO, 0x912, METHOD_BUFFERED, FILE_ANY_ACCESS)
enum { RB2K_CURSOR_QUERY, RB2K_CURSOR_SET, RB2K_CURSOR_MOVE, RB2K_CURSOR_DROP };
enum { RB2K_CURSOR_MONO, RB2K_CURSOR_COLOR, RB2K_CURSOR_ALPHA, RB2K_CURSOR_TYPES };
enum { RB2K_CURSOR_TRANSPARENT, RB2K_CURSOR_UNSUPPORTED, RB2K_CURSOR_INVERT,
       RB2K_CURSOR_FLAGS, RB2K_CURSOR_BITMAP, RB2K_CURSOR_SIZE, RB2K_CURSOR_XLATE, RB2K_CURSOR_DEVICE };
typedef struct {
    ULONG op, kind, owner;
    LONG x, y;
    ULONG hotX, hotY, width, height;
} RB2K_CURSOR_REQUEST;
typedef struct {
    RB2K_CURSOR_REQUEST info;
    ULONG pixels[RB2K_CURSOR_PIXELS]; /* 64x64, BGRA, transparent padding */
} RB2K_CURSOR_SHAPE;
typedef struct {
    ULONG flags, reason, maskFormat, colorFormat;
    LONG maskWidth, maskHeight, colorWidth, colorHeight;
} RB2K_CURSOR_REJECTION;
typedef struct { RB2K_CURSOR_REQUEST info; RB2K_CURSOR_REJECTION why; } RB2K_CURSOR_DECLINE;
typedef struct {
    ULONG enabled, accepted[RB2K_CURSOR_TYPES], declined, inverted, failed;
    ULONG transfers, updates, moves, hides, restores;
    ULONG width, height, hotX, hotY, hash, visible;
    LONG x, y;
    RB2K_CURSOR_REJECTION lastDecline;
} RB2K_CURSOR_STATS;
typedef struct {
    ULONG width, height, hotX, hotY, hotPixel, opaque, partial, partialColor, hash;
} RB2K_CURSOR_SOURCE;
#endif
