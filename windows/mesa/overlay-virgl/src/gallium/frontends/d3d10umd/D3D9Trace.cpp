/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 *
 * A trace of what the Direct3D 9 runtime asks of the driver, for finding the one answer a
 * program could not take when nothing else says so. Off unless the environment variable
 * RIPPLE_UMD_TRACE is set in the program's environment: 1 writes every call that failed to
 * the driver's log (RippleLog.h), 2 every call, 3 also the arguments of the calls that move
 * pictures (resource creation, lock, blit, fill, present). The adapter's capability questions
 * are written with their type either way.
 */

#include "D3D9Private.h"
#include "D3D9Trace.h"
#include "RippleLog.h"

static int d3d9_trace_level = -1;

int
D3D9TraceLevel(void)
{
   if (d3d9_trace_level < 0) {
      const char *value = getenv("RIPPLE_UMD_TRACE");
      d3d9_trace_level = value ? atoi(value) : 0;
   }
   return d3d9_trace_level;
}

static void
D3D9TraceResult(const char *name, HRESULT hr)
{
   if (FAILED(hr) || d3d9_trace_level >= 2)
      ripple_logf("d3d9: %s -> 0x%08lx", name, (unsigned long)hr);
}

/* Level 3: what a call was about. Any other argument says nothing. */
template <typename T>
static void
D3D9TraceArg(T)
{
}

static void
D3D9TraceArg(D3DDDIARG_CREATERESOURCE *a)
{
   const D3DDDI_SURFACEINFO *s = a->SurfCount ? a->pSurfList : NULL;
   ripple_logf("  resource %p: format %d, pool %d, flags 0x%08x, %ux%u, %u surfaces, %u levels",
               a->hResource, (int)a->Format, (int)a->Pool, a->Flags.Value, s ? s->Width : 0,
               s ? s->Height : 0, a->SurfCount, a->MipLevels);
   D3D9SubResource *sub = a->hResource ? D3D9GetSubResource(a->hResource, 0) : NULL;
   ripple_logf("    memory from Windows %p pitch %u; the driver's %p pitch %u", s ? s->pSysMem : NULL,
               s ? s->SysMemPitch : 0, sub ? (void *)sub->data : NULL, sub ? sub->pitch : 0);
}

static bool
D3D9TraceIsColor32(HANDLE handle)
{
   const D3D9Resource *resource = D3D9CastResource(handle);
   return resource &&
      (resource->format == D3DDDIFMT_A8R8G8B8 || resource->format == D3DDDIFMT_X8R8G8B8 ||
       resource->format == D3DDDIFMT_A8B8G8R8 || resource->format == D3DDDIFMT_X8B8G8R8);
}

static UINT
D3D9TracePixel(const uint8_t *data, size_t pitch, size_t x, size_t y)
{
   UINT pixel;
   memcpy(&pixel, data + y * pitch + x * sizeof(pixel), sizeof(pixel));
   return pixel;
}

/* How much of a surface in memory holds a picture: of 256 points spread over it, how many are
 * not black. The pixel in the middle alone cannot tell an empty frame from a dark one. */
static unsigned
D3D9TracePointsSet(const D3D9SubResource *sub)
{
   unsigned set = 0;
   for (unsigned j = 0; j < 16; ++j) {
      const size_t y = (uint64_t)sub->height * (2 * j + 1) / 32;
      for (unsigned i = 0; i < 16; ++i) {
         const size_t x = (uint64_t)sub->width * (2 * i + 1) / 32;
         set += (D3D9TracePixel(sub->data, sub->pitch, x, y) & 0x00ffffff) != 0;
      }
   }
   return set;
}

static bool
D3D9TraceHasPixels(HANDLE handle, const D3D9SubResource *sub)
{
   /* Compressed textures can have a generous pitch but store one row per four texels. */
   if (!sub || !sub->data || !D3D9TraceIsColor32(handle) || !sub->width ||
       !sub->height || sub->width > sub->pitch / sizeof(UINT))
      return false;
   const size_t row_bytes = (size_t)sub->width * sizeof(UINT);
   return row_bytes <= sub->data_capacity &&
      sub->height - 1 <= (sub->data_capacity - row_bytes) / sub->pitch;
}

static void
D3D9TraceArg(D3DDDIARG_LOCK *a)
{
   D3D9SubResource *sub = D3D9GetSubResource(a->hResource, a->SubResourceIndex);
   ripple_logf("  lock %p.%u flags 0x%08x pitch %u at %p (memory %p)", a->hResource, a->SubResourceIndex,
               a->Flags.Value, a->Pitch, a->pSurfData, sub ? (void *)sub->data : NULL);
   if (D3D9TraceHasPixels(a->hResource, sub))
      ripple_logf("    in memory at the lock: %u of 256 points not black", D3D9TracePointsSet(sub));
}

static void
D3D9TraceArg(const D3DDDIARG_UNLOCK *a)
{
   D3D9SubResource *sub = D3D9GetSubResource(a->hResource, a->SubResourceIndex);
   if (D3D9TraceHasPixels(a->hResource, sub))
      ripple_logf("  unlock %p.%u, pixel in the middle 0x%08x, %u of 256 points not black", a->hResource,
                  a->SubResourceIndex,
                  D3D9TracePixel(sub->data, sub->pitch, sub->width / 2, sub->height / 2),
                  D3D9TracePointsSet(sub));
}

static void
D3D9TraceArg(const D3DDDIARG_RENDERSTATE *a)
{
   if (a)
      ripple_logf("  render state %u = 0x%08x", (unsigned)a->State, (unsigned)a->Value);
}

static void
D3D9TraceArg(const D3DDDIARG_TEXTURESTAGESTATE *a)
{
   if (a)
      ripple_logf("  stage %u state %u = 0x%08x", (unsigned)a->Stage, (unsigned)a->State,
                  (unsigned)a->Value);
}

static void
D3D9TraceArg(const D3DDDIARG_DRAWPRIMITIVE *a)
{
   if (a)
      ripple_logf("  primitive type %u, first vertex %u, count %u", (unsigned)a->PrimitiveType,
                  (unsigned)a->VStart, (unsigned)a->PrimitiveCount);
}

static void
D3D9TraceArg(const D3DDDIARG_BLT *a)
{
   ripple_logf("  blt %p.%u (%ld,%ld)-(%ld,%ld) to %p.%u (%ld,%ld)-(%ld,%ld) flags 0x%08x", a->hSrcResource,
               a->SrcSubResourceIndex, a->SrcRect.left, a->SrcRect.top, a->SrcRect.right, a->SrcRect.bottom,
               a->hDstResource, a->DstSubResourceIndex, a->DstRect.left, a->DstRect.top, a->DstRect.right,
               a->DstRect.bottom, a->Flags.Value);
   /* A copy of the CPU's pixels: the one in the middle, to see whether there is a picture */
   D3D9SubResource *src = D3D9GetSubResource(a->hSrcResource, a->SrcSubResourceIndex);
   if (D3D9TraceHasPixels(a->hSrcResource, src))
      ripple_logf("    source in memory %ux%u, pixel in the middle 0x%08x", src->width, src->height,
                  D3D9TracePixel(src->data, src->pitch, src->width / 2, src->height / 2));
}

static void
D3D9TraceArg(const D3DDDIARG_COLORFILL *a)
{
   ripple_logf("  fill %p.%u (%ld,%ld)-(%ld,%ld) with 0x%08x", a->hResource, a->SubResourceIndex, a->DstRect.left,
               a->DstRect.top, a->DstRect.right, a->DstRect.bottom, (unsigned)a->Color);
}

static void
D3D9TraceArg(const D3DDDIARG_PRESENT *a)
{
   ripple_logf("  present %p.%u to %p.%u flags 0x%08x", a->hSrcResource, a->SrcSubResourceIndex, a->hDstResource,
               a->DstSubResourceIndex, a->Flags.Value);
   D3D9Resource *r = a->hSrcResource ? D3D9CastResource(a->hSrcResource) : NULL;
   D3D9SubResource *sub = r ? D3D9GetSubResource(a->hSrcResource, a->SrcSubResourceIndex) : NULL;
   if (r && r->pipe_resource)
      ripple_logf("    source flags 0x%08x, pipe format %d bind 0x%x samples %u, display copy %p, allocation 0x%lx",
                  r->flags.Value, (int)r->pipe_resource->format, r->pipe_resource->bind,
                  r->pipe_resource->nr_samples, (void *)r->display_resource,
                  sub ? (unsigned long)sub->allocation : 0);
}

static void
D3D9TraceArg(D3DDDIARG_CREATEVIDEOPROCESSDEVICE *a)
{
   if (a && a->pVideoProcGuid)
      ripple_logf("  video processor %08lx: video %ux%u format %d kind %u, %u/%u frames in, %u/%u out",
                  (unsigned long)a->pVideoProcGuid->Data1, a->VideoDesc.SampleWidth, a->VideoDesc.SampleHeight,
                  (int)a->VideoDesc.Format, (unsigned)a->VideoDesc.SampleFormat.SampleFormat,
                  a->VideoDesc.InputSampleFreq.Numerator, a->VideoDesc.InputSampleFreq.Denominator,
                  a->VideoDesc.OutputFrameFreq.Numerator, a->VideoDesc.OutputFrameFreq.Denominator);
}

static void
D3D9TraceArg(const D3DDDIARG_VIDEOPROCESSBLT *a)
{
   ripple_logf("  video blt to (%ld,%ld)-(%ld,%ld), %u samples", a->TargetRect.left, a->TargetRect.top,
               a->TargetRect.right, a->TargetRect.bottom, a->NumSrcSurfaces);
   for (UINT i = 0; i < a->NumSrcSurfaces && a->pSrcSurfaces; ++i) {
      const DXVADDI_VIDEOSAMPLE *v = &a->pSrcSurfaces[i];
      D3D9Resource *r = D3D9CastResource(v->SrcResource);
      ripple_logf("    sample %p.%u (%ld,%ld)-(%ld,%ld) to (%ld,%ld)-(%ld,%ld), format %d pool %d", v->SrcResource,
                  v->SrcSubResourceIndex, v->SrcRect.left, v->SrcRect.top, v->SrcRect.right, v->SrcRect.bottom,
                  v->DstRect.left, v->DstRect.top, v->DstRect.right, v->DstRect.bottom, r ? (int)r->format : -1,
                  r ? (int)r->pool : -1);
      /* 2: a progressive frame, 3 and 4: two fields interleaved, the even or the odd rows first */
      ripple_logf("      frame kind %u, shown from %lld to %lld, this output is for %lld",
                  (unsigned)v->SampleFormat.SampleFormat, (long long)v->Start, (long long)v->End,
                  (long long)a->TargetFrame);
   }
}

/* Level 3, before the call: what a mapped surface holds as it is unlocked */
template <typename T>
static void
D3D9TraceBefore(T)
{
}

/* What Windows itself passes with a lock: for a surface in its own memory, where it has it */
static void
D3D9TraceBefore(D3DDDIARG_LOCK *a)
{
   if (a && a->Flags.NotifyOnly)
      ripple_logf("  locking %p.%u: Windows passes %p pitch %u", a->hResource, a->SubResourceIndex,
                  a->pSurfData, a->Pitch);
}

static void
D3D9TraceBefore(const D3DDDIARG_UNLOCK *a)
{
   D3D9SubResource *sub = D3D9GetSubResource(a->hResource, a->SubResourceIndex);
   if (!sub || !sub->transfer || !sub->transfer_map || !D3D9TraceIsColor32(a->hResource) ||
       !sub->transfer->resource || sub->transfer->resource->target == PIPE_BUFFER)
      return;
   const pipe_box &box = sub->transfer->box;
   const size_t stride = sub->transfer->stride;
   if (box.width > 0 && box.height > 0 && (size_t)box.width <= stride / sizeof(UINT) &&
       (size_t)box.height <= SIZE_MAX / stride)
      ripple_logf("  unlocking %p.%u, mapped pixel in the middle 0x%08x", a->hResource, a->SubResourceIndex,
                  D3D9TracePixel((const uint8_t *)sub->transfer_map, stride, box.width / 2, box.height / 2));
}

/*
 * One wrapper per entry of the function table, whatever its arguments: the real function and
 * its name are kept per place of use (Id), and the wrapper calls through and writes the result.
 */
template <int Id, typename... Args>
static auto
D3D9TraceWrap(HRESULT (APIENTRY *real)(Args...), const char *name)
   -> HRESULT (APIENTRY *)(Args...)
{
   static HRESULT (APIENTRY *s_real)(Args...);
   static const char *s_name;

   struct Thunk {
      static HRESULT APIENTRY call(Args... args)
      {
         if (d3d9_trace_level >= 3)
            (D3D9TraceBefore(args), ...);
         HRESULT hr = s_real(args...);
         D3D9TraceResult(s_name, hr);
         /* what the call was about: at level 3, and for a call that failed */
         if (d3d9_trace_level >= 3 || FAILED(hr))
            (D3D9TraceArg(args), ...);
         return hr;
      }
   };

   if (!real)
      return nullptr;
   s_real = real;
   s_name = name;
   return &Thunk::call;
}

#define TRACE(member) funcs->member = D3D9TraceWrap<__LINE__>(funcs->member, #member + 3)

void
D3D9TraceDeviceFuncs(D3DDDI_DEVICEFUNCS *funcs)
{
   if (D3D9TraceLevel() <= 0)
      return;

   TRACE(pfnSetRenderState);
   TRACE(pfnUpdateWInfo);
   TRACE(pfnValidateDevice);
   TRACE(pfnSetTextureStageState);
   TRACE(pfnSetTexture);
   TRACE(pfnSetPixelShader);
   TRACE(pfnSetPixelShaderConst);
   TRACE(pfnSetStreamSourceUm);
   TRACE(pfnSetIndices);
   TRACE(pfnSetIndicesUm);
   TRACE(pfnDrawPrimitive);
   TRACE(pfnDrawIndexedPrimitive);
   TRACE(pfnDrawRectPatch);
   TRACE(pfnDrawTriPatch);
   TRACE(pfnDrawPrimitive2);
   TRACE(pfnDrawIndexedPrimitive2);
   TRACE(pfnVolBlt);
   TRACE(pfnBufBlt);
   TRACE(pfnTexBlt);
   TRACE(pfnStateSet);
   TRACE(pfnSetPriority);
   TRACE(pfnClear);
   TRACE(pfnUpdatePalette);
   TRACE(pfnSetPalette);
   TRACE(pfnSetVertexShaderConst);
   TRACE(pfnMultiplyTransform);
   TRACE(pfnSetTransform);
   TRACE(pfnSetViewport);
   TRACE(pfnSetZRange);
   TRACE(pfnSetMaterial);
   TRACE(pfnSetLight);
   TRACE(pfnCreateLight);
   TRACE(pfnDestroyLight);
   TRACE(pfnSetClipPlane);
   TRACE(pfnGetInfo);
   TRACE(pfnLock);
   TRACE(pfnUnlock);
   TRACE(pfnCreateResource);
   TRACE(pfnDestroyResource);
   TRACE(pfnSetDisplayMode);
   TRACE(pfnPresent);
   TRACE(pfnFlush);
   TRACE(pfnCreateVertexShaderFunc);
   TRACE(pfnDeleteVertexShaderFunc);
   TRACE(pfnSetVertexShaderFunc);
   TRACE(pfnCreateVertexShaderDecl);
   TRACE(pfnDeleteVertexShaderDecl);
   TRACE(pfnSetVertexShaderDecl);
   TRACE(pfnSetVertexShaderConstI);
   TRACE(pfnSetVertexShaderConstB);
   TRACE(pfnSetScissorRect);
   TRACE(pfnSetStreamSource);
   TRACE(pfnSetStreamSourceFreq);
   TRACE(pfnSetConvolutionKernelMono);
   TRACE(pfnComposeRects);
   TRACE(pfnBlt);
   TRACE(pfnCreateVideoProcessDevice);
   TRACE(pfnDestroyVideoProcessDevice);
   TRACE(pfnVideoProcessBeginFrame);
   TRACE(pfnVideoProcessEndFrame);
   TRACE(pfnSetVideoProcessRenderTarget);
   TRACE(pfnVideoProcessBlt);
   TRACE(pfnColorFill);
   TRACE(pfnDepthFill);
   TRACE(pfnCreateQuery);
   TRACE(pfnDestroyQuery);
   TRACE(pfnIssueQuery);
   TRACE(pfnGetQueryData);
   TRACE(pfnSetRenderTarget);
   TRACE(pfnSetDepthStencil);
   TRACE(pfnGenerateMipSubLevels);
   TRACE(pfnSetPixelShaderConstI);
   TRACE(pfnSetPixelShaderConstB);
   TRACE(pfnCreatePixelShader);
   TRACE(pfnDeletePixelShader);
   TRACE(pfnDestroyDevice);
   TRACE(pfnQueryResourceResidency);
   TRACE(pfnOpenResource);
   TRACE(pfnGetCaptureAllocationHandle);
   TRACE(pfnCaptureToSysMem);
#if D3D_UMD_INTERFACE_VERSION >= D3D_UMD_INTERFACE_VERSION_WIN7
   TRACE(pfnCreateVideoProcessor);
   TRACE(pfnSetVideoProcessBltState);
   TRACE(pfnGetVideoProcessBltStatePrivate);
   TRACE(pfnSetVideoProcessStreamState);
   TRACE(pfnGetVideoProcessStreamStatePrivate);
   TRACE(pfnVideoProcessBltHD);
   TRACE(pfnDestroyVideoProcessor);
   TRACE(pfnResolveSharedResource);
#endif
}

static HRESULT (APIENTRY *d3d9_real_get_caps)(HANDLE, const D3DDDIARG_GETCAPS *);
static HRESULT (APIENTRY *d3d9_real_create_device)(HANDLE, D3DDDIARG_CREATEDEVICE *);

static HRESULT APIENTRY
D3D9TraceGetCaps(HANDLE hAdapter, const D3DDDIARG_GETCAPS *data)
{
   HRESULT hr = d3d9_real_get_caps(hAdapter, data);

   ripple_logf("d3d9: GetCaps type %u, %u bytes -> 0x%08lx", data ? (unsigned)data->Type : 0,
               data ? data->DataSize : 0, (unsigned long)hr);
   /* The formats and what each can do, one line each: Direct3D 10 Level 9 wants a fixed set
    * of them with certain operations before it makes a device (docs/windows/mesa-win7.md) */
   if (data && SUCCEEDED(hr) && data->Type == D3DDDICAPS_GETFORMATDATA && data->pData) {
      const FORMATOP *ops = (const FORMATOP *)data->pData;
      for (UINT i = 0; i < data->DataSize / sizeof(FORMATOP); ++i)
         ripple_logf("  format %u (0x%08x) operations 0x%08x", (unsigned)ops[i].Format,
                     (unsigned)ops[i].Format, (unsigned)ops[i].Operations);
   }
   /* The video processor questions (24 to 30) name the processor and the video */
   if (data && data->Type >= D3DDDICAPS_GETVIDEOPROCESSORRTFORMATCOUNT &&
       data->Type <= D3DDDICAPS_FILTERPROPERTYRANGE && data->pInfo) {
      const DXVADDI_VIDEOPROCESSORINPUT *in = (const DXVADDI_VIDEOPROCESSORINPUT *)data->pInfo;
      const GUID *g = in->pVideoProcGuid;
      ripple_logf("  processor %08lx-%04x-%04x, video %ux%u format 0x%08x, target format %d",
                  g ? (unsigned long)g->Data1 : 0, g ? g->Data2 : 0, g ? g->Data3 : 0,
                  in->VideoDesc.SampleWidth, in->VideoDesc.SampleHeight,
                  (unsigned)in->VideoDesc.Format, (int)in->RenderTargetFormat);
   }
   return hr;
}

static HRESULT APIENTRY
D3D9TraceCreateDevice(HANDLE hAdapter, D3DDDIARG_CREATEDEVICE *data)
{
   HRESULT hr = d3d9_real_create_device(hAdapter, data);

   ripple_logf("d3d9: CreateDevice interface %u version %u -> 0x%08lx", data ? data->Interface : 0,
               data ? data->Version : 0, (unsigned long)hr);
   return hr;
}

void
D3D9TraceAdapterFuncs(D3DDDI_ADAPTERFUNCS *funcs)
{
   if (D3D9TraceLevel() <= 0)
      return;

   d3d9_real_get_caps = funcs->pfnGetCaps;
   d3d9_real_create_device = funcs->pfnCreateDevice;
   funcs->pfnGetCaps = D3D9TraceGetCaps;
   funcs->pfnCreateDevice = D3D9TraceCreateDevice;
}
