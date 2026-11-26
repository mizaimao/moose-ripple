/* Private display DLL to miniport interface for offscreen surfaces.
 * Rows are tightly packed. XRGB uses four bytes per pixel and Z16 uses two.
 * WRITE takes the surface ID followed immediately by that many bytes. */
#ifndef RB2K_DDSURFACE_H
#define RB2K_DDSURFACE_H
/* Host images cover the largest screen mode, 1600x1200 or 1680x1050. */
#define RB2K_DD_MAX_WIDTH 1680UL
#define RB2K_DD_MAX_HEIGHT 1200UL
#define RB2K_DD_MAX_BYTES (RB2K_DD_MAX_WIDTH * RB2K_DD_MAX_HEIGHT * 4)
/* VIDEO_MODE_INFORMATION.DriverSpecificAttributeFlags: the mode's desktop lives in guest
 * memory, not VGA memory (RipplePrimaryInMemory; GDI's changes go to the host). */
#define RB2K_MODE_PRIMARY_IN_MEMORY 0x1
/* Offscreen surfaces, render targets and depth buffers may be larger than the screen: Direct3D 9
 * programs make 2048x2048 render target textures (3DMark05's shadow maps). Readback staging keeps
 * the screen's size (its attach request fits one queue packet), so those larger than the screen
 * are drawn into and sampled but cannot be locked. */
#define RB2K_DD_TARGET_SIDE 2048UL
/* Host images at once: render targets (a full-screen chain takes two),
 * depth buffers and render target textures (3DMark 2001 SE alone makes more
 * than a dozen small ones). Each is a virgl context. */
#define RB2K_DD_SLOTS 256
#define RB2K_DD_HOST_SURFACES 1
#define RB2K_DD_NATIVE_D3D7 2
#define RB2K_DD_Z16_STORAGE 4
#define RB2K_DD_Z16_RENDER 8
#define RB2K_DD_PLAIN_SURFACES 16   /* plain 2D offscreen surfaces too */
#define RB2K_DD_D3D8 32             /* the Direct3D 8 driver interface */
#define RB2K_DD_VERTEX_SHADERS 64   /* vertex shaders on the host GPU */
#define RB2K_DD_Z24S8 128           /* depth buffers of 24 bits with 8 of stencil */
/* What a draw packet's vertices are, taken in order: triangles, lines (two each) or points. */
#define RB2K_PRIM_TRIANGLES 0
#define RB2K_PRIM_LINES 1
#define RB2K_PRIM_POINTS 2
/* Point draws: sprites (texture coordinates replaced across the point), and a
 * size per vertex from the vertex program in place of pointSize. */
#define RB2K_POINT_SPRITE 1
#define RB2K_POINT_PER_VERTEX 2
#define RB2K_POINT_SIZE_MAX 0x42800000UL    /* 64.0 */
#define RB2K_DD_D3D9_DDI 256        /* the Direct3D 9 driver interface (with vertex shaders) */
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
#define IOCTL_RB2K_DD_CREATE_Z24S8 RB2K_DD_IOCTL(0x922)
#define IOCTL_RB2K_DD_STRETCH RB2K_DD_IOCTL(0x923)
#define IOCTL_RB2K_D3D7_ZCLEAR RB2K_DD_IOCTL(0x919)
#define IOCTL_RB2K_D3D7_DRAW RB2K_DD_IOCTL(0x91b)
#define IOCTL_RB2K_D3D7_TEXTURE RB2K_DD_IOCTL(0x91c)
#define IOCTL_RB2K_D3D7_TEXTURE_FREE RB2K_DD_IOCTL(0x91d)
/* Several draws to one target: the target id, then records of a ULONG byte
 * count and an RB2K_D3D7_DRAW holding that many bytes (header and vertices). */
#define IOCTL_RB2K_D3D7_DRAWS RB2K_DD_IOCTL(0x91e)
#define RB2K_D7_DRAWS_BYTES (64UL * 1024)
/* Direct3D 8 pixel shaders. PROGRAM takes an RB2K_D3D8_PROGRAM and returns
 * the program id; PROGRAM_FREE takes the id. A shader draw goes into the
 * same queue as an RB2K_D3D8_DRAW whose byte count has RB2K_D8_RECORD set. */
#define IOCTL_RB2K_D3D8_PROGRAM RB2K_DD_IOCTL(0x920)
#define IOCTL_RB2K_D3D8_PROGRAM_FREE RB2K_DD_IOCTL(0x921)
/* A cube map render target: its six faces are colour images, each drawn into like any target; the
 * faces in mask are copied into the host cube texture (made when texture is 0), whose id comes back. */
#define IOCTL_RB2K_D7_CUBE_SYNC RB2K_DD_IOCTL(0x924)
typedef struct { ULONG texture, side, mask, images[6]; } RB2K_D7_CUBE_SYNC;
#define RB2K_D8_PROGRAMS 48
#define RB2K_D8_PROGRAM_BYTES 8192
#define RB2K_D8_STAGES 8            /* samplers, and texture coordinate sets */
#define RB2K_D8_VERTICES 48
#define RB2K_D8_RECORD 0x80000000UL
/* samplers: the texture stages the program samples; text: TGSI, zero-terminated. */
typedef struct { ULONG samplers; char text[RB2K_D8_PROGRAM_BYTES]; } RB2K_D3D8_PROGRAM;
/* A transformed or object-space position as for RB2K_D3D7_VERTEX, the packed
 * colours, the IEEE normal, and four IEEE components of each texture
 * coordinate set. A draw carries only its first coordSets sets, so its
 * vertices are RB2K_D8_VERTEX_WORDS(coordSets) words apart. */
typedef struct { ULONG x, y, z, rhw, diffuse, specular, nx, ny, nz, tc[RB2K_D8_STAGES][4]; } RB2K_D3D8_VERTEX;
#define RB2K_D8_VERTEX_WORDS(sets) (9UL + 4UL * (sets))
/* Draws with a vertex program (transform 2) carry the vertex shader's
 * constants c0..c(vertexConstants-1) and then each vertex's words as the
 * stream holds them, vertexWords apart (whole constants). The program reads
 * c0 at constant RB2K_D8_VS_CONST_BASE. */
#define RB2K_D8_VS_CONST_BASE 19
#define RB2K_D8_VS_CONSTANTS 256
#define RB2K_D8_VS_VERTEX_WORDS 64
#define RB2K_D8_RECORD_WORDS 4096
/* The first seven words match RB2K_D3D7_DRAW. Constants are c0..c7 (or
 * c0..c31) as IEEE words; samplers and borders are encoded as in
 * RB2K_D3D7_DRAW. */
typedef struct {
    ULONG target, depth, count;
    ULONG primitive;                /* RB2K_PRIM_*; count is a multiple of its vertices */
    ULONG pointSize, pointFlags;    /* points: IEEE size, RB2K_POINT_* */
    ULONG depthState, alphaTest, blend, cull;
    ULONG stencil, stencilRef;      /* RB2K_STENCIL_ON etc.; reference, masks */
    ULONG program;
    ULONG coordSets;                /* 0..RB2K_D8_STAGES */
    ULONG textures[RB2K_D8_STAGES], samplers[RB2K_D8_STAGES], borders[RB2K_D8_STAGES];
    ULONG constantCount;            /* 8 for version 1 programs (16 with texbem), 32 for shader model 2 */
    ULONG constants[32][4];
    ULONG fog, fogColor, fogStart, fogEnd, fogDensity;   /* as in RB2K_D3D7_DRAW */
    ULONG transform;                /* 0 transformed, 1 object space, 2 vertex program */
    ULONG matrices[3][16];
    ULONG viewport[6];
    /* The Direct3D 9 scissor test: when scissorOn, only pixels inside the rectangle (left, top,
     * right, bottom, at most 8192) are drawn, as well as inside the viewport. */
    ULONG scissorOn, scissor[4];
    ULONG vertexProgram, vertexConstants, vertexWords;   /* transform 2 */
    /* With RB_VS_FLOW programs: i0..i15 as floats (count, start, step) and b0..b15 as 1 or 0 in w,
     * sent as CONST[3..18]. */
    ULONG flowConstants[16][4];
    /* User clip planes (D3DRS_CLIPPLANEENABLE bits, IEEE planes): in clip space for vertex programs
     * (transform 2), in world space for object-space draws; pretransformed draws are not clipped. */
    ULONG clipMask, clipPlanes[6][4];
    /* Lighting as in RB2K_D3D7_DRAW (object space only); specular adds
     * the lit specular colour. */
    ULONG lighting, lightCount, lightFlags, ambient, specular;
    ULONG material[17];
    ULONG lights[8][26];
    ULONG vertices[RB2K_D8_RECORD_WORDS];
} RB2K_D3D8_DRAW;
typedef char rb2k_d8_record_fits[RB2K_D8_VERTICES * RB2K_D8_VERTEX_WORDS(RB2K_D8_STAGES) <= RB2K_D8_RECORD_WORDS &&
    4 * RB2K_D8_VS_CONSTANTS + RB2K_D8_VERTICES * RB2K_D8_VS_VERTEX_WORDS <= RB2K_D8_RECORD_WORDS ? 1 : -1];
typedef struct { ULONG clears, triangles; } RB2K_D3D7_COMPLETED;
#define RB2K_D3D7_VERTICES 96
/* IEEE transformed position, packed Direct3D diffuse and specular colours,
 * the stage-zero texture coordinate and, for lit draws, the IEEE normal. The
 * last pair is stage one's texture coordinate. */
typedef struct { ULONG x, y, z, rhw, diffuse, specular, u, v, nx, ny, nz, u1, v1; } RB2K_D3D7_VERTEX;
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
/* In a blend word: the render target keeps alpha, so destination alpha is
 * its own (without it, Direct3D reads it as one). */
#define RB2K_BLEND_DEST_ALPHA 0x10000UL
/* In a blend word: the colour channels not written (D3DRS_COLORWRITEENABLE inverted:
 * red, green, blue, alpha from bit 20), with or without RB2K_D7_ON. */
#define RB2K_BLEND_NO_WRITE_SHIFT 20
#define RB2K_BLEND_NO_WRITE (15UL << RB2K_BLEND_NO_WRITE_SHIFT)
typedef struct {
    ULONG target, depth, count;
    ULONG primitive;    /* RB2K_PRIM_*; count is a multiple of its vertices */
    ULONG pointSize, pointFlags;    /* points: IEEE size (at most RB2K_POINT_SIZE_MAX), RB2K_POINT_* */
    ULONG depthState;
    ULONG alphaTest;    /* RB2K_D7_ON | D3DCMPFUNC << 8 | reference */
    ULONG blend;        /* RB2K_D7_ON | source D3DBLEND << 8 | destination D3DBLEND; channel mask bits */
    ULONG cull;         /* D3DCULL */
    ULONG stencil, stencilRef;  /* see RB2K_STENCIL_ON */
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
    ULONG scissorOn, scissor[4];   /* the Direct3D 9 scissor test, as in RB2K_D3D8_DRAW */
    ULONG clipMask, clipPlanes[6][4];   /* user clip planes, as in RB2K_D3D8_DRAW */
    /* Nonzero with transform: light the vertices with the D3DMATERIAL7, the
     * D3DRENDERSTATE_AMBIENT colour and the first lightCount D3DLIGHT7s. */
    ULONG lighting, lightCount, lightFlags, ambient;
    ULONG material[17];
    ULONG lights[RB2K_D7_LIGHTS][26];
    /* Fog kind and flags as above, D3DRENDERSTATE_FOGCOLOR, and the IEEE
     * start, end and density render states. */
    ULONG fog, fogColor, fogStart, fogEnd, fogDensity;
    /* Texture stage one: colour and alpha words as for stage zero, where an
     * argument may also be the current colour; zero when the stage is off. */
    ULONG stage1Color, stage1Alpha, texture1, sampler1, border1;
    RB2K_D3D7_VERTEX vertices[RB2K_D3D7_VERTICES];
} RB2K_D3D7_DRAW;
/* Texels are tightly packed B8G8R8A8 rows of one mipmap level; width and
 * height are the top level's and y and rows count within the level. A zero
 * id creates a texture of the given size and levels from row zero of level
 * zero; the reply is the texture id. */
#define RB2K_D7_TEXTURES 1024
#define RB2K_D7_TEXTURE_SIDE 2048
/* The largest volume texture side (also its depth). */
#define RB2K_D7_VOLUME_SIDE 256
#define RB2K_D7_UPLOAD_BYTES (256UL * 1024)
#define RB2K_D7_LEVELS 12
/* format: 0 for ARGB texels, RB2K_D7_BLOCKS_DXT* for rows of 4x4 blocks as
 * the program wrote them (y a multiple of 4), or RB2K_D7_TEXELS_* for signed
 * texels as they are (bump maps); the miniport answers texture id zero when
 * the host cannot sample that format. */
/* faces: 1, or 6 for a cube map (square), face 0..5 in Direct3D's order.
 * depth: a volume's slices at level zero (each level has half as many, at
 * least one), with face the slice; 0 or 1 otherwise. */
typedef struct { ULONG id, target, width, height, y, rows, levels, level, format, face, faces, depth; } RB2K_D3D7_TEXTURE;
/* A texture id in a draw record with this bit names an image instead: a
 * render target texture, sampled as it is on the host. */
#define RB2K_TEXTURE_IMAGE 0x40000000UL
#define RB2K_D7_BLOCKS_DXT1 1
#define RB2K_D7_BLOCKS_DXT3 2
#define RB2K_D7_BLOCKS_DXT5 3
#define RB2K_D7_TEXELS_V8U8 4       /* two signed bytes, u then v */
#define RB2K_D7_TEXELS_Q8W8V8U8 5   /* four signed bytes, u v w q */
#define RB2K_D7_UPLOAD_FORMATS 6
/* An upload format's block: its side in texels, and its bytes. */
#define RB2K_D7_BLOCK_SIDE(f) ((f) >= RB2K_D7_BLOCKS_DXT1 && (f) <= RB2K_D7_BLOCKS_DXT5 ? 4 : 1)
#define RB2K_D7_BLOCK_BYTES(f) ((f) == RB2K_D7_BLOCKS_DXT1 ? 8 : (f) == RB2K_D7_BLOCKS_DXT3 || (f) == RB2K_D7_BLOCKS_DXT5 ? 16 : \
                                (f) == RB2K_D7_TEXELS_V8U8 ? 2 : 4)
typedef struct { ULONG target, depth, flags, color, depthBits, stencil; } RB2K_D3D7_ZCLEAR;
enum { RB2K_CLEAR_COLOR = 1, RB2K_CLEAR_DEPTH = 2, RB2K_CLEAR_STENCIL = 4 };
/* Stencil state of a draw (both faces): RB2K_STENCIL_ON, the comparison
 * (D3DCMP_* - 1) in bits 0-2 and the fail, depth-fail and pass operations
 * (Gallium's PIPE_STENCIL_OP_*) in bits 4-6, 8-10 and 12-14; and the
 * reference value, read mask and write mask in bytes 0, 1 and 2. */
#define RB2K_STENCIL_ON 0x80000000UL
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
/* A scaled copy of a rectangle of one colour image into a rectangle of another, point-sampled or
 * linear (StretchRect of a Direct3D 9 runtime). Both rectangles lie inside their images. */
typedef struct { ULONG source, destination; RB2K_RECT from, to; ULONG linear; } RB2K_DD_STRETCH;
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
