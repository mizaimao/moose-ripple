/* Private display DLL to miniport interface for offscreen 32-bit surfaces.
 * Rows are tightly packed. A successful READ returns width*height*4 bytes.
 * WRITE takes the surface ID followed immediately by that many bytes. */
#ifndef RB2K_DDSURFACE_H
#define RB2K_DDSURFACE_H
#define RB2K_DD_MAX_WIDTH 1024UL
#define RB2K_DD_MAX_HEIGHT 768UL
#define RB2K_DD_MAX_BYTES (RB2K_DD_MAX_WIDTH * RB2K_DD_MAX_HEIGHT * 4)
#define RB2K_DD_SLOTS 4
#define RB2K_DD_IOCTL(n) CTL_CODE(FILE_DEVICE_VIDEO,n,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define IOCTL_RB2K_DD_QUERY   RB2K_DD_IOCTL(0x901)
#define IOCTL_RB2K_DD_CREATE  RB2K_DD_IOCTL(0x902)
#define IOCTL_RB2K_DD_FILL    RB2K_DD_IOCTL(0x903)
#define IOCTL_RB2K_DD_READ    RB2K_DD_IOCTL(0x904)
#define IOCTL_RB2K_DD_WRITE   RB2K_DD_IOCTL(0x905)
#define IOCTL_RB2K_DD_DESTROY RB2K_DD_IOCTL(0x906)
#define IOCTL_RB2K_DD_COPY    RB2K_DD_IOCTL(0x907)
#define IOCTL_RB2K_DD_PRESENT RB2K_DD_IOCTL(0x908)
typedef struct {
    ULONG maxBytes, slots;
    ULONG hostFills, readbacks, uploads, hostCopies;
    ULONG presents, presentReadbacks, presentBytes;
} RB2K_DD_CAPS;
typedef struct { ULONG width, height; } RB2K_DD_CREATE;
typedef struct { ULONG id, color; } RB2K_DD_FILL;
typedef struct { ULONG source, destination; } RB2K_DD_COPY;
/* One visible rectangle, in source and primary pixel coordinates. */
typedef struct { ULONG source, srcX, srcY, dstX, dstY, width, height; } RB2K_DD_PRESENT;
#endif
