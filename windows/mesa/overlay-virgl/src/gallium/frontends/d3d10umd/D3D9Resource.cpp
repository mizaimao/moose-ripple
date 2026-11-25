/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 * Copyright 2026 VMLabDev and contributors
 */
#include "D3D9Private.h"
#include "Format.h"

#include "D3D9Host.h"
#include "RippleLog.h"
#include "D3D9Trace.h"
#include "D3D9Dxt.h"

#include "tgsi/tgsi_ureg.h"
#include "util/format/u_format.h"
#include "util/format_srgb.h"
#include "util/u_draw.h"
#include "util/u_sampler.h"
#include "util/u_surface.h"
#include "util/u_math.h"
#include "util/u_threaded_context.h"
#include "util/u_upload_mgr.h"
#include "virtio/virtio-gpu/virgl_hw.h"
#include "virtio/wddm/viogpu_wddm_driver.h"

#include <stdlib.h>
#include <stdio.h>

static bool
D3D9OrderedContextWorkerEnabled()
{
   static int enabled = -1;

   if (enabled < 0) {
      enabled = yttrium_gdi_debug_get_bool_option(
         "D3D10UMD_YTTRIUM_ORDERED_CONTEXT_WORKER", true) ? 1 : 0;
   }
   return enabled != 0;
}

static void
D3D9ReleaseWorkerBufferUpload(D3D9Device *device, D3D9SubResource *sub)
{
   if (!sub || !sub->worker_upload_buffer)
      return;

   if (device && device->pipe) {
      pipe_resource_release(device->pipe, sub->worker_upload_buffer);
      sub->worker_upload_buffer = NULL;
   } else {
      pipe_resource_reference(&sub->worker_upload_buffer, NULL);
   }
   sub->worker_upload_offset = 0;
   sub->worker_upload_source_offset = 0;
   sub->worker_upload_size = 0;
}

static bool
D3D9CaptureWorkerBufferUpload(D3D9Device *device, D3D9SubResource *sub)
{
   if (!device || !device->pipe || !device->pipe->stream_uploader || !sub ||
       !sub->transfer_map || sub->transfer_box.x < 0 ||
       sub->transfer_box.width <= 0)
      return false;

   const unsigned source_offset = (unsigned)sub->transfer_box.x;
   const unsigned size = (unsigned)sub->transfer_box.width;
   unsigned upload_offset = 0;
   struct pipe_resource *upload_buffer = NULL;
   u_upload_data_ref(device->pipe->stream_uploader, source_offset, size, 16,
                     sub->transfer_map, &upload_offset, &upload_buffer);
   if (!upload_buffer || upload_offset < source_offset)
      return false;

   D3D9ReleaseWorkerBufferUpload(device, sub);
   sub->worker_upload_buffer = upload_buffer;
   sub->worker_upload_offset = upload_offset;
   sub->worker_upload_source_offset = source_offset;
   sub->worker_upload_size = size;
   return true;
}

static bool
D3D9ResourcesMatchForManagedUpload(const D3D9Resource *src,
                                   const D3D9Resource *dst,
                                   UINT *src_base_subresource);
static bool
D3D9ResourcesMatchForVolumeUpload(const D3D9Resource *src,
                                  const D3D9Resource *dst,
                                  UINT *src_base_subresource);
static bool
D3D9ManagedUploadSourceClean(const D3D9Resource *src,
                             const D3D9Resource *dst);

static UINT
D3D9FormatBytesPerPixel(D3DDDIFORMAT format)
{
   switch (format) {
   case D3DDDIFMT_A8R8G8B8:
   case D3DDDIFMT_X8R8G8B8:
   case D3DDDIFMT_A8B8G8R8:
   case D3DDDIFMT_X8B8G8R8:
   case D3DDDIFMT_A2R10G10B10:
   case D3D9_FMT_A2B10G10R10:
   case D3DDDIFMT_D24X8:
   case D3DDDIFMT_D24S8:
   case D3D9_FMT_DF24:
   case D3D9_FMT_INTZ:
   case D3DDDIFMT_INDEX32:
   case D3D9_FMT_G16R16:
   case D3D9_FMT_G16R16F:
   case D3D9_FMT_R32F:
   case D3D9_FMT_Q8W8V8U8:
   case D3D9_FMT_V16U16:
   case D3D9_FMT_D32F_LOCKABLE:
      return 4;
   case D3D9_FMT_A16B16G16R16:
   case D3D9_FMT_A16B16G16R16F:
   case D3D9_FMT_G32R32F:
   case D3D9_FMT_Q16W16V16U16:
      return 8;
   case D3D9_FMT_A32B32G32R32F:
      return 16;
   case D3DDDIFMT_R5G6B5:
   case D3DDDIFMT_X1R5G5B5:
   case D3DDDIFMT_A1R5G5B5:
   case D3D9_FMT_G8R8:
   case D3DDDIFMT_D16:
   case D3D9_FMT_DF16:
   case D3DDDIFMT_INDEX16:
   case D3DDDIFMT_A4R4G4B4:
   case D3D9_FMT_X4R4G4B4:
   case D3D9_FMT_A8P8:
   case D3D9_FMT_A8L8:
   case D3D9_FMT_D16_LOCKABLE:
   case D3D9_FMT_L16:
   case D3D9_FMT_R16F:
   case D3D9_FMT_V8U8:
   case D3D9_FMT_L6V5U5:
   case D3D9_FMT_YUY2:
      return 2;
   case D3D9_FMT_NV12: /* its luma plane; the rows of chroma: D3D9FormatRowCount */
   case D3D9_FMT_R8:
   case D3DDDIFMT_A8:
   case D3D9_FMT_P8:
   case D3D9_FMT_L8:
      return 1;
   default:
      return 4;
   }
}

/* The two 32-bit RGB formats, which differ in whether the fourth byte means anything */
static bool
D3D9IsRgb32(D3DDDIFORMAT format)
{
   return format == D3DDDIFMT_X8R8G8B8 || format == D3DDDIFMT_A8R8G8B8;
}

static bool
D3D9FormatIsBlockCompressed(D3DDDIFORMAT format)
{
   switch (format) {
   case D3D9_FMT_DXT1:
   case D3D9_FMT_DXT2:
   case D3D9_FMT_DXT3:
   case D3D9_FMT_DXT4:
   case D3D9_FMT_DXT5:
   case D3D9_FMT_ATI1:
      return true;
   default:
      return false;
   }
}

static bool
D3D9FormatsHaveSameMemoryLayout(D3DDDIFORMAT a, D3DDDIFORMAT b)
{
   if (a == b)
      return true;

   return (a == D3DDDIFMT_A8R8G8B8 && b == D3DDDIFMT_X8R8G8B8) ||
      (a == D3DDDIFMT_X8R8G8B8 && b == D3DDDIFMT_A8R8G8B8);
}

static UINT
D3D9FormatBlockSize(D3DDDIFORMAT format)
{
   switch (format) {
   case D3D9_FMT_DXT1:
   case D3D9_FMT_ATI1:
      return 8;
   case D3D9_FMT_DXT2:
   case D3D9_FMT_DXT3:
   case D3D9_FMT_DXT4:
   case D3D9_FMT_DXT5:
      return 16;
   default:
      return D3D9FormatBytesPerPixel(format);
   }
}

static UINT
D3D9FormatRowPitch(D3DDDIFORMAT format, UINT width)
{
   if (D3D9FormatIsBlockCompressed(format))
      return MAX2(1u, (width + 3u) / 4u) * D3D9FormatBlockSize(format);
   return width * D3D9FormatBytesPerPixel(format);
}

static UINT
D3D9AlignPitch(UINT pitch)
{
   return (pitch + 3u) & ~3u;
}

static UINT
D3D9FormatRowCount(D3DDDIFORMAT format, UINT height)
{
   if (D3D9FormatIsBlockCompressed(format))
      return MAX2(1u, (height + 3u) / 4u);
   if (format == D3D9_FMT_NV12)
      return height + (height + 1u) / 2u;
   return height ? height : 1;
}

/* moose-ripple: a host renderer without S3TC (UTM's, on Apple's GPUs through ANGLE) refuses
 * DXT textures, and the program's context with them. There they are kept as R8G8B8A8 and
 * decompressed on upload (D3D9TextureSubdata); the program still sees DXT. */
static bool d3d9_dxt_emulated;

void
D3D9InitFormatsForScreen(struct pipe_screen *screen)
{
   static const enum pipe_format dxt[] = {
      PIPE_FORMAT_DXT1_RGBA, PIPE_FORMAT_DXT3_RGBA, PIPE_FORMAT_DXT5_RGBA,
   };
   d3d9_dxt_emulated = false;
   for (unsigned i = 0; i < ARRAY_SIZE(dxt); ++i) {
      if (!screen->is_format_supported(screen, dxt[i], PIPE_TEXTURE_2D, 0, 0,
                                       PIPE_BIND_SAMPLER_VIEW))
         d3d9_dxt_emulated = true;
   }
   if (d3d9_dxt_emulated)
      ripple_logf("d3d9: the host renderer has no S3TC; DXT textures are decompressed on upload");
}

static enum pipe_format
D3D9DxtPipeFormat(D3DDDIFORMAT format)
{
   switch (format) {
   case D3D9_FMT_DXT1:
      return PIPE_FORMAT_DXT1_RGBA;
   case D3D9_FMT_DXT2:
   case D3D9_FMT_DXT3:
      return PIPE_FORMAT_DXT3_RGBA;
   case D3D9_FMT_DXT4:
   case D3D9_FMT_DXT5:
      return PIPE_FORMAT_DXT5_RGBA;
   default:
      return PIPE_FORMAT_NONE;
   }
}

static bool
D3D9FormatIsEmulatedDxt(D3DDDIFORMAT format)
{
   return d3d9_dxt_emulated && D3D9DxtPipeFormat(format) != PIPE_FORMAT_NONE;
}

static enum pipe_format
D3D9FormatToPipe(D3DDDIFORMAT format)
{
   if (D3D9FormatIsEmulatedDxt(format))
      return PIPE_FORMAT_R8G8B8A8_UNORM;
   switch (format) {
   case D3DDDIFMT_A8R8G8B8:
      return PIPE_FORMAT_B8G8R8A8_UNORM;
   case D3DDDIFMT_X8R8G8B8:
      return PIPE_FORMAT_B8G8R8X8_UNORM;
   case D3DDDIFMT_A8B8G8R8:
      return PIPE_FORMAT_R8G8B8A8_UNORM;
   case D3DDDIFMT_X8B8G8R8:
      return PIPE_FORMAT_R8G8B8X8_UNORM;
   case D3DDDIFMT_R5G6B5:
      return PIPE_FORMAT_B5G6R5_UNORM;
   case D3DDDIFMT_A1R5G5B5:
      return PIPE_FORMAT_B5G5R5A1_UNORM;
   /* moose-ripple: the three 16-bit formats the adapter also lists (D3D9Adapter.cpp) */
   case D3DDDIFMT_X1R5G5B5:
      return PIPE_FORMAT_B5G5R5X1_UNORM;
   case D3DDDIFMT_A4R4G4B4:
      return PIPE_FORMAT_B4G4R4A4_UNORM;
   case D3D9_FMT_X4R4G4B4:
      return PIPE_FORMAT_B4G4R4X4_UNORM;
   case D3DDDIFMT_A2R10G10B10:
      return PIPE_FORMAT_B10G10R10A2_UNORM;
   case D3D9_FMT_A2B10G10R10:
      return PIPE_FORMAT_R10G10B10A2_UNORM;
   case D3DDDIFMT_A8:
      return PIPE_FORMAT_A8_UNORM;
   case D3D9_FMT_R8:
      return PIPE_FORMAT_R8_UNORM;
   case D3D9_FMT_P8:
   case D3D9_FMT_L8:
      return PIPE_FORMAT_R8_UNORM;
   case D3D9_FMT_A8P8:
   case D3D9_FMT_A8L8:
   case D3D9_FMT_G8R8:
      return PIPE_FORMAT_R8G8_UNORM;
   case D3D9_FMT_L16:
      return PIPE_FORMAT_R16_UNORM;
   case D3D9_FMT_G16R16:
      return PIPE_FORMAT_R16G16_UNORM;
   case D3D9_FMT_A16B16G16R16:
      return PIPE_FORMAT_R16G16B16A16_UNORM;
   case D3D9_FMT_V8U8:
      return PIPE_FORMAT_R8G8_SNORM;
   case D3D9_FMT_Q8W8V8U8:
   case D3D9_FMT_L6V5U5:
   case D3D9_FMT_X8L8V8U8:
      return PIPE_FORMAT_R8G8B8A8_SNORM;
   case D3D9_FMT_V16U16:
      return PIPE_FORMAT_R16G16_SNORM;
   case D3D9_FMT_Q16W16V16U16:
      return PIPE_FORMAT_R16G16B16A16_SNORM;
   case D3D9_FMT_R16F:
      return PIPE_FORMAT_R16_FLOAT;
   case D3D9_FMT_G16R16F:
      return PIPE_FORMAT_R16G16_FLOAT;
   case D3D9_FMT_A16B16G16R16F:
      return PIPE_FORMAT_R16G16B16A16_FLOAT;
   case D3D9_FMT_R32F:
      return PIPE_FORMAT_R32_FLOAT;
   case D3D9_FMT_G32R32F:
      return PIPE_FORMAT_R32G32_FLOAT;
   case D3D9_FMT_A32B32G32R32F:
      return PIPE_FORMAT_R32G32B32A32_FLOAT;
   case D3DDDIFMT_D16:
   case D3D9_FMT_D16_LOCKABLE:
   case D3D9_FMT_DF16:
      return PIPE_FORMAT_Z16_UNORM;
   case D3D9_FMT_D32F_LOCKABLE:
      return PIPE_FORMAT_Z32_FLOAT;
   case D3DDDIFMT_D24S8:
      /* moose-ripple: whichever order of the two parts the renderer has (Format.h) */
      return FormatDepth24Stencil8();
   case D3DDDIFMT_D24X8:
   case D3D9_FMT_DF24:
   case D3D9_FMT_INTZ:
      return PIPE_FORMAT_Z24X8_UNORM;
   case D3D9_FMT_DXT1:
      return PIPE_FORMAT_DXT1_RGBA;
   case D3D9_FMT_DXT2:
   case D3D9_FMT_DXT3:
      return PIPE_FORMAT_DXT3_RGBA;
   case D3D9_FMT_DXT4:
   case D3D9_FMT_DXT5:
      return PIPE_FORMAT_DXT5_RGBA;
   case D3D9_FMT_ATI1:
      return PIPE_FORMAT_RGTC1_UNORM;
   default:
      return PIPE_FORMAT_NONE;
   }
}

static D3DDDIFORMAT
D3D9PipeToFormat(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      return D3DDDIFMT_A8R8G8B8;
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      return D3DDDIFMT_X8R8G8B8;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      return D3DDDIFMT_A8B8G8R8;
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      return D3DDDIFMT_X8B8G8R8;
   case PIPE_FORMAT_B5G6R5_UNORM:
      return D3DDDIFMT_R5G6B5;
   case PIPE_FORMAT_B5G5R5A1_UNORM:
      return D3DDDIFMT_A1R5G5B5;
   case PIPE_FORMAT_B4G4R4A4_UNORM:
      return D3DDDIFMT_A4R4G4B4;
   case PIPE_FORMAT_Z16_UNORM:
      return D3DDDIFMT_D16;
   case PIPE_FORMAT_Z32_FLOAT:
      return D3D9_FMT_D32F_LOCKABLE;
   case PIPE_FORMAT_Z24X8_UNORM:
      return D3DDDIFMT_D24X8;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
      return D3DDDIFMT_D24S8;
   default:
      return D3DDDIFMT_UNKNOWN;
   }
}

static D3DDDIFORMAT
D3D9VirglToFormat(ULONG format)
{
   switch (format) {
   case VIRGL_FORMAT_B8G8R8A8_UNORM:
      return D3DDDIFMT_A8R8G8B8;
   case VIRGL_FORMAT_B8G8R8X8_UNORM:
      return D3DDDIFMT_X8R8G8B8;
   case VIRGL_FORMAT_R8G8B8A8_UNORM:
      return D3DDDIFMT_A8B8G8R8;
   case VIRGL_FORMAT_R8G8B8X8_UNORM:
      return D3DDDIFMT_X8B8G8R8;
   case VIRGL_FORMAT_B5G6R5_UNORM:
      return D3DDDIFMT_R5G6B5;
   case VIRGL_FORMAT_B5G5R5A1_UNORM:
      return D3DDDIFMT_A1R5G5B5;
   case VIRGL_FORMAT_Z16_UNORM:
      return D3DDDIFMT_D16;
   case VIRGL_FORMAT_Z24X8_UNORM:
      return D3DDDIFMT_D24X8;
   case VIRGL_FORMAT_Z24_UNORM_S8_UINT:
      return D3DDDIFMT_D24S8;
   default:
      return D3DDDIFMT_UNKNOWN;
   }
}

static enum pipe_format
D3D9ResourcePipeFormat(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return PIPE_FORMAT_R8_UINT;
   return D3D9FormatToPipe(data->Format);
}

static const VIOGPU_CREATE_ALLOCATION_EXCHANGE *
D3D9OpenResourceAllocationPrivate(const D3DDDIARG_OPENRESOURCE *data)
{
   if (!data || !data->pOpenAllocationInfo || data->NumAllocations != 1 ||
       !data->pOpenAllocationInfo[0].pPrivateDriverData ||
       data->pOpenAllocationInfo[0].PrivateDriverDataSize <
          sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE))
      return NULL;

   return (const VIOGPU_CREATE_ALLOCATION_EXCHANGE *)
      data->pOpenAllocationInfo[0].pPrivateDriverData;
}

static bool
D3D9OpenResourceIsRuntimeStandardPrimary(const D3DDDIARG_OPENRESOURCE *data,
                                         const VIOGPU_CREATE_ALLOCATION_EXCHANGE **out_alloc)
{
   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *alloc =
      D3D9OpenResourceAllocationPrivate(data);
   if (!alloc)
      return false;

   if (out_alloc)
      *out_alloc = alloc;

   return data->Flags.Fullscreen &&
      (alloc->ResourceOptions.bind & VIRGL_BIND_DISPLAY_TARGET) != 0;
}

static bool
D3D9PoolNeedsReadback(D3DDDI_POOL pool);

static bool
D3D9ResourceIsColorTarget(const D3DDDIARG_CREATERESOURCE *data)
{
   return data->Flags.RenderTarget || data->Flags.Primary ||
      data->Flags.DiscardRenderTarget;
}

static bool
D3D9ResourceUsesDisplayTargetBind(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.Primary)
      return true;

   if (data->MultisampleType)
      return false;

   /* Windows requires a LINEAR allocation for presentation. A local resource
    * may render into a separate OPTIMAL image, copied at the handoff.
    * Legacy D3DSWAPEFFECT_DISCARD backbuffers
    * arrive as non-Texture render targets (or DiscardRenderTarget) and already
    * hit this.  D3D9Ex FLIPEX backbuffers instead arrive as Texture render
    * targets marked SharedResource (cross-process, for DWM); without
    * DISPLAY_TARGET they were built as non-scannable OPTIMAL device-local images
    * and the kernel flip presented black.  Treat a shared render target as a
    * display target too (mirroring the unconditional D3D10_DDI_BIND_PRESENT ->
    * PIPE_BIND_DISPLAY_TARGET on the DXGI side), while still excluding
    * cube/volume targets and leaving plain (non-shared) render-to-texture
    * surfaces untouched so they keep OPTIMAL tiling. */
   return data->Flags.DiscardRenderTarget ||
      (data->Flags.RenderTarget && !data->Flags.CubeMap &&
       !data->Flags.Volume &&
       (data->Flags.SharedResource || !data->Flags.Texture));
}

static bool
D3D9ResourceCanBeColorCopyTarget(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return false;

   switch (data->Format) {
   case D3DDDIFMT_A8R8G8B8:
   case D3DDDIFMT_X8R8G8B8:
   case D3DDDIFMT_A8B8G8R8:
   case D3DDDIFMT_X8B8G8R8:
   case D3DDDIFMT_R5G6B5:
   case D3DDDIFMT_A1R5G5B5:
   case D3DDDIFMT_X1R5G5B5:
   case D3DDDIFMT_A4R4G4B4:
   case D3D9_FMT_X4R4G4B4:
   case D3DDDIFMT_A2R10G10B10:
   case D3D9_FMT_A2B10G10R10:
   case D3D9_FMT_A16B16G16R16:
      return true;
   default:
      return false;
   }
}

/* DirectDraw presents straight from its own surfaces: a windowed primary
 * Blt reaches the driver as a Present whose source is the application's
 * off-screen plain surface (no RenderTarget or Texture flag).  Like a D3D9
 * swapchain back buffer, that source has to be a display target, or the
 * KMD present reads an image it cannot see and the window stays empty. */
static bool
D3D9ResourceIsDDrawPresentSource(const D3D9Device *device,
                                 const D3DDDIARG_CREATERESOURCE *data)
{
   return device->runtime_interface < 9 &&
      D3D9PoolNeedsReadback(data->Pool) && !data->MultisampleType &&
      !(data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume ||
        data->Flags.ZBuffer || data->Flags.VertexBuffer ||
        data->Flags.IndexBuffer) &&
      D3D9ResourceCanBeColorCopyTarget(data);
}

static unsigned
D3D9PipeBind(const D3DDDIARG_CREATERESOURCE *data)
{
   const enum pipe_format format = D3D9ResourcePipeFormat(data);
   const bool depth = util_format_is_depth_or_stencil(format);

   if (data->Flags.VertexBuffer)
      return PIPE_BIND_VERTEX_BUFFER;
   if (data->Flags.IndexBuffer)
      return PIPE_BIND_INDEX_BUFFER;
   if (depth) {
      unsigned bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SHARED;
      if (data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume)
         bind |= PIPE_BIND_SAMPLER_VIEW;
      return bind;
   }

   /* Private texture rendering and readback use Gallium images.  Requiring a
    * shared allocation here also forces the winsys to choose an externally
    * reproducible layout, even for mip chains that are never exported.
    * Preserve allocations for shared resources and presentation/capture
    * surfaces, including multisampled backbuffers. */
   const bool private_texture =
      (data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume) &&
      !data->Flags.SharedResource && !data->Flags.Primary &&
      !data->Flags.DiscardRenderTarget &&
      !D3D9ResourceUsesDisplayTargetBind(data);
   const unsigned shared_bind = private_texture ? 0 : PIPE_BIND_SHARED;
   unsigned bind = 0;
   if (D3D9ResourceIsColorTarget(data)) {
      bind |= PIPE_BIND_RENDER_TARGET | shared_bind;
      if (D3D9ResourceUsesDisplayTargetBind(data))
         bind |= PIPE_BIND_DISPLAY_TARGET;
   }
   if (data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume)
      bind |= PIPE_BIND_SAMPLER_VIEW;
   if (data->Flags.AutogenMipmap &&
       D3D9ResourceCanBeColorCopyTarget(data))
      bind |= PIPE_BIND_RENDER_TARGET | shared_bind;
   if (D3D9PoolNeedsReadback(data->Pool) &&
       D3D9ResourceCanBeColorCopyTarget(data))
      bind |= PIPE_BIND_RENDER_TARGET | shared_bind;
   return bind;
}

static UINT
D3D9PipeArraySize(const D3DDDIARG_CREATERESOURCE *data)
{
   if (!data || data->Flags.VertexBuffer || data->Flags.IndexBuffer ||
       data->Flags.Volume || data->Flags.Primary)
      return 1;

   if (data->Flags.CubeMap)
      return 6;

   const UINT mip_levels = data->MipLevels ? data->MipLevels : 1;
   if (data->SurfCount > mip_levels && data->SurfCount % mip_levels == 0)
      return data->SurfCount / mip_levels;

   return 1;
}

static enum pipe_texture_target
D3D9PipeTarget(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return PIPE_BUFFER;
   if (data->Flags.CubeMap)
      return PIPE_TEXTURE_CUBE;
   if (data->Flags.Volume)
      return PIPE_TEXTURE_3D;
   if (D3D9PipeArraySize(data) > 1)
      return PIPE_TEXTURE_2D_ARRAY;
   return PIPE_TEXTURE_2D;
}

static enum pipe_resource_usage
D3D9PipeUsage(const D3DDDIARG_CREATERESOURCE *data)
{
   switch (data->Pool) {
   case D3DDDIPOOL_SYSTEMMEM:
   case D3D9_POOL_STAGINGMEM:
      return PIPE_USAGE_STAGING;
   default:
      /* Dynamic draw buffers need replaceable storage across discard locks.
       * Queued draws must retain the bytes preceding each discard. */
      if (data->Flags.Dynamic &&
          (data->Flags.VertexBuffer || data->Flags.IndexBuffer))
         return PIPE_USAGE_DYNAMIC;
      return PIPE_USAGE_DEFAULT;
   }
}

static D3DKMT_HANDLE
D3D9GetPipeResourceAllocation(D3D9Device *device,
                              struct pipe_resource *pipe_resource)
{
   struct winsys_handle whandle;

   if (!device || !device->screen || !device->screen->resource_get_handle ||
       !pipe_resource)
      return 0;

   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_D3DKMT_ALLOC;
   if (!device->screen->resource_get_handle(device->screen, device->pipe,
                                            pipe_resource, &whandle, 0))
      return 0;

   return (D3DKMT_HANDLE)(uintptr_t)whandle.handle;
}

static D3DKMT_HANDLE
D3D9GetPipeAllocation(D3D9Device *device, D3D9Resource *resource)
{
   if (!resource)
      return 0;

   return D3D9GetPipeResourceAllocation(device, resource->display_resource ?
      resource->display_resource : resource->pipe_resource);
}

bool
D3D9UploadSubResource(D3D9Device *device, D3D9Resource *resource,
                      UINT subresource_index);

static bool
D3D9CopyTextureToCpuDestination(D3D9Device *device,
                                D3D9Resource *dst_resource,
                                D3D9SubResource *dst_sub,
                                UINT dst_subresource_index,
                                const RECT *dst_rect,
                                D3D9Resource *src_resource,
                                D3D9SubResource *src_sub,
                                UINT src_subresource_index,
                                const RECT *src_rect);

static bool
D3D9PoolNeedsReadback(D3DDDI_POOL pool)
{
   return pool != D3DDDIPOOL_SYSTEMMEM && pool != D3D9_POOL_STAGINGMEM;
}

static bool
D3D9EnsureCpuBackingSize(D3D9SubResource *sub, size_t size)
{
   if (!sub)
      return false;
   if (size < sub->size)
      size = sub->size;
   if (sub->data && sub->data_capacity >= size)
      return true;

   uint8_t *data = (uint8_t *)calloc(1, size ? size : 1);
   if (!data)
      return false;

   if (sub->data) {
      memcpy(data, sub->data, MIN2(sub->data_capacity, size));
      if (sub->owns_data)
         free(sub->data);
   }
   sub->data = data;
   sub->data_capacity = size;
   sub->owns_data = true;
   return true;
}

static bool
D3D9EnsureCpuBacking(D3D9SubResource *sub)
{
   return D3D9EnsureCpuBackingSize(sub, sub ? sub->size : 0);
}

static size_t
D3D9PackedMipChainSize(const D3D9Resource *resource)
{
   if (!resource || !resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume || !resource->mip_levels)
      return 0;

   size_t size = 0;
   const UINT count = MIN2(resource->mip_levels, resource->surf_count);
   for (UINT i = 0; i < count; ++i)
      size += resource->surfaces[i].size;
   return size;
}

static bool
D3D9LockCoversFullSubResource(const D3D9SubResource *sub,
                              const struct pipe_box *box)
{
   return sub && box && box->x == 0 && box->y == 0 && box->z == 0 &&
      box->width == (int)sub->width && box->height == (int)sub->height &&
      box->depth == (int)(sub->depth ? sub->depth : 1);
}

static bool
D3D9DistributePackedMipLock(D3D9Resource *resource)
{
   if (!resource || !resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume || resource->mip_levels <= 1 ||
       resource->surf_count < resource->mip_levels)
      return true;

   D3D9SubResource *base = &resource->surfaces[0];
   if (!base->data)
      return false;

   const size_t packed_size = D3D9PackedMipChainSize(resource);
   if (!packed_size || base->data_capacity < packed_size)
      return false;

   size_t offset = base->size;
   for (UINT i = 1; i < resource->mip_levels; ++i) {
      D3D9SubResource *sub = &resource->surfaces[i];
      if (!D3D9EnsureCpuBacking(sub))
         return false;

      memcpy(sub->data, base->data + offset, sub->size);
      sub->cpu_dirty = true;
      offset += sub->size;
   }

   return true;
}

static bool
D3D9HasContiguousMipData(const D3D9Resource *resource)
{
   if (!resource || !resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume || resource->mip_levels <= 1 ||
       resource->surf_count < resource->mip_levels ||
       !resource->surfaces[0].data)
      return false;

   const uint8_t *base = resource->surfaces[0].data;
   size_t offset = resource->surfaces[0].size;
   for (UINT i = 1; i < resource->mip_levels; ++i) {
      const D3D9SubResource *sub = &resource->surfaces[i];
      if (sub->data != base + offset)
         return false;
      offset += sub->size;
   }
   return true;
}

static void
D3D9MarkMipChainCpuDirty(D3D9Resource *resource)
{
   if (!resource)
      return;

   const UINT count = MIN2(resource->mip_levels, resource->surf_count);
   for (UINT i = 0; i < count; ++i) {
      if (resource->surfaces[i].data)
         resource->surfaces[i].cpu_dirty = true;
   }
}

static bool
D3D9UploadDirtyTextureSubResources(D3D9Device *device, D3D9Resource *resource)
{
   if (!resource)
      return false;

   for (UINT i = 0; i < resource->surf_count; ++i) {
      if (!D3D9UploadSubResource(device, resource, i))
         return false;
   }

   return true;
}

static bool
D3D9NotifyLockCoversFullMipChain(const D3D9Resource *resource,
                                 const D3DDDIARG_LOCK *data)
{
   return resource && data && data->Flags.NotifyOnly &&
      resource->pool == D3DDDIPOOL_SYSTEMMEM &&
      resource->flags.Texture && !resource->flags.CubeMap &&
      !resource->flags.Volume && resource->mip_levels > 1 &&
      !data->Flags.RangeValid && !data->Flags.AreaValid &&
      !data->Flags.BoxValid;
}

static UINT
D3D9ResolveNotifyLockSubResource(D3D9Resource *resource,
                                 const D3DDDIARG_LOCK *data)
{
   if (!resource || !data || !data->Flags.NotifyOnly ||
       data->Flags.ReadOnly || data->SubResourceIndex != 0 ||
       data->Flags.RangeValid || data->Flags.AreaValid ||
       data->Flags.BoxValid || resource->pool != D3DDDIPOOL_SYSTEMMEM ||
       !resource->flags.Texture || resource->mip_levels <= 1)
      return data ? data->SubResourceIndex : 0;

   /* The D3D9 runtime reports full-surface managed mip locks as subresource 0. */
   UINT subresource_index = resource->notify_lock_mip_cursor;
   if (subresource_index >= resource->mip_levels)
      subresource_index = 0;

   resource->notify_lock_mip_cursor = subresource_index + 1;
   if (resource->notify_lock_mip_cursor >= resource->mip_levels)
      resource->notify_lock_mip_cursor = 0;

   resource->notify_lock_subresource = subresource_index;
   resource->notify_lock_subresource_valid = true;
   return subresource_index;
}

static UINT
D3D9ResolveNotifyUnlockSubResource(D3D9Resource *resource,
                                   const D3DDDIARG_UNLOCK *data)
{
   if (!resource || !data || !resource->notify_lock_subresource_valid)
      return data ? data->SubResourceIndex : 0;

   resource->notify_lock_subresource_valid = false;
   return resource->notify_lock_subresource;
}

static RECT
D3D9FullRect(const D3D9SubResource *sub)
{
   RECT rect;

   rect.left = 0;
   rect.top = 0;
   rect.right = sub ? (LONG)sub->width : 0;
   rect.bottom = sub ? (LONG)sub->height : 0;
   return rect;
}

static RECT
D3D9ClampRect(const D3D9SubResource *sub, const RECT *rect)
{
   RECT clamped = rect ? *rect : D3D9FullRect(sub);

   if (!sub)
      return clamped;
   if (clamped.right <= clamped.left || clamped.bottom <= clamped.top)
      clamped = D3D9FullRect(sub);

   if (clamped.left < 0)
      clamped.left = 0;
   if (clamped.top < 0)
      clamped.top = 0;
   if (clamped.right > (LONG)sub->width)
      clamped.right = (LONG)sub->width;
   if (clamped.bottom > (LONG)sub->height)
      clamped.bottom = (LONG)sub->height;
   return clamped;
}

static bool
D3D9IntersectRect(RECT *rect, const RECT *bounds)
{
   if (!rect || !bounds || rect->right <= rect->left ||
       rect->bottom <= rect->top || bounds->right <= bounds->left ||
       bounds->bottom <= bounds->top)
      return false;

   if (rect->left < bounds->left)
      rect->left = bounds->left;
   if (rect->top < bounds->top)
      rect->top = bounds->top;
   if (rect->right > bounds->right)
      rect->right = bounds->right;
   if (rect->bottom > bounds->bottom)
      rect->bottom = bounds->bottom;

   return rect->right > rect->left && rect->bottom > rect->top;
}

static RECT
D3D9ClipRect(const D3D9SubResource *sub, const RECT *rect)
{
   RECT clipped = rect ? *rect : D3D9FullRect(sub);
   const RECT bounds = D3D9FullRect(sub);

   if (!D3D9IntersectRect(&clipped, &bounds))
      clipped.left = clipped.top = clipped.right = clipped.bottom = 0;
   return clipped;
}

static bool
D3D9ClearRectForSubResource(D3D9Device *device, const D3D9SubResource *sub,
                            const RECT *rect, RECT *clear_rect)
{
   if (!sub || !clear_rect)
      return false;

   *clear_rect = D3D9ClipRect(sub, rect);
   if (clear_rect->right <= clear_rect->left ||
       clear_rect->bottom <= clear_rect->top)
      return false;

   if (device->viewport.Width && device->viewport.Height) {
      RECT viewport;
      viewport.left = (LONG)device->viewport.X;
      viewport.top = (LONG)device->viewport.Y;
      viewport.right = viewport.left + (LONG)device->viewport.Width;
      viewport.bottom = viewport.top + (LONG)device->viewport.Height;
      if (!D3D9IntersectRect(clear_rect, &viewport))
         return false;
   }

   if (device->render_states[D3DDDIRS_SCISSORTESTENABLE] &&
       !D3D9IntersectRect(clear_rect, &device->scissor_rect))
      return false;

   return true;
}

static void
D3D9InitPipeSurface(D3D9Resource *resource, UINT subresource_index,
                    struct pipe_surface *surface)
{
   /* A description only: with this Mesa a surface to clear has to be created from it */
   memset(surface, 0, sizeof(*surface));
   pipe_resource_reference(&surface->texture, resource->pipe_resource);
   surface->format = resource->pipe_resource->format;
   surface->u.tex.level = D3D9PipeMipLevel(resource, subresource_index);
   surface->u.tex.first_layer = D3D9PipeLayer(resource, subresource_index);
   surface->u.tex.last_layer = surface->u.tex.first_layer;
}

static void
D3D9ReleasePipeSurface(struct pipe_surface *surface)
{
   if (!surface)
      return;

   pipe_resource_reference(&surface->texture, NULL);
}

static void
D3D9PipeBoxFromRect(const RECT *rect, struct pipe_box *box)
{
   memset(box, 0, sizeof(*box));
   box->x = rect->left;
   box->y = rect->top;
   box->z = 0;
   box->width = rect->right - rect->left;
   box->height = rect->bottom - rect->top;
   box->depth = 1;
}

static void
D3D9PipeBoxFromD3DBox(const D3DDDIBOX *src, struct pipe_box *box)
{
   memset(box, 0, sizeof(*box));
   box->x = src->Left;
   box->y = src->Top;
   box->z = src->Front;
   box->width = src->Right - src->Left;
   box->height = src->Bottom - src->Top;
   box->depth = src->Back - src->Front;
}

/* L6V5U5 and X8L8V8U8 are stored as R8G8B8A8_SNORM (D3D9FormatToPipe):
 * U and V keep their signed values, scaled to 8 bits, and the unsigned
 * luminance goes into the positive half of blue. Returns a tight buffer of
 * width x rows x depth texels, or NULL for other formats. */
static bool
D3D9FormatIsConvertedBump(D3DDDIFORMAT format)
{
   return format == D3D9_FMT_L6V5U5 || format == D3D9_FMT_X8L8V8U8;
}

static int8_t
D3D9Snorm5To8(unsigned v)
{
   int s = (int)(v & 31u);
   if (s >= 16)
      s -= 32;
   if (s < -15)
      s = -15;
   return (int8_t)((s * 127 + (s < 0 ? -7 : 7)) / 15);
}

static uint8_t *
D3D9ConvertBumpTexels(D3DDDIFORMAT format, const void *data, unsigned stride,
                      uintptr_t layer_stride, UINT width, UINT rows,
                      UINT depth)
{
   if (!D3D9FormatIsConvertedBump(format))
      return NULL;
   uint8_t *out = (uint8_t *)malloc((size_t)width * 4 * rows * depth);
   if (!out)
      return NULL;
   uint8_t *dst = out;
   for (UINT z = 0; z < depth; ++z) {
      for (UINT y = 0; y < rows; ++y) {
         const uint8_t *src = (const uint8_t *)data + (size_t)z * layer_stride +
                              (size_t)y * stride;
         for (UINT x = 0; x < width; ++x, dst += 4) {
            if (format == D3D9_FMT_X8L8V8U8) {
               const uint8_t *t = src + x * 4;
               dst[0] = t[0] == 0x80 ? 0x81 : t[0];
               dst[1] = t[1] == 0x80 ? 0x81 : t[1];
               dst[2] = (uint8_t)((t[2] * 127 + 127) / 255);
            } else {
               const unsigned t = src[x * 2] | (unsigned)src[x * 2 + 1] << 8;
               dst[0] = (uint8_t)D3D9Snorm5To8(t);
               dst[1] = (uint8_t)D3D9Snorm5To8(t >> 5);
               dst[2] = (uint8_t)((((t >> 10) & 63u) * 127 + 31) / 63);
            }
            dst[3] = 127;
         }
      }
   }
   return out;
}

static bool
D3D9TextureSubdata(D3D9Device *device, D3D9Resource *resource,
                   UINT subresource_index, unsigned usage,
                   const struct pipe_box *box, const void *data,
                   unsigned stride, uintptr_t layer_stride)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !resource || !resource->pipe_resource || !box || !data ||
       resource->pipe_resource->target == PIPE_BUFFER)
      return false;

   const UINT row_bytes = D3D9FormatRowPitch(resource->format, box->width);
   const UINT rows = D3D9FormatRowCount(resource->format, box->height);
   const uintptr_t tight_layer_stride = (uintptr_t)row_bytes * rows;
   const UINT depth = box->depth ? (UINT)box->depth : 1;
   if (!row_bytes || !rows || !tight_layer_stride)
      return false;

   if (stride < row_bytes || layer_stride < tight_layer_stride)
      return false;

   const void *upload_data = data;
   unsigned upload_stride = stride;
   uintptr_t upload_layer_stride = layer_stride;
   uint8_t *tight = NULL;
   /* Gallium's 2D copy accepts the original row pitch as a signed int.
    * Window bitmaps often describe a rectangle inside a wider surface. */
   const bool pitched_bitmap = depth == 1 && stride <= INT_MAX &&
      (resource->format == D3DDDIFMT_A8R8G8B8 || resource->format == D3DDDIFMT_X8R8G8B8);

   if (D3D9FormatIsConvertedBump(resource->format)) {
      tight = D3D9ConvertBumpTexels(resource->format, data, stride,
                                    layer_stride, box->width, rows, depth);
      if (!tight)
         return false;
      upload_data = tight;
      upload_stride = box->width * 4;
      upload_layer_stride = (uintptr_t)upload_stride * rows;
   } else if (D3D9FormatIsEmulatedDxt(resource->format)) {
      /* moose-ripple: blocks to R8G8B8A8 texels (d3d9_dxt_emulated); a level smaller than a
       * block takes the part of the block it covers */
      const UINT height = box->height ? (UINT)box->height : 1;
      upload_stride = box->width * 4;
      upload_layer_stride = (uintptr_t)upload_stride * height;
      tight = (uint8_t *)malloc((size_t)upload_layer_stride * depth);
      if (!tight)
         return false;
      const enum d3d9_dxt_kind kind =
         resource->format == D3D9_FMT_DXT1 ? D3D9_DXT_KIND_1 :
         resource->format == D3D9_FMT_DXT2 || resource->format == D3D9_FMT_DXT3 ? D3D9_DXT_KIND_3 :
                                                                                   D3D9_DXT_KIND_5;
      for (UINT z = 0; z < depth; ++z)
         d3d9_dxt_decode(kind, tight + (size_t)z * upload_layer_stride, upload_stride,
                         (const uint8_t *)data + (size_t)z * layer_stride, stride, box->width,
                         height);
      upload_data = tight;
   } else if (!pitched_bitmap && (stride != row_bytes || layer_stride != tight_layer_stride)) {
      if (depth > SIZE_MAX / tight_layer_stride)
         return false;
      tight = (uint8_t *)malloc((size_t)tight_layer_stride * depth);
      if (!tight)
         return false;

      const uint8_t *src = (const uint8_t *)data;
      for (UINT z = 0; z < depth; ++z) {
         const uint8_t *src_slice = src + (size_t)z * layer_stride;
         uint8_t *dst_slice = tight + (size_t)z * tight_layer_stride;
         for (UINT y = 0; y < rows; ++y)
            memcpy(dst_slice + (size_t)y * row_bytes,
                   src_slice + (size_t)y * stride, row_bytes);
      }

      upload_data = tight;
      upload_stride = row_bytes;
      upload_layer_stride = tight_layer_stride;
   }

   /* The three steps of Gallium's default texture_subdata, so that the timing report can tell
    * them apart: under processor emulation the copy pays for every page of memory that is new. */
   struct pipe_context *pipe = device->pipe;
   struct pipe_transfer *transfer = NULL;
   const bool timed = D3D9TimesOn();
   unsigned long long start = timed ? D3D9TimesNow() : 0;
   void *map = pipe->texture_map(pipe, resource->pipe_resource,
                                 D3D9PipeMipLevel(resource, subresource_index),
                                 usage | PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE, box, &transfer);
   if (!map) {
      free(tight);
      return false;
   }
   if (timed) {
      D3D9TimesCallback(D3D9_TIMED_UPLOAD_MAP, start);
      start = D3D9TimesNow();
   }
   util_copy_box((uint8_t *)map, resource->pipe_resource->format, transfer->stride,
                 transfer->layer_stride, 0, 0, 0, box->width, box->height, box->depth,
                 (const uint8_t *)upload_data, upload_stride, upload_layer_stride, 0, 0, 0);
   if (timed) {
      D3D9TimesCallback(D3D9_TIMED_UPLOAD_COPY, start);
      start = D3D9TimesNow();
   }
   pipe->texture_unmap(pipe, transfer);
   if (timed)
      D3D9TimesCallback(D3D9_TIMED_UPLOAD_UNMAP, start);
   free(tight);
   return true;
}

static void
D3D9ColorToPipe(D3DCOLOR color, union pipe_color_union *pipe_color,
                bool srgb_write)
{
   const float red = ((color >> 16) & 0xff) / 255.0f;
   const float green = ((color >> 8) & 0xff) / 255.0f;
   const float blue = (color & 0xff) / 255.0f;

   if (srgb_write) {
      pipe_color->f[0] = util_format_linear_float_to_srgb_8unorm(red) /
         255.0f;
      pipe_color->f[1] = util_format_linear_float_to_srgb_8unorm(green) /
         255.0f;
      pipe_color->f[2] = util_format_linear_float_to_srgb_8unorm(blue) /
         255.0f;
   } else {
      pipe_color->f[0] = red;
      pipe_color->f[1] = green;
      pipe_color->f[2] = blue;
   }
   pipe_color->f[3] = ((color >> 24) & 0xff) / 255.0f;
}

static bool
D3D9IsYttriumScreen(struct pipe_screen *screen)
{
   if (!screen || !screen->get_name)
      return false;

   const char *name = screen->get_name(screen);
   return name && strcmp(name, "yttrium") == 0;
}

static bool
D3D9ClearColorRectUpload(struct pipe_context *pipe,
                         struct pipe_surface *surface,
                         const union pipe_color_union *clear_color,
                         unsigned x, unsigned y,
                         unsigned width, unsigned height)
{
   if (!pipe || !pipe->texture_subdata || !surface || !surface->texture ||
       !clear_color || !width || !height)
      return false;

   const enum pipe_format format = surface->format;
   const struct util_format_pack_description *pack =
      util_format_pack_description(format);
   if (!pack)
      return false;

   const bool pure_uint = util_format_is_pure_uint(format);
   const bool pure_sint = util_format_is_pure_sint(format);
   if ((pure_uint && !pack->pack_rgba_uint) ||
       (pure_sint && !pack->pack_rgba_sint) ||
       (!pure_uint && !pure_sint && !pack->pack_rgba_float))
      return false;

   const unsigned stride = util_format_get_stride(format, width);
   const uintptr_t layer_stride =
      (uintptr_t)util_format_get_2d_size(format, stride, height);
   const unsigned first_layer = surface->u.tex.first_layer;
   const unsigned layer_count =
      surface->u.tex.last_layer >= surface->u.tex.first_layer ?
      surface->u.tex.last_layer - surface->u.tex.first_layer + 1 : 1;

   if (!stride || !layer_stride || layer_stride > SIZE_MAX / layer_count)
      return false;

   uint8_t *data = (uint8_t *)malloc((size_t)layer_stride * layer_count);
   if (!data)
      return false;

   uint8_t *row = (uint8_t *)malloc(stride);
   void *src = NULL;
   if (!row)
      goto fail;

   if (pure_uint) {
      uint32_t *rgba = (uint32_t *)malloc(width * 4 * sizeof(*rgba));
      if (!rgba)
         goto fail;
      for (unsigned i = 0; i < width; ++i) {
         rgba[i * 4 + 0] = clear_color->ui[0];
         rgba[i * 4 + 1] = clear_color->ui[1];
         rgba[i * 4 + 2] = clear_color->ui[2];
         rgba[i * 4 + 3] = clear_color->ui[3];
      }
      pack->pack_rgba_uint(row, 0, rgba, 0, width, 1);
      src = rgba;
   } else if (pure_sint) {
      int32_t *rgba = (int32_t *)malloc(width * 4 * sizeof(*rgba));
      if (!rgba)
         goto fail;
      for (unsigned i = 0; i < width; ++i) {
         rgba[i * 4 + 0] = clear_color->i[0];
         rgba[i * 4 + 1] = clear_color->i[1];
         rgba[i * 4 + 2] = clear_color->i[2];
         rgba[i * 4 + 3] = clear_color->i[3];
      }
      pack->pack_rgba_sint(row, 0, rgba, 0, width, 1);
      src = rgba;
   } else {
      float *rgba = (float *)malloc(width * 4 * sizeof(*rgba));
      if (!rgba)
         goto fail;
      for (unsigned i = 0; i < width; ++i) {
         rgba[i * 4 + 0] = clear_color->f[0];
         rgba[i * 4 + 1] = clear_color->f[1];
         rgba[i * 4 + 2] = clear_color->f[2];
         rgba[i * 4 + 3] = clear_color->f[3];
      }
      pack->pack_rgba_float(row, 0, rgba, 0, width, 1);
      src = rgba;
   }

   for (unsigned layer = 0; layer < layer_count; ++layer) {
      uint8_t *layer_data = data + (size_t)layer_stride * layer;
      for (unsigned row_index = 0; row_index < height; ++row_index)
         memcpy(layer_data + (size_t)stride * row_index, row, stride);
   }

   struct pipe_box box;
   box.x = x;
   box.y = y;
   box.z = first_layer;
   box.width = width;
   box.height = height;
   box.depth = layer_count;
   pipe->texture_subdata(pipe, surface->texture, surface->u.tex.level, 0, &box,
                         data, stride, layer_stride);

   free(src);
   free(row);
   free(data);
   return true;

fail:
   free(src);
   free(row);
   free(data);
   return false;
}

static unsigned
D3D9PipeMapUsage(D3DDDI_LOCKFLAGS flags)
{
   unsigned usage = 0;

   if (flags.ReadOnly)
      usage |= PIPE_MAP_READ;
   else if (flags.WriteOnly)
      usage |= PIPE_MAP_WRITE;
   else
      usage |= PIPE_MAP_READ | PIPE_MAP_WRITE;

   if (flags.Discard)
      usage |= PIPE_MAP_DISCARD_RANGE;
   if (flags.NoOverwrite)
      usage |= PIPE_MAP_UNSYNCHRONIZED;

   return usage;
}

#ifdef RIPPLE_VISTA
static bool
D3D9ClearPrimaryRect(struct pipe_context *pipe, const struct pipe_surface *primary,
                     const union pipe_color_union *color, const RECT &rect)
{
   /* A display-segment allocation need not have a writable virgl backing.
    * Clear a rectangle-sized render target and copy its pixels on the GPU.
    * This also works on hosts whose render-target clear covers a whole view. */
   struct pipe_resource templ = {};
   templ.target = PIPE_TEXTURE_2D;
   templ.format = primary->format;
   templ.width0 = rect.right - rect.left;
   templ.height0 = rect.bottom - rect.top;
   templ.depth0 = templ.array_size = 1;
   templ.nr_samples = primary->texture->nr_samples;
   templ.nr_storage_samples = primary->texture->nr_storage_samples;
   templ.bind = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *tile = pipe->screen->resource_create(pipe->screen, &templ);
   if (!tile)
      return false;
   struct pipe_surface view = {};
   view.format = tile->format;
   struct pipe_surface *target = pipe->create_surface(pipe, tile, &view);
   const bool ready = target != NULL;
   if (ready) {
      pipe->clear_render_target(pipe, target, color, 0, 0, templ.width0, templ.height0, true);
      struct pipe_box box;
      u_box_2d(0, 0, templ.width0, templ.height0, &box);
      pipe->resource_copy_region(pipe, primary->texture, primary->u.tex.level,
                                 rect.left, rect.top, primary->u.tex.first_layer, tile, 0, &box);
      pipe_surface_reference(&target, NULL);
   }
   pipe_resource_reference(&tile, NULL);
   return ready;
}
#endif

static bool
D3D9ClearColorPipe(D3D9Device *device, HANDLE resource_handle,
                   UINT subresource_index, const RECT *rect,
                   D3DCOLOR color, bool srgb_write, bool prefer_upload)
{
   if (!device || !device->pipe || !device->pipe->clear_render_target)
      return false;

   D3D9Resource *resource = D3D9CastResource(resource_handle);
   D3D9SubResource *sub = D3D9GetSubResource(resource_handle,
                                             subresource_index);
   if (!resource || !resource->pipe_resource || !sub)
      return false;

   const RECT dst = D3D9ClipRect(sub, rect);
   if (dst.right <= dst.left || dst.bottom <= dst.top)
      return true;

   struct pipe_surface surface;
   union pipe_color_union clear_color;

   D3D9InitPipeSurface(resource, subresource_index, &surface);
   D3D9ColorToPipe(color, &clear_color, srgb_write);
   const unsigned width = (unsigned)(dst.right - dst.left);
   const unsigned height = (unsigned)(dst.bottom - dst.top);
   const bool full_clear = dst.left == 0 && dst.top == 0 &&
      width == sub->width && height == sub->height;
   if (!full_clear && sub->cpu_dirty &&
       !D3D9UploadSubResource(device, resource, subresource_index)) {
      D3D9ReleasePipeSurface(&surface);
      return false;
   }
   const bool yttrium = D3D9IsYttriumScreen(device->pipe->screen);
   const bool upload_full_clear = prefer_upload && yttrium &&
      !(resource->pipe_resource->bind & PIPE_BIND_DISPLAY_TARGET);
   const bool needs_upload = !full_clear ||
      upload_full_clear ||
      (yttrium && D3D9PipeMipLevel(resource, subresource_index) != 0);

   bool cleared = false;
#ifdef RIPPLE_VISTA
   if (!full_clear && resource->flags.Primary && !yttrium) {
      if (!D3D9ClearPrimaryRect(device->pipe, &surface, &clear_color, dst)) {
         D3D9ReleasePipeSurface(&surface);
         return false;
      }
      cleared = true;
   } else
#endif
   if (needs_upload)
      cleared = D3D9ClearColorRectUpload(device->pipe, &surface, &clear_color,
                                         (unsigned)dst.left, (unsigned)dst.top, width, height);
   if (cleared) {
      D3D9ReleasePipeSurface(&surface);
      sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
      return true;
   }

   if (needs_upload && yttrium) {
      static volatile LONG logged;
      D3D9WarnOncef(&logged, "unsupported color clear upload format=%u "
                    "sub=%u rect=%ld,%ld-%ld,%ld\n",
                    surface.format, subresource_index, dst.left, dst.top,
                    dst.right, dst.bottom);
      D3D9ReleasePipeSurface(&surface);
      return false;
   }

   struct pipe_surface *target =
      device->pipe->create_surface(device->pipe, surface.texture, &surface);
   if (target) {
      device->pipe->clear_render_target(device->pipe, target, &clear_color,
                                        (unsigned)dst.left, (unsigned)dst.top,
                                        width, height, true);
      pipe_surface_reference(&target, NULL);
   }
   D3D9ReleasePipeSurface(&surface);
   sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
   return true;
}

static bool
D3D9ClearDepthPipe(D3D9Device *device, HANDLE resource_handle,
                   UINT subresource_index, const RECT *rect,
                   bool clear_depth, float depth,
                   bool clear_stencil, UINT stencil)
{
   if (!device || !device->pipe || !device->pipe->clear_depth_stencil)
      return false;

   D3D9Resource *resource = D3D9CastResource(resource_handle);
   D3D9SubResource *sub = D3D9GetSubResource(resource_handle,
                                             subresource_index);
   if (!resource || !resource->pipe_resource || !sub)
      return false;

   const RECT dst = D3D9ClipRect(sub, rect);
   if (dst.right <= dst.left || dst.bottom <= dst.top)
      return true;

   struct pipe_surface surface;
   unsigned clear_flags = 0;

   if (clear_depth)
      clear_flags |= PIPE_CLEAR_DEPTH;
   if (clear_stencil)
      clear_flags |= PIPE_CLEAR_STENCIL;

   D3D9InitPipeSurface(resource, subresource_index, &surface);

   const unsigned width = (unsigned)(dst.right - dst.left);
   const unsigned height = (unsigned)(dst.bottom - dst.top);
   struct pipe_surface *target =
      device->pipe->create_surface(device->pipe, surface.texture, &surface);
   if (target) {
      device->pipe->clear_depth_stencil(device->pipe, target, clear_flags,
                                        depth, stencil,
                                        (unsigned)dst.left, (unsigned)dst.top,
                                        width, height, true);
      pipe_surface_reference(&target, NULL);
   }
   D3D9ReleasePipeSurface(&surface);
   sub->cpu_dirty = false;
   return true;
}

static void
D3D9ReleaseWatchedBuffer(D3D9Resource *resource)
{
   resource->may_watch_buffer = false;
   if (resource->watched_buffer) {
      VirtualFree(resource->watched_buffer, 0, MEM_RELEASE);
      resource->watched_buffer = NULL;
   }
}

static HRESULT
D3D9CopyBuffer(D3D9Device *device, HANDLE dst_resource_handle,
               HANDLE src_resource_handle, UINT dst_offset,
               UINT src_offset, UINT size)
{
   D3D9Resource *dst_resource = D3D9CastResource(dst_resource_handle);
   D3D9Resource *src_resource = D3D9CastResource(src_resource_handle);
   if (!device || !dst_resource || !src_resource ||
       !dst_resource->surf_count || !src_resource->surf_count)
      return E_INVALIDARG;

   D3D9SubResource *dst = &dst_resource->surfaces[0];
   D3D9SubResource *src = &src_resource->surfaces[0];
   if (dst_offset > dst->size || src_offset > src->size ||
       size > dst->size - dst_offset || size > src->size - src_offset)
      return E_INVALIDARG;
   if (!size)
      return S_OK;

   if (dst_resource->pipe_resource && src_resource->pipe_resource &&
       dst_resource->pipe_resource->target == PIPE_BUFFER &&
       src_resource->pipe_resource->target == PIPE_BUFFER &&
       device->pipe && device->pipe->resource_copy_region) {
      if (src->cpu_dirty &&
          !D3D9UploadSubResource(device, src_resource, 0))
         return E_NOTIMPL;

      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.x = src_offset;
      box.width = size;
      box.height = 1;
      box.depth = 1;
      device->pipe->resource_copy_region(device->pipe,
                                         dst_resource->pipe_resource, 0,
                                         dst_offset, 0, 0,
                                         src_resource->pipe_resource, 0,
                                         &box);
      /* Future locks must see the GPU copy, including untouched bytes. */
      D3D9ReleaseWatchedBuffer(dst_resource);
      dst->cpu_dirty = false;
      return S_OK;
   }

   D3D9Warnf("unsupported buffer blit without pipe copy dst=%p src=%p\n",
             dst_resource, src_resource);
   return E_NOTIMPL;
}

static bool
D3D9CopyCpuSourceToTexture(D3D9Device *device, D3D9Resource *dst_resource,
                           D3D9SubResource *dst_sub,
                           UINT dst_subresource_index,
                           const RECT *dst_rect,
                           D3D9Resource *src_resource,
                           D3D9SubResource *src_sub,
                           const RECT *src_rect)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !dst_resource || !dst_sub || !src_resource || !src_sub ||
       !dst_resource->pipe_resource || !src_sub->data ||
       dst_resource->pipe_resource->target == PIPE_BUFFER ||
       dst_resource->format != src_resource->format)
      return false;

   if (src_resource->pipe_resource && !src_sub->cpu_dirty &&
       D3D9PoolNeedsReadback(src_resource->pool))
      return false;

   const UINT bpp = D3D9FormatBytesPerPixel(src_resource->format);
   if (!bpp)
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG dst_width = dst.right - dst.left;
   const LONG dst_height = dst.bottom - dst.top;
   const LONG src_width = src.right - src.left;
   const LONG src_height = src.bottom - src.top;
   if (dst_width <= 0 || dst_height <= 0 ||
       src_width <= 0 || src_height <= 0)
      return true;

   /* texture_subdata is a direct upload, not a stretch operation.  Let the
    * upload-plus-blit path below handle differently sized rectangles instead
    * of silently cropping them to the smaller extent.
    */
   if (dst_width != src_width || dst_height != src_height)
      return false;

   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.x = dst.left;
   box.y = dst.top;
   box.z = D3D9PipeLayer(dst_resource, dst_subresource_index);
   box.width = dst_width;
   box.height = dst_height;
   box.depth = 1;

   const uint8_t *src_data = src_sub->data +
      (size_t)src.top * src_sub->pitch + (size_t)src.left * bpp;
   if (!D3D9TextureSubdata(device, dst_resource, dst_subresource_index,
                           PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                           &box, src_data, src_sub->pitch,
                           src_sub->slice_pitch))
      return false;
   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

static void
D3D9InitBlitInfo(struct pipe_blit_info *info,
                 struct pipe_resource *dst_resource, UINT dst_level,
                 UINT dst_layer,
                 enum pipe_format dst_format, const RECT *dst_rect,
                 struct pipe_resource *src_resource, UINT src_level,
                 UINT src_layer,
                 enum pipe_format src_format, const RECT *src_rect)
{
   memset(info, 0, sizeof(*info));

   info->dst.resource = dst_resource;
   info->dst.level = dst_level;
   info->dst.format = dst_format;
   info->dst.box.x = dst_rect->left;
   info->dst.box.y = dst_rect->top;
   info->dst.box.z = dst_layer;
   info->dst.box.width = dst_rect->right - dst_rect->left;
   info->dst.box.height = dst_rect->bottom - dst_rect->top;
   info->dst.box.depth = 1;
   info->src.resource = src_resource;
   info->src.level = src_level;
   info->src.format = src_format;
   info->src.box.x = src_rect->left;
   info->src.box.y = src_rect->top;
   info->src.box.z = src_layer;
   info->src.box.width = src_rect->right - src_rect->left;
   info->src.box.height = src_rect->bottom - src_rect->top;
   info->src.box.depth = 1;
   info->mask = util_format_get_mask(dst_format) &
                util_format_get_mask(src_format);
   info->filter = PIPE_TEX_FILTER_NEAREST;
}

static bool D3D9BlitVideoSurface(D3D9Device *device, D3D9Resource *dst_resource,
                                D3D9SubResource *dst_sub, UINT dst_index, const RECT *dst,
                                D3D9Resource *src_resource, D3D9SubResource *src_sub,
                                const RECT *src, enum pipe_tex_filter filter);

static bool
D3D9BlitCpuOnlySourceToTexture(D3D9Device *device,
                               D3D9Resource *dst_resource,
                               D3D9SubResource *dst_sub,
                               UINT dst_subresource_index,
                               const RECT *dst_rect,
                               D3D9Resource *src_resource,
                               D3D9SubResource *src_sub,
                               const RECT *src_rect,
                               enum pipe_tex_filter filter)
{
   if (!device || !device->screen || !device->pipe ||
       !device->pipe->texture_subdata || !device->pipe->blit ||
       !dst_resource || !dst_sub || !dst_resource->pipe_resource ||
       dst_resource->pipe_resource->target == PIPE_BUFFER ||
       !src_resource || src_resource->pipe_resource || !src_sub ||
       !src_sub->data)
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG dst_width = dst.right - dst.left;
   const LONG dst_height = dst.bottom - dst.top;
   const LONG src_width = src.right - src.left;
   const LONG src_height = src.bottom - src.top;
   if (dst_width <= 0 || dst_height <= 0 ||
       src_width <= 0 || src_height <= 0)
      return true;

   if (D3D_UMD_INTERFACE_VERSION < D3D_UMD_INTERFACE_VERSION_WIN7 &&
       D3D9IsVideoFormat(src_resource->format))
      return D3D9BlitVideoSurface(device, dst_resource, dst_sub, dst_subresource_index,
                                 &dst, src_resource, src_sub, &src, filter);

   const enum pipe_format src_format =
      D3D9FormatToPipe(src_resource->format);
   const UINT bpp = D3D9FormatBytesPerPixel(src_resource->format);
   const enum pipe_format dst_format =
      dst_resource->pipe_resource->format;
   if (src_format == PIPE_FORMAT_NONE || !bpp ||
       D3D9FormatIsBlockCompressed(src_resource->format) ||
       (src_format != dst_format &&
        !(D3D9IsRgb32(src_resource->format) && D3D9IsRgb32(dst_resource->format))) ||
       util_format_is_depth_or_stencil(src_format) ||
       util_format_is_depth_or_stencil(dst_format) ||
       dst_resource->pipe_resource->nr_samples > 1)
      return false;

   const size_t max_size = ~(size_t)0;
   const size_t width = (size_t)src_width;
   const size_t height = (size_t)src_height;
   const size_t bytes_per_pixel = (size_t)bpp;
   const size_t pitch = (size_t)src_sub->pitch;
   if (width > max_size / bytes_per_pixel)
      return false;

   const size_t row_bytes = width * bytes_per_pixel;
   if (!pitch || row_bytes > pitch ||
       (size_t)src.top > max_size / pitch)
      return false;

   size_t data_offset = (size_t)src.top * pitch;
   if ((size_t)src.left > (max_size - data_offset) / bytes_per_pixel)
      return false;
   data_offset += (size_t)src.left * bytes_per_pixel;

   if (data_offset > max_size - row_bytes)
      return false;
   const size_t remaining = max_size - data_offset - row_bytes;
   if (height - 1 > remaining / pitch)
      return false;

   const size_t required = data_offset + (height - 1) * pitch + row_bytes;
   if (required > src_sub->data_capacity)
      return false;

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = src_format;
   templ.width0 = src_width;
   templ.height0 = src_height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.bind = PIPE_BIND_SAMPLER_VIEW;
   templ.usage = PIPE_USAGE_DEFAULT;

   struct pipe_resource *upload =
      device->screen->resource_create(device->screen, &templ);
   if (!upload)
      return false;

   struct pipe_box upload_box;
   memset(&upload_box, 0, sizeof(upload_box));
   upload_box.width = src_width;
   upload_box.height = src_height;
   upload_box.depth = 1;
   D3D9Resource upload_resource;
   memset(&upload_resource, 0, sizeof(upload_resource));
   upload_resource.format = src_resource->format;
   upload_resource.mip_levels = 1;
   upload_resource.pipe_resource = upload;
   const uint8_t *src_data = src_sub->data + data_offset;
   if (!D3D9TextureSubdata(device, &upload_resource, 0,
                           PIPE_MAP_WRITE |
                              PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                           &upload_box, src_data, src_sub->pitch,
                           src_sub->slice_pitch)) {
      pipe_resource_reference(&upload, NULL);
      return false;
   }

   RECT upload_rect;
   upload_rect.left = 0;
   upload_rect.top = 0;
   upload_rect.right = src_width;
   upload_rect.bottom = src_height;
   struct pipe_blit_info info;
   D3D9InitBlitInfo(&info, dst_resource->pipe_resource,
                    D3D9PipeMipLevel(dst_resource,
                                     dst_subresource_index),
                    D3D9PipeLayer(dst_resource,
                                  dst_subresource_index),
                    dst_format, &dst,
                    upload, 0, 0, src_format, &upload_rect);
   info.filter = filter;
   device->pipe->blit(device->pipe, &info);
   pipe_resource_reference(&upload, NULL);

   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

/* A multisampled colour surface copied to a plain one of another format or another size:
 * resolved first into a surface of its own size and format, then converted or stretched
 * from there. moose-ripple: the stretch too. Asked to resolve and stretch in one blit, the
 * host took one sample of each pixel, and the smoothing was gone. */
static bool
D3D9ResolveThenConvertPipe(D3D9Device *device, D3D9Resource *dst_resource,
                           D3D9SubResource *dst_sub,
                           UINT dst_subresource_index, const RECT *dst,
                           D3D9Resource *src_resource,
                           UINT src_subresource_index, const RECT *src,
                           unsigned filter)
{
   if (!device || !device->screen || !device->pipe || !device->pipe->blit ||
       !dst_resource || !dst_sub || !src_resource ||
       !dst_resource->pipe_resource || !src_resource->pipe_resource)
      return false;

   struct pipe_resource *dst_pipe = dst_resource->pipe_resource;
   struct pipe_resource *src_pipe = src_resource->pipe_resource;
   if (src_pipe->nr_samples <= 1 || dst_pipe->nr_samples > 1 ||
       src_pipe->target == PIPE_BUFFER || dst_pipe->target == PIPE_BUFFER ||
       util_format_is_depth_or_stencil(src_pipe->format))
      return false;

   const LONG dst_width = dst->right - dst->left;
   const LONG dst_height = dst->bottom - dst->top;
   const LONG src_width = src->right - src->left;
   const LONG src_height = src->bottom - src->top;
   if (dst_width <= 0 || dst_height <= 0 || src_width <= 0 || src_height <= 0)
      return false;
   const bool stretched = dst_width != src_width || dst_height != src_height;
   /* the same size and format: a plain resolve, which the caller's own blit does */
   if (!stretched && src_pipe->format == dst_pipe->format)
      return false;

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = src_pipe->format;
   templ.width0 = src_width;
   templ.height0 = src_height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   templ.usage = PIPE_USAGE_DEFAULT;

   struct pipe_resource *resolved =
      device->screen->resource_create(device->screen, &templ);
   if (!resolved)
      return false;

   RECT resolved_rect;
   resolved_rect.left = 0;
   resolved_rect.top = 0;
   resolved_rect.right = src_width;
   resolved_rect.bottom = src_height;

   struct pipe_blit_info info;
   D3D9InitBlitInfo(&info, resolved, 0, 0, resolved->format, &resolved_rect,
                    src_pipe, D3D9PipeMipLevel(src_resource,
                                               src_subresource_index),
                    D3D9PipeLayer(src_resource, src_subresource_index),
                    src_pipe->format, src);
   device->pipe->blit(device->pipe, &info);

   D3D9InitBlitInfo(&info, dst_pipe,
                    D3D9PipeMipLevel(dst_resource, dst_subresource_index),
                    D3D9PipeLayer(dst_resource, dst_subresource_index),
                    dst_pipe->format, dst, resolved, 0, 0,
                    resolved->format, &resolved_rect);
   if (stretched)
      info.filter = filter;
   device->pipe->blit(device->pipe, &info);

   pipe_resource_reference(&resolved, NULL);
   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

/* Both surfaces in memory (a plain surface updated from a system-memory one, or copied to
 * another plain one): the rows are copied. Direct3D 9 allows no stretching between them. */
static bool
D3D9CopyInMemory(D3D9Resource *dst_resource, D3D9SubResource *dst_sub,
                 const RECT *dst_rect, D3D9Resource *src_resource,
                 D3D9SubResource *src_sub, const RECT *src_rect)
{
   if (dst_resource->pipe_resource || src_resource->pipe_resource ||
       !src_sub->data || D3D9PoolNeedsReadback(dst_resource->pool) ||
       dst_sub->cpu_locked || dst_sub->transfer)
      return false;
   const UINT bpp = D3D9FormatBytesPerPixel(src_resource->format);
   if (!bpp || bpp != D3D9FormatBytesPerPixel(dst_resource->format) ||
       D3D9FormatIsBlockCompressed(src_resource->format) ||
       D3D9FormatIsBlockCompressed(dst_resource->format))
      return false;
   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG width = dst.right - dst.left;
   const LONG height = dst.bottom - dst.top;
   if (width != src.right - src.left || height != src.bottom - src.top)
      return false;
   if (width <= 0 || height <= 0)
      return true;
   if (!D3D9EnsureCpuBacking(dst_sub))
      return false;
   for (LONG y = 0; y < height; ++y)
      memmove(dst_sub->data + (size_t)(dst.top + y) * dst_sub->pitch + (size_t)dst.left * bpp,
              src_sub->data + (size_t)(src.top + y) * src_sub->pitch + (size_t)src.left * bpp,
              (size_t)width * bpp);
   dst_sub->cpu_dirty = true;
   return true;
}

static bool
D3D9CopyRectPipe(D3D9Device *device, HANDLE dst_resource_handle,
                 UINT dst_subresource_index, const RECT *dst_rect,
                 HANDLE src_resource_handle, UINT src_subresource_index,
                 const RECT *src_rect, enum pipe_tex_filter filter)
{
   if (!device || !device->pipe || !device->pipe->resource_copy_region)
      return false;

   D3D9Resource *dst_resource = D3D9CastResource(dst_resource_handle);
   D3D9Resource *src_resource = D3D9CastResource(src_resource_handle);
   D3D9SubResource *dst_sub = D3D9GetSubResource(dst_resource_handle,
                                                 dst_subresource_index);
   D3D9SubResource *src_sub = D3D9GetSubResource(src_resource_handle,
                                                 src_subresource_index);
   if (!dst_resource || !src_resource || !src_sub || !dst_sub)
      return false;

   /* which way the copy went, for the timing report */
   const unsigned long long timed = D3D9TimesOn() ? D3D9TimesNow() : 0;
   const auto went = [timed](enum D3D9TimedCallback way) {
      if (timed)
         D3D9TimesCallback(way, timed);
      return true;
   };

   if (D3D9CopyTextureToCpuDestination(device, dst_resource, dst_sub,
                                       dst_subresource_index, dst_rect,
                                       src_resource, src_sub,
                                       src_subresource_index, src_rect))
      return went(D3D9_TIMED_COPY_READBACK);

   if (D3D9CopyCpuSourceToTexture(device, dst_resource, dst_sub,
                                  dst_subresource_index, dst_rect,
                                  src_resource, src_sub, src_rect))
      return went(D3D9_TIMED_COPY_UPLOAD);

   if (D3D9BlitCpuOnlySourceToTexture(device, dst_resource, dst_sub,
                                      dst_subresource_index, dst_rect,
                                      src_resource, src_sub, src_rect,
                                      filter))
      return went(D3D9_TIMED_COPY_CPU_BLIT);
   if (D3D9CopyInMemory(dst_resource, dst_sub, dst_rect,
                        src_resource, src_sub, src_rect))
      return went(D3D9_TIMED_COPY_MEMORY);

   if (!dst_resource->pipe_resource || !src_resource->pipe_resource)
      return false;

   if (src_sub->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, src_subresource_index))
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG dst_width = dst.right - dst.left;
   const LONG dst_height = dst.bottom - dst.top;
   const LONG src_width = src.right - src.left;
   const LONG src_height = src.bottom - src.top;
   if (dst_width <= 0 || dst_height <= 0 || src_width <= 0 ||
       src_height <= 0)
      return true;

   if (src_resource == device->last_systemmem_texture &&
       dst_resource->managed_upload_complete &&
       D3D9ManagedUploadSourceClean(src_resource, dst_resource))
      return went(D3D9_TIMED_COPY_SKIPPED);

   if (D3D9ResolveThenConvertPipe(device, dst_resource, dst_sub,
                                  dst_subresource_index, &dst,
                                  src_resource, src_subresource_index, &src, filter))
      return went(D3D9_TIMED_COPY_CONVERT);

   const bool dst_depth_or_stencil =
      util_format_is_depth_or_stencil(dst_resource->pipe_resource->format);
   const bool src_depth_or_stencil =
      util_format_is_depth_or_stencil(src_resource->pipe_resource->format);
   if (dst_depth_or_stencil != src_depth_or_stencil)
      return false;

   if ((dst_depth_or_stencil ||
        dst_width != src_width || dst_height != src_height ||
        dst_resource->pipe_resource->format !=
        src_resource->pipe_resource->format ||
        dst_resource->pipe_resource->nr_samples !=
        src_resource->pipe_resource->nr_samples) &&
       device->pipe->blit) {
      struct pipe_blit_info info;
      D3D9InitBlitInfo(&info, dst_resource->pipe_resource,
                       D3D9PipeMipLevel(dst_resource, dst_subresource_index),
                       D3D9PipeLayer(dst_resource, dst_subresource_index),
                       dst_resource->pipe_resource->format, &dst,
                       src_resource->pipe_resource,
                       D3D9PipeMipLevel(src_resource, src_subresource_index),
                       D3D9PipeLayer(src_resource, src_subresource_index),
                       src_resource->pipe_resource->format, &src);
      info.filter = filter;
      device->pipe->blit(device->pipe, &info);
      dst_sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
      return went(D3D9_TIMED_COPY_SCALED);
   }

   const LONG width = dst.right - dst.left < src.right - src.left ?
      dst.right - dst.left : src.right - src.left;
   const LONG height = dst.bottom - dst.top < src.bottom - src.top ?
      dst.bottom - dst.top : src.bottom - src.top;
   if (width <= 0 || height <= 0)
      return true;

   struct pipe_box src_box;
   D3D9PipeBoxFromRect(&src, &src_box);
   src_box.z = D3D9PipeLayer(src_resource, src_subresource_index);
   src_box.width = width;
   src_box.height = height;

   device->pipe->resource_copy_region(device->pipe,
                                      dst_resource->pipe_resource,
                                      D3D9PipeMipLevel(dst_resource,
                                                       dst_subresource_index),
                                      (unsigned)dst.left,
                                      (unsigned)dst.top,
                                      D3D9PipeLayer(dst_resource,
                                                   dst_subresource_index),
                                      src_resource->pipe_resource,
                                      D3D9PipeMipLevel(src_resource,
                                                       src_subresource_index),
                                      &src_box);
   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return went(D3D9_TIMED_COPY_REGION);
}

static bool
D3D9CopyVolumePipe(D3D9Device *device, HANDLE dst_resource_handle,
                   HANDLE src_resource_handle, const D3DDDIBOX *src_box,
                   UINT dst_x, UINT dst_y, UINT dst_z)
{
   if (!device || !device->pipe || !device->pipe->resource_copy_region)
      return false;

   D3D9Resource *dst_resource = D3D9CastResource(dst_resource_handle);
   D3D9Resource *src_resource = D3D9CastResource(src_resource_handle);
   if (!dst_resource || !src_resource || !dst_resource->pipe_resource ||
       !src_resource->pipe_resource || !dst_resource->surf_count ||
       !src_resource->surf_count)
      return false;

   D3D9SubResource *dst = &dst_resource->surfaces[0];
   D3D9SubResource *src = &src_resource->surfaces[0];
   if (src_box->Right <= src_box->Left || src_box->Bottom <= src_box->Top ||
       src_box->Back <= src_box->Front)
      return true;

   UINT width = src_box->Right - src_box->Left;
   UINT height = src_box->Bottom - src_box->Top;
   UINT depth = src_box->Back - src_box->Front;
   if (src_box->Right > src->width || src_box->Bottom > src->height ||
       src_box->Back > src->depth || dst_x > dst->width ||
       dst_y > dst->height || dst_z > dst->depth)
      return false;
   width = MIN2(width, dst->width - dst_x);
   height = MIN2(height, dst->height - dst_y);
   depth = MIN2(depth, dst->depth - dst_z);
   if (!width || !height || !depth)
      return true;

   if (src->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, 0))
      return false;

   struct pipe_box box;
   D3D9PipeBoxFromD3DBox(src_box, &box);
   box.width = width;
   box.height = height;
   box.depth = depth;
   device->pipe->resource_copy_region(device->pipe,
                                      dst_resource->pipe_resource, 0,
                                      dst_x, dst_y, dst_z,
                                      src_resource->pipe_resource, 0,
                                      &box);
   dst->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, 0);
   return true;
}

static struct pipe_resource *
D3D9GetReadbackResource(D3D9Device *device, D3D9Resource *resource,
                        D3D9SubResource *sub)
{
   if (sub->readback_resource)
      return sub->readback_resource;

   if (!device || !device->screen || !resource || !resource->pipe_resource)
      return NULL;

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = resource->pipe_resource->target;
   templ.format = resource->pipe_resource->format;
   templ.width0 = sub->width;
   templ.height0 = sub->height;
   templ.depth0 = sub->depth ? sub->depth : 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.usage = PIPE_USAGE_STAGING;

   sub->readback_resource = device->screen->resource_create(device->screen,
                                                            &templ);
   return sub->readback_resource;
}

static void *
D3D9ReadbackTextureLock(D3D9Device *device, D3D9Resource *resource,
                        D3D9SubResource *sub, UINT subresource_index,
                        unsigned usage, const struct pipe_box *box,
                        D3DDDIARG_LOCK *data)
{
   if (!device || !device->pipe || !device->pipe->resource_copy_region ||
       !device->pipe->texture_map || !resource || !resource->pipe_resource ||
       !sub || !box || !data)
      return NULL;

   struct pipe_resource *readback =
      D3D9GetReadbackResource(device, resource, sub);
   if (!readback)
      return NULL;

   struct pipe_box full_box;
   memset(&full_box, 0, sizeof(full_box));
   full_box.width = sub->width;
   full_box.height = sub->height;
   full_box.depth = sub->depth ? sub->depth : 1;

   device->pipe->resource_copy_region(device->pipe, readback, 0,
                                      0, 0, 0,
                                      resource->pipe_resource,
                                      D3D9PipeMipLevel(resource,
                                                       subresource_index),
                                      &full_box);
   yttrium_gdi_flush_labeled(device->pipe, NULL, 0,
                             "D3D9 resource readback");

   void *map = device->pipe->texture_map(device->pipe, readback, 0,
                                         usage, box, &sub->transfer);
   if (!map || !sub->transfer) {
      sub->transfer = NULL;
      return NULL;
   }

   sub->transfer_on_readback = true;
   sub->transfer_write = (usage & PIPE_MAP_WRITE) != 0;
   sub->transfer_box = *box;
   data->Pitch = sub->transfer->stride;
   data->SlicePitch = sub->transfer->layer_stride;
   return map;
}

static void
D3D9EndReadbackLock(D3D9Device *device, D3D9SubResource *sub)
{
   if (!device || !device->pipe || !sub || !sub->transfer)
      return;

   if (device->pipe->texture_unmap)
      device->pipe->texture_unmap(device->pipe, sub->transfer);
   sub->transfer = NULL;
   sub->transfer_on_readback = false;
   sub->transfer_write = false;
   memset(&sub->transfer_box, 0, sizeof(sub->transfer_box));
}

static bool
D3D9CopyTextureToCpuDestination(D3D9Device *device,
                                D3D9Resource *dst_resource,
                                D3D9SubResource *dst_sub,
                                UINT dst_subresource_index,
                                const RECT *dst_rect,
                                D3D9Resource *src_resource,
                                D3D9SubResource *src_sub,
                                UINT src_subresource_index,
                                const RECT *src_rect)
{
   if (!device || !device->pipe || !device->pipe->texture_map ||
       !device->pipe->texture_unmap || !dst_resource || !dst_sub ||
       !src_resource || !src_sub || D3D9PoolNeedsReadback(dst_resource->pool))
      return false;

   if (!src_resource->pipe_resource ||
       src_resource->pipe_resource->target == PIPE_BUFFER ||
       src_resource->format != dst_resource->format || src_sub->transfer ||
       dst_sub->transfer || dst_sub->cpu_locked)
      return false;

   if (!D3D9EnsureCpuBacking(dst_sub))
      return false;

   if (src_sub->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, src_subresource_index))
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG width = dst.right - dst.left < src.right - src.left ?
      dst.right - dst.left : src.right - src.left;
   const LONG height = dst.bottom - dst.top < src.bottom - src.top ?
      dst.bottom - dst.top : src.bottom - src.top;
   if (width <= 0 || height <= 0)
      return true;

   const UINT bpp = D3D9FormatBytesPerPixel(src_resource->format);
   if (!bpp)
      return false;

   struct pipe_box src_box;
   D3D9PipeBoxFromRect(&src, &src_box);
   src_box.width = width;
   src_box.height = height;

   D3DDDIARG_LOCK src_lock;
   memset(&src_lock, 0, sizeof(src_lock));
   void *src_map = D3D9ReadbackTextureLock(device, src_resource, src_sub,
                                           src_subresource_index,
                                           PIPE_MAP_READ, &src_box,
                                           &src_lock);
   if (!src_map)
      return false;

   const size_t row_bytes = (size_t)width * bpp;
   const unsigned src_stride = src_sub->transfer->stride;
   const unsigned dst_stride = dst_sub->pitch;
   const uint8_t *src_bytes = (const uint8_t *)src_map;
   uint8_t *dst_bytes = dst_sub->data +
      (size_t)dst.top * dst_sub->pitch + (size_t)dst.left * bpp;
   for (LONG y = 0; y < height; ++y) {
      memcpy(dst_bytes + (size_t)y * dst_stride,
             src_bytes + (size_t)y * src_stride, row_bytes);
   }

   D3D9EndReadbackLock(device, src_sub);
   dst_sub->cpu_dirty = dst_resource->pipe_resource != NULL;
   return true;
}

bool
D3D9UploadSubResource(D3D9Device *device, D3D9Resource *resource,
                      UINT subresource_index)
{
   if (!device || !device->pipe || !resource || !resource->pipe_resource ||
       subresource_index >= resource->surf_count)
      return false;

   D3D9SubResource *sub = &resource->surfaces[subresource_index];
   if (!sub->cpu_dirty || !sub->data)
      return true;

   if (resource->pipe_resource->target == PIPE_BUFFER) {
      if (!device->pipe->buffer_subdata) {
         D3D9Warnf("unsupported CPU-sourced buffer upload without pipe "
                   "buffer_subdata resource=%p sub=%u\n",
                   resource, subresource_index);
         return false;
      }
   } else if (!device->pipe->texture_subdata) {
      D3D9Warnf("unsupported CPU-sourced texture upload without pipe "
                "texture_subdata resource=%p sub=%u\n",
                resource, subresource_index);
      return false;
   }

   if (resource->pipe_resource->target != PIPE_BUFFER &&
       (!sub->width || !sub->height))
      return false;


   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.z = D3D9PipeLayer(resource, subresource_index);
   box.width = sub->width;
   box.height = sub->height;
   box.depth = sub->depth ? sub->depth : 1;

   if (resource->pipe_resource->target == PIPE_BUFFER &&
       device->pipe->buffer_subdata) {
      device->pipe->buffer_subdata(device->pipe, resource->pipe_resource,
                                   PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                                   0, (unsigned)sub->size, sub->data);
   } else {
      if (!D3D9TextureSubdata(device, resource, subresource_index,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box, sub->data, sub->pitch,
                              sub->slice_pitch))
         return false;
   }
   sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
   if (D3D9PoolNeedsReadback(resource->pool)) {
      if (sub->owns_data)
         free(sub->data);
      sub->data = NULL;
      sub->data_capacity = 0;
      sub->owns_data = false;
   }
   return true;
}

static bool
D3D9ResourceIsTexture(const D3D9Resource *resource)
{
   return resource &&
      (resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume);
}

static UINT
D3D9ManagedUploadSrcSubResource(const D3D9Resource *src,
                                const D3D9Resource *dst,
                                UINT src_base_subresource,
                                UINT dst_subresource)
{
   if (src && dst && src->flags.CubeMap && dst->flags.CubeMap &&
       src->mip_levels && dst->mip_levels) {
      const UINT face = dst_subresource / dst->mip_levels;
      const UINT mip = dst_subresource % dst->mip_levels;
      return face * src->mip_levels + src_base_subresource + mip;
   }

   return src_base_subresource + dst_subresource;
}

static bool
D3D9ResourcesMatchForManagedUpload(const D3D9Resource *src,
                                   const D3D9Resource *dst,
                                   UINT *src_base_subresource)
{
   if (src_base_subresource)
      *src_base_subresource = 0;

   if (!D3D9ResourceIsTexture(src) || !D3D9ResourceIsTexture(dst) ||
       !dst->pipe_resource || src->format != dst->format ||
       src->flags.CubeMap != dst->flags.CubeMap ||
       src->flags.Volume != dst->flags.Volume)
      return false;

   if (src->surf_count == dst->surf_count &&
       src->mip_levels == dst->mip_levels) {
      for (UINT i = 0; i < src->surf_count; ++i) {
         if (src->surfaces[i].width != dst->surfaces[i].width ||
             src->surfaces[i].height != dst->surfaces[i].height ||
             src->surfaces[i].depth != dst->surfaces[i].depth)
            return false;
      }
      return true;
   }

   if (src->flags.CubeMap && dst->flags.CubeMap &&
       src->mip_levels >= dst->mip_levels &&
       src->surf_count >= src->mip_levels * 6 &&
       dst->surf_count == dst->mip_levels * 6) {
      const UINT max_base = src->mip_levels - dst->mip_levels;
      for (UINT base = 1; base <= max_base; ++base) {
         bool match = true;
         for (UINT i = 0; i < dst->surf_count; ++i) {
            const UINT src_index =
               D3D9ManagedUploadSrcSubResource(src, dst, base, i);
            const D3D9SubResource *src_sub = &src->surfaces[src_index];
            const D3D9SubResource *dst_sub = &dst->surfaces[i];

            if (src_sub->width != dst_sub->width ||
                src_sub->height != dst_sub->height ||
                src_sub->depth != dst_sub->depth) {
               match = false;
               break;
            }
         }
         if (match) {
            if (src_base_subresource)
               *src_base_subresource = base;
            return true;
         }
      }
   }

   if (!src->flags.Texture || dst->flags.CubeMap || dst->flags.Volume ||
       !src->mip_levels || !dst->mip_levels ||
       dst->surf_count != dst->mip_levels ||
       src->surf_count < dst->surf_count)
      return false;

   for (UINT base = 1; base + dst->surf_count <= src->surf_count; ++base) {
      bool match = true;
      for (UINT i = 0; i < dst->surf_count; ++i) {
         const D3D9SubResource *src_sub = &src->surfaces[base + i];
         const D3D9SubResource *dst_sub = &dst->surfaces[i];

         if (src_sub->width != dst_sub->width ||
             src_sub->height != dst_sub->height ||
             src_sub->depth != dst_sub->depth) {
            match = false;
            break;
         }
      }
      if (match) {
         if (src_base_subresource)
            *src_base_subresource = base;
         return true;
      }
   }

   return false;
}

static bool
D3D9ResourcesMatchForVolumeUpload(const D3D9Resource *src,
                                  const D3D9Resource *dst,
                                  UINT *src_base_subresource)
{
   if (src_base_subresource)
      *src_base_subresource = 0;

   if (!src || !dst || !src->flags.Volume || !dst->flags.Volume ||
       !src->mip_levels || !dst->mip_levels ||
       !D3D9FormatsHaveSameMemoryLayout(src->format, dst->format) ||
       src->surf_count < dst->surf_count)
      return false;

   for (UINT base = 0; base + dst->surf_count <= src->surf_count; ++base) {
      bool match = true;

      for (UINT i = 0; i < dst->surf_count; ++i) {
         const D3D9SubResource *src_sub = &src->surfaces[base + i];
         const D3D9SubResource *dst_sub = &dst->surfaces[i];

         if (src_sub->width != dst_sub->width ||
             src_sub->height != dst_sub->height ||
             src_sub->depth != dst_sub->depth) {
            match = false;
            break;
         }
      }

      if (match) {
         if (src_base_subresource)
            *src_base_subresource = base;
         return true;
      }
   }

   return false;
}

static bool
D3D9ResourcesMatchForManagedBufferUpload(const D3D9Resource *src,
                                         const D3D9Resource *dst)
{
   if (!src || !dst || !src->surf_count || !dst->surf_count ||
       src->pool != D3DDDIPOOL_SYSTEMMEM ||
       dst->pool == D3DDDIPOOL_SYSTEMMEM)
      return false;

   if (src->flags.VertexBuffer != dst->flags.VertexBuffer ||
       src->flags.IndexBuffer != dst->flags.IndexBuffer ||
       !(src->flags.VertexBuffer || src->flags.IndexBuffer))
      return false;

   return src->surfaces[0].size == dst->surfaces[0].size;
}

static bool
D3D9ManagedUploadSourceClean(const D3D9Resource *src, const D3D9Resource *dst)
{
   UINT src_base_subresource = 0;
   if (!D3D9ResourcesMatchForManagedUpload(src, dst, &src_base_subresource))
      return false;

   for (UINT i = 0; i < dst->surf_count; ++i) {
      const UINT src_index =
         D3D9ManagedUploadSrcSubResource(src, dst, src_base_subresource, i);
      if (src->surfaces[src_index].cpu_dirty)
         return false;
   }

   return true;
}

static bool
D3D9UploadManagedTextureData(D3D9Device *device, D3D9Resource *dst,
                             D3D9Resource *src)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !dst || !src)
      return false;

   UINT src_base_subresource = 0;
   if (!D3D9ResourcesMatchForManagedUpload(src, dst, &src_base_subresource))
      return false;

   if (!src_base_subresource && src->surf_count == dst->surf_count &&
       src->mip_levels == dst->mip_levels) {
      if (src->managed_default_resource &&
          src->managed_default_resource != dst &&
          src->managed_default_resource->managed_source_resource == src)
         src->managed_default_resource->managed_source_resource = NULL;
      if (dst->managed_source_resource &&
          dst->managed_source_resource != src &&
          dst->managed_source_resource->managed_default_resource == dst)
         dst->managed_source_resource->managed_default_resource = NULL;
      src->managed_default_resource = dst;
      dst->managed_source_resource = src;
      pipe_resource_reference(&src->managed_default_pipe_resource,
                              dst->pipe_resource);
   } else if (src->managed_default_pipe_resource) {
      pipe_resource_reference(&dst->managed_source_pipe_resource,
                              src->managed_default_pipe_resource);
      dst->managed_source_base_level =
         D3D9PipeMipLevel(src, src_base_subresource);
   }

   bool uploaded = false;
   for (UINT i = 0; i < dst->surf_count; ++i) {
      const UINT src_index =
         D3D9ManagedUploadSrcSubResource(src, dst, src_base_subresource, i);
      D3D9SubResource *src_sub = &src->surfaces[src_index];
      D3D9SubResource *dst_sub = &dst->surfaces[i];
      if (!src_sub->data)
         continue;

      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.z = D3D9PipeLayer(dst, i);
      box.width = dst_sub->width;
      box.height = dst_sub->height;
      box.depth = dst_sub->depth ? dst_sub->depth : 1;

      if (!D3D9TextureSubdata(device, dst, i,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box, src_sub->data, src_sub->pitch,
                              src_sub->slice_pitch))
         return false;
      dst_sub->cpu_dirty = false;
      src_sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(dst, i);
      uploaded = true;
   }

   dst->managed_upload_complete = uploaded;
   return uploaded;
}

static bool
D3D9UploadVolumeTextureData(D3D9Device *device, D3D9Resource *dst,
                            D3D9Resource *src)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !dst || !src || !dst->pipe_resource)
      return false;

   if (D3D9PoolNeedsReadback(src->pool))
      return false;

   UINT src_base_subresource = 0;
   if (!D3D9ResourcesMatchForVolumeUpload(src, dst, &src_base_subresource))
      return false;

   for (UINT i = 0; i < dst->surf_count; ++i) {
      D3D9SubResource *src_sub = &src->surfaces[src_base_subresource + i];
      D3D9SubResource *dst_sub = &dst->surfaces[i];
      if (!src_sub->data)
         return false;

      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.width = dst_sub->width;
      box.height = dst_sub->height;
      box.depth = dst_sub->depth ? dst_sub->depth : 1;

      if (!D3D9TextureSubdata(device, dst, i,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box, src_sub->data, src_sub->pitch,
                              src_sub->slice_pitch))
         return false;

      dst_sub->cpu_dirty = false;
      src_sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(dst, i);
   }

   return true;
}

static bool
D3D9CopyToDisplayResource(D3D9Device *device, D3D9Resource *resource,
                           UINT subresource_index)
{
   if (!resource->display_resource)
      return true;
   D3D9SubResource *sub = D3D9GetSubResource(resource, subresource_index);
   if (!device->pipe || !resource->pipe_resource || !sub)
      return false;

   struct pipe_resource *src = resource->pipe_resource;
   struct pipe_resource *dst = resource->display_resource;
   if (src->nr_samples > 1) {
      if (!device->pipe->blit)
         return false;
      RECT rect = D3D9FullRect(sub);
      struct pipe_blit_info info;
      D3D9InitBlitInfo(&info, dst, 0, 0, dst->format, &rect,
                        src, D3D9PipeMipLevel(resource, subresource_index),
                        D3D9PipeLayer(resource, subresource_index),
                        src->format, &rect);
      device->pipe->blit(device->pipe, &info);
   } else {
      if (!device->pipe->resource_copy_region)
         return false;
      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.z = D3D9PipeLayer(resource, subresource_index);
      box.width = sub->width;
      box.height = sub->height;
      box.depth = 1;
      device->pipe->resource_copy_region(device->pipe, dst, 0, 0, 0, 0,
         src, D3D9PipeMipLevel(resource, subresource_index), &box);
   }
   return true;
}

bool
D3D9PublishResource(D3D9Device *device, D3D9Resource *resource)
{
   return device && resource && device->pipe &&
      D3D9CopyToDisplayResource(device, resource, 0);
}

#ifdef RIPPLE_VISTA
/* Save an application render target to a BMP. The present hook can use a
 * process-specific request file or the shared ripple-swapdump trigger. */
void
D3D9DumpImage(D3D9Device *device, struct pipe_resource *image, unsigned level, const char *path)
{
   if (!image || util_format_get_blocksize(image->format) != 4 || !device->pipe->texture_map) {
      ripple_logf("image dump skipped: %s, unsupported resource or format %u",
                  path, image ? (unsigned)image->format : 0);
      return;
   }

   const unsigned w = u_minify(image->width0, level), h = u_minify(image->height0, level);
   struct pipe_box box;
   u_box_2d(0, 0, w, h, &box);
   /* A scanout or shared primary need not have guest backing for a transfer.
    * Copy its rendered contents into an ordinary readable resource first. */
   struct pipe_resource templ = {};
   templ.target = PIPE_TEXTURE_2D;
   templ.format = image->format;
   templ.width0 = w;
   templ.height0 = h;
   templ.depth0 = templ.array_size = 1;
   templ.nr_samples = templ.nr_storage_samples = 1;
   templ.usage = PIPE_USAGE_STAGING;
   const bool resolve = image->nr_samples > 1;
   if (resolve)
      templ.bind = PIPE_BIND_RENDER_TARGET;
   struct pipe_resource *staging = device->screen->resource_create(device->screen, &templ);
   if (!staging || (resolve ? !device->pipe->blit : !device->pipe->resource_copy_region)) {
      pipe_resource_reference(&staging, NULL);
      ripple_logf("image dump staging unavailable: %s, %ux%u format %u", path, w, h, image->format);
      return;
   }
   if (resolve) {
      struct pipe_blit_info blit = {};
      blit.src.resource = image;
      blit.src.level = level;
      blit.src.format = image->format;
      blit.src.box = box;
      blit.dst.resource = staging;
      blit.dst.format = templ.format;
      blit.dst.box = box;
      blit.mask = PIPE_MASK_RGBA;
      blit.filter = PIPE_TEX_FILTER_NEAREST;
      device->pipe->blit(device->pipe, &blit);
   } else {
      device->pipe->resource_copy_region(device->pipe, staging, 0, 0, 0, 0, image, level, &box);
   }
   struct pipe_transfer *transfer = NULL;
   const uint8_t *map = (const uint8_t *)device->pipe->texture_map(
      device->pipe, staging, 0, PIPE_MAP_READ, &box, &transfer);
   if (!map) {
      ripple_logf("image dump map failed: %s, %ux%u format %u bind 0x%x samples %u",
                  path, w, h, (unsigned)image->format, image->bind, image->nr_samples);
      pipe_resource_reference(&staging, NULL);
      return;
   }

   /* A reader watching the file should only see a whole frame, including on a retry. */
   char pending[MAX_PATH];
   const int length = snprintf(pending, sizeof(pending), "%s.%lu.tmp", path, GetCurrentThreadId());
   FILE *f = length > 0 && (size_t)length < sizeof(pending) ? fopen(pending, "wb") : NULL;
   bool complete = false;
   if (f) {
      BITMAPFILEHEADER bf = {};
      BITMAPINFOHEADER bi = {};
      bf.bfType = 0x4d42;
      bf.bfOffBits = sizeof(bf) + sizeof(bi);
      bf.bfSize = bf.bfOffBits + w * h * 4;
      bi.biSize = sizeof(bi);
      bi.biWidth = w;
      bi.biHeight = -(LONG)h;
      bi.biPlanes = 1;
      bi.biBitCount = 32;
      bi.biCompression = BI_RGB;
      complete = fwrite(&bf, sizeof(bf), 1, f) == 1 && fwrite(&bi, sizeof(bi), 1, f) == 1;
      /* BMP keeps blue, green, red: swap where the image keeps red first */
      const bool rgb = util_format_description(image->format)->swizzle[0] == PIPE_SWIZZLE_X;
      const size_t row_bytes = (size_t)w * 4;
      uint8_t *row = rgb ? (uint8_t *)malloc(row_bytes) : NULL;
      if (rgb && !row)
         complete = false;
      for (unsigned y = 0; complete && y < h; ++y) {
         const uint8_t *src = map + (size_t)y * transfer->stride;
         if (rgb) {
            memcpy(row, src, row_bytes);
            for (unsigned x = 0; x < w; ++x) {
               const uint8_t r = row[x * 4];
               row[x * 4] = row[x * 4 + 2];
               row[x * 4 + 2] = r;
            }
            src = row;
         }
         complete = fwrite(src, row_bytes, 1, f) == 1;
      }
      free(row);
      if (fclose(f) != 0)
         complete = false;
      if (complete)
         complete = MoveFileExA(pending, path, MOVEFILE_REPLACE_EXISTING) != FALSE;
      if (!complete)
         DeleteFileA(pending);
   }
   device->pipe->texture_unmap(device->pipe, transfer);
   pipe_resource_reference(&staging, NULL);
   ripple_logf("image dump%s: %s, %ux%u, format %u", complete ? "" : " write failed",
               path, w, h, (unsigned)image->format);
}

static void
D3D9SwapDump(D3D9Device *device, struct pipe_resource *image, unsigned level)
{
   static DWORD next_look;
   /* The optional path lets a test request only its own next presentation.
    * Other processes continue to watch the default trigger. */
   static const char *const request = getenv("RIPPLE_SWAPDUMP_REQUEST");
   const char *trigger = request && *request ? request : "C:\\Users\\Public\\ripple-swapdump";
   const DWORD now = GetTickCount();
   if ((LONG)(now - next_look) < 0)
      return;
   next_look = now + 500;
   if (GetFileAttributesA(trigger) == INVALID_FILE_ATTRIBUTES)
      return;
   DeleteFileA(trigger);
   char path[MAX_PATH];
   snprintf(path, sizeof(path), "C:\\Users\\Public\\swap-%lu.bmp", GetCurrentProcessId());
   D3D9DumpImage(device, image, level, path);
}

/* Vista: RIPPLE_DRAWDUMP=N,M,... in a program's environment saves the first render target
 * after its Nth, Mth, ... draw since it started, as C:\Users\Public\draw-PID-N.bmp. */
void
D3D9DrawDump(D3D9Device *device)
{
   static unsigned wanted[32], nwanted, draws;
   static bool parsed;
   if (!parsed) {
      parsed = true;
      const char *list = getenv("RIPPLE_DRAWDUMP");
      while (list && *list && nwanted < ARRAYSIZE(wanted)) {
         char *end;
         unsigned long n = strtoul(list, &end, 10);
         if (end == list)
            break;
         wanted[nwanted++] = (unsigned)n;
         list = *end ? end + 1 : end;
      }
   }
   if (!nwanted)
      return;
   ++draws;
   for (unsigned i = 0; i < nwanted; ++i) {
      if (wanted[i] != draws)
         continue;
      D3D9Resource *rt = D3D9CastResource(device->render_targets[0]);
      if (!rt || !rt->pipe_resource)
         return;
      char path[MAX_PATH];
      snprintf(path, sizeof(path), "C:\\Users\\Public\\draw-%lu-%u.bmp",
               GetCurrentProcessId(), draws);
      D3D9DumpImage(device, rt->pipe_resource,
                    D3D9PipeMipLevel(rt, device->render_target_subresources[0]), path);
   }
}
#endif

static HRESULT
D3D9FlushFrontbuffer(D3D9Device *device, D3D9Resource *src_resource,
                     UINT src_subresource_index, D3DKMT_HANDLE dst_allocation,
                     bool application_scanout)
{
   if (!device || !device->pipe || !device->screen ||
       !device->screen->flush_frontbuffer || !src_resource ||
       !src_resource->pipe_resource)
      return E_INVALIDARG;

   struct gdikmt_present_info present_info;
   memset(&present_info, 0, sizeof(present_info));
   present_info.magic = GDIKMT_PRESENT_INFO_MAGIC;
   present_info.version = 3;
   present_info.hDstAllocation = dst_allocation;
   present_info.status = S_OK;
   present_info.application_scanout = application_scanout;

   struct pipe_resource *flush_resource = src_resource->pipe_resource;
   unsigned flush_level = D3D9PipeMipLevel(src_resource,
                                           src_subresource_index);
   struct pipe_resource *resolved = NULL;
   D3D9SubResource *flip_sub =
      D3D9GetSubResource(src_resource, src_subresource_index);
#ifdef RIPPLE_VISTA
   D3D9SwapDump(device, flush_resource, flush_level);
#endif

   if (flip_sub && flip_sub->flip_resource) {
      /* Vista flip chain: the frame drawn in the one render image goes to the buffer
       * Windows flips to, never to the one on screen. */
      if (!device->pipe->resource_copy_region)
         return E_FAIL;
      struct pipe_box box;
      u_box_2d(0, 0, flip_sub->width, flip_sub->height, &box);
      device->pipe->resource_copy_region(device->pipe, flip_sub->flip_resource, 0, 0, 0, 0,
                                         flush_resource, flush_level, &box);
      flush_resource = flip_sub->flip_resource;
      flush_level = 0;
   } else if (src_resource->display_resource) {
      if (!D3D9CopyToDisplayResource(device, src_resource,
                                      src_subresource_index))
         return E_FAIL;
      flush_resource = src_resource->display_resource;
      flush_level = 0;
   } else if (flush_resource->nr_samples > 1 && device->pipe->blit) {
      D3D9SubResource *src_sub =
         D3D9GetSubResource(src_resource, src_subresource_index);
      if (!src_sub)
         return E_INVALIDARG;

      {
         struct pipe_resource templ;
         memset(&templ, 0, sizeof(templ));
         templ.target = PIPE_TEXTURE_2D;
         templ.format = flush_resource->format;
         templ.width0 = src_sub->width;
         templ.height0 = src_sub->height;
         templ.depth0 = 1;
         templ.array_size = 1;
         templ.last_level = 0;
         templ.nr_samples = 1;
         templ.nr_storage_samples = 1;
         templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                      PIPE_BIND_SHARED;
         templ.usage = PIPE_USAGE_DEFAULT;
         if (D3D9IsYttriumScreen(device->screen)) {
            if (!src_resource->flags.SharedResource &&
                (src_resource->flags.Texture || src_resource->flags.Primary))
               templ.flags |= YTTRIUM_GDI_RESOURCE_FLAG_LOCAL_RENDER;
            /* The resolved image is the legacy Present source actually read
             * by KMD. Preserve the original source's CPU-blt qualification. */
            if (src_resource->flags.DiscardRenderTarget)
               templ.flags |= YTTRIUM_GDI_RESOURCE_FLAG_CPU_READBACK;
         }

         resolved = device->screen->resource_create(device->screen, &templ);
         if (!resolved)
            return E_OUTOFMEMORY;
      }

      RECT rect = D3D9FullRect(src_sub);
      struct pipe_blit_info info;
      D3D9InitBlitInfo(&info, resolved, 0, 0, resolved->format, &rect,
                       flush_resource, flush_level,
                       D3D9PipeLayer(src_resource, src_subresource_index),
                       flush_resource->format, &rect);
      device->pipe->blit(device->pipe, &info);
      flush_resource = resolved;
      flush_level = 0;
   }

   /* The Yttrium screen hook owns its exact asynchronous publication ticket. */
   if (!D3D9IsYttriumScreen(device->screen))
      yttrium_gdi_flush_labeled(device->pipe, NULL, 0,
                                "D3D9 present pre-flush");
   device->screen->flush_frontbuffer(device->screen,
                                     device->pipe,
                                     flush_resource,
                                     flush_level,
                                     0,
                                     &present_info,
                                     0,
                                     NULL);
   pipe_resource_reference(&resolved, NULL);
   return present_info.status;
}

static size_t
D3D9ResourceDimensionSize(const D3DDDIARG_CREATERESOURCE *data,
                          const D3DDDI_SURFACEINFO *surface)
{
   const UINT depth = surface->Depth ? surface->Depth : 1;
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return surface->Width;

   const UINT pitch =
      D3D9AlignPitch(D3D9FormatRowPitch(data->Format, surface->Width));
   return (size_t)pitch * D3D9FormatRowCount(data->Format,
                                             surface->Height) * depth;
}

static UINT
D3D9ResourcePitch(const D3DDDIARG_CREATERESOURCE *data,
                  const D3DDDI_SURFACEINFO *surface)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return surface->Width;
   return D3D9AlignPitch(D3D9FormatRowPitch(data->Format, surface->Width));
}

static UINT
D3D9ResourceSlicePitch(const D3DDDIARG_CREATERESOURCE *data,
                       const D3DDDI_SURFACEINFO *surface, UINT pitch)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return surface->Width;
   return pitch * D3D9FormatRowCount(data->Format, surface->Height);
}

static void
D3D9RegisterResource(D3D9Device *device, D3D9Resource *resource)
{
   resource->device = device;
   resource->next = device->resources;
   if (resource->next)
      resource->next->previous = resource;
   device->resources = resource;
}

static void
D3D9FreeResource(D3D9Resource *resource)
{
   if (!resource)
      return;

   if (resource->device) {
      if (resource->previous)
         resource->previous->next = resource->next;
      else
         resource->device->resources = resource->next;
      if (resource->next)
         resource->next->previous = resource->previous;
   }

   D3D9ReleaseWatchedBuffer(resource);
   D3D9ReleaseKept(resource);
   pipe_resource_reference(&resource->pipe_resource, NULL);
   pipe_resource_reference(&resource->display_resource, NULL);
   pipe_resource_reference(&resource->managed_default_pipe_resource, NULL);
   pipe_resource_reference(&resource->managed_source_pipe_resource, NULL);
   for (UINT i = 0; i < resource->surf_count; ++i) {
      pipe_resource_reference(&resource->surfaces[i].readback_resource, NULL);
      pipe_resource_reference(&resource->surfaces[i].flip_resource, NULL);
      pipe_resource_reference(
         &resource->surfaces[i].worker_upload_buffer, NULL);
      if (resource->surfaces[i].owns_data)
         free(resource->surfaces[i].data);
   }
   free(resource->surfaces);
   free(resource);
}

static HRESULT
D3D9DeallocateResource(D3D9Device *device, D3D9Resource *resource)
{
   if (!device || !resource)
      return E_INVALIDARG;

   bool has_allocation = false;
   for (UINT i = 0; i < resource->surf_count; ++i)
      has_allocation |= resource->surfaces[i].allocation != 0;
   if (!has_allocation)
      return S_OK;

   if (!device->callbacks.pfnDeallocateCb) {
      D3D9Warnf("ERROR: D3D9 resource deallocation unavailable "
                "owner=d3d9-resource reason=missing-pfnDeallocateCb "
                "action=retain-resource resource=%p runtime=%p\n",
                resource, resource->runtime_handle);
      return E_NOTIMPL;
   }

   if (resource->runtime_handle) {
      D3DDDICB_DEALLOCATE deallocate;
      memset(&deallocate, 0, sizeof(deallocate));
      deallocate.hResource = resource->runtime_handle;
      HRESULT hr = device->callbacks.pfnDeallocateCb(device->hRTDevice,
                                                     &deallocate);
      if (FAILED(hr)) {
         D3D9Warnf("ERROR: D3D9 resource deallocation failed "
                   "owner=d3d9-resource reason=pfnDeallocateCb-failed "
                   "action=retain-resource resource=%p runtime=%p "
                   "status=0x%08lx\n",
                   resource, resource->runtime_handle, hr);
         return hr;
      }

      resource->runtime_handle = NULL;
      for (UINT i = 0; i < resource->surf_count; ++i)
         resource->surfaces[i].allocation = 0;
      return S_OK;
   }

   for (UINT i = 0; i < resource->surf_count; ++i) {
      if (!resource->surfaces[i].allocation)
         continue;

      D3DKMT_HANDLE allocation = resource->surfaces[i].allocation;
      D3DDDICB_DEALLOCATE deallocate;
      memset(&deallocate, 0, sizeof(deallocate));
      deallocate.NumAllocations = 1;
      deallocate.HandleList = &allocation;
      HRESULT hr = device->callbacks.pfnDeallocateCb(device->hRTDevice,
                                                     &deallocate);
      if (FAILED(hr)) {
         D3D9Warnf("ERROR: D3D9 allocation deallocation failed "
                   "owner=d3d9-resource reason=pfnDeallocateCb-failed "
                   "action=retain-resource resource=%p subresource=%u "
                   "allocation=0x%lx status=0x%08lx\n",
                   resource, i, (unsigned long)allocation, hr);
         return hr;
      }
      for (UINT j = i; j < resource->surf_count; ++j) {
         if (resource->surfaces[j].allocation == allocation)
            resource->surfaces[j].allocation = 0;
      }
   }

   return S_OK;
}

/* A plain video-memory surface of a Direct3D 9 program: no render target, no texture, only
 * pixels the program locks and writes, fills with a colour, or copies from another such
 * surface, and that are then copied or stretched to something that is drawn. Windows' video
 * renderer keeps every decoded frame in one, and its decoder locks it without discarding.
 * Kept in the driver's memory, a frame costs the decoder nothing but its own writes and the
 * host one upload when the frame is shown; as a host texture every such lock first fetched
 * the old contents back from the host.
 * Not DirectDraw's plain surfaces: those are presented from (D3D9ResourceIsDDrawPresentSource).
 */
static bool
D3D9IsPlainInMemory(const D3D9Device *device, const D3DDDIARG_CREATERESOURCE *data)
{
   /* What such a surface may say it is for: nothing, or a decoder's frames. One in a video
    * format may also be marked for the video processor; nothing can draw to those formats
    * here, so it too only holds a frame. */
   D3DDDI_RESOURCEFLAGS allowed = {};
   allowed.DecodeRenderTarget = 1;
   if (D3D9IsVideoFormat(data->Format))
      allowed.VideoProcessRenderTarget = 1;
   return device->runtime_interface >= 9 &&
      (data->Flags.Value & ~allowed.Value) == 0 &&
      !data->MultisampleType && D3D9PoolNeedsReadback(data->Pool) &&
      !data->pSurfList[0].pSysMem &&
      (D3D9IsRgb32(data->Format) || D3D9IsVideoFormat(data->Format));
}

static HRESULT
D3D9CopyInitialData(const D3DDDIARG_CREATERESOURCE *data,
                    D3D9SubResource *dst,
                    const D3DDDI_SURFACEINFO *src)
{
   if (!src->pSysMem)
      return S_OK;

   if (!D3D9PoolNeedsReadback(data->Pool)) {
      /* Windows' own memory, used where it lies, so with the pitch Windows gives for it. A
       * surface that is part of a wider bitmap (a window's contents inside its frame, as the
       * compositor hands them over on its Direct3D 9 path: 657 pixels wide with a pitch of
       * 2708, not 2628) has rows further apart than its width; read with the pitch of the
       * width, every row came out shifted and the window was diagonal stripes. The size
       * ends with the last row: what follows that is not the surface's. */
      if (src->SysMemPitch && !(data->Flags.VertexBuffer || data->Flags.IndexBuffer)) {
         const UINT row_bytes = D3D9FormatRowPitch(data->Format, src->Width);
         const UINT rows = D3D9FormatRowCount(data->Format, src->Height ? src->Height : 1);
         const UINT depth = src->Depth ? src->Depth : 1;
         if (rows && src->SysMemPitch >= row_bytes) {
            dst->pitch = src->SysMemPitch;
            dst->slice_pitch = src->SysMemSlicePitch ?
               src->SysMemSlicePitch : dst->pitch * rows;
            dst->size = (size_t)dst->slice_pitch * (depth - 1) +
               (size_t)dst->pitch * (rows - 1) + row_bytes;
         }
      }
      dst->data = (uint8_t *)(uintptr_t)src->pSysMem;
      dst->data_capacity = dst->size;
      dst->owns_data = false;
      dst->cpu_dirty = true;
      return S_OK;
   }

   dst->data = (uint8_t *)malloc(dst->size);
   if (!dst->data)
      return E_OUTOFMEMORY;
   dst->owns_data = true;
   dst->data_capacity = dst->size;

   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer) {
      memcpy(dst->data, src->pSysMem, dst->size);
      dst->cpu_dirty = true;
      return S_OK;
   }

   const UINT row_bytes = D3D9FormatRowPitch(data->Format, src->Width);
   const UINT rows = D3D9FormatRowCount(data->Format, src->Height);
   const UINT depth = src->Depth ? src->Depth : 1;
   const UINT src_pitch = src->SysMemPitch ? src->SysMemPitch : row_bytes;
   const UINT src_slice_pitch =
      src->SysMemSlicePitch ? src->SysMemSlicePitch : src_pitch * rows;

   for (UINT z = 0; z < depth; ++z) {
      const uint8_t *src_slice = (const uint8_t *)src->pSysMem +
         (size_t)z * src_slice_pitch;
      uint8_t *dst_slice = dst->data + (size_t)z * dst->slice_pitch;
      for (UINT y = 0; y < rows; ++y)
         memcpy(dst_slice + (size_t)y * dst->pitch,
                src_slice + (size_t)y * src_pitch,
                row_bytes);
   }

   dst->cpu_dirty = true;
   return S_OK;
}


HRESULT APIENTRY
D3D9SetPriority(HANDLE hDevice, const D3DDDIARG_SETPRIORITY *data)
{
   (void)hDevice;
   if (!data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   resource->priority = data->Priority;
   return S_OK;
}

HRESULT APIENTRY
D3D9VolBlt(HANDLE hDevice, const D3DDDIARG_VOLUMEBLT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   D3D9Resource *dst_resource = D3D9CastResource(data->hDstResource);
   D3D9Resource *src_resource = D3D9CastResource(data->hSrcResource);
   if (D3D9UploadVolumeTextureData(device, dst_resource, src_resource))
      return S_OK;

   if (D3D9CopyVolumePipe(device, data->hDstResource, data->hSrcResource,
                          &data->SrcBox, data->DstX, data->DstY,
                          data->DstZ))
      return S_OK;

   D3D9Warnf("unsupported volume blit without pipe copy dst=%p src=%p\n",
             data->hDstResource, data->hSrcResource);
   return E_NOTIMPL;
}

HRESULT APIENTRY
D3D9BufBlt(HANDLE hDevice, const D3DDDIARG_BUFFERBLT *data)
{
   if (!data)
      return E_INVALIDARG;

   return D3D9CopyBuffer((D3D9Device *)hDevice, data->hDstResource,
                         data->hSrcResource, data->Offset,
                         data->SrcRange.Offset, data->SrcRange.Size);
}

HRESULT APIENTRY
D3D9TexBlt(HANDLE hDevice, const D3DDDIARG_TEXBLT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   D3D9Resource *dst_resource = D3D9CastResource(data->hDstResource);
   D3D9Resource *src_resource = D3D9CastResource(data->hSrcResource);
   if (src_resource && dst_resource &&
       src_resource->pool == D3DDDIPOOL_SYSTEMMEM &&
       (src_resource->flags.Texture || src_resource->flags.CubeMap) &&
       !src_resource->flags.Volume && src_resource->mip_levels > 1 &&
       data->CubeMapFace == 0 && data->DstPoint.x == 0 &&
       data->DstPoint.y == 0 && data->SrcRect.left == 0 &&
       data->SrcRect.top == 0 &&
       data->SrcRect.right == (LONG)src_resource->surfaces[0].width &&
       data->SrcRect.bottom == (LONG)src_resource->surfaces[0].height &&
       D3D9ResourcesMatchForManagedUpload(src_resource, dst_resource, NULL)) {
      return D3D9UploadManagedTextureData(device, dst_resource,
                                          src_resource) ? S_OK : E_NOTIMPL;
   }

   RECT dst_rect;
   dst_rect.left = data->DstPoint.x;
   dst_rect.top = data->DstPoint.y;
   dst_rect.right = dst_rect.left + (data->SrcRect.right - data->SrcRect.left);
   dst_rect.bottom = dst_rect.top + (data->SrcRect.bottom - data->SrcRect.top);

   if (D3D9CopyRectPipe(device, data->hDstResource, data->CubeMapFace,
                        &dst_rect, data->hSrcResource, data->CubeMapFace,
                        &data->SrcRect, PIPE_TEX_FILTER_NEAREST))
      return S_OK;

   D3D9Warnf("unsupported texture blit without pipe copy dst=%p src=%p "
             "sub=%u\n",
             data->hDstResource, data->hSrcResource, data->CubeMapFace);
   return E_NOTIMPL;
}

/* moose-ripple: the screen's own primary takes turns with the back buffer the device made
 * (D3D9OpenResource), so in a multisampled full-screen device it is drawn to under the
 * device's multisampled depth buffer. But it is the screen's image, one sample a pixel, and
 * Windows opens it before it makes the back buffer that says how many samples the device
 * has. So when it becomes the render target it is given what a multisampled back buffer has
 * from its creation: a private multisampled image to draw into (pipe_resource), resolved
 * into the image on screen (display_resource) when it is presented. In a device without
 * multisampling it loses that image again. */
static void
D3D9MatchStandardPrimarySamples(D3D9Device *device, D3D9Resource *resource)
{
   if (!resource || !resource->runtime_standard_primary || !resource->pipe_resource ||
       !device->screen)
      return;

   const struct pipe_resource *shown = resource->display_resource ?
      resource->display_resource : resource->pipe_resource;
   const unsigned wanted = MAX2(device->primary_samples, 1u);
   if (wanted == MAX2(resource->pipe_resource->nr_samples, 1u))
      return;

   if (wanted > 1) {
      struct pipe_resource templ;
      memset(&templ, 0, sizeof(templ));
      templ.target = PIPE_TEXTURE_2D;
      templ.format = shown->format;
      templ.width0 = shown->width0;
      templ.height0 = shown->height0;
      templ.depth0 = 1;
      templ.array_size = 1;
      templ.nr_samples = wanted;
      templ.nr_storage_samples = wanted;
      templ.bind = PIPE_BIND_RENDER_TARGET;
      templ.usage = PIPE_USAGE_DEFAULT;
      struct pipe_resource *drawn = device->screen->resource_create(device->screen, &templ);
      if (!drawn)
         return;
      if (resource->display_resource)
         pipe_resource_reference(&resource->pipe_resource, NULL);
      else
         resource->display_resource = resource->pipe_resource;
      resource->pipe_resource = drawn;
   } else {
      pipe_resource_reference(&resource->pipe_resource, NULL);
      resource->pipe_resource = resource->display_resource;
      resource->display_resource = NULL;
   }
}

HRESULT APIENTRY
D3D9SetRenderTarget(HANDLE hDevice, const D3DDDIARG_SETRENDERTARGET *data)
{

   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data ||
       data->RenderTargetIndex >= ARRAYSIZE(device->render_targets))
      return E_INVALIDARG;

   device->render_targets[data->RenderTargetIndex] = data->hRenderTarget;
   device->render_target_subresources[data->RenderTargetIndex] =
      data->SubResourceIndex;
   D3D9MatchStandardPrimarySamples(device, D3D9CastResource(data->hRenderTarget));
#ifndef RIPPLE_VISTA
   if (data->RenderTargetIndex == 0) {
      D3D9SubResource *sub =
         D3D9GetSubResource(data->hRenderTarget, data->SubResourceIndex);
      if (sub) {
         device->viewport.X = 0;
         device->viewport.Y = 0;
         device->viewport.Width = sub->width;
         device->viewport.Height = sub->height;
      }
   }
#endif
   /* Vista: the viewport stays as the runtime last set it. The runtime sends a new one
    * when the program changes it, but not when it moves a flip chain's drawing to the
    * next buffer. The compositor redraws only what changed, through a viewport
    * that small: reset to the whole surface, that piece covered the screen. */
   return S_OK;
}

HRESULT APIENTRY
D3D9SetDepthStencil(HANDLE hDevice, const D3DDDIARG_SETDEPTHSTENCIL *data)
{

   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   device->depth_stencil = data->hZBuffer;
   return S_OK;
}

HRESULT APIENTRY
D3D9CreateResource(HANDLE hDevice, D3DDDIARG_CREATERESOURCE *data)
{

   if (!hDevice || !data || !data->pSurfList || !data->SurfCount)
      return E_INVALIDARG;

   D3D9Device *device = (D3D9Device *)hDevice;
   const bool yttrium = D3D9IsYttriumScreen(device->screen);
   D3D9Resource *resource = (D3D9Resource *)calloc(1, sizeof(*resource));
   bool runtime_owns_private_allocation = false;
   if (!resource)
      return E_OUTOFMEMORY;

   resource->runtime_handle = data->hResource;
   resource->format = data->Format;
   resource->pool = data->Pool;
   resource->flags = data->Flags;
   resource->surf_count = data->SurfCount;
   resource->mip_levels = data->MipLevels;
   resource->surfaces = (D3D9SubResource *)calloc(data->SurfCount,
                                                  sizeof(*resource->surfaces));
   if (!resource->surfaces) {
      free(resource);
      return E_OUTOFMEMORY;
   }

   for (UINT i = 0; i < data->SurfCount; ++i) {
      const D3DDDI_SURFACEINFO *surface = &data->pSurfList[i];
      D3D9SubResource *sub = &resource->surfaces[i];
      sub->width = surface->Width;
      sub->height = surface->Height ? surface->Height : 1;
      sub->depth = surface->Depth ? surface->Depth : 1;
      sub->pitch = D3D9ResourcePitch(data, surface);
      sub->slice_pitch = D3D9ResourceSlicePitch(data, surface, sub->pitch);
      sub->size = D3D9ResourceDimensionSize(data, surface);

      if (!sub->size)
         sub->size = 1;

      HRESULT hr = D3D9CopyInitialData(data, sub, surface);
      if (FAILED(hr)) {
         D3D9FreeResource(resource);
         return hr;
      }
   }

   /* A plain surface kept in memory (D3D9IsPlainInMemory): from here on it behaves as a
    * system-memory surface whose memory the driver allocates at the first use. */
   const bool plain_in_memory = D3D9IsPlainInMemory(device, data);
   if (plain_in_memory)
      resource->pool = D3DDDIPOOL_SYSTEMMEM;

   /* Plain system-memory surfaces need no Gallium resource or screen. */
   const bool cpu_only_plain = plain_in_memory ||
      (!D3D9PoolNeedsReadback(data->Pool) &&
       !(data->Flags.VertexBuffer || data->Flags.IndexBuffer ||
         data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume ||
         D3D9ResourceIsColorTarget(data) || data->Flags.ZBuffer));

   if (!cpu_only_plain && device->screen) {
      const enum pipe_format pipe_format = D3D9ResourcePipeFormat(data);
      if (pipe_format != PIPE_FORMAT_NONE) {
         struct pipe_resource templ;
         memset(&templ, 0, sizeof(templ));

         templ.target = D3D9PipeTarget(data);
         templ.format = pipe_format;
         templ.width0 = resource->surfaces[0].width;
         templ.height0 = templ.target == PIPE_BUFFER ?
            1 : resource->surfaces[0].height;
         templ.depth0 = templ.target == PIPE_BUFFER ?
            1 : resource->surfaces[0].depth;
         templ.array_size = D3D9PipeArraySize(data);
         templ.last_level = resource->mip_levels ? resource->mip_levels - 1 : 0;
         if (data->Flags.AutogenMipmap && templ.target != PIPE_BUFFER) {
            const UINT max_dimension =
               MAX3(templ.width0, templ.height0, templ.depth0);
            templ.last_level = max_dimension ? util_logbase2(max_dimension) : 0;
         }
         /* moose-ripple: on virgl too a multisampled primary is two images, the one on
          * screen and a private multisampled one to draw into, resolved at each present
          * (below, and D3D9CopyToDisplayResource). Windows 7 makes a full-screen back
          * buffer as a primary of one surface. Without the pair it was drawn to with one
          * sample a pixel under a depth buffer with several, which no host accepts. */
         const bool msaa_primary = data->Flags.Primary &&
            (yttrium ? data->MultisampleType && data->SurfCount > 1 :
                       data->MultisampleType > 1);
         /* Both bind SHARED and the DDI SharedResource flag can describe
          * runtime-managed storage. Keep the existing conservative split
          * qualification; neither bit proves application sharing intent. */
         const bool optimal_local_display = yttrium &&
            !data->Flags.SharedResource &&
            D3D9PoolNeedsReadback(data->Pool) &&
            D3D9ResourceIsColorTarget(data) &&
            templ.target == PIPE_TEXTURE_2D && templ.array_size == 1 &&
            !templ.last_level &&
            (D3D9PipeBind(data) & PIPE_BIND_DISPLAY_TARGET);
         templ.nr_samples = data->MultisampleType ? data->MultisampleType : 1;
         if (data->Flags.Primary && data->MultisampleType && !msaa_primary)
            templ.nr_samples = 1;
         templ.nr_storage_samples = templ.nr_samples;
         templ.bind = D3D9PipeBind(data);
         if (D3D9ResourceIsDDrawPresentSource(device, data))
            templ.bind |= PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                          PIPE_BIND_SHARED;
         templ.usage = D3D9PipeUsage(data);
         /* Non-texture render-target surfaces can be legacy CPU-blt sources
          * even without DiscardRenderTarget. Keep those conservative until
          * their actual Present is qualified. */
         if (yttrium && templ.target != PIPE_BUFFER &&
             !data->Flags.SharedResource &&
             (data->Flags.Texture || data->Flags.Primary))
            templ.flags |= YTTRIUM_GDI_RESOURCE_FLAG_LOCAL_RENDER;
         if (yttrium && data->Flags.DiscardRenderTarget)
            templ.flags |= YTTRIUM_GDI_RESOURCE_FLAG_CPU_READBACK;
         if (msaa_primary)
            templ.bind &= ~(PIPE_BIND_DISPLAY_TARGET | (yttrium ? 0 : PIPE_BIND_SHARED));
         else if (yttrium && data->Flags.Primary)
            templ.flags |= PIPE_RESOURCE_FLAG_FRONTEND_PRIV;

         struct pipe_resource display_templ = templ;
         if (optimal_local_display) {
            templ.bind &= ~(PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET);
            templ.flags &= ~(PIPE_RESOURCE_FLAG_FRONTEND_PRIV |
                             YTTRIUM_GDI_RESOURCE_FLAG_CPU_READBACK);
            templ.flags |= YTTRIUM_GDI_RESOURCE_FLAG_LOCAL_RENDER;
         }

         HANDLE old_hRTResource = device->gdi_device.hRTResource;
         boolean old_hRTResourceIsD3D9 =
            device->gdi_device.hRTResourceIsD3D9;
         UINT old_allocationVidPn = device->gdi_device.allocationVidPn;
         boolean old_isPrimary = device->gdi_device.isPrimary;
         /* moose-ripple: allocations belong to the runtime's resource only where the
          * runtime has to find them through it, primaries and shared resources. The
          * rest are the driver's own: the virgl winsys keeps some for reuse, and frees
          * others only when the commands that use them have gone to the host, both
          * after the runtime's resource is gone. */
         device->gdi_device.hRTResource =
            (data->Flags.Primary || data->Flags.SharedResource) ?
            resource->runtime_handle : NULL;
         device->gdi_device.hRTResourceIsD3D9 = true;
         device->gdi_device.allocationVidPn =
            data->Flags.Primary ? data->VidPnSourceId : 0;
         device->gdi_device.isPrimary = data->Flags.Primary;

         D3DKMT_HANDLE display_allocation = 0;
         if (msaa_primary || optimal_local_display) {
            display_templ.nr_samples = 1;
            display_templ.nr_storage_samples = 1;
            display_templ.bind = D3D9PipeBind(data);
            if (data->Flags.Primary)
               display_templ.flags |= PIPE_RESOURCE_FLAG_FRONTEND_PRIV;
            if (optimal_local_display) {
               /* The linear child is copied and sampled, never rendered with
                * depth; the private optimal image owns rendering. */
               display_templ.bind &= ~PIPE_BIND_RENDER_TARGET;
               display_templ.bind |= PIPE_BIND_DISPLAY_TARGET;
               display_templ.flags |= YTTRIUM_GDI_RESOURCE_FLAG_CPU_READBACK;
            }
            resource->display_resource =
               device->screen->resource_create(device->screen,
                                               &display_templ);
            if (!resource->display_resource) {
               device->gdi_device.hRTResource = old_hRTResource;
               device->gdi_device.hRTResourceIsD3D9 =
                  old_hRTResourceIsD3D9;
               device->gdi_device.allocationVidPn = old_allocationVidPn;
               device->gdi_device.isPrimary = old_isPrimary;
               D3D9FreeResource(resource);
               return E_OUTOFMEMORY;
            }
            display_allocation =
               D3D9GetPipeResourceAllocation(
                  device, resource->display_resource);
            if (!display_allocation) {
               D3D9Warnf("ERROR D3D9 display resource "
                         "has no D3DKMT allocation resource=%p pipe=%p\n",
                         resource, resource->display_resource);
               device->gdi_device.hRTResource = old_hRTResource;
               device->gdi_device.hRTResourceIsD3D9 =
                  old_hRTResourceIsD3D9;
               device->gdi_device.allocationVidPn = old_allocationVidPn;
               device->gdi_device.isPrimary = old_isPrimary;
               D3D9FreeResource(resource);
               return E_OUTOFMEMORY;
            }

            device->gdi_device.allocationVidPn = 0;
            device->gdi_device.isPrimary = false;
            device->gdi_device.hRTResource = NULL;
            device->gdi_device.hRTResourceIsD3D9 = false;
         }

         bool flip_chain = false;
#ifdef RIPPLE_VISTA
         /* Vista makes a primary flip chain (the compositor's, a full-screen game's) one
          * resource of several surfaces; Windows 7 makes each buffer a resource of its own.
          * Each surface gets an image of its own for Windows to flip to. All of them are
          * drawn in the one pipe_resource below, copied to the surface's image at its flip
          * (D3D9FlushFrontbuffer). With one image for the whole chain, the compositor drew
          * into the buffer on screen: the picture flickered and was mostly missing. */
         flip_chain = data->Flags.Primary && data->SurfCount > 1 &&
            !msaa_primary && !optimal_local_display;
         if (flip_chain) {
            struct pipe_resource flip_templ = templ;
            flip_templ.nr_samples = 1;
            flip_templ.nr_storage_samples = 1;
            flip_templ.flags |= PIPE_RESOURCE_FLAG_FRONTEND_PRIV;
            for (UINT i = 0; i < resource->surf_count; ++i) {
               D3D9SubResource *sub = &resource->surfaces[i];
               sub->flip_resource =
                  device->screen->resource_create(device->screen, &flip_templ);
               sub->allocation = sub->flip_resource ?
                  D3D9GetPipeResourceAllocation(device, sub->flip_resource) : 0;
               if (!sub->allocation) {
                  D3D9Warnf("ERROR D3D9 flip chain buffer %u of %u has no allocation "
                            "resource=%p\n", i, resource->surf_count, resource);
                  device->gdi_device.hRTResource = old_hRTResource;
                  device->gdi_device.hRTResourceIsD3D9 = old_hRTResourceIsD3D9;
                  device->gdi_device.allocationVidPn = old_allocationVidPn;
                  device->gdi_device.isPrimary = old_isPrimary;
                  D3D9FreeResource(resource);
                  return E_OUTOFMEMORY;
               }
            }
            /* the image drawn in is the driver's own */
            device->gdi_device.allocationVidPn = 0;
            device->gdi_device.isPrimary = false;
            device->gdi_device.hRTResource = NULL;
            device->gdi_device.hRTResourceIsD3D9 = false;
         }
#endif

         resource->pipe_resource =
            device->screen->resource_create(device->screen, &templ);
         device->gdi_device.hRTResource = old_hRTResource;
         device->gdi_device.hRTResourceIsD3D9 = old_hRTResourceIsD3D9;
         device->gdi_device.allocationVidPn = old_allocationVidPn;
         device->gdi_device.isPrimary = old_isPrimary;

         if (!resource->pipe_resource) {
            D3D9FreeResource(resource);
            return E_OUTOFMEMORY;
         }
         /* what the screen's own primary has to match when it takes its turn as the
          * back buffer (D3D9MatchStandardPrimarySamples) */
         if (data->Flags.Primary)
            device->primary_samples = msaa_primary ? templ.nr_samples : 1;

         /*
          * A multisampled primary has two Gallium resources: the runtime-
          * associated single-sample display image and a private multisample
          * render image.  The D3D9 runtime tracks the latter allocation as
          * part of the primary resource even though it was allocated through
          * the standalone HandleList path, and retires it during Reset before
          * DestroyResource drops the Gallium reference.  Transfer allocation
          * ownership to the runtime once creation can no longer fail; Venus
          * object teardown remains owned by the pipe resource.
          */
         runtime_owns_private_allocation = msaa_primary && !optimal_local_display;

         const D3DKMT_HANDLE allocation =
            D3D9GetPipeAllocation(device, resource);
         const D3DKMT_HANDLE render_allocation = msaa_primary &&
            !optimal_local_display ? D3D9GetPipeResourceAllocation(
               device, resource->pipe_resource) : allocation;
         for (UINT i = 0; i < resource->surf_count && !flip_chain; ++i) {
            if (optimal_local_display || (msaa_primary && i == 0))
               resource->surfaces[i].allocation = display_allocation;
            else
               resource->surfaces[i].allocation = render_allocation;
         }
         if (!allocation && D3D9ResourceIsColorTarget(data) &&
             (templ.bind & (PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET))) {
            D3D9Warnf("unsupported pipe resource has no D3DKMT allocation "
                      "resource=%p pipe=%p format=%u bind=0x%x\n",
                      resource, resource->pipe_resource, templ.format,
                      templ.bind);
         }

         /* A plain system-memory texture may wrap a window bitmap solely
          * for a Blt. Its pixels are already in sub->data. Keep cpu_dirty
          * set until an operation actually reads its own Gallium image. */
         D3DDDI_RESOURCEFLAGS texture_only = {};
         texture_only.Texture = 1;
         const bool borrowed_bitmap = data->Pool == D3DDDIPOOL_SYSTEMMEM &&
            data->Flags.Value == texture_only.Value && data->SurfCount == 1 &&
            data->MipLevels <= 1 && data->pSurfList[0].pSysMem &&
            (data->Format == D3DDDIFMT_A8R8G8B8 || data->Format == D3DDDIFMT_X8R8G8B8);
         for (UINT i = 0; !borrowed_bitmap && i < resource->surf_count; ++i) {
            if (!D3D9UploadSubResource(device, resource, i)) {
               D3D9FreeResource(resource);
               return E_NOTIMPL;
            }
         }

         /* Creating a texture does not identify an upload source. The most recently
          * created system-memory texture may be unrelated even when its dimensions
          * match. Reading it here crashed the compositor when its borrowed bitmap
          * was no longer mapped. TexBlt supplies the actual source when the runtime
          * uploads a managed texture; leave that work to the explicit copy. */
         if ((data->Flags.VertexBuffer || data->Flags.IndexBuffer) &&
             data->Pool != D3DDDIPOOL_SYSTEMMEM &&
             D3D9ResourcesMatchForManagedBufferUpload(
                device->last_systemmem_buffer, resource)) {
            resource->managed_source_resource =
               device->last_systemmem_buffer;
            device->last_systemmem_buffer->managed_default_resource =
               resource;
         }
         resource->may_watch_buffer =
            data->Pool == D3DDDIPOOL_VIDEOMEMORY &&
            (data->Flags.VertexBuffer || data->Flags.IndexBuffer) &&
            data->Flags.Dynamic && data->Flags.WriteOnly && !data->Flags.SharedResource &&
            !data->Flags.MightDrawFromLocked && !resource->managed_source_resource &&
            data->SurfCount == 1 && !data->pSurfList[0].pSysMem &&
            resource->surfaces[0].size <= INT_MAX && device->pipe && device->pipe->buffer_map;
      } else {
         D3D9Warnf("unsupported CPU-only resource for unsupported "
                   "format=%u pool=%u flags=0x%08x resource=%p\n",
                   data->Format, data->Pool, data->Flags.Value, resource);
      }
   } else if (!cpu_only_plain) {
      D3D9Warnf("unsupported CPU-only resource without Yttrium screen "
                "format=%u pool=%u flags=0x%08x resource=%p\n",
                data->Format, data->Pool, data->Flags.Value, resource);
   }
   if (runtime_owns_private_allocation)
      yttrium_gdi_resource_set_allocation_ownership(resource->pipe_resource,
                                                    false);

   data->hResource = resource;
   D3D9RegisterResource(device, resource);
   if (D3D9ResourceIsColorTarget(data) && !device->render_targets[0]) {
      device->render_targets[0] = resource;
      device->render_target_subresources[0] = 0;
   }
   if (data->Flags.ZBuffer && !device->depth_stencil) {
      device->depth_stencil = resource;
   }
   if (data->Pool == D3DDDIPOOL_SYSTEMMEM && D3D9ResourceIsTexture(resource))
      device->last_systemmem_texture = resource;
   if (data->Pool == D3DDDIPOOL_SYSTEMMEM &&
       (data->Flags.VertexBuffer || data->Flags.IndexBuffer))
      device->last_systemmem_buffer = resource;
   return S_OK;
}

HRESULT APIENTRY
D3D9OpenResource(HANDLE hDevice, D3DDDIARG_OPENRESOURCE *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;

   if (!device || !data || !data->hKMResource || !data->NumAllocations ||
       !device->screen || !device->screen->resource_from_handle)
      return E_INVALIDARG;

   /* moose-ripple: the screen's own primary, which Windows opens for a full-screen device,
    * is opened like any other resource. A full-screen swap chain flips between it and the
    * back buffer the device creates, so every second frame is drawn into it; left without
    * a Gallium resource, as it was, the first Clear after the first flip failed and the
    * program got an error for its second frame. Only if it cannot be opened does it stay
    * without one (the form below), so that the device can at least be made. */
   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *standard_primary_alloc = NULL;
   const bool standard_primary =
      D3D9OpenResourceIsRuntimeStandardPrimary(data, &standard_primary_alloc);

   struct winsys_handle whandle;
   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_WIN32_HANDLE;
   whandle.handle = (HANDLE)(uintptr_t)data->hKMResource;

   HANDLE old_hRTResource = device->gdi_device.hRTResource;
   boolean old_hRTResourceIsD3D9 =
      device->gdi_device.hRTResourceIsD3D9;
   const D3DDDIARG_OPENRESOURCE *old_d3d9_open =
      device->gdi_device.pD3D9OpenResource;
   device->gdi_device.hRTResource = data->hResource;
   device->gdi_device.hRTResourceIsD3D9 = true;
   device->gdi_device.pD3D9OpenResource = data;
   struct pipe_resource *pipe_resource =
      device->screen->resource_from_handle(device->screen, NULL, &whandle, 0);
   device->gdi_device.pD3D9OpenResource = old_d3d9_open;
   device->gdi_device.hRTResource = old_hRTResource;
   device->gdi_device.hRTResourceIsD3D9 = old_hRTResourceIsD3D9;

   if (!pipe_resource && standard_primary) {
      const D3DDDIFORMAT d3d_format =
         D3D9VirglToFormat(standard_primary_alloc->ResourceOptions.format);
      if (d3d_format == D3DDDIFMT_UNKNOWN)
         return E_NOTIMPL;

      const UINT mip_levels =
         standard_primary_alloc->ResourceOptions.last_level + 1;
      const UINT array_size =
         standard_primary_alloc->ResourceOptions.array_size ?
         standard_primary_alloc->ResourceOptions.array_size : 1;
      const UINT surf_count = mip_levels * array_size;
      D3D9Resource *resource =
         (D3D9Resource *)calloc(1, sizeof(*resource));
      if (!resource)
         return E_OUTOFMEMORY;

      resource->surfaces =
         (D3D9SubResource *)calloc(surf_count, sizeof(*resource->surfaces));
      if (!resource->surfaces) {
         free(resource);
         return E_OUTOFMEMORY;
      }

      resource->runtime_handle = data->hResource;
      resource->format = d3d_format;
      resource->pool = D3DDDIPOOL_VIDEOMEMORY;
      resource->surf_count = surf_count;
      resource->mip_levels = mip_levels;
      resource->flags.RenderTarget =
         (standard_primary_alloc->ResourceOptions.bind &
          VIRGL_BIND_RENDER_TARGET) != 0;
      resource->flags.Primary = 1;
      resource->runtime_standard_primary = true;

      const D3DKMT_HANDLE allocation =
         data->pOpenAllocationInfo[0].hAllocation;
      for (UINT layer = 0; layer < array_size; ++layer) {
         for (UINT level = 0; level < mip_levels; ++level) {
            const UINT index = layer * mip_levels + level;
            D3D9SubResource *sub = &resource->surfaces[index];
            sub->width =
               MAX2(standard_primary_alloc->ResourceOptions.width >> level,
                    1u);
            sub->height =
               MAX2(standard_primary_alloc->ResourceOptions.height >> level,
                    1u);
            sub->depth =
               MAX2(standard_primary_alloc->ResourceOptions.depth >> level,
                    1u);
            sub->pitch =
               D3D9AlignPitch(D3D9FormatRowPitch(d3d_format, sub->width));
            sub->slice_pitch =
               sub->pitch * D3D9FormatRowCount(d3d_format, sub->height);
            sub->size = (size_t)sub->slice_pitch * sub->depth;
            sub->allocation = allocation;
         }
      }

      data->hResource = resource;
      D3D9RegisterResource(device, resource);
      D3D9Warnf("WARNING: D3D9 opened runtime standard-primary fullscreen "
                "without Gallium backing resource=%p km=0x%lx "
                "allocation=0x%lx %ux%u format=%u bind=0x%lx "
                "owner=d3d9_runtime reason=standard_primary_not_opened\n",
                resource, (unsigned long)data->hKMResource,
                (unsigned long)allocation,
                standard_primary_alloc->ResourceOptions.width,
                standard_primary_alloc->ResourceOptions.height,
                d3d_format,
                (unsigned long)standard_primary_alloc->ResourceOptions.bind);
      return S_OK;
   }

   if (!pipe_resource)
      return E_OUTOFMEMORY;

   const D3DDDIFORMAT d3d_format = D3D9PipeToFormat(pipe_resource->format);
   if (d3d_format == D3DDDIFMT_UNKNOWN ||
       pipe_resource->target == PIPE_BUFFER) {
      pipe_resource_reference(&pipe_resource, NULL);
      return E_NOTIMPL;
   }

   const UINT mip_levels = pipe_resource->last_level + 1;
   const UINT array_size = pipe_resource->array_size ?
      pipe_resource->array_size : 1;
   const UINT surf_count = mip_levels * array_size;
   D3D9Resource *resource = (D3D9Resource *)calloc(1, sizeof(*resource));
   if (!resource) {
      pipe_resource_reference(&pipe_resource, NULL);
      return E_OUTOFMEMORY;
   }

   resource->surfaces = (D3D9SubResource *)calloc(surf_count,
                                                  sizeof(*resource->surfaces));
   if (!resource->surfaces) {
      free(resource);
      pipe_resource_reference(&pipe_resource, NULL);
      return E_OUTOFMEMORY;
   }

   resource->runtime_handle = data->hResource;
   resource->format = d3d_format;
   resource->pool = D3DDDIPOOL_VIDEOMEMORY;
   resource->surf_count = surf_count;
   resource->mip_levels = mip_levels;
   resource->pipe_resource = pipe_resource;
   resource->flags.RenderTarget =
      (pipe_resource->bind & PIPE_BIND_RENDER_TARGET) != 0;
   resource->flags.ZBuffer =
      (pipe_resource->bind & PIPE_BIND_DEPTH_STENCIL) != 0;
   resource->flags.Primary =
      standard_primary || (pipe_resource->bind & PIPE_BIND_DISPLAY_TARGET) != 0;
   resource->runtime_standard_primary = standard_primary;

   D3DKMT_HANDLE allocation = D3D9GetPipeAllocation(device, resource);
   if (!allocation && data->pOpenAllocationInfo && data->NumAllocations)
      allocation = data->pOpenAllocationInfo[0].hAllocation;
   for (UINT layer = 0; layer < array_size; ++layer) {
      for (UINT level = 0; level < mip_levels; ++level) {
         const UINT index = layer * mip_levels + level;
         D3D9SubResource *sub = &resource->surfaces[index];
         sub->width = MAX2(pipe_resource->width0 >> level, 1u);
         sub->height = MAX2(pipe_resource->height0 >> level, 1u);
         sub->depth = MAX2(pipe_resource->depth0 >> level, 1u);
         sub->pitch = D3D9AlignPitch(D3D9FormatRowPitch(d3d_format,
                                                        sub->width));
         sub->slice_pitch =
            sub->pitch * D3D9FormatRowCount(d3d_format, sub->height);
         sub->size = (size_t)sub->slice_pitch * sub->depth;
         sub->allocation = allocation;
      }
   }

   data->hResource = resource;
   D3D9RegisterResource(device, resource);
   return S_OK;
}

HRESULT APIENTRY
D3D9Lock(HANDLE hDevice, D3DDDIARG_LOCK *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   UINT subresource_index = D3D9ResolveNotifyLockSubResource(resource, data);
   if (subresource_index >= resource->surf_count) {
      return E_INVALIDARG;
   }

   D3D9SubResource *sub = &resource->surfaces[subresource_index];
   const bool buffer_resource =
      (resource->pipe_resource &&
       resource->pipe_resource->target == PIPE_BUFFER) ||
      resource->flags.VertexBuffer || resource->flags.IndexBuffer;
   if (resource->flags.VertexBuffer && D3D9OrderedContextWorkerEnabled())
      D3D9ReleaseWorkerBufferUpload(device, sub);

   if (data->Flags.NotifyOnly) {
      D3D9ReleaseWatchedBuffer(resource);
      if (buffer_resource && D3D9EnsureCpuBacking(sub)) {
         UINT offset = 0;
         UINT size = (UINT)sub->size;
         if (data->Flags.RangeValid) {
            offset = data->Range.Offset;
            if (offset > sub->size)
               return E_INVALIDARG;
            size = data->Range.Size ? data->Range.Size :
               (UINT)(sub->size - offset);
         }
         if (offset > sub->size || size > sub->size - offset)
            return E_INVALIDARG;

         data->pSurfData = sub->data + offset;
         data->Pitch = 0;
         data->SlicePitch = 0;
         sub->notify_only_locked = true;
         sub->notify_only_write = !data->Flags.ReadOnly;
         sub->notify_only_full_mip_chain = false;
         return S_OK;
      }

      if (!D3D9PoolNeedsReadback(resource->pool) &&
          !(resource->flags.VertexBuffer || resource->flags.IndexBuffer)) {
         struct pipe_box box;
         memset(&box, 0, sizeof(box));
         if (data->Flags.BoxValid)
            D3D9PipeBoxFromD3DBox(&data->Box, &box);
         else if (data->Flags.AreaValid)
            D3D9PipeBoxFromRect(&data->Area, &box);
         else {
            box.width = sub->width;
            box.height = sub->height;
            box.depth = sub->depth ? sub->depth : 1;
         }

         if (box.width < 0 || box.height < 0 || box.depth < 0 ||
             box.x > sub->width || box.y > sub->height ||
             box.z > sub->depth ||
             (UINT)box.width > sub->width - box.x ||
             (UINT)box.height > sub->height - box.y ||
             (UINT)box.depth > sub->depth - box.z)
            return E_INVALIDARG;

         const UINT bpp = D3D9FormatBytesPerPixel(resource->format);
         if (bpp && D3D9EnsureCpuBacking(sub)) {
            data->Pitch = sub->pitch;
            data->SlicePitch = sub->slice_pitch;
            data->pSurfData = sub->data + (size_t)box.z * sub->slice_pitch +
               (size_t)box.y * sub->pitch + (size_t)box.x * bpp;
            sub->notify_only_locked = true;
            sub->notify_only_write = !data->Flags.ReadOnly;
            sub->notify_only_full_mip_chain =
               D3D9NotifyLockCoversFullMipChain(resource, data);
            return S_OK;
         }
      }

      sub->notify_only_locked = true;
      sub->notify_only_write = !data->Flags.ReadOnly;
      sub->notify_only_full_mip_chain =
         D3D9NotifyLockCoversFullMipChain(resource, data);
      data->pSurfData = NULL;
      data->Pitch = 0;
      data->SlicePitch = 0;
      return S_OK;
   }

   if (sub->transfer || sub->cpu_locked) {
      if (sub->transfer && resource->pipe_resource &&
         resource->pipe_resource->target == PIPE_BUFFER &&
         data->Flags.Discard) {
         if (!device->pipe || !device->pipe->buffer_unmap)
            return E_NOTIMPL;
         device->pipe->buffer_unmap(device->pipe, sub->transfer);
         sub->transfer = NULL;
         sub->transfer_map = NULL;
         sub->transfer_on_readback = false;
         sub->transfer_write = false;
         memset(&sub->transfer_box, 0, sizeof(sub->transfer_box));
      } else {
         return E_INVALIDARG;
      }
   }

   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   const unsigned usage = D3D9PipeMapUsage(data->Flags);
   void *map = NULL;

   if (buffer_resource) {
      if (!resource->pipe_resource || !device->pipe) {
         D3D9Warnf("unsupported buffer lock without pipe resource "
                   "resource=%p sub=%u flags=0x%08x\n",
                   resource, subresource_index, data->Flags.Value);
         return E_NOTIMPL;
      }
      if (!device->pipe->buffer_map)
         return E_NOTIMPL;

      box.x = data->Flags.RangeValid ? data->Range.Offset : 0;
      box.width = data->Flags.RangeValid && data->Range.Size ?
         data->Range.Size : (int)(sub->size - box.x);
      box.height = 1;
      box.depth = 1;
      if (box.x > sub->size || box.width < 0 ||
          (size_t)box.width > sub->size - box.x)
         return E_INVALIDARG;

      /* An initially empty private vertex buffer can start streaming at any
       * offset. Its shadow preserves neighboring vertices on later partial
       * writes. Index buffers keep the compositor's whole-buffer first lock. */
      if (resource->may_watch_buffer) {
         resource->may_watch_buffer = false;
         const bool whole_buffer = !box.x && (size_t)box.width == sub->size;
         if ((whole_buffer || (resource->flags.VertexBuffer && box.width > 0 &&
                               !data->Flags.ReadOnly)) &&
             (data->Flags.NoOverwrite || data->Flags.Discard)) {
            resource->watched_buffer = (uint8_t *)VirtualAlloc(
               NULL, sub->size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
         }
      }
      if (resource->watched_buffer) {
         resource->watched_append = data->Flags.NoOverwrite && !data->Flags.Discard;
         if (data->Flags.Discard)
            resource->watched_discard = true;
         data->pSurfData = resource->watched_buffer + box.x;
         data->Pitch = data->SlicePitch = 0;
         sub->cpu_locked = true;
         return S_OK;
      }
      if (D3D9ResourceUsesCpuBufferStorage(resource)) {
         if (!D3D9EnsureCpuBacking(sub))
            return E_OUTOFMEMORY;

         data->pSurfData = sub->data + box.x;
         data->Pitch = 0;
         data->SlicePitch = 0;
         sub->cpu_locked = true;
         sub->cpu_lock_write = (usage & PIPE_MAP_WRITE) != 0;
         sub->cpu_lock_packed_mips = false;
         return S_OK;
      }

      map = device->pipe->buffer_map(device->pipe, resource->pipe_resource,
                                     D3D9PipeMipLevel(resource,
                                                      subresource_index),
                                     usage, &box, &sub->transfer);
      data->Pitch = 0;
      data->SlicePitch = 0;
      if (sub->transfer) {
         sub->transfer_write = (usage & PIPE_MAP_WRITE) != 0;
         sub->transfer_box = box;
         sub->transfer_map = map;
      }
   } else {
      if (data->Flags.BoxValid) {
         D3D9PipeBoxFromD3DBox(&data->Box, &box);
      } else if (data->Flags.AreaValid) {
         D3D9PipeBoxFromRect(&data->Area, &box);
      } else {
         box.width = sub->width;
         box.height = sub->height;
         box.depth = sub->depth ? sub->depth : 1;
      }
      if (box.width < 0 || box.height < 0 || box.depth < 0 ||
          box.x > sub->width || box.y > sub->height || box.z > sub->depth ||
          (UINT)box.width > sub->width - box.x ||
          (UINT)box.height > sub->height - box.y ||
          (UINT)box.depth > sub->depth - box.z)
         return E_INVALIDARG;

      if (!D3D9PoolNeedsReadback(resource->pool)) {
         size_t cpu_backing_size = sub->size;
         const bool packed_mip_lock =
            resource->pool == D3DDDIPOOL_SYSTEMMEM &&
            subresource_index == 0 &&
            (usage & PIPE_MAP_WRITE) &&
            D3D9LockCoversFullSubResource(sub, &box);
         if (packed_mip_lock) {
            const size_t packed_size = D3D9PackedMipChainSize(resource);
            if (packed_size)
               cpu_backing_size = packed_size;
         }

         if (!D3D9EnsureCpuBackingSize(sub, cpu_backing_size))
            return E_OUTOFMEMORY;

         const UINT bpp = D3D9FormatBytesPerPixel(resource->format);
         data->Pitch = sub->pitch;
         data->SlicePitch = sub->slice_pitch;
         data->pSurfData = sub->data + (size_t)box.z * sub->slice_pitch +
            (size_t)box.y * sub->pitch + (size_t)box.x * bpp;
         sub->cpu_locked = true;
         sub->cpu_lock_write = (usage & PIPE_MAP_WRITE) != 0;
         sub->cpu_lock_packed_mips =
            packed_mip_lock && cpu_backing_size > sub->size;
         return S_OK;
      }

      if (!resource->pipe_resource || !device->pipe) {
         D3D9Warnf("unsupported lock without pipe resource resource=%p "
                   "sub=%u flags=0x%08x\n",
                   resource, subresource_index, data->Flags.Value);
         return E_NOTIMPL;
      }

      if (sub->cpu_dirty &&
          !D3D9UploadSubResource(device, resource, subresource_index))
         return E_NOTIMPL;

      /* A write-only lock without Discard still has to keep the texels the
       * caller does not write.  DirectDraw's emulation layer relies on that:
       * for a colour-key or mirror blit the HAL does not offer, it locks
       * the whole destination write-only and stores only the blitted
       * rectangle.  A plain write map of a GPU texture uploads the whole
       * box from an uninitialised staging copy, so such locks go through
       * the readback path, which starts from the current contents. */
      const bool preserve_contents =
         (usage & PIPE_MAP_READ) || !(usage & PIPE_MAP_DISCARD_RANGE);
      if (preserve_contents && D3D9PoolNeedsReadback(resource->pool)) {
         map = D3D9ReadbackTextureLock(device, resource, sub,
                                       subresource_index,
                                       usage | PIPE_MAP_READ, &box, data);
      } else {
         if (!device->pipe->texture_map)
            return E_NOTIMPL;

         map = device->pipe->texture_map(device->pipe,
                                         resource->pipe_resource,
                                         D3D9PipeMipLevel(
                                            resource, subresource_index),
                                         usage, &box,
                                         &sub->transfer);
         if (sub->transfer) {
            data->Pitch = sub->transfer->stride;
            data->SlicePitch = sub->transfer->layer_stride;
            sub->transfer_write = (usage & PIPE_MAP_WRITE) != 0;
            sub->transfer_box = box;
         }
      }
   }

   if (!map || !sub->transfer)
      return E_FAIL;

   data->pSurfData = map;
   return S_OK;
}

/* The caller may change only part of a dirty page. The shadow preserves adjacent data,
 * so a page uploaded again carries the bytes the host already has and the new ones.
 *
 * An append (NoOverwrite) is written unsynchronized: the caller promised that queued
 * draws do not use what it wrote, and the rest of the page is unchanged. An ordered copy
 * costs one wait for the GPU per lock on a host whose GL maps a busy buffer by waiting
 * (ANGLE on Metal), with the host's device thread stopped meanwhile.
 * A discard takes new storage: draws queued before it keep the old one, and an ordered
 * copy left in the queue would land after later appends to the same page and undo them.
 * A lock with neither promise keeps the ordered copy, and so does every upload after it
 * until the next discard, so that none overtakes it. */
static HRESULT
D3D9UploadWatchedBuffer(D3D9Device *device, D3D9Resource *resource, size_t size)
{
   uint8_t *base = resource->watched_buffer;
   void *pages[64];
   ULONG_PTR count = ARRAYSIZE(pages);
   ULONG granularity = 0;
   size_t first = 0, end = size;
   if (GetWriteWatch(0, base, size, pages, &count, &granularity) == 0 &&
       count < ARRAYSIZE(pages)) {
      if (!count)
         return S_OK;
      first = size;
      end = 0;
      for (ULONG_PTR i = 0; i < count; ++i) {
         const size_t offset = (uint8_t *)pages[i] - base;
         first = MIN2(first, offset);
         end = MAX2(end, offset + granularity);
      }
      end = MIN2(end, size);
   }
   /* A full page list or a failed query falls back to the entire buffer. */
   struct pipe_context *pipe = device->pipe;
   unsigned usage = PIPE_MAP_WRITE;
   if ((!first && end == size) || resource->watched_discard)
      usage |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
   else if (resource->watched_append && !resource->watched_ordered)
      usage |= PIPE_MAP_DISCARD_RANGE | PIPE_MAP_UNSYNCHRONIZED;
   else
      usage |= PIPE_MAP_DISCARD_RANGE;
   struct pipe_transfer *transfer = NULL;
   void *map = pipe_buffer_map_range(pipe, resource->pipe_resource, (unsigned)first,
                                     (unsigned)(end - first), usage, &transfer);
   if (!map)
      return E_OUTOFMEMORY;
   memcpy(map, base + first, end - first);
   pipe_buffer_unmap(pipe, transfer);
   if (usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE)
      resource->watched_discard = resource->watched_ordered = false;
   else if (!(usage & PIPE_MAP_UNSYNCHRONIZED))
      resource->watched_ordered = true;
   ResetWriteWatch(base, size);
   return S_OK;
}

HRESULT APIENTRY
D3D9Unlock(HANDLE hDevice, const D3DDDIARG_UNLOCK *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   UINT subresource_index = D3D9ResolveNotifyUnlockSubResource(resource, data);
   if (subresource_index >= resource->surf_count) {
      return E_INVALIDARG;
   }

   D3D9SubResource *sub = &resource->surfaces[subresource_index];
   if (data->Flags.NotifyOnly || sub->notify_only_locked) {
      if (sub->notify_only_write && !resource->managed_default_pipe_resource) {
         const bool full_mip_chain =
            sub->notify_only_full_mip_chain ||
            (resource->pool == D3DDDIPOOL_SYSTEMMEM &&
             subresource_index == 0 && D3D9HasContiguousMipData(resource));
         if (full_mip_chain)
            D3D9MarkMipChainCpuDirty(resource);
         else
            sub->cpu_dirty = true;
         if (resource->pipe_resource &&
             !(full_mip_chain ?
               D3D9UploadDirtyTextureSubResources(device, resource) :
               D3D9UploadSubResource(device, resource, subresource_index)))
            return E_NOTIMPL;
      }
      sub->notify_only_locked = false;
      sub->notify_only_write = false;
      sub->notify_only_full_mip_chain = false;
      return S_OK;
   }

   if (sub->cpu_locked) {
      HRESULT hr = S_OK;
      if (resource->watched_buffer) {
         sub->cpu_locked = false;
         return D3D9UploadWatchedBuffer(device, resource, sub->size);
      }
      if (sub->cpu_lock_write) {
         if (sub->cpu_lock_packed_mips &&
             !D3D9DistributePackedMipLock(resource))
            hr = E_NOTIMPL;
         sub->cpu_dirty = true;
      }
      sub->cpu_locked = false;
      sub->cpu_lock_write = false;
      sub->cpu_lock_packed_mips = false;
      if (FAILED(hr))
         return hr;
      return S_OK;
   }

   if (!sub->transfer) {
      return E_INVALIDARG;
   }

   bool worker_upload_ok = true;
   if (sub->transfer_on_readback) {
      if (!device->pipe->texture_unmap)
         return E_NOTIMPL;

      device->pipe->texture_unmap(device->pipe, sub->transfer);
      sub->transfer = NULL;

      if (sub->transfer_write) {
         if (!device->pipe->resource_copy_region)
            return E_NOTIMPL;

         device->pipe->resource_copy_region(device->pipe,
                                            resource->pipe_resource,
                                            D3D9PipeMipLevel(
                                               resource,
                                               subresource_index),
                                            sub->transfer_box.x,
                                            sub->transfer_box.y,
                                            sub->transfer_box.z,
                                            sub->readback_resource, 0,
                                            &sub->transfer_box);
      }
   } else if (resource->pipe_resource &&
       resource->pipe_resource->target == PIPE_BUFFER) {
      if (!device->pipe->buffer_unmap)
         return E_NOTIMPL;
      if (sub->transfer_write && resource->flags.VertexBuffer &&
          D3D9OrderedContextWorkerEnabled())
         worker_upload_ok = D3D9CaptureWorkerBufferUpload(device, sub);
      device->pipe->buffer_unmap(device->pipe, sub->transfer);
      sub->transfer = NULL;
   } else {
      if (!device->pipe->texture_unmap)
         return E_NOTIMPL;
      device->pipe->texture_unmap(device->pipe, sub->transfer);
      sub->transfer = NULL;
   }
   sub->transfer_on_readback = false;
   sub->transfer_map = NULL;
   if (sub->transfer_write)
      D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
   sub->transfer_write = false;
   memset(&sub->transfer_box, 0, sizeof(sub->transfer_box));
   sub->cpu_dirty = false;
   return worker_upload_ok ? S_OK : E_OUTOFMEMORY;
}

HRESULT APIENTRY
D3D9LockAsync(HANDLE hDevice, D3DDDIARG_LOCKASYNC *data)
{
   if (!data)
      return E_INVALIDARG;

   D3DDDIARG_LOCK lock;
   memset(&lock, 0, sizeof(lock));
   lock.hResource = data->hResource;
   lock.SubResourceIndex = data->SubResourceIndex;
   lock.Flags.WriteOnly = TRUE;
   lock.Flags.NoOverwrite = data->Flags.NoOverwrite;
   lock.Flags.Discard = data->Flags.Discard;
   lock.Flags.RangeValid = data->Flags.RangeValid;
   lock.Flags.AreaValid = data->Flags.AreaValid;
   lock.Flags.BoxValid = data->Flags.BoxValid;
   lock.Flags.NotifyOnly = data->Flags.NotifyOnly;
   if (data->Flags.BoxValid)
      lock.Box = data->Box;
   else if (data->Flags.AreaValid)
      lock.Area = data->Area;
   else
      lock.Range = data->Range;

   HRESULT hr = D3D9Lock(hDevice, &lock);
   if (FAILED(hr))
      return hr;

   data->hCookie = NULL;
   data->pSurfData = lock.pSurfData;
   data->Pitch = lock.Pitch;
   data->SlicePitch = lock.SlicePitch;
   return S_OK;
}

HRESULT APIENTRY
D3D9UnlockAsync(HANDLE hDevice, const D3DDDIARG_UNLOCKASYNC *data)
{
   if (!data)
      return E_INVALIDARG;

   D3DDDIARG_UNLOCK unlock;
   memset(&unlock, 0, sizeof(unlock));
   unlock.hResource = data->hResource;
   unlock.SubResourceIndex = data->SubResourceIndex;
   return D3D9Unlock(hDevice, &unlock);
}

HRESULT APIENTRY
D3D9GetPitch(HANDLE hDevice, D3DDDIARG_GETPITCH *data)
{
   (void)hDevice;
   if (!data || !data->hResource)
      return E_INVALIDARG;

   D3D9SubResource *sub =
      D3D9GetSubResource(data->hResource, data->SubResourceIndex);
   if (!sub)
      return E_INVALIDARG;

   data->Pitch = sub->pitch;
   return S_OK;
}

#if D3D_UMD_INTERFACE_VERSION >= D3D_UMD_INTERFACE_VERSION_WIN7
HRESULT APIENTRY
D3D9ResolveSharedResource(HANDLE hDevice,
                          const D3DDDIARG_RESOLVESHAREDRESOURCE *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   if (!resource)
      return E_INVALIDARG;

   if (resource->pipe_resource && device->pipe) {
      const bool yttrium = D3D9IsYttriumScreen(device->screen);
      if (yttrium && !D3D9PublishResource(device, resource))
         return E_FAIL;
      if (yttrium)
         return yttrium_gdi_flush_handoff(device->pipe,
                                          "D3D9 resolve shared resource") ?
            S_OK : D3DDDIERR_DEVICEREMOVED;
      yttrium_gdi_flush_labeled(device->pipe, NULL,
                                0,
                                "D3D9 resolve shared resource");
   }
   return S_OK;
}

#endif

HRESULT APIENTRY
D3D9QueryResourceResidency(HANDLE hDevice,
                           const D3DDDIARG_QUERYRESOURCERESIDENCY *data)
{
   (void)hDevice;
   if (!data || (!data->pHandleList && data->NumResources))
      return E_INVALIDARG;

   for (UINT i = 0; i < data->NumResources; ++i) {
      D3D9Resource *resource = D3D9CastResource(data->pHandleList[i]);
      if (!resource)
         return E_INVALIDARG;
   }

   return S_OK;
}

HRESULT APIENTRY
D3D9GetCaptureAllocationHandle(HANDLE hDevice,
                               D3DDDIARG_GETCAPTUREALLOCATIONHANDLE *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!data || !data->hResource)
      return E_INVALIDARG;

   D3D9SubResource *sub = D3D9GetSubResource(data->hResource, 0);
   if (!sub || !sub->allocation)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   if (resource->pipe_resource && sub->cpu_dirty &&
       !D3D9UploadSubResource(device, resource, 0))
      return E_FAIL;
   if (resource->display_resource &&
       (!D3D9PublishResource(device, resource) ||
        !yttrium_gdi_flush_handoff(device->pipe, "D3D9 capture allocation")))
      return D3DDDIERR_DEVICEREMOVED;
   data->hAllocation = sub->allocation;
   return S_OK;
}

HRESULT APIENTRY
D3D9CaptureToSysMem(HANDLE hDevice, const D3DDDIARG_CAPTURETOSYSMEM *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data || !data->hSrcResource || !data->hDstResource)
      return E_INVALIDARG;

   HRESULT hr = D3D9CopyRectPipe(device, data->hDstResource,
                                 data->DstSubResourceIndex,
                                 &data->DstRect, data->hSrcResource, 0,
                                 &data->SrcRect,
                                 PIPE_TEX_FILTER_NEAREST) ? S_OK : E_NOTIMPL;
   return hr;
}

/*
 * DirectDraw source colour-key and mirror blits.  The D3D9 runtime passes
 * them to pfnBlt for DirectDraw only, and only when the HAL caps report
 * DDRAW_CAPS_COLORKEY | DDRAW_CKEYCAPS_SRCBLT or DDRAW_FXCAPS_BLTMIRROR*.
 * They are drawn as one quad: the fragment shader maps each destination
 * pixel to a source texel the way a nearest stretch does, mirrors the
 * index, and discards texels equal to the key.  The key is a pixel value
 * in the source format, so it is compared after unpacking with the same
 * conversion the sampler uses, within half a step of the widest channel.
 */
static void *
D3D9CreateKeyBlitVs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_VERTEX);
   if (!ureg)
      return NULL;
   struct ureg_src pos = ureg_DECL_vs_input(ureg, 0);
   struct ureg_dst out_pos = ureg_DECL_output(ureg, TGSI_SEMANTIC_POSITION, 0);
   ureg_MOV(ureg, out_pos, pos);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* The source texel for destination pixel k is the one DirectDraw's emulation
 * layer picks, with s = source / destination size:
 *   no mirror   source + floor((k + 0.5) * s)
 *   any mirror  source + floor(k * s) on both axes, with k counted from the
 *               right (bottom) on a mirrored axis.
 * Constants: c0.xy offset and c1.zw sign, so frag.xy * c1.zw + c0.xy is
 * k + 0.5 or k; c0.zw a quarter-step bias against rounding; c1.xy s; c2.xy
 * the source origin + 0.5, c2.zw 1 / texture size; c3 the key; c4 the
 * per-channel tolerance (> 1 for channels not compared). */
static void *
D3D9CreateKeyBlitFs(struct pipe_context *pipe, bool color_key)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;
   struct ureg_src frag = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_POSITION, 0,
                                             TGSI_INTERPOLATE_LINEAR);
   struct ureg_src c0 = ureg_DECL_constant(ureg, 0);
   struct ureg_src c1 = ureg_DECL_constant(ureg, 1);
   struct ureg_src c2 = ureg_DECL_constant(ureg, 2);
   struct ureg_src c3 = ureg_DECL_constant(ureg, 3);
   struct ureg_src c4 = ureg_DECL_constant(ureg, 4);
   struct ureg_src sampler = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst t = ureg_DECL_temporary(ureg);
   struct ureg_dst texel = ureg_DECL_temporary(ureg);
   const unsigned xy = TGSI_WRITEMASK_XY;
   const struct ureg_src c0_zw = ureg_swizzle(c0, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W,
                                              TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W);
   const struct ureg_src c1_zw = ureg_swizzle(c1, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W,
                                              TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W);
   const struct ureg_src c2_zw = ureg_swizzle(c2, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W,
                                              TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W);

   ureg_MAD(ureg, ureg_writemask(t, xy), frag, c1_zw, c0);
   ureg_MAD(ureg, ureg_writemask(t, xy), ureg_src(t), c1, c0_zw);
   ureg_FLR(ureg, ureg_writemask(t, xy), ureg_src(t));
   ureg_ADD(ureg, ureg_writemask(t, xy), ureg_src(t), c2);
   ureg_MUL(ureg, ureg_writemask(t, xy), ureg_src(t), c2_zw);
   ureg_MOV(ureg, ureg_writemask(t, TGSI_WRITEMASK_ZW), ureg_imm1f(ureg, 0.0f));
   ureg_TEX(ureg, texel, TGSI_TEXTURE_2D, ureg_src(t), sampler);
   if (color_key) {
      /* Discard when every compared channel is within tolerance. */
      ureg_ADD(ureg, t, ureg_src(texel), ureg_negate(c3));
      ureg_SLT(ureg, t, ureg_abs(ureg_src(t)), c4);
      ureg_MUL(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
               ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X),
               ureg_scalar(ureg_src(t), TGSI_SWIZZLE_Y));
      ureg_MUL(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
               ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X),
               ureg_scalar(ureg_src(t), TGSI_SWIZZLE_Z));
      ureg_MUL(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
               ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X),
               ureg_scalar(ureg_src(t), TGSI_SWIZZLE_W));
      ureg_KILL_IF(ureg, ureg_negate(ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X)));
   }
   ureg_MOV(ureg, out, ureg_src(texel));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

static bool
D3D9InitKeyBlitState(D3D9Device *device)
{
   struct pipe_context *pipe = device->pipe;
   if (device->key_blit.vs)
      return true;

   struct pipe_sampler_state sampler;
   memset(&sampler, 0, sizeof(sampler));
   sampler.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.min_img_filter = PIPE_TEX_FILTER_NEAREST;
   sampler.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   sampler.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;

   struct pipe_blend_state blend;
   memset(&blend, 0, sizeof(blend));
   blend.rt[0].colormask = PIPE_MASK_RGBA;

   struct pipe_rasterizer_state rast;
   memset(&rast, 0, sizeof(rast));
   rast.half_pixel_center = 1;
   rast.clip_halfz = 1;
   rast.cull_face = PIPE_FACE_NONE;
   rast.fill_front = PIPE_POLYGON_MODE_FILL;
   rast.fill_back = PIPE_POLYGON_MODE_FILL;
   rast.line_width = 1.0f;
   rast.point_size = 1.0f;

   struct pipe_depth_stencil_alpha_state dsa;
   memset(&dsa, 0, sizeof(dsa));

   struct pipe_vertex_element element;
   memset(&element, 0, sizeof(element));
   element.src_format = PIPE_FORMAT_R32G32_FLOAT;
   element.src_stride = 2 * sizeof(float);

   device->key_blit.sampler = pipe->create_sampler_state(pipe, &sampler);
   device->key_blit.blend = pipe->create_blend_state(pipe, &blend);
   device->key_blit.rasterizer = pipe->create_rasterizer_state(pipe, &rast);
   device->key_blit.dsa = pipe->create_depth_stencil_alpha_state(pipe, &dsa);
   device->key_blit.velems =
      pipe->create_vertex_elements_state(pipe, 1, &element);
   device->key_blit.vs = D3D9CreateKeyBlitVs(pipe);
   if (device->key_blit.sampler && device->key_blit.blend &&
       device->key_blit.rasterizer && device->key_blit.dsa &&
       device->key_blit.velems && device->key_blit.vs)
      return true;
   D3D9DestroyKeyBlitState(device);
   return false;
}

void
D3D9DestroyKeyBlitState(D3D9Device *device)
{
   struct pipe_context *pipe = device ? device->pipe : NULL;
   if (!pipe)
      return;
   if (device->key_blit.velems)
      pipe->delete_vertex_elements_state(pipe, device->key_blit.velems);
   if (device->key_blit.dsa)
      pipe->delete_depth_stencil_alpha_state(pipe, device->key_blit.dsa);
   if (device->key_blit.rasterizer)
      pipe->delete_rasterizer_state(pipe, device->key_blit.rasterizer);
   if (device->key_blit.blend)
      pipe->delete_blend_state(pipe, device->key_blit.blend);
   if (device->key_blit.sampler)
      pipe->delete_sampler_state(pipe, device->key_blit.sampler);
   for (unsigned i = 0; i < ARRAYSIZE(device->key_blit.fs); ++i) {
      if (device->key_blit.fs[i])
         pipe->delete_fs_state(pipe, device->key_blit.fs[i]);
   }
   if (device->key_blit.video_fs)
      pipe->delete_fs_state(pipe, device->key_blit.video_fs);
   if (device->key_blit.video_sampler)
      pipe->delete_sampler_state(pipe, device->key_blit.video_sampler);
   if (device->key_blit.vs)
      pipe->delete_vs_state(pipe, device->key_blit.vs);
   memset(&device->key_blit, 0, sizeof(device->key_blit));
}

static bool
D3D9KeyBlitPipe(D3D9Device *device, const D3DDDIARG_BLT *data)
{
   struct pipe_context *pipe = device->pipe;

   /* this sets the context's state itself and leaves it empty */
   D3D9SettleDrawState(device);

   D3D9Resource *dst_resource = D3D9CastResource(data->hDstResource);
   D3D9Resource *src_resource = D3D9CastResource(data->hSrcResource);
   D3D9SubResource *dst_sub = D3D9GetSubResource(data->hDstResource,
                                                 data->DstSubResourceIndex);
   D3D9SubResource *src_sub = D3D9GetSubResource(data->hSrcResource,
                                                 data->SrcSubResourceIndex);
   if (!pipe || !dst_resource || !src_resource || !dst_sub || !src_sub ||
       !dst_resource->pipe_resource || !src_resource->pipe_resource ||
       !pipe->create_sampler_view)
      return false;

   struct pipe_resource *src_tex = src_resource->pipe_resource;
   struct pipe_resource *dst_tex = dst_resource->pipe_resource;
   const enum pipe_format src_format = src_tex->format;
   if (util_format_is_depth_or_stencil(src_format) ||
       util_format_is_depth_or_stencil(dst_tex->format) ||
       util_format_is_compressed(src_format) ||
       src_tex->nr_samples > 1 || dst_tex->nr_samples > 1 ||
       src_tex->target != PIPE_TEXTURE_2D)
      return false;

   const RECT src = data->SrcRect;
   const RECT dst = data->DstRect;
   const LONG sw = src.right - src.left, sh = src.bottom - src.top;
   const LONG dw = dst.right - dst.left, dh = dst.bottom - dst.top;
   if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
      return true;
   if (src.left < 0 || src.top < 0 || src.right > (LONG)src_sub->width ||
       src.bottom > (LONG)src_sub->height || dst.left < 0 || dst.top < 0 ||
       dst.right > (LONG)dst_sub->width || dst.bottom > (LONG)dst_sub->height)
      return false;

   if (src_sub->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, data->SrcSubResourceIndex))
      return false;
   if (dst_sub->cpu_dirty &&
       !D3D9UploadSubResource(device, dst_resource, data->DstSubResourceIndex))
      return false;

   const unsigned src_level =
      D3D9PipeMipLevel(src_resource, data->SrcSubResourceIndex);
   const unsigned src_layer =
      D3D9PipeLayer(src_resource, data->SrcSubResourceIndex);
   const unsigned dst_level =
      D3D9PipeMipLevel(dst_resource, data->DstSubResourceIndex);
   const unsigned dst_layer =
      D3D9PipeLayer(dst_resource, data->DstSubResourceIndex);

   /* A surface cannot be sampled while it is the render target: blit
    * within one surface from a copy of the source rectangle. */
   struct pipe_resource *copy = NULL;
   unsigned view_level = src_level, view_layer = src_layer;
   LONG src_x = src.left, src_y = src.top;
   unsigned tex_w = u_minify(src_tex->width0, src_level);
   unsigned tex_h = u_minify(src_tex->height0, src_level);
   if (src_tex == dst_tex) {
      struct pipe_resource templ;
      memset(&templ, 0, sizeof(templ));
      templ.target = PIPE_TEXTURE_2D;
      templ.format = src_format;
      templ.width0 = (unsigned)sw;
      templ.height0 = (unsigned)sh;
      templ.depth0 = 1;
      templ.array_size = 1;
      templ.bind = PIPE_BIND_SAMPLER_VIEW;
      copy = pipe->screen->resource_create(pipe->screen, &templ);
      if (!copy)
         return false;
      struct pipe_box box;
      u_box_2d_zslice(src.left, src.top, src_layer, sw, sh, &box);
      pipe->resource_copy_region(pipe, copy, 0, 0, 0, 0, src_tex, src_level,
                                 &box);
      src_tex = copy;
      view_level = view_layer = 0;
      src_x = src_y = 0;
      tex_w = (unsigned)sw;
      tex_h = (unsigned)sh;
   }

   const bool color_key = data->Flags.SrcColorKey != 0;
   if (!D3D9InitKeyBlitState(device)) {
      pipe_resource_reference(&copy, NULL);
      return false;
   }
   void *fs = device->key_blit.fs[color_key ? 1 : 0];
   if (!fs) {
      fs = D3D9CreateKeyBlitFs(pipe, color_key);
      device->key_blit.fs[color_key ? 1 : 0] = fs;
   }

   struct pipe_sampler_view view_templ;
   u_sampler_view_default_template(&view_templ, src_tex, src_format);
   view_templ.u.tex.first_level = view_templ.u.tex.last_level = view_level;
   view_templ.u.tex.first_layer = view_templ.u.tex.last_layer = view_layer;
   struct pipe_sampler_view *view =
      pipe->create_sampler_view(pipe, src_tex, &view_templ);

   /* Yttrium reads vertex data after the draw call returns, so the quad
    * goes through the stream uploader rather than as a user buffer. */
   const unsigned fb_w = dst_sub->width, fb_h = dst_sub->height;
   const float x0 = 2.0f * dst.left / fb_w - 1.0f;
   const float x1 = 2.0f * dst.right / fb_w - 1.0f;
   const float y0 = 2.0f * dst.top / fb_h - 1.0f;
   const float y1 = 2.0f * dst.bottom / fb_h - 1.0f;
   const float verts[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
   struct pipe_vertex_buffer vb;
   memset(&vb, 0, sizeof(vb));
   unsigned vb_offset = 0;
   struct pipe_resource *vb_release = NULL;
   u_upload_data(pipe->stream_uploader, 0, sizeof(verts), 16, verts,
                 &vb_offset, &vb.buffer.resource, &vb_release);
   vb.buffer_offset = vb_offset;

   bool ok = fs && view && vb.buffer.resource;
   if (ok) {
      struct pipe_framebuffer_state fb;
      memset(&fb, 0, sizeof(fb));
      fb.width = fb_w;
      fb.height = fb_h;
      fb.nr_cbufs = 1;
      {
         /* moose-ripple: Mesa 24.3 wants the surface as an object */
         struct pipe_surface templ;
         memset(&templ, 0, sizeof(templ));
         templ.format = dst_tex->format;
         templ.u.tex.level = dst_level;
         templ.u.tex.first_layer = templ.u.tex.last_layer = dst_layer;
         fb.cbufs[0] = pipe->create_surface(pipe, dst_tex, &templ);
      }
      /* The virgl context keeps the pointer without a reference of its own, and sends the
       * surface's resource again with a draw that follows a flush: the surface has to live
       * until it is off the context. */
      struct pipe_surface *target = fb.cbufs[0];
      pipe->set_framebuffer_state(pipe, &fb);

      struct pipe_viewport_state vp;
      memset(&vp, 0, sizeof(vp));
      vp.scale[0] = fb_w * 0.5f;
      vp.scale[1] = fb_h * 0.5f;
      vp.scale[2] = 1.0f;
      vp.translate[0] = fb_w * 0.5f;
      vp.translate[1] = fb_h * 0.5f;
      vp.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
      vp.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
      vp.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
      vp.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
      pipe->set_viewport_states(pipe, 0, 1, &vp);

      const bool mirror_x = data->Flags.MirrorLeftRight != 0;
      const bool mirror_y = data->Flags.MirrorUpDown != 0;
      const bool mirror = mirror_x || mirror_y;
      const float centre = mirror ? 0.5f : 0.0f;
      float constants[20];
      memset(constants, 0, sizeof(constants));
      constants[0] = mirror_x ? (float)dst.right - centre : -(float)dst.left - centre;
      constants[1] = mirror_y ? (float)dst.bottom - centre : -(float)dst.top - centre;
      constants[2] = 0.25f / (float)dw;
      constants[3] = 0.25f / (float)dh;
      constants[4] = (float)sw / (float)dw;
      constants[5] = (float)sh / (float)dh;
      constants[6] = mirror_x ? -1.0f : 1.0f;
      constants[7] = mirror_y ? -1.0f : 1.0f;
      constants[8] = (float)src_x + 0.5f;
      constants[9] = (float)src_y + 0.5f;
      constants[10] = 1.0f / tex_w;
      constants[11] = 1.0f / tex_h;
      if (color_key) {
         uint32_t key = data->ColorKey;
         util_format_unpack_rgba(src_format, &constants[12], &key, 1);
         unsigned bits = 0;
         for (unsigned c = 0; c < 3; ++c)
            bits = MAX2(bits, util_format_get_component_bits(
                                 src_format, UTIL_FORMAT_COLORSPACE_RGB, c));
         const float tolerance = bits ? 0.5f / (float)((1u << MIN2(bits, 24u)) - 1u) :
            0.5f / 255.0f;
         /* The emulation layer compares colour only; alpha never keys. */
         for (unsigned c = 0; c < 3; ++c)
            constants[16 + c] = tolerance;
         constants[19] = 2.0f;
      }
      struct pipe_constant_buffer cb;
      memset(&cb, 0, sizeof(cb));
      cb.user_buffer = constants;
      cb.buffer_size = sizeof(constants);

      pipe->bind_blend_state(pipe, device->key_blit.blend);
      pipe->bind_rasterizer_state(pipe, device->key_blit.rasterizer);
      pipe->bind_depth_stencil_alpha_state(pipe, device->key_blit.dsa);
      if (pipe->set_sample_mask)
         pipe->set_sample_mask(pipe, ~0u);
      pipe->bind_vs_state(pipe, device->key_blit.vs);
      pipe->bind_gs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, fs);
      pipe->set_constant_buffer(pipe, MESA_SHADER_VERTEX, 0, false, NULL);
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, false, &cb);
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                                &device->key_blit.sampler);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, false, &view);
      pipe->bind_vertex_elements_state(pipe, device->key_blit.velems);
      {
         /* moose-ripple: Mesa 24.3's context takes over one reference to the buffer */
         struct pipe_resource *held = NULL;
         pipe_resource_reference(&held, vb.buffer.resource);
      }
      pipe->set_vertex_buffers(pipe, 1, &vb);

      util_draw_arrays(pipe, MESA_PRIM_TRIANGLE_STRIP, 0, 4);

      pipe->set_vertex_buffers(pipe, 0, NULL);
      pipe->bind_vertex_elements_state(pipe, NULL);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, false, NULL);
      void *null_sampler = NULL;
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1,
                                &null_sampler);
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, false, NULL);
      pipe->bind_vs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, NULL);
      pipe->bind_depth_stencil_alpha_state(pipe, NULL);
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->bind_blend_state(pipe, NULL);
      memset(&fb, 0, sizeof(fb));
      pipe->set_framebuffer_state(pipe, &fb);
      pipe_surface_reference(&target, NULL);
      ok = !yttrium_gdi_take_draw_failure(pipe);
      if (ok) {
         dst_sub->cpu_dirty = false;
         D3D9MarkAutogenMipmapsDirty(dst_resource, data->DstSubResourceIndex);
      }
   }

   pipe_resource_release(pipe, vb_release);
   if (view)
      pipe_sampler_view_release(view);
   pipe_resource_reference(&copy, NULL);
   return ok;
}

HRESULT APIENTRY
D3D9Blt(HANDLE hDevice, const D3DDDIARG_BLT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   if (data->Flags.DstColorKey || data->Flags.LinearToSrgb ||
       data->Flags.Rotate) {
      static volatile LONG logged;
      D3D9WarnOncef(
         &logged,
         "unsupported D3D9 blit owner=d3d9-blt "
         "reason=dstkey-srgb-rotate-not-implemented "
         "action=reject flags=0x%08x color_key=0x%08x\n",
         data->Flags.Value, data->ColorKey);
      return E_NOTIMPL;
   }

   if (data->Flags.SrcColorKey || data->Flags.MirrorLeftRight ||
       data->Flags.MirrorUpDown) {
      const HRESULT hr = D3D9KeyBlitPipe(device, data) ? S_OK : E_NOTIMPL;
      D3D9Tracef("Blt colorkey=%u key=0x%08x mirror=%u%u hr=0x%08lx\n",
                 data->Flags.SrcColorKey, data->ColorKey,
                 data->Flags.MirrorLeftRight, data->Flags.MirrorUpDown, hr);
      return hr;
   }

   HRESULT hr = D3D9CopyRectPipe(device, data->hDstResource,
                                 data->DstSubResourceIndex, &data->DstRect,
                                 data->hSrcResource,
                                 data->SrcSubResourceIndex,
                                 &data->SrcRect,
                                 data->Flags.Linear ? PIPE_TEX_FILTER_LINEAR :
                                                      PIPE_TEX_FILTER_NEAREST) ?
      S_OK : E_NOTIMPL;
   return hr;
}

HRESULT APIENTRY
D3D9ColorFill(HANDLE hDevice, const D3DDDIARG_COLORFILL *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   /* A plain surface kept in memory (D3D9IsPlainInMemory) is filled there */
   D3D9Resource *resource = D3D9CastResource(data->hResource);
   D3D9SubResource *sub =
      D3D9GetSubResource(data->hResource, data->SubResourceIndex);
   if (resource && sub && !resource->pipe_resource &&
       !D3D9PoolNeedsReadback(resource->pool) && D3D9IsRgb32(resource->format) &&
       !sub->cpu_locked && D3D9EnsureCpuBacking(sub)) {
      const RECT dst = D3D9ClipRect(sub, &data->DstRect);
      for (LONG y = dst.top; y < dst.bottom; ++y) {
         UINT *row = (UINT *)(sub->data + (size_t)y * sub->pitch);
         for (LONG x = dst.left; x < dst.right; ++x)
            row[x] = data->Color;
      }
      sub->cpu_dirty = true;
      return S_OK;
   }

   const bool cleared =
      D3D9ClearColorPipe(device, data->hResource, data->SubResourceIndex,
                         &data->DstRect, data->Color, false, true);
   HRESULT hr = cleared ? S_OK : E_NOTIMPL;
   return hr;
}

/*
 * The video processor (DirectX Video Acceleration 2): the progressive device, and the
 * deinterlacing one, which differs only in showing an interlaced frame one field at a time
 * (D3D9VideoField).
 *
 * Windows' video renderer hands every decoded frame to a video processor with a target and
 * two rectangles, and expects the frame's part scaled to its place on the target, the rest
 * of the target in the background colour. Without one in the driver it falls back to a
 * software processor of its own, and with this driver that path showed nothing but black
 * (docs/windows/mesa-win7.md, "Windows Media Player").
 *
 * Frames come in surfaces kept in memory (D3D9IsPlainInMemory), as RGB or in one of two
 * video formats (D3D9IsVideoFormat). Every frame ends up as RGB in a texture this keeps and
 * is scaled from there to the target by a blit. An RGB frame is uploaded into it. An NV12
 * frame, which is what the decoders deliver, goes to the host as its two planes and is
 * converted there by one draw into that texture (D3D9VideoDrawNv12): a byte and a half a
 * pixel to upload and nothing to compute here. A YUY2 frame, or NV12 on a host without the
 * two texture formats, is converted here on the processor (D3D9VideoToRgb) and uploaded.
 */
struct D3D9VideoProcessor {
   HANDLE render_target;
   UINT render_target_subresource;
   struct pipe_resource *frame;        /* the frame last uploaded, on the host */
   const D3D9Resource *frame_source;   /* and the surface it came from */
   struct pipe_resource *luma;         /* an NV12 frame's two planes on the host, */
   struct pipe_resource *chroma;       /* converted there into `frame` (D3D9VideoDrawNv12) */
   uint8_t *rgb;                       /* a video-format frame converted, before its upload */
   size_t rgb_capacity;
   bool bob;                           /* the deinterlacing device (D3D9VideoField) */
   UINT frame_field;                   /* which field of the source `frame` holds: 0 both */
};

bool
D3D9VideoProcessorTakes(D3DDDIFORMAT video, D3DDDIFORMAT target)
{
#ifdef RIPPLE_VISTA
   if (target == D3D9_FMT_YUY2)
      return video == D3D9_FMT_YUY2;
#endif
   return (D3D9IsRgb32(video) || D3D9IsVideoFormat(video)) && D3D9IsRgb32(target);
}

HRESULT APIENTRY
D3D9CreateVideoProcessDevice(HANDLE hDevice,
                             D3DDDIARG_CREATEVIDEOPROCESSDEVICE *data)
{
   if (!hDevice || !data || !data->pVideoProcGuid)
      return E_INVALIDARG;
   const bool bob =
      !memcmp(data->pVideoProcGuid, &D3D9BobVideoProcessor, sizeof(GUID));
   if ((!bob &&
        memcmp(data->pVideoProcGuid, &D3D9ProgressiveVideoProcessor, sizeof(GUID))) ||
       !D3D9VideoProcessorTakes(data->VideoDesc.Format, data->RenderTargetFormat))
      return E_INVALIDARG;
   D3D9VideoProcessor *processor =
      (D3D9VideoProcessor *)calloc(1, sizeof(*processor));
   if (!processor)
      return E_OUTOFMEMORY;
   processor->bob = bob;
   data->hVideoProcess = processor;
   return S_OK;
}

static void
D3D9ReleaseVideoFrame(D3D9VideoProcessor *processor)
{
   pipe_resource_reference(&processor->frame, NULL);
   pipe_resource_reference(&processor->luma, NULL);
   pipe_resource_reference(&processor->chroma, NULL);
   free(processor->rgb);
}

HRESULT APIENTRY
D3D9DestroyVideoProcessDevice(HANDLE hDevice, HANDLE hVideoProcessor)
{
   (void)hDevice;
   D3D9VideoProcessor *processor = (D3D9VideoProcessor *)hVideoProcessor;
   if (processor) {
      D3D9ReleaseVideoFrame(processor);
      free(processor);
   }
   return S_OK;
}

HRESULT APIENTRY
D3D9VideoProcessBeginFrame(HANDLE hDevice, HANDLE hVideoProcess)
{
   return hDevice && hVideoProcess ? S_OK : E_INVALIDARG;
}

HRESULT APIENTRY
D3D9VideoProcessEndFrame(HANDLE hDevice, D3DDDIARG_VIDEOPROCESSENDFRAME *data)
{
   return hDevice && data ? S_OK : E_INVALIDARG;
}

HRESULT APIENTRY
D3D9SetVideoProcessRenderTarget(
   HANDLE hDevice, const D3DDDIARG_SETVIDEOPROCESSRENDERTARGET *data)
{
   if (!hDevice || !data || !data->hVideoProcess)
      return E_INVALIDARG;
   D3D9VideoProcessor *processor = (D3D9VideoProcessor *)data->hVideoProcess;
   processor->render_target = data->hRenderTarget;
   processor->render_target_subresource = data->SubResourceIndex;
   return S_OK;
}

/* The background colour comes as studio-range Y'CbCr (BT.601), sixteen bits a channel */
static D3DCOLOR
D3D9VideoBackground(const DXVADDI_AYUVSAMPLE16 *colour)
{
   const float y = 1.164f * ((colour->Y >> 8) - 16);
   const float cb = (float)((colour->Cb >> 8) - 128);
   const float cr = (float)((colour->Cr >> 8) - 128);
   const int r = (int)(y + 1.596f * cr + 0.5f);
   const int g = (int)(y - 0.813f * cr - 0.391f * cb + 0.5f);
   const int b = (int)(y + 2.018f * cb + 0.5f);
   return 0xff000000u | ((UINT)CLAMP(r, 0, 255) << 16) |
      ((UINT)CLAMP(g, 0, 255) << 8) | (UINT)CLAMP(b, 0, 255);
}

/* A frame's destination cut to what of the target may be drawn, and its source cut by the
 * same proportions. False if nothing is left. */
static bool
D3D9VideoClip(const RECT *bounds, RECT *src, RECT *dst)
{
   const LONG dst_width = dst->right - dst->left;
   const LONG dst_height = dst->bottom - dst->top;
   const LONG src_width = src->right - src->left;
   const LONG src_height = src->bottom - src->top;
   if (dst_width <= 0 || dst_height <= 0 || src_width <= 0 || src_height <= 0)
      return false;
   RECT cut;
   cut.left = MAX2(dst->left, bounds->left);
   cut.top = MAX2(dst->top, bounds->top);
   cut.right = MIN2(dst->right, bounds->right);
   cut.bottom = MIN2(dst->bottom, bounds->bottom);
   if (cut.right <= cut.left || cut.bottom <= cut.top)
      return false;
   RECT part;
   part.left = src->left + (LONG)((LONGLONG)(cut.left - dst->left) * src_width / dst_width);
   part.right = src->left + (LONG)((LONGLONG)(cut.right - dst->left) * src_width / dst_width);
   part.top = src->top + (LONG)((LONGLONG)(cut.top - dst->top) * src_height / dst_height);
   part.bottom = src->top + (LONG)((LONGLONG)(cut.bottom - dst->top) * src_height / dst_height);
   if (part.right <= part.left)
      part.right = part.left + 1;
   if (part.bottom <= part.top)
      part.bottom = part.top + 1;
   *src = part;
   *dst = cut;
   return true;
}

/* What turns Y'CbCr (each 0 to 1, as sampled) into RGB: three rows of four, the fourth
 * column the constant part. The matrix and the range are the frame's own (its extended
 * format); where it does not say, high definition is BT.709, anything smaller BT.601, and
 * the range the studio one (16 to 235), as video is unless it says otherwise. */
static void
D3D9VideoMatrix(const DXVADDI_EXTENDEDFORMAT *colour, UINT height, float rows[3][4])
{
   const UINT matrix = colour ? colour->VideoTransferMatrix : 0;   /* 1: BT.709, 2: BT.601 */
   const bool bt709 = matrix == 1 || (matrix != 2 && height > 576);
   const bool full_range = colour && colour->NominalRange == 1;   /* 1: 0 to 255 */
   const float kr = bt709 ? 0.2126f : 0.299f;
   const float kb = bt709 ? 0.0722f : 0.114f;
   const float kg = 1.0f - kr - kb;
   const float luma = full_range ? 1.0f : 255.0f / 219.0f;
   const float chroma = full_range ? 1.0f : 255.0f / 224.0f;
   const float luma_zero = full_range ? 0.0f : 16.0f / 255.0f;
   const float chroma_zero = 128.0f / 255.0f;
   const float from_cb[3] = { 0.0f, -2.0f * kb * (1.0f - kb) / kg * chroma,
                              2.0f * (1.0f - kb) * chroma };
   const float from_cr[3] = { 2.0f * (1.0f - kr) * chroma,
                              -2.0f * kr * (1.0f - kr) / kg * chroma, 0.0f };
   for (unsigned c = 0; c < 3; ++c) {
      rows[c][0] = luma;
      rows[c][1] = from_cb[c];
      rows[c][2] = from_cr[c];
      rows[c][3] = -luma * luma_zero - (from_cb[c] + from_cr[c]) * chroma_zero;
   }
}

/* The conversion draw's fragment shader. A fragment's place on the target gives the place
 * in the frame (c0: scale in xy, offset in zw, in texture coordinates, which are the same
 * for both planes of a whole frame, c4 the same for the chroma plane, which differs when the
 * planes hold one field); luma from the first texture, the chroma pair from the second;
 * then the three rows of the matrix (c1 to c3). */
static void *
D3D9CreateVideoFs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;
   struct ureg_src frag = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_POSITION, 0,
                                             TGSI_INTERPOLATE_LINEAR);
   struct ureg_src c0 = ureg_DECL_constant(ureg, 0);
   struct ureg_src rows[3] = { ureg_DECL_constant(ureg, 1), ureg_DECL_constant(ureg, 2),
                               ureg_DECL_constant(ureg, 3) };
   struct ureg_src c4 = ureg_DECL_constant(ureg, 4);
   struct ureg_src samplers[2] = { ureg_DECL_sampler(ureg, 0), ureg_DECL_sampler(ureg, 1) };
   for (unsigned i = 0; i < 2; ++i)
      ureg_DECL_sampler_view(ureg, i, TGSI_TEXTURE_2D, TGSI_RETURN_TYPE_FLOAT,
                             TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                             TGSI_RETURN_TYPE_FLOAT);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst coord = ureg_DECL_temporary(ureg);
   struct ureg_dst ycc = ureg_DECL_temporary(ureg);
   struct ureg_dst texel = ureg_DECL_temporary(ureg);

   ureg_MAD(ureg, ureg_writemask(coord, TGSI_WRITEMASK_XY), frag, c0,
            ureg_swizzle(c0, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W));
   ureg_MOV(ureg, ureg_writemask(coord, TGSI_WRITEMASK_ZW), ureg_imm1f(ureg, 0.0f));
   ureg_TEX(ureg, texel, TGSI_TEXTURE_2D, ureg_src(coord), samplers[0]);
   ureg_MOV(ureg, ureg_writemask(ycc, TGSI_WRITEMASK_X), ureg_src(texel));
   ureg_MAD(ureg, ureg_writemask(coord, TGSI_WRITEMASK_XY), frag, c4,
            ureg_swizzle(c4, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W));
   ureg_TEX(ureg, texel, TGSI_TEXTURE_2D, ureg_src(coord), samplers[1]);
   ureg_MOV(ureg, ureg_writemask(ycc, TGSI_WRITEMASK_YZ),
            ureg_swizzle(ureg_src(texel), TGSI_SWIZZLE_X, TGSI_SWIZZLE_X, TGSI_SWIZZLE_Y,
                         TGSI_SWIZZLE_Y));
   ureg_MOV(ureg, ureg_writemask(ycc, TGSI_WRITEMASK_W), ureg_imm1f(ureg, 1.0f));
   ureg_DP4(ureg, ureg_writemask(out, TGSI_WRITEMASK_X), ureg_src(ycc), rows[0]);
   ureg_DP4(ureg, ureg_writemask(out, TGSI_WRITEMASK_Y), ureg_src(ycc), rows[1]);
   ureg_DP4(ureg, ureg_writemask(out, TGSI_WRITEMASK_Z), ureg_src(ycc), rows[2]);
   ureg_MOV(ureg, ureg_writemask(out, TGSI_WRITEMASK_W), ureg_imm1f(ureg, 1.0f));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* An NV12 frame converted by the host into the processor's frame texture: its luma plane
 * as a one-channel texture and its chroma plane as a two-channel texture of half the size,
 * and one quad over the whole frame. Drawn into a texture of the processor's own, not the
 * target: the target is whatever the program has bound, and scaling to it is the blit's job
 * as for any other frame. False if the host lacks either texture format or the draw fails;
 * the caller then converts the frame itself. */
static bool
D3D9VideoDrawNv12(D3D9Device *device, D3D9VideoProcessor *processor,
                  D3D9Resource *src_resource, D3D9SubResource *src_sub,
                  const DXVADDI_EXTENDEDFORMAT *colour, UINT field)
{
   struct pipe_context *pipe = device->pipe;
   struct pipe_screen *screen = device->screen;
   struct pipe_resource *dst_tex = processor->frame;
   const UINT width = src_sub->width, height = src_sub->height;
   const UINT chroma_width = (width + 1) / 2, chroma_height = (height + 1) / 2;
   /* One field (D3D9VideoField): every second row of each plane goes up, from the first row
    * for the top field and the second for the bottom one, and the draw stretches them over
    * the frame, each row at the height it has in the frame. */
   const UINT first_row = field == 2 ? 1 : 0, row_step = field ? 2 : 1;
   const UINT luma_rows = field ? (height + 1 - first_row) / 2 : height;
   const UINT chroma_rows = field ? (chroma_height + 1 - first_row) / 2 : chroma_height;
   /* RIPPLE_VIDEO_CPU (any value): convert in the driver instead, to compare the two */
   static int in_driver = -1;
   if (in_driver < 0)
      in_driver = getenv("RIPPLE_VIDEO_CPU") != NULL;
   if (in_driver || src_resource->format != D3D9_FMT_NV12 || !screen || !dst_tex ||
       !pipe->texture_subdata || !pipe->create_sampler_view ||
       !(dst_tex->bind & PIPE_BIND_RENDER_TARGET) ||
       src_sub->data_capacity < src_sub->size ||
       src_sub->pitch < 2 * chroma_width || !luma_rows || !chroma_rows ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R8_UNORM, PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_SAMPLER_VIEW) ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R8G8_UNORM, PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_SAMPLER_VIEW))
      return false;

   if (processor->luma && processor->chroma &&
       (processor->luma->width0 != width || processor->luma->height0 != luma_rows ||
        processor->chroma->height0 != chroma_rows)) {
      pipe_resource_reference(&processor->luma, NULL);
      pipe_resource_reference(&processor->chroma, NULL);
   }
   if (!processor->luma || !processor->chroma) {
      struct pipe_resource templ;
      memset(&templ, 0, sizeof(templ));
      templ.target = PIPE_TEXTURE_2D;
      templ.depth0 = 1;
      templ.array_size = 1;
      templ.nr_samples = 1;
      templ.nr_storage_samples = 1;
      templ.bind = PIPE_BIND_SAMPLER_VIEW;
      templ.usage = PIPE_USAGE_DEFAULT;
      templ.format = PIPE_FORMAT_R8_UNORM;
      templ.width0 = width;
      templ.height0 = luma_rows;
      pipe_resource_reference(&processor->luma, NULL);
      processor->luma = screen->resource_create(screen, &templ);
      templ.format = PIPE_FORMAT_R8G8_UNORM;
      templ.width0 = chroma_width;
      templ.height0 = chroma_rows;
      pipe_resource_reference(&processor->chroma, NULL);
      processor->chroma = screen->resource_create(screen, &templ);
      if (!processor->luma || !processor->chroma)
         return false;
   }

   /* this sets the context's state itself and leaves it empty */
   D3D9SettleDrawState(device);
   if (!D3D9InitKeyBlitState(device))
      return false;
   if (!device->key_blit.video_fs)
      device->key_blit.video_fs = D3D9CreateVideoFs(pipe);
   if (!device->key_blit.video_sampler) {
      struct pipe_sampler_state sampler;
      memset(&sampler, 0, sizeof(sampler));
      sampler.wrap_s = sampler.wrap_t = sampler.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      sampler.min_img_filter = sampler.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
      sampler.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
      device->key_blit.video_sampler = pipe->create_sampler_state(pipe, &sampler);
   }
   if (!device->key_blit.video_fs || !device->key_blit.video_sampler)
      return false;

   {
      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.depth = 1;
      const UINT stride = src_sub->pitch * row_step;
      box.width = width;
      box.height = luma_rows;
      pipe->texture_subdata(pipe, processor->luma, 0,
                            PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, &box,
                            src_sub->data + (size_t)src_sub->pitch * first_row, stride,
                            stride * luma_rows);
      box.width = chroma_width;
      box.height = chroma_rows;
      pipe->texture_subdata(pipe, processor->chroma, 0,
                            PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, &box,
                            src_sub->data + (size_t)src_sub->pitch * (height + first_row),
                            stride, stride * chroma_rows);
   }

   struct pipe_sampler_view *views[2] = { NULL, NULL };
   struct pipe_sampler_view view_templ;
   u_sampler_view_default_template(&view_templ, processor->luma, PIPE_FORMAT_R8_UNORM);
   views[0] = pipe->create_sampler_view(pipe, processor->luma, &view_templ);
   u_sampler_view_default_template(&view_templ, processor->chroma, PIPE_FORMAT_R8G8_UNORM);
   views[1] = pipe->create_sampler_view(pipe, processor->chroma, &view_templ);

   const unsigned fb_w = width, fb_h = height;
   const float verts[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
   struct pipe_vertex_buffer vb;
   memset(&vb, 0, sizeof(vb));
   unsigned vb_offset = 0;
   struct pipe_resource *vb_release = NULL;
   u_upload_data(pipe->stream_uploader, 0, sizeof(verts), 16, verts,
                 &vb_offset, &vb.buffer.resource, &vb_release);
   vb.buffer_offset = vb_offset;

   bool ok = views[0] && views[1] && vb.buffer.resource;
   if (ok) {
      struct pipe_framebuffer_state fb;
      memset(&fb, 0, sizeof(fb));
      fb.width = fb_w;
      fb.height = fb_h;
      fb.nr_cbufs = 1;
      {
         struct pipe_surface templ;
         memset(&templ, 0, sizeof(templ));
         templ.format = dst_tex->format;
         fb.cbufs[0] = pipe->create_surface(pipe, dst_tex, &templ);
      }
      /* The virgl context keeps the pointer without a reference of its own, and sends the
       * surface's resource again with a draw that follows a flush: the surface has to live
       * until it is off the context. */
      struct pipe_surface *target = fb.cbufs[0];
      pipe->set_framebuffer_state(pipe, &fb);

      struct pipe_viewport_state vp;
      memset(&vp, 0, sizeof(vp));
      vp.scale[0] = fb_w * 0.5f;
      vp.scale[1] = fb_h * 0.5f;
      vp.scale[2] = 1.0f;
      vp.translate[0] = fb_w * 0.5f;
      vp.translate[1] = fb_h * 0.5f;
      vp.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
      vp.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
      vp.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
      vp.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
      pipe->set_viewport_states(pipe, 0, 1, &vp);

      /* A pixel of the frame texture (y + 0.5) to each plane, 0 to 1 over its rows. Whole
       * frame: the same place in both. A field: its luma row k is the frame's row 2k (top) or
       * 2k + 1 (bottom), and its chroma row m lies over the four frame rows from 4m (top) or
       * 4m + 2 (bottom). */
      float constants[20];
      constants[0] = constants[16] = 1.0f / width;
      constants[2] = constants[18] = 0.0f;
      if (field) {
         constants[1] = 1.0f / (2.0f * luma_rows);
         constants[3] = (0.5f - first_row) / (2.0f * luma_rows);
         constants[17] = 1.0f / (4.0f * chroma_rows);
         constants[19] = (1.0f - 2.0f * first_row) / (4.0f * chroma_rows);
      } else {
         constants[1] = constants[17] = 1.0f / height;
         constants[3] = constants[19] = 0.0f;
      }
      float rows[3][4];
      D3D9VideoMatrix(colour, height, rows);
      memcpy(&constants[4], rows, sizeof(rows));
      struct pipe_constant_buffer cb;
      memset(&cb, 0, sizeof(cb));
      cb.user_buffer = constants;
      cb.buffer_size = sizeof(constants);

      void *samplers[2] = { device->key_blit.video_sampler, device->key_blit.video_sampler };
      pipe->bind_blend_state(pipe, device->key_blit.blend);
      pipe->bind_rasterizer_state(pipe, device->key_blit.rasterizer);
      pipe->bind_depth_stencil_alpha_state(pipe, device->key_blit.dsa);
      if (pipe->set_sample_mask)
         pipe->set_sample_mask(pipe, ~0u);
      pipe->bind_vs_state(pipe, device->key_blit.vs);
      pipe->bind_gs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, device->key_blit.video_fs);
      pipe->set_constant_buffer(pipe, MESA_SHADER_VERTEX, 0, false, NULL);
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, false, &cb);
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 2, samplers);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 2, 0, false, views);
      pipe->bind_vertex_elements_state(pipe, device->key_blit.velems);
      {
         /* Mesa 24.3's context takes over one reference to the buffer */
         struct pipe_resource *held = NULL;
         pipe_resource_reference(&held, vb.buffer.resource);
      }
      pipe->set_vertex_buffers(pipe, 1, &vb);
      util_draw_arrays(pipe, MESA_PRIM_TRIANGLE_STRIP, 0, 4);

      pipe->set_vertex_buffers(pipe, 0, NULL);
      pipe->bind_vertex_elements_state(pipe, NULL);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 2, false, NULL);
      void *no_samplers[2] = { NULL, NULL };
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 2, no_samplers);
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, false, NULL);
      pipe->bind_vs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, NULL);
      pipe->bind_depth_stencil_alpha_state(pipe, NULL);
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->bind_blend_state(pipe, NULL);
      memset(&fb, 0, sizeof(fb));
      pipe->set_framebuffer_state(pipe, &fb);
      pipe_surface_reference(&target, NULL);
      ok = !yttrium_gdi_take_draw_failure(pipe);
   }
   pipe_resource_release(pipe, vb_release);
   for (unsigned i = 0; i < 2; ++i) {
      if (views[i])
         pipe_sampler_view_release(views[i]);
   }
   return ok;
}

/* A frame in a video format to 32-bit RGB (B, G, R, 255), `out` holding width x height
 * pixels without padding. The matrix and the range are the frame's own (its extended
 * format); where it does not say, high definition is BT.709, anything smaller BT.601, and
 * the range the studio one (16 to 235), as video is unless it says otherwise. Chroma is taken
 * from the nearest sample. */
static void
D3D9VideoToRgb(D3DDDIFORMAT format, const D3D9SubResource *sub,
               const DXVADDI_EXTENDEDFORMAT *colour, UINT field, uint8_t *out)
{
   const UINT matrix = colour ? colour->VideoTransferMatrix : 0;   /* 1: BT.709, 2: BT.601 */
   const bool bt709 = matrix == 1 || (matrix != 2 && sub->height > 576);
   const bool full_range = colour && colour->NominalRange == 1;   /* 1: 0 to 255 */
   const float kr = bt709 ? 0.2126f : 0.299f;
   const float kb = bt709 ? 0.0722f : 0.114f;
   const float kg = 1.0f - kr - kb;
   const float luma_gain = full_range ? 1.0f : 255.0f / 219.0f;
   const float chroma_gain = full_range ? 1.0f : 255.0f / 224.0f;
   const int luma_zero = full_range ? 0 : 16;

   /* in 1/1024ths: what each of Y, Cb and Cr adds to each of red, green and blue */
   int from_y[256], red_cr[256], green_cb[256], green_cr[256], blue_cb[256];
   for (int i = 0; i < 256; ++i) {
      from_y[i] = (int)(luma_gain * (i - luma_zero) * 1024.0f) + 512;
      red_cr[i] = (int)(2.0f * (1.0f - kr) * chroma_gain * (i - 128) * 1024.0f);
      blue_cb[i] = (int)(2.0f * (1.0f - kb) * chroma_gain * (i - 128) * 1024.0f);
      green_cb[i] = (int)(2.0f * kb * (1.0f - kb) / kg * chroma_gain * (i - 128) * 1024.0f);
      green_cr[i] = (int)(2.0f * kr * (1.0f - kr) / kg * chroma_gain * (i - 128) * 1024.0f);
   }

   const UINT width = sub->width;
   const UINT first_row = field == 2 ? 1 : 0;
   const UINT chroma_height = (sub->height + 1) / 2;
   for (UINT y = 0; y < sub->height; ++y) {
      /* One field (D3D9VideoField): each of its rows fills two rows of the frame. Its luma
       * row k is the frame's row 2k (top) or 2k + 1 (bottom); two luma rows of a field share
       * a chroma row of that field, which are every second row of the chroma plane. */
      UINT row = y, chroma_row = y / 2;
      if (field) {
         const UINT k = (y > first_row ? y - first_row : 0) / 2;
         row = MIN2(2 * k + first_row, sub->height - 1);
         chroma_row = MIN2(2 * (k / 2) + first_row, chroma_height - 1);
      }
      const uint8_t *luma = sub->data + (size_t)row * sub->pitch;
      /* NV12: the chroma pairs of this row, in the plane after the luma rows */
      const uint8_t *chroma = format == D3D9_FMT_NV12 ?
         sub->data + (size_t)(sub->height + chroma_row) * sub->pitch : NULL;
      uint8_t *pixel = out + (size_t)y * width * 4;
      for (UINT x = 0; x < width; ++x, pixel += 4) {
         int l, cb, cr;
         if (format == D3D9_FMT_NV12) {
            l = luma[x];
            cb = chroma[x & ~1u];
            cr = chroma[(x & ~1u) + 1];
         } else {   /* YUY2: Y0 Cb Y1 Cr */
            l = luma[2 * x];
            cb = luma[4 * (x / 2) + 1];
            cr = luma[4 * (x / 2) + 3];
         }
         const int r = (from_y[l] + red_cr[cr]) >> 10;
         const int g = (from_y[l] - green_cb[cb] - green_cr[cr]) >> 10;
         const int b = (from_y[l] + blue_cb[cb]) >> 10;
         pixel[0] = (uint8_t)CLAMP(b, 0, 255);
         pixel[1] = (uint8_t)CLAMP(g, 0, 255);
         pixel[2] = (uint8_t)CLAMP(r, 0, 255);
         pixel[3] = 255;
      }
   }
}

/* A frame in memory: uploaded into the processor's own texture when it changed, then
 * scaled from there. */
static bool
D3D9VideoBlitFrame(D3D9Device *device, D3D9VideoProcessor *processor,
                   D3D9Resource *dst_resource, D3D9SubResource *dst_sub,
                   UINT dst_subresource_index, const RECT *dst,
                   D3D9Resource *src_resource, D3D9SubResource *src_sub,
                   const RECT *src, const DXVADDI_EXTENDEDFORMAT *colour, UINT field,
                   enum pipe_tex_filter filter)
{
   const bool video = D3D9IsVideoFormat(src_resource->format);
   if (!video)
      field = 0;   /* an RGB frame is shown whole */
   const enum pipe_format format = video ? PIPE_FORMAT_B8G8R8X8_UNORM :
      D3D9FormatToPipe(src_resource->format);
   if (!device->screen || !device->pipe->texture_subdata || !device->pipe->blit ||
       format == PIPE_FORMAT_NONE || !dst_resource->pipe_resource || !src_sub->data ||
       !src_sub->width || !src_sub->height ||
       (video && src_sub->width % 2 != 0))
      return false;
   const size_t rows = D3D9FormatRowCount(src_resource->format, src_sub->height);
   const size_t row_bytes = (size_t)src_sub->width * D3D9FormatBytesPerPixel(src_resource->format);
   if (!row_bytes || src_sub->pitch < row_bytes || src_sub->data_capacity < row_bytes ||
       rows - 1 > (src_sub->data_capacity - row_bytes) / src_sub->pitch)
      return false;

   if (processor->frame &&
       (processor->frame->width0 != src_sub->width ||
        processor->frame->height0 != src_sub->height ||
        processor->frame->format != format)) {
      pipe_resource_reference(&processor->frame, NULL);
      processor->frame_source = NULL;
   }
   if (!processor->frame) {
      struct pipe_resource templ;
      memset(&templ, 0, sizeof(templ));
      templ.target = PIPE_TEXTURE_2D;
      templ.format = format;
      templ.width0 = src_sub->width;
      templ.height0 = src_sub->height;
      templ.depth0 = 1;
      templ.array_size = 1;
      templ.nr_samples = 1;
      templ.nr_storage_samples = 1;
      /* a video frame is drawn into it by the conversion (D3D9VideoDrawNv12) */
      templ.bind = PIPE_BIND_SAMPLER_VIEW | (video ? PIPE_BIND_RENDER_TARGET : 0);
      templ.usage = PIPE_USAGE_DEFAULT;
      processor->frame = device->screen->resource_create(device->screen, &templ);
      if (!processor->frame)
         return false;
      processor->frame_source = NULL;
   }
   /* The renderer draws a frame again when its window is repainted: no second upload */
   const bool stale = processor->frame_source != src_resource || src_sub->cpu_dirty ||
      processor->frame_field != field;
   bool converted = false;
   if (stale &&
       D3D9VideoDrawNv12(device, processor, src_resource, src_sub, colour, field)) {
      processor->frame_source = src_resource;
      processor->frame_field = field;
      src_sub->cpu_dirty = false;
      converted = true;
   }
   if (stale && !converted) {
      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.width = src_sub->width;
      box.height = src_sub->height;
      box.depth = 1;
      const uint8_t *pixels = src_sub->data;
      UINT pitch = src_sub->pitch;
      if (video) {
         const size_t needed = (size_t)src_sub->width * src_sub->height * 4;
         if (processor->rgb_capacity < needed) {
            free(processor->rgb);
            processor->rgb = (uint8_t *)malloc(needed);
            processor->rgb_capacity = processor->rgb ? needed : 0;
            if (!processor->rgb)
               return false;
         }
         D3D9VideoToRgb(src_resource->format, src_sub, colour, field, processor->rgb);
         pixels = processor->rgb;
         pitch = src_sub->width * 4;
      }
      device->pipe->texture_subdata(device->pipe, processor->frame, 0,
                                    PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                    &box, pixels, pitch, pitch * src_sub->height);
      processor->frame_source = src_resource;
      processor->frame_field = field;
      src_sub->cpu_dirty = false;
   }

   struct pipe_blit_info info;
   D3D9InitBlitInfo(&info, dst_resource->pipe_resource,
                    D3D9PipeMipLevel(dst_resource, dst_subresource_index),
                    D3D9PipeLayer(dst_resource, dst_subresource_index),
                    dst_resource->pipe_resource->format, dst,
                    processor->frame, 0, 0, format, src);
   info.filter = filter;
   device->pipe->blit(device->pipe, &info);
   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

/* StretchRect has no persistent video processor. Its source can be updated
 * between two blits or used concurrently by a separate video processor. */
static bool
D3D9BlitVideoSurface(D3D9Device *device, D3D9Resource *dst_resource,
                    D3D9SubResource *dst_sub, UINT dst_index, const RECT *dst,
                    D3D9Resource *src_resource, D3D9SubResource *src_sub,
                    const RECT *src, enum pipe_tex_filter filter)
{
   const enum pipe_format target = dst_resource->pipe_resource->format;
   if (util_format_is_depth_or_stencil(target) || util_format_is_compressed(target) ||
       dst_resource->pipe_resource->nr_samples > 1)
      return false;
   D3D9VideoProcessor converter = {};
   const bool dirty = src_sub->cpu_dirty;
   const bool ok = D3D9VideoBlitFrame(device, &converter, dst_resource, dst_sub,
                                     dst_index, dst, src_resource, src_sub, src,
                                     NULL, 0, filter);
   src_sub->cpu_dirty = dirty;
   D3D9ReleaseVideoFrame(&converter);
   return ok;
}

/* Which field of an interlaced frame is due: 0 for the whole frame, 1 for the top field (the
 * even rows), 2 for the bottom one. Only the deinterlacing device shows fields, each in turn
 * stretched to the frame's height ("bob"), and only of a frame that says its two fields are
 * interleaved. The first field belongs to the first half of the frame's time and the second
 * to the rest; the renderer asks once for each. The other device shows such a frame whole,
 * both fields woven together, combed where something moved. */
static UINT
D3D9VideoField(const D3D9VideoProcessor *processor, const D3DDDIARG_VIDEOPROCESSBLT *data,
               const DXVADDI_VIDEOSAMPLE *sample)
{
   const UINT kind = sample->SampleFormat.SampleFormat;
   /* RIPPLE_VIDEO_WEAVE (any value): show interlaced frames whole after all, to compare */
   static int weave = -1;
   if (weave < 0)
      weave = getenv("RIPPLE_VIDEO_WEAVE") != NULL;
   if (weave || !processor->bob ||
       (kind != DXVADDI_SampleFieldInterleavedEvenFirst &&
        kind != DXVADDI_SampleFieldInterleavedOddFirst))
      return 0;
   const bool second =
      data->TargetFrame >= sample->Start + (sample->End - sample->Start) / 2;
   const bool top_first = kind == DXVADDI_SampleFieldInterleavedEvenFirst;
   return top_first != second ? 1 : 2;
}

#ifdef RIPPLE_VISTA
static bool
D3D9HasYuy2Data(const D3D9SubResource *sub)
{
   if (!sub || !sub->data || !sub->width || !sub->height || (sub->width & 1))
      return false;
   const uint64_t row_bytes = (uint64_t)sub->width * 2;
   return sub->pitch >= row_bytes &&
          (uint64_t)(sub->height - 1) * sub->pitch + row_bytes <= sub->data_capacity;
}

/* Vista's VMR9 asks for a YUY2 intermediate. Each pair of output pixels
 * has two lumas and one shared pair of chroma values. */
static HRESULT
D3D9VideoBltYuy2(D3D9VideoProcessor *processor, const D3DDDIARG_VIDEOPROCESSBLT *data,
                 D3D9SubResource *output, const RECT &target)
{
   /* A fresh video-processor destination can be written before its first lock. */
   if (!D3D9EnsureCpuBacking(output))
      return E_OUTOFMEMORY;
   if (!D3D9HasYuy2Data(output))
      return E_INVALIDARG;

   const D3D9SubResource *input = NULL;
   RECT src = {}, dst = {};
   UINT field = 0;
   if (data->NumSrcSurfaces) {
      const DXVADDI_VIDEOSAMPLE *sample = &data->pSrcSurfaces[0];
      src = sample->SrcRect;
      dst = sample->DstRect;
      if (D3D9VideoClip(&target, &src, &dst)) {
         const D3D9Resource *resource = D3D9CastResource(sample->SrcResource);
         input = D3D9GetSubResource(sample->SrcResource, sample->SrcSubResourceIndex);
         if (!resource || resource->format != D3D9_FMT_YUY2 || !D3D9HasYuy2Data(input) ||
             src.left < 0 || src.top < 0 || (UINT)src.right > input->width ||
             (UINT)src.bottom > input->height)
            return E_INVALIDARG;
         field = D3D9VideoField(processor, data, sample);
      }
   }

   const uint8_t *source = input ? input->data : NULL;
   uint8_t *saved = NULL;
   if (source == output->data) {
      const size_t bytes = (size_t)(input->height - 1) * input->pitch + (size_t)input->width * 2;
      saved = (uint8_t *)malloc(bytes);
      if (!saved)
         return E_OUTOFMEMORY;
      memcpy(saved, source, bytes);
      source = saved;
   }

   const unsigned bg_y = data->BackgroundColor.Y >> 8;
   const unsigned bg_u = data->BackgroundColor.Cb >> 8;
   const unsigned bg_v = data->BackgroundColor.Cr >> 8;
   for (LONG y = target.top; y < target.bottom; ++y) {
      uint8_t *out = output->data + (size_t)y * output->pitch;
      const uint8_t *in = NULL;
      if (source && y >= dst.top && y < dst.bottom) {
         LONG sy = src.top + (LONG)((LONGLONG)(y - dst.top) * (src.bottom - src.top) /
                                    (dst.bottom - dst.top));
         if (field)
            sy = CLAMP((sy & ~1L) + (LONG)field - 1, src.top, src.bottom - 1);
         in = source + (size_t)sy * input->pitch;
      }
      for (LONG x = target.left & ~1L; x < target.right; x += 2) {
         uint8_t *pair = out + (size_t)x * 2;
         unsigned sum_u = 0, sum_v = 0;
         for (LONG n = 0; n < 2; ++n) {
            const LONG dx = x + n;
            if (dx < target.left || dx >= target.right)
               continue;
            unsigned luma = bg_y, u = bg_u, v = bg_v;
            if (in && dx >= dst.left && dx < dst.right) {
               const LONG sx = src.left + (LONG)((LONGLONG)(dx - dst.left) * (src.right - src.left) /
                                                  (dst.right - dst.left));
               const uint8_t *p = in + (size_t)(sx & ~1L) * 2;
               luma = p[(sx & 1) * 2];
               u = p[1];
               v = p[3];
            }
            pair[n * 2] = (uint8_t)luma;
            sum_u += u;
            sum_v += v;
         }
         /* On a partial pair the neighbour outside the target keeps its chroma. */
         if (x >= target.left && x + 1 < target.right) {
            pair[1] = (uint8_t)((sum_u + 1) / 2);
            pair[3] = (uint8_t)((sum_v + 1) / 2);
         }
      }
   }
   free(saved);
   output->cpu_dirty = true;
   return S_OK;
}
#endif

HRESULT APIENTRY
D3D9VideoProcessBlt(HANDLE hDevice, const D3DDDIARG_VIDEOPROCESSBLT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !device->pipe || !data || !data->hVideoProcess ||
       (data->NumSrcSurfaces && !data->pSrcSurfaces))
      return E_INVALIDARG;
   D3D9VideoProcessor *processor = (D3D9VideoProcessor *)data->hVideoProcess;
   D3D9Resource *dst_resource = D3D9CastResource(processor->render_target);
   D3D9SubResource *dst_sub =
      D3D9GetSubResource(processor->render_target,
                         processor->render_target_subresource);
   if (!dst_resource || !dst_sub)
      return E_INVALIDARG;

   RECT target;
   target.left = MAX2(data->TargetRect.left, 0);
   target.top = MAX2(data->TargetRect.top, 0);
   target.right = MIN2(data->TargetRect.right, (LONG)dst_sub->width);
   target.bottom = MIN2(data->TargetRect.bottom, (LONG)dst_sub->height);
   if (target.right <= target.left || target.bottom <= target.top)
      return S_OK;
#ifdef RIPPLE_VISTA
   if (dst_resource->format == D3D9_FMT_YUY2)
      return D3D9VideoBltYuy2(processor, data, dst_sub, target);
#endif

   /* The first sample is the video. Sub-streams (subtitles, a DVD's menu highlights) are
    * not offered, so Windows mixes those itself before it comes here. */
   RECT src = {}, dst = {};
   bool drawn = false;
   if (data->NumSrcSurfaces) {
      const DXVADDI_VIDEOSAMPLE *sample = &data->pSrcSurfaces[0];
      D3D9Resource *src_resource = D3D9CastResource(sample->SrcResource);
      D3D9SubResource *src_sub =
         D3D9GetSubResource(sample->SrcResource, sample->SrcSubResourceIndex);
      src = sample->SrcRect;
      dst = sample->DstRect;
      if (src_resource && src_sub && D3D9VideoClip(&target, &src, &dst)) {
         const UINT field = D3D9VideoField(processor, data, sample);
         if (field && D3D9TraceLevel() >= 3)
            ripple_logf("  video blt shows the %s field", field == 1 ? "top" : "bottom");
         if (!src_resource->pipe_resource && src_sub->data)
            drawn = D3D9VideoBlitFrame(device, processor, dst_resource, dst_sub,
                                       processor->render_target_subresource, &dst,
                                       src_resource, src_sub, &src,
                                       &sample->SampleFormat, field, PIPE_TEX_FILTER_LINEAR);
         else
            drawn = D3D9CopyRectPipe(device, processor->render_target,
                                     processor->render_target_subresource, &dst,
                                     sample->SrcResource, sample->SrcSubResourceIndex,
                                     &src, PIPE_TEX_FILTER_LINEAR);
         if (!drawn)
            return E_NOTIMPL;
      }
   }

   /* What of the target the video leaves free takes the background colour */
   const D3DCOLOR background = D3D9VideoBackground(&data->BackgroundColor);
   RECT strips[4];
   UINT count = 0;
   if (!drawn) {
      strips[count++] = target;
   } else {
      if (dst.top > target.top)
         strips[count++] = RECT{ target.left, target.top, target.right, dst.top };
      if (dst.bottom < target.bottom)
         strips[count++] = RECT{ target.left, dst.bottom, target.right, target.bottom };
      if (dst.left > target.left)
         strips[count++] = RECT{ target.left, dst.top, dst.left, dst.bottom };
      if (dst.right < target.right)
         strips[count++] = RECT{ dst.right, dst.top, target.right, dst.bottom };
   }
   for (UINT i = 0; i < count; ++i)
      D3D9ClearColorPipe(device, processor->render_target,
                         processor->render_target_subresource, &strips[i],
                         background, false, true);
   return S_OK;
}

HRESULT APIENTRY
D3D9DepthFill(HANDLE hDevice, const D3DDDIARG_DEPTHFILL *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   const float depth = data->Depth / 16777215.0f;
   HRESULT hr = D3D9ClearDepthPipe(device, data->hResource,
                                   data->SubResourceIndex, &data->DstRect,
                                   true, depth, false, 0) ?
      S_OK : E_NOTIMPL;
   return hr;
}

HRESULT APIENTRY
D3D9Present(HANDLE hDevice, const D3DDDIARG_PRESENT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;

   if (!device || !data)
      return E_INVALIDARG;

   HANDLE src_handle = data->hSrcResource ? data->hSrcResource :
      device->render_targets[0];
   UINT src_subresource_index = data->hSrcResource ?
      data->SrcSubResourceIndex : device->render_target_subresources[0];
   D3D9SubResource *src =
      D3D9GetSubResource(src_handle, src_subresource_index);
   D3D9SubResource *dst =
      D3D9GetSubResource(data->hDstResource, data->DstSubResourceIndex);
   D3D9Resource *src_resource = D3D9CastResource(src_handle);

   if (!src || !src_resource)
      return E_INVALIDARG;

   if (!src_resource->pipe_resource && src_resource->runtime_standard_primary) {
      D3D9Warnf("WARNING: D3D9 acknowledged runtime standard-primary "
                "Present without Gallium backing resource=%p "
                "allocation=0x%lx flags=0x%08x "
                "owner=d3d9_runtime reason=fullscreen_ownership_transition\n",
                src_resource, (unsigned long)src->allocation,
                src_resource->flags.Value);
      return S_OK;
   }

   if (!src_resource->pipe_resource) {
      D3D9Warnf("unsupported D3D9Present from resource without Gallium "
                "backing resource=%p allocation=0x%lx flags=0x%08x "
                "owner=d3d9 reason=no_gallium_backing_present\n",
                src_resource, (unsigned long)src->allocation,
                src_resource->flags.Value);
      return E_INVALIDARG;
   }

   if (src->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, src_subresource_index))
      return E_NOTIMPL;
   HRESULT hr = D3D9FlushFrontbuffer(device, src_resource,
                                     src_subresource_index,
                                     dst ? dst->allocation : 0,
                                     src_resource->flags.Primary != 0 &&
                                        data->Flags.Flip != 0);
   return hr;
}

HRESULT APIENTRY
D3D9Clear(HANDLE hDevice, const D3DDDIARG_CLEAR *data, UINT rect_count,
          const RECT *rects)
{

   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data || (rect_count && !rects))
      return E_INVALIDARG;

   const bool clear_color = (data->Flags & D3DCLEAR_TARGET) != 0;
   const bool clear_depth = (data->Flags & D3DCLEAR_ZBUFFER) != 0;
   const bool clear_stencil = (data->Flags & D3DCLEAR_STENCIL) != 0;
   if (!clear_color && !clear_depth && !clear_stencil)
      return S_OK;

   const UINT count = rect_count ? rect_count : 1;
   HRESULT first_failure = S_OK;

   for (UINT i = 0; i < count; ++i) {
      const RECT *rect = rect_count ? &rects[i] : NULL;

      if (clear_color) {
         for (UINT rt = 0; rt < ARRAYSIZE(device->render_targets); ++rt) {
            if (!device->render_targets[rt])
               continue;

            RECT clear_rect;
            D3D9SubResource *sub =
               D3D9GetSubResource(device->render_targets[rt],
                                  device->render_target_subresources[rt]);
            if (!D3D9ClearRectForSubResource(device, sub, rect, &clear_rect))
               continue;

            if (!D3D9ClearColorPipe(device, device->render_targets[rt],
                                    device->render_target_subresources[rt],
                                    &clear_rect, data->FillColor,
                                    device->render_states[
                                       D3DDDIRS_SRGBWRITEENABLE] != 0,
                                    false) &&
                SUCCEEDED(first_failure))
               first_failure = E_NOTIMPL;
         }
      }

      if ((clear_depth || clear_stencil) && device->depth_stencil) {
         RECT clear_rect;
         D3D9SubResource *sub = D3D9GetSubResource(device->depth_stencil, 0);
         if (!D3D9ClearRectForSubResource(device, sub, rect, &clear_rect))
            continue;

         if (!D3D9ClearDepthPipe(device, device->depth_stencil, 0,
                                 &clear_rect,
                                 clear_depth, data->FillDepth,
                                 clear_stencil, data->FillStencil) &&
             SUCCEEDED(first_failure))
            first_failure = E_NOTIMPL;
      }
   }

   return first_failure;
}

HRESULT APIENTRY
D3D9DestroyResource(HANDLE hDevice, HANDLE resource)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Resource *d3d9_resource = D3D9CastResource(resource);

   /*
    * Venus batches retain pipe_resource references until their GPU work
    * completes.  A D3D9 allocation created under data->hResource must be
    * deallocated before DestroyResource returns, because the runtime handle is
    * no longer valid afterwards.  Retire batches which reference resources
    * owned by this wrapper before dropping its final frontend references.
    */
   if (device && device->pipe && device->pipe->flush_resource &&
       d3d9_resource) {
      bool runtime_allocation = false;
      if (d3d9_resource->pipe_resource) {
         runtime_allocation |= yttrium_gdi_resource_has_runtime_allocation(
            d3d9_resource->pipe_resource);
         device->pipe->flush_resource(device->pipe,
                                      d3d9_resource->pipe_resource);
      }
      if (d3d9_resource->display_resource) {
         runtime_allocation |= yttrium_gdi_resource_has_runtime_allocation(
            d3d9_resource->display_resource);
         device->pipe->flush_resource(
            device->pipe, d3d9_resource->display_resource);
      }
      if (runtime_allocation) {
         struct pipe_context *driver_pipe =
            threaded_context_unwrap_sync(device->pipe);
         if (d3d9_resource->pipe_resource) {
            yttrium_gdi_pipeline_invalidate_resource(
               driver_pipe, d3d9_resource->pipe_resource);
         }
         if (d3d9_resource->display_resource) {
            yttrium_gdi_pipeline_invalidate_resource(
               driver_pipe, d3d9_resource->display_resource);
         }
      }
   }

   if (d3d9_resource && !d3d9_resource->pipe_resource) {
      const HRESULT hr = D3D9DeallocateResource(device, d3d9_resource);
      if (FAILED(hr))
         return hr;
   }

   if (device && device->last_systemmem_texture == d3d9_resource)
      device->last_systemmem_texture = NULL;
   if (device && device->last_systemmem_buffer == d3d9_resource)
      device->last_systemmem_buffer = NULL;
   if (device) {
      for (UINT i = 0; i < ARRAYSIZE(device->render_targets); ++i) {
         if (device->render_targets[i] == resource) {
            device->render_targets[i] = NULL;
            device->render_target_subresources[i] = 0;
         }
      }
      if (device->depth_stencil == resource)
         device->depth_stencil = NULL;
   }
   if (d3d9_resource && d3d9_resource->managed_source_resource &&
       d3d9_resource->managed_source_resource->managed_default_resource ==
       d3d9_resource) {
      d3d9_resource->managed_source_resource->managed_default_resource = NULL;
      if (d3d9_resource->managed_source_resource->
             managed_default_pipe_resource == d3d9_resource->pipe_resource) {
         /*
          * A managed SYSTEMMEM wrapper retains the uploaded default-pool
          * resource so later draws can reuse it.  Drop that hidden Gallium
          * reference while the default resource's D3D9 runtime handle is
          * still valid; otherwise the allocation survives this callback and
          * pfnDeallocateCb rejects its eventual destruction.
          */
         pipe_resource_reference(
            &d3d9_resource->managed_source_resource->
                managed_default_pipe_resource,
            NULL);
      }
   }
   if (d3d9_resource && d3d9_resource->managed_default_resource &&
       d3d9_resource->managed_default_resource->managed_source_resource ==
       d3d9_resource)
      d3d9_resource->managed_default_resource->managed_source_resource = NULL;
   if (d3d9_resource && device && device->pipe) {
      for (UINT i = 0; i < d3d9_resource->surf_count; ++i)
         D3D9ReleaseWorkerBufferUpload(
            device, &d3d9_resource->surfaces[i]);
   }
   D3D9FreeResource(d3d9_resource);
   return S_OK;
}
