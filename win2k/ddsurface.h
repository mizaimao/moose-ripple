/* Private display DLL to miniport interface for offscreen surfaces.
 * Rows are tightly packed. XRGB uses four bytes per pixel and Z16 uses two.
 * WRITE takes the surface ID followed immediately by that many bytes. */
#ifndef RB2K_DDSURFACE_H
#define RB2K_DDSURFACE_H
#define RB2K_DD_MAX_WIDTH 1024UL
#define RB2K_DD_MAX_HEIGHT 768UL
#define RB2K_DD_MAX_BYTES (RB2K_DD_MAX_WIDTH * RB2K_DD_MAX_HEIGHT * 4)
#define RB2K_DD_SLOTS 4
#define RB2K_DD_HOST_SURFACES 1
#define RB2K_DD_NATIVE_D3D7 2
#define RB2K_DD_Z16_STORAGE 4
#define RB2K_DD_Z16_RENDER 8
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
#define IOCTL_RB2K_D3D7_CLEAR RB2K_DD_IOCTL(0x913)
#define IOCTL_RB2K_D3D7_PROFILE RB2K_DD_IOCTL(0x914)
#define IOCTL_RB2K_D3D7_FLUSH RB2K_DD_IOCTL(0x915)
#define IOCTL_RB2K_D3D7_COMPLETED RB2K_DD_IOCTL(0x916)
#define IOCTL_RB2K_D3D7_FLIGHT RB2K_DD_IOCTL(0x917)
#define IOCTL_RB2K_DD_CREATE_Z16 RB2K_DD_IOCTL(0x918)
#define IOCTL_RB2K_D3D7_ZCLEAR RB2K_DD_IOCTL(0x919)
#define IOCTL_RB2K_D3D7_DRAW RB2K_DD_IOCTL(0x91b)
#define IOCTL_RB2K_D3D7_TEXTURE RB2K_DD_IOCTL(0x91c)
#define IOCTL_RB2K_D3D7_TEXTURE_FREE RB2K_DD_IOCTL(0x91d)
typedef struct { ULONG clears, triangles; } RB2K_D3D7_COMPLETED;
#define RB2K_D3D7_VERTICES 96
/* IEEE transformed position, packed Direct3D diffuse and specular colours,
 * the stage-zero texture coordinate and, for lit draws, the IEEE normal. */
typedef struct { ULONG x, y, z, rhw, diffuse, specular, u, v, nx, ny, nz; } RB2K_D3D7_VERTEX;
#define RB2K_D7_LIGHTS 8
/* Lighting flags: normalize normals, local viewer, then the Direct3D material
 * source (D3DMCS_*, already resolved against the vertex format) of the
 * diffuse, specular, ambient and emissive colours, two bits each. */
#define RB2K_D7_NORMALIZE 1
#define RB2K_D7_LOCAL_VIEWER 2
#define RB2K_D7_SOURCE_SHIFT 4
/* Fog: the factor comes from the specular alpha, from the camera-space
 * depth of each vertex (T&L only), or from each pixel's depth (table fog,
 * the depth z or, when the projection is perspective, w); then the
 * D3DFOGMODE of the computed kinds, range fog and w-based table fog. */
#define RB2K_D7_FOG_ALPHA 1
#define RB2K_D7_FOG_VERTEX 2
#define RB2K_D7_FOG_TABLE 3
#define RB2K_D7_FOG_MODE_SHIFT 4
#define RB2K_D7_FOG_RANGE 0x100
#define RB2K_D7_FOG_W 0x200
/* The low three depth-state bits are D3DCMPFUNC minus one. Disabled depth
 * uses zero. The other words keep Direct3D's own values; the miniport
 * validates each one before it creates a host object for it. */
#define RB2K_Z_ENABLE 16
#define RB2K_Z_WRITE 8
#define RB2K_D7_ON 0x80000000UL
typedef struct {
    ULONG target, depth, count;
    ULONG depthState;
    ULONG alphaTest;    /* RB2K_D7_ON | D3DCMPFUNC << 8 | reference */
    ULONG blend;        /* RB2K_D7_ON | source D3DBLEND << 8 | destination D3DBLEND */
    ULONG cull;         /* D3DCULL */
    ULONG colorStage;   /* D3DTEXTUREOP | first argument << 8 | second argument << 16 */
    ULONG alphaStage;
    ULONG specular;     /* nonzero adds the interpolated specular colour */
    ULONG factor;       /* D3DRENDERSTATE_TEXTUREFACTOR */
    ULONG texture;      /* host texture, zero for none */
    ULONG sampler;      /* address U | V << 4 | MAG << 8 | MIN << 12, Direct3D values */
    ULONG border;
    /* Nonzero: positions are in object space, with w 1, and the host
     * applies the world, view and projection matrices (Direct3D row-major,
     * IEEE words) and the viewport (pixels; minimum and maximum z as IEEE). */
    ULONG transform;
    ULONG matrices[3][16];
    ULONG viewport[6];
    /* Nonzero with transform: light the vertices with the D3DMATERIAL7, the
     * D3DRENDERSTATE_AMBIENT colour and the first lightCount D3DLIGHT7s. */
    ULONG lighting, lightCount, lightFlags, ambient;
    ULONG material[17];
    ULONG lights[RB2K_D7_LIGHTS][26];
    /* Fog kind and flags as above, D3DRENDERSTATE_FOGCOLOR, and the IEEE
     * start, end and density render states. */
    ULONG fog, fogColor, fogStart, fogEnd, fogDensity;
    RB2K_D3D7_VERTEX vertices[RB2K_D3D7_VERTICES];
} RB2K_D3D7_DRAW;
/* Texels are tightly packed B8G8R8A8 rows. A zero id creates a texture of
 * the given size from row zero; the reply is the texture id. */
#define RB2K_D7_TEXTURES 1024
#define RB2K_D7_TEXTURE_SIDE 1024
#define RB2K_D7_UPLOAD_BYTES (256UL * 1024)
typedef struct { ULONG id, target, width, height, y, rows; } RB2K_D3D7_TEXTURE;
typedef struct { ULONG target, depth, flags, color, depthBits; } RB2K_D3D7_ZCLEAR;
enum { RB2K_CLEAR_COLOR = 1, RB2K_CLEAR_DEPTH = 2 };
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
