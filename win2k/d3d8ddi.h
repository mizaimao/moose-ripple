/* The parts of the Direct3D 8 driver interface on NT that the Windows 2000
 * DDK headers lack: the GetDriverInfo2 requests, the D3DCAPS8 record, the
 * format list and the DrawPrimitives2 commands added in DirectX 8. Floats
 * are IEEE bit patterns, because the display driver uses no floating point. */
#ifndef RB2K_D3D8DDI_H
#define RB2K_D3D8DDI_H

/* GetDriverInfo2 shares its GUID with GUID_DDStereoMode; a request carries
 * the magic value where a stereo query has its height. */
#define RB_GDI2_MAGIC 0xffffffffUL
#define RB_DDHALINFO_GETDRIVERINFO2 0x00000008UL
/* The runtime marks the blits of Present with these, for DirectX 8 drivers only. */
#define RB_DDBLT_PRESENTATION 0x10000000UL
#define RB_DDBLT_LAST_PRESENTATION 0x20000000UL
enum {
    RbGdi2Caps8 = 1, RbGdi2FormatCount = 2, RbGdi2Format = 3, RbGdi2DxVersion = 4,
    RbGdi2Caps9 = 0x10, RbGdi2ExtendedModeCount = 0x11, RbGdi2ExtendedMode = 0x12,
    RbGdi2AdapterGroup = 0x13, RbGdi2MultisampleLevels = 0x16, RbGdi2DeferredAgpAware = 0x18,
    RbGdi2FreeDeferredAgp = 0x19, RbGdi2DeferAgpFrees = 0x20, RbGdi2QueryCount = 0x21,
    RbGdi2Query = 0x22, RbGdi2DdiVersion = 0x23
};
/* The first four words of every GetDriverInfo2 buffer; the reply overwrites them. */
typedef struct { ULONG reserved, magic, type, expectedSize; } RB_GDI2_HEADER;
typedef struct { RB_GDI2_HEADER gdi2; ULONG count, reserved; } RB_GDI2_FORMAT_COUNT;
typedef struct { RB_GDI2_HEADER gdi2; ULONG index; DDPIXELFORMAT format; } RB_GDI2_FORMAT;
typedef struct { RB_GDI2_HEADER gdi2; ULONG version, reserved; } RB_GDI2_DX_VERSION;

/* A format-list entry: dwFlags RB_DDPF_D3DFORMAT, dwFourCC the D3DFORMAT,
 * and the operations in the word that holds dwRBitMask for RGB formats. */
#define RB_DDPF_D3DFORMAT 0x00200000UL
enum {
    RbFormatTexture = 0x1, RbFormatOffscreenTarget = 0x8, RbFormatSameTarget = 0x10,
    RbFormatZStencil = 0x40, RbFormatSameTargetUpToAlpha = 0x100, RbFormatDisplayMode = 0x400,
    RbFormat3D = 0x800, RbFormatOffscreenPlain = 0x4000
};
enum {
    RbFmtA8R8G8B8 = 21, RbFmtX8R8G8B8 = 22, RbFmtR5G6B5 = 23, RbFmtX1R5G5B5 = 24,
    RbFmtA1R5G5B5 = 25, RbFmtA4R4G4B4 = 26, RbFmtD16 = 80,
    RbFmtDxt1 = 0x31545844, RbFmtDxt2 = 0x32545844, RbFmtDxt3 = 0x33545844, RbFmtDxt4 = 0x34545844,
    RbFmtDxt5 = 0x35545844
};

/* D3DCAPS8, in the order of the DirectX 8 header. */
typedef struct {
    ULONG deviceType, adapterOrdinal, caps, caps2, caps3, presentationIntervals, cursorCaps;
    ULONG devCaps, primitiveMiscCaps, rasterCaps, zCmpCaps, srcBlendCaps, destBlendCaps;
    ULONG alphaCmpCaps, shadeCaps, textureCaps, textureFilterCaps, cubeTextureFilterCaps;
    ULONG volumeTextureFilterCaps, textureAddressCaps, volumeTextureAddressCaps, lineCaps;
    ULONG maxTextureWidth, maxTextureHeight, maxVolumeExtent, maxTextureRepeat;
    ULONG maxTextureAspectRatio, maxAnisotropy, maxVertexW;
    ULONG guardBandLeft, guardBandTop, guardBandRight, guardBandBottom, extentsAdjust;
    ULONG stencilCaps, fvfCaps, textureOpCaps, maxTextureBlendStages, maxSimultaneousTextures;
    ULONG vertexProcessingCaps, maxActiveLights, maxUserClipPlanes, maxVertexBlendMatrices;
    ULONG maxVertexBlendMatrixIndex, maxPointSize, maxPrimitiveCount, maxVertexIndex;
    ULONG maxStreams, maxStreamStride, vertexShaderVersion, maxVertexShaderConst;
    ULONG pixelShaderVersion, maxPixelShaderValue;
} RB_D3DCAPS8;
typedef char rb_d3dcaps8_size[sizeof(RB_D3DCAPS8) == 53 * 4 ? 1 : -1];
#define RB_D3DCAPS2_CANRENDERWINDOWED 0x00080000UL
#define RB_D3DPRESENT_INTERVAL_ONE 0x00000001UL
#define RB_D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000UL

/* DrawPrimitives2 commands added in DirectX 8. */
enum {
    RbDp2CreateVertexShader = 45, RbDp2DeleteVertexShader = 46, RbDp2SetVertexShader = 47,
    RbDp2SetVertexShaderConst = 48, RbDp2SetStreamSource = 49, RbDp2SetStreamSourceUm = 50,
    RbDp2SetIndices = 51, RbDp2DrawPrimitive = 52, RbDp2DrawIndexedPrimitive = 53,
    RbDp2CreatePixelShader = 54, RbDp2DeletePixelShader = 55, RbDp2SetPixelShader = 56,
    RbDp2SetPixelShaderConst = 57, RbDp2ClippedTriangleFan = 58, RbDp2DrawPrimitive2 = 59,
    RbDp2DrawIndexedPrimitive2 = 60, RbDp2DrawRectPatch = 61, RbDp2DrawTriPatch = 62,
    RbDp2VolumeBlt = 63, RbDp2BufferBlt = 64, RbDp2MultiplyTransform = 65,
    RbDp2AddDirtyRect = 66, RbDp2AddDirtyBox = 67
};
typedef struct { ULONG handle; } RB_DP2_HANDLE;
typedef struct { ULONG handle, declBytes, codeBytes; } RB_DP2_CREATE_VERTEX_SHADER;
typedef struct { ULONG reg, count; } RB_DP2_SHADER_CONST;
typedef struct { ULONG stream, handle, stride; } RB_DP2_STREAM_SOURCE;
typedef struct { ULONG stream, stride; } RB_DP2_STREAM_SOURCE_UM;
typedef struct { ULONG handle, stride; } RB_DP2_INDICES;
typedef struct { ULONG type, start, count; } RB_DP2_DRAW_PRIMITIVE;
typedef struct { ULONG type; LONG baseVertex; ULONG minIndex, vertices, startIndex, count; } RB_DP2_DRAW_INDEXED;
typedef struct { ULONG firstOffset, edgeFlags, count; } RB_DP2_CLIPPED_FAN;
typedef struct { ULONG type, firstOffset, count; } RB_DP2_DRAW_PRIMITIVE2;
typedef struct { ULONG type; LONG baseOffset; ULONG minIndex, vertices, startOffset, count; } RB_DP2_DRAW_INDEXED2;
typedef struct { ULONG handle, codeBytes; } RB_DP2_CREATE_PIXEL_SHADER;
typedef struct { ULONG handle, flags; } RB_DP2_PATCH;
typedef struct { ULONG dest, source, x, y, z, box[6], flags; } RB_DP2_VOLUME_BLT;
typedef struct { ULONG dest, source, offset, range[2], flags; } RB_DP2_BUFFER_BLT;
typedef struct { ULONG type, matrix[16]; } RB_DP2_MULTIPLY_TRANSFORM;
typedef struct { ULONG surface; LONG rect[4]; } RB_DP2_DIRTY_RECT;
typedef struct { ULONG surface, box[6]; } RB_DP2_DIRTY_BOX;
/* D3DPRIMITIVETYPE */
enum { RbPointList = 1, RbLineList, RbLineStrip, RbTriangleList, RbTriangleStrip, RbTriangleFan };

#endif
