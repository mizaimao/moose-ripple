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
/* D3DGDI2_TYPE_GETDDIVERSION: the runtime's DirectX version in, the driver interface out. */
typedef struct { RB_GDI2_HEADER gdi2; ULONG dxVersion, ddiVersion; } RB_GDI2_DDI_VERSION;
#define RB_DX9_DDI_VERSION 4

/* A format-list entry: dwFlags RB_DDPF_D3DFORMAT, dwFourCC the D3DFORMAT,
 * and the operations in the word that holds dwRBitMask for RGB formats. */
#define RB_DDPF_D3DFORMAT 0x00200000UL
enum {
    RbFormatTexture = 0x1, RbFormatVolumeTexture = 0x2, RbFormatCubeTexture = 0x4, RbFormatOffscreenTarget = 0x8, RbFormatSameTarget = 0x10,
    RbFormatZStencil = 0x40, RbFormatSameTargetUpToAlpha = 0x100, RbFormatDisplayMode = 0x400,
    RbFormat3D = 0x800, RbFormatOffscreenPlain = 0x4000, RbFormatBumpMap = 0x10000
};
enum {
    RbFmtA8R8G8B8 = 21, RbFmtX8R8G8B8 = 22, RbFmtR5G6B5 = 23, RbFmtX1R5G5B5 = 24,
    RbFmtA1R5G5B5 = 25, RbFmtA4R4G4B4 = 26, RbFmtA8 = 28, RbFmtL8 = 50, RbFmtA8L8 = 51, RbFmtL16 = 81, RbFmtV8U8 = 60, RbFmtQ8W8V8U8 = 63, RbFmtD16 = 80, RbFmtD24S8 = 75, RbFmtD24X8 = 77,
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
/* D3DCAPS9: D3DCAPS8 (whose last word is PixelShader1xMaxValue there), then the DirectX 9 part. */
typedef struct {
    RB_D3DCAPS8 caps8;
    ULONG devCaps2, maxNpatchTessellationLevel, reserved5, masterAdapterOrdinal, adapterOrdinalInGroup;
    ULONG numberOfAdaptersInGroup, declTypes, numSimultaneousRTs, stretchRectFilterCaps;
    ULONG vs20Caps, vs20DynamicFlowControlDepth, vs20NumTemps, vs20StaticFlowControlDepth;
    ULONG ps20Caps, ps20DynamicFlowControlDepth, ps20NumTemps, ps20StaticFlowControlDepth, ps20NumInstructionSlots;
    ULONG vertexTextureFilterCaps, maxVShaderInstructionsExecuted, maxPShaderInstructionsExecuted;
    ULONG maxVertexShader30InstructionSlots, maxPixelShader30InstructionSlots;
} RB_D3DCAPS9;
typedef char rb_d3dcaps9_size[sizeof(RB_D3DCAPS9) == 76 * 4 ? 1 : -1];
#define RB_D3DCAPS2_CANRENDERWINDOWED 0x00080000UL
#define RB_D3DDTCAPS_UBYTE4 0x00000001UL
#define RB_D3DPMISCCAPS_LINEPATTERNREP 0x00000004UL
#define RB_D3DPMISCCAPS_FOGINFVF 0x00002000UL
#define RB_D3DPRASTERCAPS_SCISSORTEST 0x01000000UL
#define RB_D3DCAPS2_DYNAMICTEXTURES 0x20000000UL
#define RB_D3DVTXPCAPS_NO_VSDT_UBYTE4 0x00000080UL
#define RB_D3DDEVCAPS2_STREAMOFFSET 0x00000001UL
#define RB_D3DPTFILTERCAPS_MINFPOINT 0x00000100UL
#define RB_D3DPTFILTERCAPS_MINFLINEAR 0x00000200UL
#define RB_D3DPTFILTERCAPS_MAGFPOINT 0x01000000UL
#define RB_D3DPTFILTERCAPS_MAGFLINEAR 0x02000000UL
#define RB_D3DPRESENT_INTERVAL_ONE 0x00000001UL
#define RB_D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000UL

/* D3DVERTEXELEMENT9, and the Direct3D 9 commands this driver reads. */
typedef struct { USHORT stream, offset; UCHAR type, method, usage, usageIndex; } RB_VERTEX_ELEMENT9;
enum {
    RbDp2CreateVertexShaderDecl = 71, RbDp2DeleteVertexShaderDecl = 72, RbDp2SetVertexShaderDecl = 73,
    RbDp2CreateVertexShaderFunc = 74, RbDp2DeleteVertexShaderFunc = 75, RbDp2SetVertexShaderFunc = 76,
    RbDp2SetVertexShaderConstI = 77, RbDp2SetScissorRect = 79, RbDp2SetStreamSource2 = 80, RbDp2Blt = 81,
    RbDp2ColorFill = 82, RbDp2SetVertexShaderConstB = 83, RbDp2CreateQuery = 84, RbDp2SetRenderTarget2 = 85,
    RbDp2SetDepthStencil = 86, RbDp2GenerateMipSubLevels = 89, RbDp2DeleteQuery = 90, RbDp2IssueQuery = 91,
    RbDp2SetPixelShaderConstI = 93, RbDp2SetPixelShaderConstB = 94, RbDp2SetStreamSourceFreq = 95,
    RbDp2SurfaceBlt = 96
};
typedef struct { ULONG handle, count; } RB_DP2_CREATE_DECL;      /* elements follow */
typedef struct { ULONG handle, bytes; } RB_DP2_CREATE_FUNC;      /* code follows */
typedef struct { ULONG stream, handle, offset, stride; } RB_DP2_STREAM_SOURCE2;
typedef struct { ULONG index, handle; } RB_DP2_RENDER_TARGET2;
/* BLT and SURFACEBLT (StretchRect, UpdateSurface), and COLORFILL. */
typedef struct { ULONG source; RECTL sourceRect; ULONG sourceLevel; ULONG dest; RECTL destRect; ULONG destLevel; ULONG flags; } RB_DP2_BLT9;
typedef struct { ULONG surface; RECTL rect; ULONG color; } RB_DP2_COLOR_FILL;

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
/* box: left, top, right, bottom, front, back */
typedef struct { ULONG dest, source, x, y, z, box[6], flags; } RB_DP2_VOLUME_BLT;
typedef struct { ULONG dest, source, offset, range[2], flags; } RB_DP2_BUFFER_BLT;
typedef struct { ULONG type, matrix[16]; } RB_DP2_MULTIPLY_TRANSFORM;
typedef struct { ULONG surface; LONG rect[4]; } RB_DP2_DIRTY_RECT;
typedef struct { ULONG surface, box[6]; } RB_DP2_DIRTY_BOX;
/* D3DPRIMITIVETYPE */
enum { RbPointList = 1, RbLineList, RbLineStrip, RbTriangleList, RbTriangleStrip, RbTriangleFan };

#endif
