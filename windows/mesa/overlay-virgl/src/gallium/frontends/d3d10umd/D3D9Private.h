/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 * Copyright 2026 VMLabDev and contributors
 */

#pragma once

#include "Debug.h"
#include "util/u_inlines.h"

#include <winddk_compat.h>

#undef D3D_UMD_INTERFACE_VERSION
#ifdef RIPPLE_VISTA
#define D3D_UMD_INTERFACE_VERSION D3D_UMD_INTERFACE_VERSION_VISTA
#else
#define D3D_UMD_INTERFACE_VERSION D3D_UMD_INTERFACE_VERSION_WIN7
#endif

#include <d3d9.h>
#include <d3dumddi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "gdikmt/gdikmt.h"
#include "gdikmt_d3dddi.h"
#include "D3D9Host.h"
#include "D3D9Compat.h"

#ifndef D3DCAPS2_CANRENDERWINDOWED
#define D3DCAPS2_CANRENDERWINDOWED 0x00080000
#endif

#ifndef D3DDEVCAPS_FLOATTLVERTEX
#define D3DDEVCAPS_FLOATTLVERTEX 0x00000001
#endif

#ifndef D3DPRASTERCAPS_SUBPIXEL
#define D3DPRASTERCAPS_SUBPIXEL 0x00000020
#endif

#ifndef D3DPRASTERCAPS_STIPPLE
#define D3DPRASTERCAPS_STIPPLE 0x00000200
#endif

#ifndef D3DPRASTERCAPS_ZBIAS
#define D3DPRASTERCAPS_ZBIAS 0x00004000
#endif

#ifndef D3DPTEXTURECAPS_TRANSPARENCY
#define D3DPTEXTURECAPS_TRANSPARENCY 0x00000008
#endif

#define D3D9_TEXTURE_STAGE_COUNT (D3DVERTEXTEXTURESAMPLER3 + 1)
#define D3D9_TEXTURE_STAGE_STATE_COUNT 35
#define D3D9_MAX_USER_CLIP_PLANES 8

EXTERN_C struct pipe_screen *
d3d10_create_screen(gdikmt_device *device);

static inline bool
D3D9CanExpandPointsWithGs(struct pipe_screen *screen)
{
#ifdef RIPPLE_VISTA
   /* A rejected geometry shader invalidates the whole virgl context. */
   return screen->get_param(screen, PIPE_CAP_GLSL_FEATURE_LEVEL) >= 150 &&
          screen->get_param(screen, PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES) >= 4;
#else
   return true;
#endif
}

struct D3D9Adapter
{
   HANDLE hRTAdapter;
   D3DDDI_ADAPTERCALLBACKS callbacks;
   UINT ms_types;
   float max_point_size;
};

struct D3D9Resource;
struct D3D9KeptObject;

struct D3D9BoundConstants
{
   void *data; /* owned copy of the last packed buffer */
   unsigned size;
   bool valid;
};

/* What the draws have left bound in the context (D3D9Draw.cpp, D3D9Bind).
 * The context holds the state objects. */
struct D3D9Bound
{
   bool live;
   void *blend;
   void *rasterizer;
   void *dsa;
   void *vertex_elements;
   void *vs;
   void *fs;
   void *gs;
   D3D9BoundConstants vs_constants;
   D3D9BoundConstants ps_constants;
   bool fb_valid;
   struct pipe_framebuffer_state fb;
   /* the values last set, where a draw sets the same again */
   bool viewport_valid;
   struct pipe_viewport_state viewport;
   bool blend_color_valid;
   struct pipe_blend_color blend_color;
   bool stencil_ref_valid;
   unsigned stencil_ref;
   bool sample_mask_valid;
   unsigned sample_mask;
   unsigned ps_count;
   unsigned vs_count;
   void *samplers[D3D9_TEXTURE_STAGE_COUNT];
   struct pipe_sampler_view *views[D3D9_TEXTURE_STAGE_COUNT];
};

struct D3D9Device
{
   HANDLE hRTDevice;
   /* D3DDDIARG_CREATEDEVICE::Interface: 9 for the D3D9 runtime, lower for
    * DirectDraw and Direct3D 7 and earlier. */
   UINT runtime_interface;
   D3DDDI_DEVICECALLBACKS callbacks;
   struct pipe_screen *screen;
   struct pipe_context *pipe;
   bool exact_occlusion;
   /* samples a pixel of the last primary the device made: 0 or 1 without multisampling */
   unsigned primary_samples;
   struct D3D9OcclusionSegment *active_occlusion;
   struct pipe_query *retired_occlusion;
   UINT occlusion_boundaries;
   struct D3D9Query *building_queries;
   struct D3D9Query *queries;
   D3D9Resource *resources;
   struct gdikmt_device_d3dddi gdi_device;
   UINT render_states[256];
   UINT texture_stage_states[D3D9_TEXTURE_STAGE_COUNT][D3D9_TEXTURE_STAGE_STATE_COUNT];
   UINT texture_transform_count;
   BOOL texture_transform_count_pending;
   BOOL texture_transform_count_consumed;
   D3DMATRIX transforms[512];
   D3DDDIARG_VIEWPORTINFO viewport;
   D3DDDIARG_ZRANGE zrange;
   D3DDDIARG_WINFO winfo;
   RECT scissor_rect;
   D3DDDIARG_SETMATERIAL material;
   FLOAT clip_planes[D3D9_MAX_USER_CLIP_PLANES][4];
   HANDLE pixel_shader;
   HANDLE vertex_shader_func;
   HANDLE vertex_shader_decl;
   FLOAT vs_float_constants[256][4];
   INT vs_int_constants[16][4];
   BOOL vs_bool_constants[256];
   FLOAT ps_float_constants[224][4];
   INT ps_int_constants[16][4];
   BOOL ps_bool_constants[256];
   HANDLE textures[D3D9_TEXTURE_STAGE_COUNT];
   struct pipe_resource *null_textures[4];
   /* State objects kept from draw to draw (D3D9Draw.cpp, D3D9Kept) */
   struct D3D9KeptObject *kept;
   unsigned kept_serial;
   struct D3D9Bound bound;
   D3D9Resource *last_systemmem_texture;
   D3D9Resource *last_systemmem_buffer;
   /* D3D9KeyBlitPipe's shaders and state, made on first use. */
   struct {
      void *vs, *fs[2], *sampler, *blend, *rasterizer, *dsa, *velems;
      /* the video processor's conversion draw (D3D9VideoDrawNv12) shares the rest */
      void *video_fs, *video_sampler;
   } key_blit;
   HANDLE render_targets[4];
   UINT render_target_subresources[4];
   HANDLE depth_stencil;
   HANDLE index_buffer;
   const void *index_sysmem;
   UINT index_stride;
   struct {
      HANDLE vertex_buffer;
      const void *sysmem;
      UINT offset;
      UINT stride;
      UINT divider;
   } streams[16];
   struct {
      UINT flags;
      HANDLE resource;
      PALETTEENTRY entries[256];
   } palettes[256];
   struct {
      BOOL enabled;
      D3DDDI_LIGHT data;
   } lights[16];
};

struct D3D9SubResource
{
   UINT width;
   UINT height;
   UINT depth;
   UINT pitch;
   UINT slice_pitch;
   size_t size;
   uint8_t *data;
   size_t data_capacity;
   bool owns_data;
   D3DKMT_HANDLE allocation;
   bool cpu_dirty;
   struct pipe_transfer *transfer;
   struct pipe_resource *readback_resource;
   bool transfer_on_readback;
   bool transfer_write;
   struct pipe_box transfer_box;
   void *transfer_map;
   struct pipe_resource *worker_upload_buffer;
   unsigned worker_upload_offset;
   unsigned worker_upload_source_offset;
   unsigned worker_upload_size;
   bool cpu_locked;
   bool cpu_lock_write;
   bool cpu_lock_packed_mips;
   bool notify_only_locked;
   bool notify_only_write;
   bool notify_only_full_mip_chain;
   /* Vista: one buffer of a primary flip chain, the image shown when Windows flips to this
    * surface. Every surface of the chain is drawn in the resource's one pipe_resource, which
    * is copied here at each flip (D3D9Resource.cpp, D3D9FlushFrontbuffer). */
   struct pipe_resource *flip_resource;
};

struct D3D9Resource
{
   D3D9Device *device;
   D3D9Resource *previous;
   D3D9Resource *next;
   HANDLE runtime_handle;
   D3DDDIFORMAT format;
   D3DDDI_POOL pool;
   D3DDDI_RESOURCEFLAGS flags;
   UINT priority;
   UINT surf_count;
   UINT mip_levels;
   struct pipe_resource *pipe_resource;
   uint8_t *watched_buffer;
   /* The last sampler view and the last surface made of it, kept while the next one asked
    * for is the same (D3D9Draw.cpp); let go with D3D9ReleaseKept */
   struct pipe_sampler_view *kept_view;
   struct pipe_sampler_view kept_view_templ;
   struct pipe_surface *kept_surface;
   struct pipe_surface kept_surface_templ;
   /* Runtime-visible linear Present/capture allocation; never a draw target. */
   struct pipe_resource *display_resource;
   D3D9SubResource *surfaces;
   UINT notify_lock_mip_cursor;
   UINT notify_lock_subresource;
   bool notify_lock_subresource_valid;
   bool managed_upload_complete;
   bool may_watch_buffer;
   /* Watched buffer: its current lock only appends (NoOverwrite); a lock gave the contents
    * up (Discard) and no upload has taken new storage since; an ordered upload is queued
    * (D3D9UploadWatchedBuffer). */
   bool watched_append;
   bool watched_discard;
   bool watched_ordered;
   bool autogen_mipmap_dirty;
   struct pipe_resource *managed_default_pipe_resource;
   struct pipe_resource *managed_source_pipe_resource;
   D3D9Resource *managed_default_resource;
   D3D9Resource *managed_source_resource;
   UINT managed_source_base_level;
   bool runtime_standard_primary;
};

static inline UINT
D3D9PipeMipLevel(const D3D9Resource *resource, UINT subresource_index)
{
   const UINT mip_levels = resource && resource->mip_levels ?
      resource->mip_levels : 1;
   return subresource_index % mip_levels;
}

static inline UINT
D3D9PipeLayer(const D3D9Resource *resource, UINT subresource_index)
{
   if (!resource)
      return 0;

   if (!resource->flags.CubeMap &&
       (!resource->pipe_resource || resource->pipe_resource->array_size <= 1))
      return 0;

   const UINT mip_levels = resource->mip_levels ? resource->mip_levels : 1;
   return subresource_index / mip_levels;
}

static inline void
D3D9MarkAutogenMipmapsDirty(D3D9Resource *resource, UINT subresource_index)
{
   if (!resource || !resource->flags.AutogenMipmap ||
       D3D9PipeMipLevel(resource, subresource_index) != 0)
      return;

   resource->autogen_mipmap_dirty = true;
}

enum D3D9ObjectKind
{
   D3D9_OBJECT_SHADER,
   D3D9_OBJECT_VERTEX_DECL,
   D3D9_OBJECT_QUERY,
};

struct nine_range;

struct D3D9Object
{
   D3D9ObjectKind kind;
   void *translated_vs_cso;
   UINT translated_vs_key;
   unsigned translated_vs_const_used_size;
   unsigned *translated_vs_const_ranges;
   struct nine_range *translated_vs_lconstf_ranges;
   float *translated_vs_lconstf_data;
   bool translated_vs_outputs_point_size;
   uint16_t translated_vs_input_map[PIPE_MAX_ATTRIBS];
   uint8_t translated_vs_num_inputs;
   uint16_t translated_vs_sampler_mask;
   uint8_t translated_vs_sampler_targets[4];
   void *translated_ps_cso;
   UINT translated_ps_key;
   uint64_t translated_ps_sampler_type_overrides;
   uint16_t translated_ps_sampler_type_override_mask;
   uint16_t translated_ps_fetch4;
   uint16_t translated_ps_fetch4_ati1;
   uint16_t translated_ps_fetch4_projected_fallback;
   unsigned translated_ps_const_used_size;
   unsigned *translated_ps_const_ranges;
   struct nine_range *translated_ps_lconstf_ranges;
   float *translated_ps_lconstf_data;
   bool translated_ps_uses_face;
   uint16_t translated_ps_input_map[PIPE_MAX_ATTRIBS];
   uint8_t translated_ps_num_inputs;
   uint16_t translated_ps_sampler_mask;
   uint8_t translated_ps_sampler_targets[16];
   size_t size;
   uint8_t data[1];
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
static constexpr D3DDDIFORMAT D3D9_FMT_L6V5U5 = (D3DDDIFORMAT)61;
static constexpr D3DDDIFORMAT D3D9_FMT_X8L8V8U8 = (D3DDDIFORMAT)62;
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
static constexpr D3DDDIFORMAT D3D9_FMT_NV12 = D3D9FourCC('N', 'V', '1', '2');

/* The video formats the driver takes: frames a decoder leaves in a plain surface, which the
 * video processor converts (D3D9Resource.cpp). NV12 is a full-size plane of luma followed by
 * a half-size plane of chroma pairs; YUY2 is two bytes a pixel, luma and alternating chroma. */
static inline bool
D3D9IsVideoFormat(D3DDDIFORMAT format)
{
   return format == D3D9_FMT_NV12 || format == D3D9_FMT_YUY2;
}
static constexpr D3DDDI_POOL D3D9_POOL_STAGINGMEM = (D3DDDI_POOL)5;

static inline bool
D3D9ResourceUsesCpuBufferStorage(const D3D9Resource *resource)
{
   return resource &&
      (resource->flags.VertexBuffer || resource->flags.IndexBuffer) &&
      (resource->pool == D3DDDIPOOL_SYSTEMMEM ||
       resource->pool == D3D9_POOL_STAGINGMEM ||
       resource->flags.MightDrawFromLocked);
}

void D3D9Warnf(const char *format, ...);
void D3D9WarnOncef(volatile LONG *logged, const char *format, ...);
D3D9Resource *D3D9CastResource(HANDLE resource);
D3D9SubResource *D3D9GetSubResource(HANDLE resource_handle,
                                    UINT subresource_index);
bool D3D9PublishResource(D3D9Device *device, D3D9Resource *resource);
#ifdef RIPPLE_VISTA
/* Pictures for looking at (D3D9Resource.cpp): one image as a BMP file, and RIPPLE_DRAWDUMP */
void D3D9DumpImage(D3D9Device *device, struct pipe_resource *image, unsigned level,
                   const char *path);
void D3D9DrawDump(D3D9Device *device);
#endif

bool D3D9UploadSubResource(D3D9Device *device, D3D9Resource *resource,
                           UINT subresource_index);
D3D9Object *D3D9CreateObject(D3D9ObjectKind kind, size_t payload_size,
                             const void *payload);
bool D3D9CheckConstantRange(UINT reg, UINT count, UINT max_count);
void D3D9SetIdentityMatrix(D3DMATRIX *matrix);
void D3D9MatrixMultiply(D3DMATRIX *dst, const D3DMATRIX *a,
                        const D3DMATRIX *b);
void D3D9FillDeviceFuncs(D3DDDI_DEVICEFUNCS *funcs);
/* moose-ripple: which formats the host renderer lacks (D3D9Resource.cpp) */
void D3D9InitFormatsForScreen(struct pipe_screen *screen);

HRESULT APIENTRY D3D9SetPriority(HANDLE hDevice,
                                 const D3DDDIARG_SETPRIORITY *data);
HRESULT APIENTRY D3D9VolBlt(HANDLE hDevice,
                            const D3DDDIARG_VOLUMEBLT *data);
HRESULT APIENTRY D3D9BufBlt(HANDLE hDevice,
                            const D3DDDIARG_BUFFERBLT *data);
HRESULT APIENTRY D3D9TexBlt(HANDLE hDevice, const D3DDDIARG_TEXBLT *data);
HRESULT APIENTRY D3D9SetRenderTarget(HANDLE hDevice,
                                     const D3DDDIARG_SETRENDERTARGET *data);
HRESULT APIENTRY D3D9SetDepthStencil(HANDLE hDevice,
                                     const D3DDDIARG_SETDEPTHSTENCIL *data);
HRESULT APIENTRY D3D9CreateResource(HANDLE hDevice,
                                    D3DDDIARG_CREATERESOURCE *data);
HRESULT APIENTRY D3D9OpenResource(HANDLE hDevice,
                                  D3DDDIARG_OPENRESOURCE *data);
HRESULT APIENTRY D3D9Lock(HANDLE hDevice, D3DDDIARG_LOCK *data);
HRESULT APIENTRY D3D9Unlock(HANDLE hDevice, const D3DDDIARG_UNLOCK *data);
HRESULT APIENTRY D3D9LockAsync(HANDLE hDevice, D3DDDIARG_LOCKASYNC *data);
HRESULT APIENTRY D3D9UnlockAsync(HANDLE hDevice,
                                 const D3DDDIARG_UNLOCKASYNC *data);
HRESULT APIENTRY D3D9GetPitch(HANDLE hDevice, D3DDDIARG_GETPITCH *data);
#if D3D_UMD_INTERFACE_VERSION >= D3D_UMD_INTERFACE_VERSION_WIN7
HRESULT APIENTRY D3D9ResolveSharedResource(
   HANDLE hDevice, const D3DDDIARG_RESOLVESHAREDRESOURCE *data);
#endif
HRESULT APIENTRY D3D9QueryResourceResidency(
   HANDLE hDevice, const D3DDDIARG_QUERYRESOURCERESIDENCY *data);
HRESULT APIENTRY D3D9GetCaptureAllocationHandle(
   HANDLE hDevice, D3DDDIARG_GETCAPTUREALLOCATIONHANDLE *data);
HRESULT APIENTRY D3D9CaptureToSysMem(HANDLE hDevice,
                                     const D3DDDIARG_CAPTURETOSYSMEM *data);
HRESULT APIENTRY D3D9Blt(HANDLE hDevice, const D3DDDIARG_BLT *data);
HRESULT APIENTRY D3D9ColorFill(HANDLE hDevice,
                               const D3DDDIARG_COLORFILL *data);
HRESULT APIENTRY D3D9DepthFill(HANDLE hDevice,
                               const D3DDDIARG_DEPTHFILL *data);
HRESULT APIENTRY D3D9Present(HANDLE hDevice, const D3DDDIARG_PRESENT *data);
/* The video processor (D3D9Resource.cpp) */
extern const GUID D3D9ProgressiveVideoProcessor;
extern const GUID D3D9BobVideoProcessor;
bool D3D9VideoProcessorTakes(D3DDDIFORMAT video, D3DDDIFORMAT target);
HRESULT APIENTRY D3D9CreateVideoProcessDevice(
   HANDLE hDevice, D3DDDIARG_CREATEVIDEOPROCESSDEVICE *data);
HRESULT APIENTRY D3D9DestroyVideoProcessDevice(HANDLE hDevice,
                                               HANDLE hVideoProcessor);
HRESULT APIENTRY D3D9VideoProcessBeginFrame(HANDLE hDevice,
                                            HANDLE hVideoProcess);
HRESULT APIENTRY D3D9VideoProcessEndFrame(
   HANDLE hDevice, D3DDDIARG_VIDEOPROCESSENDFRAME *data);
HRESULT APIENTRY D3D9SetVideoProcessRenderTarget(
   HANDLE hDevice, const D3DDDIARG_SETVIDEOPROCESSRENDERTARGET *data);
HRESULT APIENTRY D3D9VideoProcessBlt(HANDLE hDevice,
                                     const D3DDDIARG_VIDEOPROCESSBLT *data);
HRESULT APIENTRY D3D9Clear(HANDLE hDevice, const D3DDDIARG_CLEAR *data,
                           UINT rect_count, const RECT *rects);
HRESULT APIENTRY D3D9DestroyResource(HANDLE hDevice, HANDLE resource);
HRESULT APIENTRY D3D9DrawPrimitive(HANDLE hDevice,
                                   const D3DDDIARG_DRAWPRIMITIVE *data,
                                   const UINT *indices);
HRESULT APIENTRY D3D9DrawIndexedPrimitive(
   HANDLE hDevice, const D3DDDIARG_DRAWINDEXEDPRIMITIVE *data);
HRESULT APIENTRY D3D9DrawPrimitive2(HANDLE hDevice,
                                    const D3DDDIARG_DRAWPRIMITIVE2 *data);
HRESULT APIENTRY D3D9DrawIndexedPrimitive2(
   HANDLE hDevice, const D3DDDIARG_DRAWINDEXEDPRIMITIVE2 *data,
   UINT index_size, const void *indices, const UINT *index_remap);
void D3D9DestroyDrawState(D3D9Device *device);
/* Takes off the context what the draws left bound: before setting the context's state by
 * hand, and before deleting a shader or state object that may be bound */
void D3D9SettleDrawState(D3D9Device *device);

/* Lets go of a resource's kept view and surface. They belong to the device's context, so
 * this has to happen while that still exists. */
static inline void
D3D9ReleaseKept(D3D9Resource *resource)
{
   pipe_sampler_view_reference(&resource->kept_view, NULL);
   pipe_surface_reference(&resource->kept_surface, NULL);
}
void D3D9DestroyKeyBlitState(D3D9Device *device);
