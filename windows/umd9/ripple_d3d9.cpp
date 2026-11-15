/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 * Copyright 2026 the moose-ripple authors
 *
 * The Direct3D 9 part of the Windows 7 display driver, so far only as far as Windows' own
 * questions go: it answers what the adapter can do and makes no device yet.
 *
 * Why it exists before it can draw: Direct3D 9 offers a hardware device only when the driver's
 * answers pass a list of checks in the runtime, and says nothing when they do not. Here the
 * answers were matched against Windows 7's list (docs/windows/kmd-port-notes.md). It is not in
 * the package yet: with it installed a program is told that there is a hardware device and then
 * fails to create one.
 *
 * The capability tables, the old-style capability structures and the shape of GetCaps and
 * OpenAdapter are taken from the Direct3D 9 driver of the Yttrium project's Mesa fork
 * (src/gallium/frontends/d3d10umd/D3D9Adapter.cpp and D3D9Private.h, MIT, by Ake Rehnman),
 * which matched them against what the Direct3D 9 runtime accepts. Changed here: no Mesa
 * underneath, no multisampling, 16 samples of anisotropic filtering always, no device.
 */
#include <windows.h>
#include <winddk_compat.h>   /* Mesa's: the annotations the WDK headers use, defined away */
#include <d3d9.h>
#include <d3dumddi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef MIN2
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#endif

/*
 * A log of what Windows asks, one line a call, when the build defines RIPPLE_D3D9_LOG as a
 * file name (build.sh: LOG=path). For bringing the driver up; off otherwise.
 */
#ifdef RIPPLE_D3D9_LOG
#include <stdio.h>
#include <stdarg.h>
static void
D3D9Log(const char *format, ...)
{
   FILE *f = fopen(RIPPLE_D3D9_LOG, "ab");
   va_list args;

   if (!f)
      return;
   va_start(args, format);
   vfprintf(f, format, args);
   va_end(args);
   fputs("\r\n", f);
   fclose(f);
}
#else
#define D3D9Log(...) ((void)0)
#endif

struct D3D9Adapter {
   HANDLE hRTAdapter;
   D3DDDI_ADAPTERCALLBACKS callbacks;
   UINT ms_types;
   UINT format_count;   /* how many of the format table's entries are reported */
   UINT query_count;
};

static constexpr D3DDDIFORMAT
D3D9FourCC(char a, char b, char c, char d)
{
   return (D3DDDIFORMAT)((uint32_t)(uint8_t)a |
                         ((uint32_t)(uint8_t)b << 8) |
                         ((uint32_t)(uint8_t)c << 16) |
                         ((uint32_t)(uint8_t)d << 24));
}


static constexpr D3DDDIFORMAT D3D9_FMT_X4R4G4B4 = (D3DDDIFORMAT)30;
static constexpr D3DDDIFORMAT D3D9_FMT_A2B10G10R10 = (D3DDDIFORMAT)31;
static constexpr D3DDDIFORMAT D3D9_FMT_G16R16 = (D3DDDIFORMAT)34;
static constexpr D3DDDIFORMAT D3D9_FMT_A16B16G16R16 = (D3DDDIFORMAT)36;
static constexpr D3DDDIFORMAT D3D9_FMT_A8P8 = (D3DDDIFORMAT)40;
static constexpr D3DDDIFORMAT D3D9_FMT_P8 = (D3DDDIFORMAT)41;
static constexpr D3DDDIFORMAT D3D9_FMT_L8 = (D3DDDIFORMAT)50;
static constexpr D3DDDIFORMAT D3D9_FMT_A8L8 = (D3DDDIFORMAT)51;
static constexpr D3DDDIFORMAT D3D9_FMT_G8R8 = (D3DDDIFORMAT)91;
static constexpr D3DDDIFORMAT D3D9_FMT_R8 = (D3DDDIFORMAT)92;
static constexpr D3DDDIFORMAT D3D9_FMT_D16_LOCKABLE = (D3DDDIFORMAT)70;
static constexpr D3DDDIFORMAT D3D9_FMT_L16 = (D3DDDIFORMAT)81;
static constexpr D3DDDIFORMAT D3D9_FMT_D32F_LOCKABLE = (D3DDDIFORMAT)82;
static constexpr D3DDDIFORMAT D3D9_FMT_Q16W16V16U16 = (D3DDDIFORMAT)110;
static constexpr D3DDDIFORMAT D3D9_FMT_R16F = (D3DDDIFORMAT)111;
static constexpr D3DDDIFORMAT D3D9_FMT_G16R16F = (D3DDDIFORMAT)112;
static constexpr D3DDDIFORMAT D3D9_FMT_A16B16G16R16F = (D3DDDIFORMAT)113;
static constexpr D3DDDIFORMAT D3D9_FMT_R32F = (D3DDDIFORMAT)114;
static constexpr D3DDDIFORMAT D3D9_FMT_G32R32F = (D3DDDIFORMAT)115;
static constexpr D3DDDIFORMAT D3D9_FMT_A32B32G32R32F = (D3DDDIFORMAT)116;
static constexpr D3DDDIFORMAT D3D9_FMT_V8U8 = (D3DDDIFORMAT)60;
static constexpr D3DDDIFORMAT D3D9_FMT_Q8W8V8U8 = (D3DDDIFORMAT)63;
static constexpr D3DDDIFORMAT D3D9_FMT_V16U16 = (D3DDDIFORMAT)64;
static constexpr D3DDDIFORMAT D3D9_FMT_DXT1 = D3D9FourCC('D', 'X', 'T', '1');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT2 = D3D9FourCC('D', 'X', 'T', '2');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT3 = D3D9FourCC('D', 'X', 'T', '3');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT4 = D3D9FourCC('D', 'X', 'T', '4');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT5 = D3D9FourCC('D', 'X', 'T', '5');
static constexpr D3DDDIFORMAT D3D9_FMT_ATI1 = D3D9FourCC('A', 'T', 'I', '1');
static constexpr D3DDDIFORMAT D3D9_FMT_DF16 = D3D9FourCC('D', 'F', '1', '6');
static constexpr D3DDDIFORMAT D3D9_FMT_DF24 = D3D9FourCC('D', 'F', '2', '4');
static constexpr D3DDDIFORMAT D3D9_FMT_INTZ = D3D9FourCC('I', 'N', 'T', 'Z');
static constexpr D3DDDIFORMAT D3D9_FMT_AYUV = D3D9FourCC('A', 'Y', 'U', 'V');
static constexpr D3DDDIFORMAT D3D9_FMT_YUY2 = D3D9FourCC('Y', 'U', 'Y', '2');

static float
D3D9FloatFromBits(uint32_t bits)
{
   float value;

   static_assert(sizeof(value) == sizeof(bits));
   memcpy(&value, &bits, sizeof(value));
   return value;
}

/* WARP is the oracle for the D3D9 runtime's adapter validation path.  Mirror
 * its DDI cap/format/query envelope closely, then layer in the compatibility
 * bits seen in older virtual WDDM drivers where they help legacy D3D9 apps.
 *
 */
static constexpr UINT d3d9_ms_types =
   (1u << (D3DDDIMULTISAMPLE_NONMASKABLE - 1)) |
   (1u << (D3DDDIMULTISAMPLE_2_SAMPLES - 1)) |
   (1u << (D3DDDIMULTISAMPLE_4_SAMPLES - 1)) |
   (1u << (D3DDDIMULTISAMPLE_8_SAMPLES - 1));

static const FORMATOP g_format_ops[] = {
   { D3DDDIFMT_X8R8G8B8, 0x009aec1f & ~FORMATOP_SRGBWRITE,
     d3d9_ms_types, 0, 0 },
   { D3DDDIFMT_R5G6B5, 0x008a6c1f, d3d9_ms_types, 0, 0 },
   { D3DDDIFMT_A8R8G8B8, 0x00dae01f,
     d3d9_ms_types, d3d9_ms_types, 0 },
   { D3DDDIFMT_A8B8G8R8, 0x00dae01f, d3d9_ms_types, 0, 0 },
   { D3DDDIFMT_A1R5G5B5, 0x00ca601f, d3d9_ms_types, 0, 0 },
   { D3DDDIFMT_X1R5G5B5, 0x00ca601f, d3d9_ms_types, 0, 0 },
   { D3DDDIFMT_A4R4G4B4, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_X4R4G4B4, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_A16B16G16R16, 0x00ca601f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_G16R16, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_A2B10G10R10, 0x00ca601f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_A16B16G16R16F, 0x00ca601f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_R16F, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_G16R16F, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_R32F, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_G32R32F, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_A32B32G32R32F, 0x00ca601f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_V8U8, 0x0083401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_Q8W8V8U8, 0x0083401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_V16U16, 0x0083401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_Q16W16V16U16, 0x0083401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_L8, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_L16, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3DDDIFMT_A8, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_A8L8, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3DDDIFMT_A2R10G10B10, 0x00ca601f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_DXT1, 0x00828007, 0, 0, 0 },
   { D3D9_FMT_DXT2, 0x00828007, 0, 0, 0 },
   { D3D9_FMT_DXT3, 0x00828007, 0, 0, 0 },
   { D3D9_FMT_DXT4, 0x00828007, 0, 0, 0 },
   { D3D9_FMT_DXT5, 0x00828007, 0, 0, 0 },
   { D3D9_FMT_ATI1, 0x0082d007, 0, 0, 8 },
   { D3D9_FMT_P8, 0x00824007, 0, 0, 0 },
   { D3D9_FMT_A8P8, 0x00820007, 0, 0, 0 },
   /* Auto depth-stencil creation can validate the blt MSAA mask even for a
    * fullscreen swapchain.  Both masks describe renderable sample counts;
    * neither advertises depth/stencil StretchRect support.
    */
   { D3DDDIFMT_D24S8, 0x000000c0, d3d9_ms_types, d3d9_ms_types, 0 },
   { D3DDDIFMT_D24X8, 0x000000c0, 0, 0, 0 },
   { D3DDDIFMT_D16, 0x000000c0, 0, 0, 0 },
   { D3D9_FMT_D16_LOCKABLE, 0x000000c0, 0, 0, 0 },
   { D3D9_FMT_D32F_LOCKABLE, 0x000000c0, 0, 0, 0 },
   { D3D9_FMT_DF16, 0x000410c1, 0, 0, 0 },
   { D3D9_FMT_DF24, 0x000410c1, 0, 0, 0 },
   { D3D9_FMT_INTZ, 0x000410c1, 0, 0, 0 },
   { D3D9_FMT_AYUV, 0x00005000, 0, 0, 32 },
   { D3D9_FMT_YUY2, 0x00005000, 0, 0, 16 },
   { D3D9_FMT_R8, 0x00c2401f, d3d9_ms_types, 0, 0 },
   { D3D9_FMT_G8R8, 0x00c2401f, d3d9_ms_types, 0, 0 },
};

static const D3DDDIQUERYTYPE g_query_types[] = {
   D3DDDIQUERYTYPE_EVENT,
   D3DDDIQUERYTYPE_OCCLUSION,
};

static void
D3D9FillCaps(const D3D9Adapter *adapter, D3DCAPS9 *caps)
{
   /* D3D9 removed the public name for the legacy D3DPTEXTURECAPS bit, but
    * the DDI caps field retains its numeric layout. */
   const DWORD texture_transparency_cap = 0x00000008u;

   memset(caps, 0, sizeof(*caps));

   caps->DeviceType = D3DDEVTYPE_HAL;
   caps->AdapterOrdinal = 0;
   caps->Caps = 0x00020000;
   caps->Caps2 = 0xe0020000;
   caps->Caps3 =
      0x000003a0 & ~D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION;
   caps->PresentationIntervals = 0x8000000f;
   caps->CursorCaps = 0x00000001;
   caps->DevCaps = 0x061bbff0;
   /* POSITIONT depth is clamped, not clipped, so do not claim CLIPTLVERTS. */
   caps->PrimitiveMiscCaps =
      0x002eeff2 &
      ~(D3DPMISCCAPS_CLIPTLVERTS |
        D3DPMISCCAPS_CLIPPLANESCALEDPOINTS);
   caps->RasterCaps =
      0x0f732190 &
      ~D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
   caps->ZCmpCaps = 0x000000ff;
   caps->SrcBlendCaps =
      0x0000ffff &
      ~(D3DPBLENDCAPS_SRCCOLOR2 | D3DPBLENDCAPS_INVSRCCOLOR2);
   caps->DestBlendCaps =
      0x0000ffff &
      ~(D3DPBLENDCAPS_SRCCOLOR2 | D3DPBLENDCAPS_INVSRCCOLOR2 |
        D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA);
   caps->AlphaCmpCaps = 0x000000ff;
   caps->ShadeCaps = 0x00084208;
   caps->TextureCaps =
      0x0001ecdd & ~texture_transparency_cap;
   caps->TextureFilterCaps = 0x07030700;
   caps->CubeTextureFilterCaps = 0x07030700;
   caps->VolumeTextureFilterCaps = 0x07030700;
   /* The Windows 10 D3D9 runtime rejects CreateDevice before calling the UMD
    * when BORDER and MIRRORONCE are removed independently or together.  Keep
    * the runtime-required pair here.  SetTextureStageState accepts them for
    * compatibility and loudly records the transparent-black BORDER and
    * clamp-to-edge MIRRORONCE approximations.
    */
   caps->TextureAddressCaps = 0x0000003f;
   caps->VolumeTextureAddressCaps = 0x0000003f;
   caps->LineCaps = 0x0000003f & ~D3DLINECAPS_ANTIALIAS;
   caps->MaxTextureWidth = 0x00004000;
   caps->MaxTextureHeight = 0x00004000;
   caps->MaxVolumeExtent = 0x00004000;
   caps->MaxTextureRepeat = 0x00002000;
   caps->MaxTextureAspectRatio = 0x00002000;
   (void)adapter;
   /* Windows 7's Direct3D 9 offers no hardware device at all at this shader level unless the
    * driver filters anisotropically, 16 samples or more: the raster capability, the minification
    * filter and this number are all on its list (docs/windows/kmd-port-notes.md).
    */
   caps->MaxAnisotropy = 16;
   caps->MaxVertexW = D3D9FloatFromBits(0x501502f9);
   caps->GuardBandLeft = D3D9FloatFromBits(0xccbebc20);
   caps->GuardBandTop = D3D9FloatFromBits(0xccbebc20);
   caps->GuardBandRight = D3D9FloatFromBits(0x4cbebc20);
   caps->GuardBandBottom = D3D9FloatFromBits(0x4cbebc20);
   caps->ExtentsAdjust = 0.0f;
   caps->StencilCaps = 0x000001ff;
   caps->FVFCaps = 0x00100008;
   caps->TextureOpCaps = 0x03ffffff;
   caps->MaxTextureBlendStages = 0x00000008;
   caps->MaxSimultaneousTextures = 0x00000008;
   caps->VertexProcessingCaps = 0x0000017b;
   caps->MaxActiveLights = 0x00000008;
   caps->MaxUserClipPlanes = 0x00000008;
   caps->MaxVertexBlendMatrices = 0x00000004;
   caps->MaxVertexBlendMatrixIndex = 0x00000000;
   caps->MaxPointSize = D3D9FloatFromBits(0x46000000);
   caps->MaxPrimitiveCount = 0x007fffff;
   caps->MaxVertexIndex = 0x00ffffff;
   caps->MaxStreams = 0x00000010;
   caps->MaxStreamStride = 0x000000ff;
   caps->VertexShaderVersion = 0xfffe0300;
   caps->MaxVertexShaderConst = 0x00000100;
   caps->PixelShaderVersion = 0xffff0300;
   caps->PixelShader1xMaxValue = D3D9FloatFromBits(0x7f7fffff);
   caps->DevCaps2 =
      0x0000006f &
      ~(D3DDEVCAPS2_DMAPNPATCH |
        D3DDEVCAPS2_ADAPTIVETESSRTPATCH |
        D3DDEVCAPS2_ADAPTIVETESSNPATCH |
        D3DDEVCAPS2_PRESAMPLEDDMAPNPATCH);
   /* Windows validates this as at least 1.0 even when patch caps are absent. */
   caps->MaxNpatchTessellationLevel = 1.0f;
   caps->Reserved5 = 0;
   caps->MasterAdapterOrdinal = 0;
   caps->AdapterOrdinalInGroup = 0;
   caps->NumberOfAdaptersInGroup = 1;
   caps->DeclTypes =
      0x000003ff & ~(D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N);
   caps->NumSimultaneousRTs = 0x00000004;
   caps->StretchRectFilterCaps = 0x03000300;
   caps->VS20Caps.Caps = 0x00000001;
   caps->VS20Caps.DynamicFlowControlDepth = 0x00000018;
   caps->VS20Caps.NumTemps = 0x00000020;
   caps->VS20Caps.StaticFlowControlDepth = 0x00000004;
   caps->PS20Caps.Caps = 0x0000001f;
   caps->PS20Caps.DynamicFlowControlDepth = 0x00000018;
   caps->PS20Caps.NumTemps = 0x00000020;
   caps->PS20Caps.StaticFlowControlDepth = 0x00000004;
   caps->PS20Caps.NumInstructionSlots = 0x00000200;
   caps->VertexTextureFilterCaps = 0x03030300;
   caps->MaxVShaderInstructionsExecuted = 0xffffffff;
   caps->MaxPShaderInstructionsExecuted = 0xffffffff;
   caps->MaxVertexShader30InstructionSlots = 0x00008000;
   caps->MaxPixelShader30InstructionSlots = 0x00008000;

}

/* D3DDDICAPS_GETD3D3CAPS uses the documented D3DHAL_GLOBALDRIVERDATA
 * layout.  Keep a private ABI definition here because the legacy d3dhal.h
 * header conflicts with the D3D9 headers in some supported toolchains.
 */
struct D3D9LegacyTransformCaps {
   DWORD dwSize;
   DWORD dwCaps;
};

struct D3D9LegacyLightingCaps {
   DWORD dwSize;
   DWORD dwCaps;
   DWORD dwLightingModel;
   DWORD dwNumLights;
};

struct D3D9LegacyPrimitiveCaps {
   DWORD dwSize;
   DWORD dwMiscCaps;
   DWORD dwRasterCaps;
   DWORD dwZCmpCaps;
   DWORD dwSrcBlendCaps;
   DWORD dwDestBlendCaps;
   DWORD dwAlphaCmpCaps;
   DWORD dwShadeCaps;
   DWORD dwTextureCaps;
   DWORD dwTextureFilterCaps;
   DWORD dwTextureBlendCaps;
   DWORD dwTextureAddressCaps;
   DWORD dwStippleWidth;
   DWORD dwStippleHeight;
};

struct D3D9LegacyDeviceDesc {
   DWORD dwSize;
   DWORD dwFlags;
   DWORD dcmColorModel;
   DWORD dwDevCaps;
   D3D9LegacyTransformCaps dtcTransformCaps;
   BOOL bClipping;
   D3D9LegacyLightingCaps dlcLightingCaps;
   D3D9LegacyPrimitiveCaps dpcLineCaps;
   D3D9LegacyPrimitiveCaps dpcTriCaps;
   DWORD dwDeviceRenderBitDepth;
   DWORD dwDeviceZBufferBitDepth;
   DWORD dwMaxBufferSize;
   DWORD dwMaxVertexCount;
};

struct D3D9LegacyGlobalDriverData {
   DWORD dwSize;
   D3D9LegacyDeviceDesc hwCaps;
   DWORD dwNumVertices;
   DWORD dwNumClipVertices;
   DWORD dwNumTextureFormats;
   void *lpTextureFormats;
};

static_assert(sizeof(D3D9LegacyTransformCaps) == 8);
static_assert(sizeof(D3D9LegacyLightingCaps) == 16);
static_assert(sizeof(D3D9LegacyPrimitiveCaps) == 56);
static_assert(sizeof(D3D9LegacyDeviceDesc) == 172);
static_assert(offsetof(D3D9LegacyGlobalDriverData, dwNumTextureFormats) == 184);
static_assert(sizeof(D3D9LegacyGlobalDriverData) ==
              (sizeof(void *) == 8 ? 200 : 192));

static constexpr DWORD d3d9_legacy_valid_color_model = 0x00000001u;
static constexpr DWORD d3d9_legacy_valid_dev_caps = 0x00000002u;
static constexpr DWORD d3d9_legacy_valid_render_bit_depth = 0x00000080u;
static constexpr DWORD d3d9_legacy_valid_z_bit_depth = 0x00000100u;
static constexpr DWORD d3d9_legacy_color_rgb = 2u;
static constexpr DWORD d3d9_legacy_depth_16 = 0x00000400u;
static constexpr DWORD d3d9_legacy_depth_24 = 0x00000200u;
static constexpr DWORD d3d9_legacy_depth_32 = 0x00000100u;

static constexpr DWORD d3d9_legacy_dev_float_tl_vertex = 0x00000001u;
static constexpr DWORD d3d9_legacy_dev_execute_system_memory = 0x00000010u;
static constexpr DWORD d3d9_legacy_dev_execute_video_memory = 0x00000020u;
static constexpr DWORD d3d9_legacy_dev_tl_vertex_system_memory = 0x00000040u;
static constexpr DWORD d3d9_legacy_dev_tl_vertex_video_memory = 0x00000080u;
static constexpr DWORD d3d9_legacy_dev_texture_system_memory = 0x00000100u;
static constexpr DWORD d3d9_legacy_dev_texture_video_memory = 0x00000200u;
static constexpr DWORD d3d9_legacy_dev_draw_prim_tl_vertex = 0x00000400u;
static constexpr DWORD d3d9_legacy_dev_render_after_flip = 0x00000800u;
static constexpr DWORD d3d9_legacy_dev_texture_nonlocal = 0x00001000u;
static constexpr DWORD d3d9_legacy_dev_draw_primitives2 = 0x00002000u;
static constexpr DWORD d3d9_legacy_dev_draw_primitives2_ex = 0x00008000u;
static constexpr DWORD d3d9_legacy_dev_hw_transform_light = 0x00010000u;
static constexpr DWORD d3d9_legacy_dev_blit_sys_to_nonlocal = 0x00020000u;
static constexpr DWORD d3d9_legacy_dev_hw_rasterization = 0x00080000u;

static void
D3D9FillLegacyCaps(const D3D9Adapter *adapter,
                   D3D9LegacyGlobalDriverData *caps)
{
   D3DCAPS9 d3d9_caps;
   D3D9FillCaps(adapter, &d3d9_caps);

   memset(caps, 0, sizeof(*caps));
   caps->dwSize = sizeof(*caps);

   D3D9LegacyDeviceDesc *hw = &caps->hwCaps;
   hw->dwSize = sizeof(*hw);
   hw->dwFlags = d3d9_legacy_valid_color_model |
                 d3d9_legacy_valid_dev_caps |
                 d3d9_legacy_valid_render_bit_depth |
                 d3d9_legacy_valid_z_bit_depth;
   hw->dcmColorModel = d3d9_legacy_color_rgb;

   const DWORD legacy_dev_caps =
      d3d9_legacy_dev_float_tl_vertex |
      d3d9_legacy_dev_execute_system_memory |
      d3d9_legacy_dev_tl_vertex_system_memory |
      d3d9_legacy_dev_texture_system_memory |
      d3d9_legacy_dev_texture_video_memory |
      d3d9_legacy_dev_draw_prim_tl_vertex |
      d3d9_legacy_dev_render_after_flip |
      d3d9_legacy_dev_texture_nonlocal |
      d3d9_legacy_dev_draw_primitives2 |
      d3d9_legacy_dev_draw_primitives2_ex |
      d3d9_legacy_dev_hw_transform_light |
      d3d9_legacy_dev_blit_sys_to_nonlocal |
      d3d9_legacy_dev_hw_rasterization;
   hw->dwDevCaps = d3d9_caps.DevCaps & legacy_dev_caps;

   /* These obsolete flags have mandatory values for legacy runtimes. */
   hw->dwDevCaps |= d3d9_legacy_dev_float_tl_vertex |
                    d3d9_legacy_dev_execute_system_memory |
                    d3d9_legacy_dev_tl_vertex_system_memory |
                    d3d9_legacy_dev_draw_prim_tl_vertex |
                    d3d9_legacy_dev_render_after_flip;
   hw->dwDevCaps &= ~(d3d9_legacy_dev_execute_video_memory |
                      d3d9_legacy_dev_tl_vertex_video_memory);

   hw->dtcTransformCaps.dwSize = sizeof(hw->dtcTransformCaps);
   hw->dlcLightingCaps.dwSize = sizeof(hw->dlcLightingCaps);
   hw->dpcLineCaps.dwSize = sizeof(hw->dpcLineCaps);
   hw->dpcTriCaps.dwSize = sizeof(hw->dpcTriCaps);
   hw->dwDeviceRenderBitDepth = d3d9_legacy_depth_16 |
                                d3d9_legacy_depth_32;
   hw->dwDeviceZBufferBitDepth = d3d9_legacy_depth_16 |
                                 d3d9_legacy_depth_24 |
                                 d3d9_legacy_depth_32;
}

static HRESULT
D3D9ValidateData(const D3DDDIARG_GETCAPS *data, UINT size)
{
   return data && data->pData && data->DataSize >= size ? S_OK : E_INVALIDARG;
}

static HRESULT APIENTRY
D3D9CloseAdapter(HANDLE hAdapter)
{
   D3D9Log("CloseAdapter");
   free(hAdapter);
   return S_OK;
}

/* No device yet: the drawing half of this driver is still to come. */
static HRESULT APIENTRY
D3D9CreateDevice(HANDLE hAdapter, D3DDDIARG_CREATEDEVICE *data)
{
   (void)hAdapter;
   (void)data;
   D3D9Log("CreateDevice: refused, no device yet");
   return E_FAIL;
}

static HRESULT APIENTRY
D3D9GetCaps(HANDLE hAdapter, const D3DDDIARG_GETCAPS *data)
{

   if (!hAdapter || !data)
      return E_INVALIDARG;

   const D3D9Adapter *adapter = (const D3D9Adapter *)hAdapter;

   D3D9Log("GetCaps type %u, %u bytes at %p, info %p", (unsigned)data->Type, data->DataSize, data->pData,
           data->pInfo);

   switch (data->Type) {
   case D3DDDICAPS_DDRAW:
      if (FAILED(D3D9ValidateData(data, sizeof(DDRAW_CAPS))))
         return E_INVALIDARG;
      memset(data->pData, 0, data->DataSize);
      ((DDRAW_CAPS *)data->pData)->Caps = DDRAW_CAPS_ZBLTS |
                                          DDRAW_CAPS_BLTDEPTHFILL;
      ((DDRAW_CAPS *)data->pData)->Caps2 = DDRAW_CAPS2_DYNAMICTEXTURES;
      ((DDRAW_CAPS *)data->pData)->CKeyCaps = 0;
      ((DDRAW_CAPS *)data->pData)->FxCaps = 0;
      return S_OK;
   case D3DDDICAPS_DDRAW_MODE_SPECIFIC:
      if (FAILED(D3D9ValidateData(data, sizeof(DDRAW_MODE_SPECIFIC_CAPS))))
         return E_INVALIDARG;
      memset(data->pData, 0, data->DataSize);
      return S_OK;
   case D3DDDICAPS_GETFORMATCOUNT:
      if (FAILED(D3D9ValidateData(data, sizeof(UINT))))
         return E_INVALIDARG;
      *(UINT *)data->pData = adapter->format_count;
      return S_OK;
   case D3DDDICAPS_GETFORMATDATA:
      if (FAILED(D3D9ValidateData(data, adapter->format_count * sizeof(FORMATOP))))
         return E_INVALIDARG;
      memcpy(data->pData, g_format_ops, adapter->format_count * sizeof(FORMATOP));
      for (UINT i = 0; i < adapter->format_count; ++i) {
         FORMATOP *format_op = &((FORMATOP *)data->pData)[i];
         format_op->BltMsTypes &= adapter->ms_types;
         format_op->FlipMsTypes &= adapter->ms_types;
      }
      return S_OK;
   case D3DDDICAPS_GETMULTISAMPLEQUALITYLEVELS:
      if (FAILED(D3D9ValidateData(data, sizeof(DDIMULTISAMPLEQUALITYLEVELSDATA))))
         return E_INVALIDARG;
      {
         DDIMULTISAMPLEQUALITYLEVELSDATA *ms =
            (DDIMULTISAMPLEQUALITYLEVELSDATA *)data->pData;
         ms->QualityLevels = 0;
      }
      return S_OK;
   case D3DDDICAPS_GETD3DQUERYCOUNT:
      if (FAILED(D3D9ValidateData(data, sizeof(UINT))))
         return E_INVALIDARG;
      *(UINT *)data->pData = adapter->query_count;
      return S_OK;
   case D3DDDICAPS_GETD3DQUERYDATA:
      if (FAILED(D3D9ValidateData(data, adapter->query_count * sizeof(g_query_types[0]))))
         return E_INVALIDARG;
      memcpy(data->pData, g_query_types, adapter->query_count * sizeof(g_query_types[0]));
      return S_OK;
   case D3DDDICAPS_GETD3D3CAPS:
      if (FAILED(D3D9ValidateData(data,
                                 sizeof(D3D9LegacyGlobalDriverData))))
         return E_INVALIDARG;
      D3D9FillLegacyCaps(adapter,
                         (D3D9LegacyGlobalDriverData *)data->pData);
      return S_OK;
   case D3DDDICAPS_GETD3D9CAPS:
      if (FAILED(D3D9ValidateData(data, sizeof(D3DCAPS9))))
         return E_INVALIDARG;
      D3D9FillCaps(adapter, (D3DCAPS9 *)data->pData);
      return S_OK;
   case D3DDDICAPS_GETGAMMARAMPCAPS:
      if (FAILED(D3D9ValidateData(data, sizeof(DDIGAMMACAPS))))
         return E_INVALIDARG;
      memset(data->pData, 0, data->DataSize);
      ((DDIGAMMACAPS *)data->pData)->GammaCaps = GAMMA_CAP_RGB256x3x16;
      return S_OK;
   default:
      D3D9Log("  not answered");
      return E_NOTIMPL;
   }
}

EXTERN_C HRESULT APIENTRY
OpenAdapter(D3DDDIARG_OPENADAPTER *open_data)
{
   if (!open_data || !open_data->pAdapterFuncs)
      return E_INVALIDARG;

   D3D9Adapter *adapter = (D3D9Adapter *)calloc(1, sizeof(*adapter));
   if (!adapter)
      return E_OUTOFMEMORY;

   adapter->hRTAdapter = open_data->hAdapter;
   if (open_data->pAdapterCallbacks)
      adapter->callbacks = *open_data->pAdapterCallbacks;
   adapter->ms_types = 0;
   adapter->format_count = ARRAYSIZE(g_format_ops);
   adapter->query_count = ARRAYSIZE(g_query_types);
#ifdef RIPPLE_D3D9_LOG
   /* For finding out which answer a Windows version cannot take: fewer entries, by environment */
   if (getenv("RIPPLE_D3D9_FORMATS"))
      adapter->format_count = MIN2((UINT)atoi(getenv("RIPPLE_D3D9_FORMATS")), adapter->format_count);
   if (getenv("RIPPLE_D3D9_QUERIES"))
      adapter->query_count = MIN2((UINT)atoi(getenv("RIPPLE_D3D9_QUERIES")), adapter->query_count);
#endif

   open_data->hAdapter = adapter;
   open_data->pAdapterFuncs->pfnGetCaps = D3D9GetCaps;
   open_data->pAdapterFuncs->pfnCreateDevice = D3D9CreateDevice;
   open_data->pAdapterFuncs->pfnCloseAdapter = D3D9CloseAdapter;
   open_data->DriverVersion = D3D_UMD_INTERFACE_VERSION_WIN7;
   D3D9Log("OpenAdapter: interface %u, version %u", open_data->Interface, open_data->Version);
   return S_OK;
}
