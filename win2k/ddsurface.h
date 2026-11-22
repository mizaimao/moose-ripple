/* Private display DLL to miniport interface for offscreen 32-bit surfaces.
 * Rows are tightly packed. A successful READ returns width*height*4 bytes.
 * WRITE takes the surface ID followed immediately by that many bytes. */
#ifndef RB2K_DDSURFACE_H
#define RB2K_DDSURFACE_H
#define RB2K_DD_MAX_WIDTH 1024UL
#define RB2K_DD_MAX_HEIGHT 768UL
#define RB2K_DD_MAX_BYTES (RB2K_DD_MAX_WIDTH * RB2K_DD_MAX_HEIGHT * 4)
#define RB2K_DD_SLOTS 4
#define RB2K_DD_HOST_SURFACES 1
#define RB2K_DD_NATIVE_D3D7 2
#define RB2K_DD_IOCTL(n) CTL_CODE(FILE_DEVICE_VIDEO,n,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define IOCTL_RB2K_DD_QUERY   RB2K_DD_IOCTL(0x901)
#define IOCTL_RB2K_DD_CREATE  RB2K_DD_IOCTL(0x902)
#define IOCTL_RB2K_DD_FILL    RB2K_DD_IOCTL(0x903)
#define IOCTL_RB2K_DD_READ    RB2K_DD_IOCTL(0x904)
#define IOCTL_RB2K_DD_WRITE   RB2K_DD_IOCTL(0x905)
#define IOCTL_RB2K_DD_DESTROY RB2K_DD_IOCTL(0x906)
#define IOCTL_RB2K_DD_COPY    RB2K_DD_IOCTL(0x907)
#define IOCTL_RB2K_DD_PRESENT RB2K_DD_IOCTL(0x908)
#define IOCTL_RB2K_DD_UNIFORM RB2K_DD_IOCTL(0x909)
#define IOCTL_RB2K_DD_FLIP_INFO RB2K_DD_IOCTL(0x90a)
#define IOCTL_RB2K_DD_FLIP      RB2K_DD_IOCTL(0x90b)
#define IOCTL_RB2K_DD_FLIP_RELEASE RB2K_DD_IOCTL(0x90c)
#define IOCTL_RB2K_DD_FLIP_ACQUIRE RB2K_DD_IOCTL(0x90d)
#define IOCTL_RB2K_DD_CREATE_TEXTURE RB2K_DD_IOCTL(0x90e)
typedef struct {
    ULONG maxBytes, slots;
    ULONG hostFills, readbacks, uploads, hostCopies;
    ULONG presents, presentReadbacks, presentBytes;
    ULONG gpuSurfaces;      /* host-surface and native D3D7 capability bits */
} RB2K_DD_CAPS;
typedef struct { ULONG width, height; } RB2K_DD_CREATE;
typedef struct { ULONG id, color; } RB2K_DD_FILL;
typedef struct { ULONG source, destination; } RB2K_DD_COPY;
/* One visible rectangle, in source and primary pixel coordinates. */
typedef struct { ULONG source, srcX, srcY, dstX, dstY, width, height; } RB2K_DD_PRESENT;
typedef struct { ULONG left, top, right, bottom; } RB2K_RECT;
enum { RB2K_CPU_READ, RB2K_CPU_DRAW, RB2K_CPU_REPLACE };
#define RB2K_GL_MAX_RECTS 32
enum { RB2K_FRAME_BATCH, RB2K_FRAME_PER_RECT, RB2K_FRAME_INLINE_UPLOADS };
typedef struct {
    ULONG context, source, width, height;
    LONG originX, originY;
    ULONG count, mode;
    RB2K_RECT rects[RB2K_GL_MAX_RECTS];
} RB2K_GL_FRAME;
typedef struct {
    ULONG frames, readbacks, uploads, readbackPixels, fullFrameReads;
    ULONG lastReadWidth, lastReadHeight;
} RB2K_GL_STATS;
enum { RB2K_GL_READ_PART, RB2K_GL_READ_FRAME, RB2K_GL_UPLOAD, RB2K_GL_COPY,
       RB2K_GL_FLUSH, RB2K_GL_TIME_COUNT };
typedef struct { LONGLONG ticks; ULONG calls, failed; } RB2K_GL_TIME;
typedef struct {
    LONGLONG frequency;
    RB2K_GL_TIME operations[RB2K_GL_TIME_COUNT];
} RB2K_GL_TIMING;
enum { RB2K_PAINT_INITIAL, RB2K_PAINT_COPY_BITMAP, RB2K_PAINT_COPY_DESKTOP,
       RB2K_PAINT_BLT, RB2K_PAINT_MASKBLT, RB2K_PAINT_TEXT, RB2K_PAINT_STROKE,
       RB2K_PAINT_FILL, RB2K_PAINT_STROKE_FILL, RB2K_PAINT_COUNT };
#define RB2K_GL_WRITERS 24
typedef struct {
    ULONG tag, uploads, small, excluded, width, height;
    ULONGLONG pixels;
} RB2K_GL_WRITER;
typedef struct {
    LONGLONG frequency, attachTicks, fenceTicks, detachTicks;
    ULONG rectangles, fences, otherUploads;
    RB2K_GL_WRITER writers[RB2K_GL_WRITERS];
    ULONG inlineBatches, inlineRects;
} RB2K_GL_PROFILE;
/* Input is a surface ID. A known value supplies every byte of a writable lock. */
typedef struct { ULONG known, color; } RB2K_DD_UNIFORM;
#endif
