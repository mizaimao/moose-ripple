/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 *
 * A trace of what the Direct3D 9 runtime asks of the driver, for finding the one answer a
 * program could not take when nothing else says so. Off unless the environment variable
 * RIPPLE_UMD_TRACE is set in the program's environment: 1 writes every call that failed to
 * the driver's log (RippleLog.h), 2 every call. The adapter's capability questions are
 * written with their type either way.
 */

#include "D3D9Private.h"
#include "D3D9Trace.h"
#include "RippleLog.h"

static int d3d9_trace_level = -1;

static int
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
         HRESULT hr = s_real(args...);
         D3D9TraceResult(s_name, hr);
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
   TRACE(pfnCreateVideoProcessor);
   TRACE(pfnSetVideoProcessBltState);
   TRACE(pfnGetVideoProcessBltStatePrivate);
   TRACE(pfnSetVideoProcessStreamState);
   TRACE(pfnGetVideoProcessStreamStatePrivate);
   TRACE(pfnVideoProcessBltHD);
   TRACE(pfnDestroyVideoProcessor);
   TRACE(pfnResolveSharedResource);
}

static HRESULT (APIENTRY *d3d9_real_get_caps)(HANDLE, const D3DDDIARG_GETCAPS *);
static HRESULT (APIENTRY *d3d9_real_create_device)(HANDLE, D3DDDIARG_CREATEDEVICE *);

static HRESULT APIENTRY
D3D9TraceGetCaps(HANDLE hAdapter, const D3DDDIARG_GETCAPS *data)
{
   HRESULT hr = d3d9_real_get_caps(hAdapter, data);

   ripple_logf("d3d9: GetCaps type %u, %u bytes -> 0x%08lx", data ? (unsigned)data->Type : 0,
               data ? data->DataSize : 0, (unsigned long)hr);
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
