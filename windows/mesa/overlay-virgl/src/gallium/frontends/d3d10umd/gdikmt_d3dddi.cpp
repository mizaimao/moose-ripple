/*
 * Copyright 2026 the moose-ripple authors
 * SPDX-License-Identifier: MIT
 *
 * The kernel bridge (gdikmt/gdikmt.h) for a Direct3D 10 device, through the Direct3D runtime's
 * callbacks. The callbacks return HRESULTs; the bridge's callers test for a negative value,
 * which is failure in both worlds, so the values are passed on as they are.
 */

#include <stdlib.h>
#include <string.h>

#include "gdikmt_d3dddi.h"
#include "D3D9Trace.h"
#ifdef RIPPLE_VISTA
#include "RippleLog.h"
#include "virtio/wddm/viogpu_wddm_driver.h"
#endif

#define DDI_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define DDI_NO_MEMORY         ((NTSTATUS)0xC0000017L)
#define DDI_BUFFER_TOO_SMALL  ((NTSTATUS)0xC0000023L)

struct d3dddi_context {
   struct gdikmt_context base;
   HANDLE hContext;
};

static inline struct gdikmt_device_d3dddi *
d3dddi_device(struct gdikmt_device *dev)
{
   return reinterpret_cast<struct gdikmt_device_d3dddi *>(dev);
}

static inline struct d3dddi_context *
d3dddi_context(struct gdikmt_context *ctx)
{
   return reinterpret_cast<struct d3dddi_context *>(ctx);
}

/* The runtime's adapter callback answers only with the driver's private data. */
static NTSTATUS
d3dddi_query_adapter_info(struct gdikmt_device *dev, KMTQUERYADAPTERINFOTYPE type, void *data,
                          UINT size)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   D3DDDICB_QUERYADAPTERINFO arg;

   if (type != KMTQAITYPE_UMDRIVERPRIVATE)
      return DDI_INVALID_PARAMETER;

   memset(&arg, 0, sizeof(arg));
   arg.pPrivateDriverData = data;
   arg.PrivateDriverDataSize = size;
   return ddi->pAdapterCallbacks->pfnQueryAdapterInfoCb(ddi->hRTAdapter, &arg);
}

static NTSTATUS
d3dddi_escape(struct gdikmt_device *dev, void *data, UINT size)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   D3DDDICB_ESCAPE arg;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = ddi->hRTDevice;
   arg.pPrivateDriverData = data;
   arg.PrivateDriverDataSize = size;
   const unsigned long long timed = D3D9TimesOn() ? D3D9TimesNow() : 0;
   const HRESULT hr = ddi->KTCallbacks.pfnEscapeCb(ddi->hRTAdapter, &arg);
   if (timed)
      D3D9TimesCallback(D3D9_TIMED_ESCAPE, timed);
   return hr;
}

static NTSTATUS
d3dddi_create_allocation(struct gdikmt_device *dev, struct gdikmt_createallocation *create)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   D3DDDICB_ALLOCATE arg;
   HRESULT hr;

   if (ddi->isPrimary) {
      for (UINT i = 0; i < create->NumAllocations; i++) {
         create->pAllocationInfo[i].Flags.Primary = 1;
         create->pAllocationInfo[i].VidPnSourceId = ddi->allocationVidPn;
      }
   }

#ifdef RIPPLE_VISTA
   /* Vista refuses a primary (E_INVALIDARG) that can live only in the aperture segment. The
    * tag has the kernel driver put it in its desktop segment, as it does the desktop's own
    * primary (viogpum.h, VIOGPU_VISTA_PRIMARY). */
   VIOGPU_VISTA_PRIMARY tagged[4];
   void *untagged[4];
   const UINT ntagged = !ddi->isPrimary ? 0 : create->NumAllocations < 4 ? create->NumAllocations : 4;
   for (UINT i = 0; i < ntagged; i++) {
      D3DDDI_ALLOCATIONINFO *info = &create->pAllocationInfo[i];
      untagged[i] = info->pPrivateDriverData;
      if (info->PrivateDriverDataSize != sizeof(tagged[i].allocation))
         continue;
      memcpy(&tagged[i].allocation, info->pPrivateDriverData, sizeof(tagged[i].allocation));
      tagged[i].tag = VIOGPU_VISTA_DRAWN_PRIMARY_TAG;
      info->pPrivateDriverData = &tagged[i];
      info->PrivateDriverDataSize = sizeof(tagged[i]);
   }
#endif

   memset(&arg, 0, sizeof(arg));
   arg.pPrivateDriverData = create->pPrivateDriverData;
   arg.PrivateDriverDataSize = create->PrivateDriverDataSize;
   arg.hResource = ddi->hRTResource;
   arg.NumAllocations = create->NumAllocations;
   arg.pAllocationInfo = create->pAllocationInfo;

   hr = ddi->KTCallbacks.pfnAllocateCb(ddi->hRTDevice, &arg);
#ifdef RIPPLE_VISTA
   for (UINT i = 0; i < ntagged; i++) {
      D3DDDI_ALLOCATIONINFO *info = &create->pAllocationInfo[i];
      if (info->pPrivateDriverData == &tagged[i]) {
         info->pPrivateDriverData = untagged[i];
         info->PrivateDriverDataSize = sizeof(tagged[i].allocation);
      }
   }
   if (FAILED(hr))
      ripple_logf("Windows refused an allocation: 0x%08lx, primary %u, display %u, %u allocations",
                  (unsigned long)hr, (unsigned)ddi->isPrimary, (unsigned)ddi->allocationVidPn,
                  (unsigned)create->NumAllocations);
#endif
   if (SUCCEEDED(hr)) {
      /* Allocations of a runtime resource are given back through that resource. */
      create->hResource = ddi->hRTResource;
   }
   return hr;
}

static NTSTATUS
d3dddi_destroy_allocation(struct gdikmt_device *dev, HANDLE hResource,
                          D3DKMT_HANDLE hAllocation)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   D3DDDICB_DEALLOCATE arg;

   /* The runtime is destroying the device and takes every allocation with it; its resources,
    * through which some would be given back, are gone already. */
   if (dev->runtime_destroying)
      return 0;

   memset(&arg, 0, sizeof(arg));
   if (hResource) {
      arg.hResource = hResource;
   } else {
      arg.NumAllocations = 1;
      arg.HandleList = &hAllocation;
   }
   return ddi->KTCallbacks.pfnDeallocateCb(ddi->hRTDevice, &arg);
}

/*
 * The runtime has opened a shared resource before it asks the driver to: all there is to do
 * is to hand over what it said, in the two steps the callers expect.
 */
struct d3dddi_opened {
   UINT NumAllocations;
   const D3DDDI_OPENALLOCATIONINFO *pOpenAllocationInfo;
   D3DKMT_HANDLE hKMResource;
   const void *pPrivateDriverData;
   UINT PrivateDriverDataSize;
};

/* What the runtime said about the resource being opened, Direct3D 10's way or Direct3D 9's */
static bool
d3dddi_opened_resource(struct gdikmt_device *dev, struct d3dddi_opened *res)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);

   if (ddi->pD3D9OpenResource) {
      const D3DDDIARG_OPENRESOURCE *in = ddi->pD3D9OpenResource;
      res->NumAllocations = in->NumAllocations;
      res->pOpenAllocationInfo = in->pOpenAllocationInfo;
      res->hKMResource = in->hKMResource;
      res->pPrivateDriverData = in->pPrivateDriverData;
      res->PrivateDriverDataSize = in->PrivateDriverDataSize;
      return true;
   }
   if (ddi->pOpenResource) {
      const D3D10DDIARG_OPENRESOURCE *in = ddi->pOpenResource;
      res->NumAllocations = in->NumAllocations;
      res->pOpenAllocationInfo = in->pOpenAllocationInfo;
      res->hKMResource = in->hKMResource.handle;
      res->pPrivateDriverData = in->pPrivateDriverData;
      res->PrivateDriverDataSize = in->PrivateDriverDataSize;
      return true;
   }
   return false;
}

static NTSTATUS
d3dddi_query_allocation(struct gdikmt_device *dev, struct gdikmt_openallocation *open)
{
   struct d3dddi_opened opened, *res = &opened;
   UINT total = 0;

   if (!d3dddi_opened_resource(dev, res))
      return DDI_INVALID_PARAMETER;

   for (UINT i = 0; i < res->NumAllocations; i++)
      total += res->pOpenAllocationInfo[i].PrivateDriverDataSize;

   open->NumAllocations = res->NumAllocations;
   open->PrivateDriverDataSize = res->PrivateDriverDataSize;
   open->TotalBufferSize = total;
   return 0;
}

static NTSTATUS
d3dddi_open_allocation(struct gdikmt_device *dev, struct gdikmt_openallocation *open)
{
   struct d3dddi_opened opened, *res = &opened;
   char *buffer = static_cast<char *>(open->pTotalBuffer);
   UINT used = 0;

   if (!d3dddi_opened_resource(dev, res))
      return DDI_INVALID_PARAMETER;
   if (open->NumAllocations < res->NumAllocations ||
       open->PrivateDriverDataSize < res->PrivateDriverDataSize)
      return DDI_BUFFER_TOO_SMALL;

   for (UINT i = 0; i < res->NumAllocations; i++) {
      const D3DDDI_OPENALLOCATIONINFO *in = &res->pOpenAllocationInfo[i];
      D3DDDI_OPENALLOCATIONINFO *out = &open->pOpenAllocation[i];

      if (in->PrivateDriverDataSize > open->TotalBufferSize - used)
         return DDI_BUFFER_TOO_SMALL;

      out->hAllocation = in->hAllocation;
      out->pPrivateDriverData = buffer + used;
      out->PrivateDriverDataSize = in->PrivateDriverDataSize;
      if (in->PrivateDriverDataSize)
         memcpy(buffer + used, in->pPrivateDriverData, in->PrivateDriverDataSize);
      used += in->PrivateDriverDataSize;
   }

   if (res->PrivateDriverDataSize)
      memcpy(open->pPrivateDriverData, res->pPrivateDriverData, res->PrivateDriverDataSize);
   open->NumAllocations = res->NumAllocations;
   open->hResource = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(res->hKMResource));
   return 0;
}

static NTSTATUS
d3dddi_lock_allocation(struct gdikmt_device *dev, D3DKMT_HANDLE hAllocation,
                       D3DDDICB_LOCKFLAGS flags, void **ptr)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   D3DDDICB_LOCK arg;
   HRESULT hr;

   memset(&arg, 0, sizeof(arg));
   arg.hAllocation = hAllocation;
   arg.Flags = flags;

   const unsigned long long timed = D3D9TimesOn() ? D3D9TimesNow() : 0;
   hr = ddi->KTCallbacks.pfnLockCb(ddi->hRTDevice, &arg);
   if (timed)
      D3D9TimesCallback(D3D9_TIMED_LOCK, timed);
   if (SUCCEEDED(hr))
      *ptr = arg.pData;
   return hr;
}

#ifdef RIPPLE_VISTA
static NTSTATUS
d3dddi_unlock_allocation(struct gdikmt_device *dev, D3DKMT_HANDLE hAllocation)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   D3DDDICB_UNLOCK arg = {};
   arg.NumAllocations = 1;
   arg.phAllocations = &hAllocation;
   const unsigned long long timed = D3D9TimesOn() ? D3D9TimesNow() : 0;
   const HRESULT hr = ddi->KTCallbacks.pfnUnlockCb(ddi->hRTDevice, &arg);
   if (timed)
      D3D9TimesCallback(D3D9_TIMED_UNLOCK, timed);
   return hr;
}
#endif

static NTSTATUS
d3dddi_render(struct gdikmt_context *ctx, struct gdikmt_render *render)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(ctx->device);
   D3DDDICB_RENDER arg;
   HRESULT hr;

   memset(&arg, 0, sizeof(arg));
   arg.hContext = d3dddi_context(ctx)->hContext;
   arg.CommandOffset = render->CommandOffset;
   arg.CommandLength = render->CommandLength;
   arg.NumAllocations = render->AllocationCount;
   arg.NumPatchLocations = render->PatchLocationCount;
   arg.NewCommandBufferSize = render->NewCommandBufferSize;
   arg.NewAllocationListSize = render->NewAllocationListSize;
   arg.NewPatchLocationListSize = render->NewPatchLocationListSize;
   arg.Flags.ResizeCommandBuffer = render->ResizeCommandBuffer;
   arg.Flags.ResizeAllocationList = render->ResizeAllocationList;
   arg.Flags.ResizePatchLocationList = render->ResizePatchLocationList;

   const unsigned long long timed = D3D9TimesOn() ? D3D9TimesNow() : 0;
   hr = ddi->KTCallbacks.pfnRenderCb(ddi->hRTDevice, &arg);
   if (timed)
      D3D9TimesCallback(D3D9_TIMED_RENDER, timed);
   if (FAILED(hr))
      return hr;

   ctx->pCommandBuffer = arg.pNewCommandBuffer;
   ctx->CommandBufferSize = arg.NewCommandBufferSize;
   ctx->pAllocationList = arg.pNewAllocationList;
   ctx->AllocationListSize = arg.NewAllocationListSize;
   ctx->pPatchLocationList = arg.pNewPatchLocationList;
   ctx->PatchLocationListSize = arg.NewPatchLocationListSize;

   if (render->CompletionEvent)
      SetEvent(render->CompletionEvent);
   return hr;
}

static void
d3dddi_context_destroy(struct gdikmt_context *ctx)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(ctx->device);
   D3DDDICB_DESTROYCONTEXT arg;

   arg.hContext = d3dddi_context(ctx)->hContext;
   ddi->KTCallbacks.pfnDestroyContextCb(ddi->hRTDevice, &arg);
   free(ctx);
}

static NTSTATUS
d3dddi_create_context(struct gdikmt_device *dev, struct gdikmt_context **out)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   struct d3dddi_context *ctx =
      static_cast<struct d3dddi_context *>(calloc(1, sizeof(struct d3dddi_context)));
   D3DDDICB_CREATECONTEXT arg;
   HRESULT hr;

   if (!ctx)
      return DDI_NO_MEMORY;

   memset(&arg, 0, sizeof(arg));
   arg.NodeOrdinal = 0;
   arg.EngineAffinity = 1;

   hr = ddi->KTCallbacks.pfnCreateContextCb(ddi->hRTDevice, &arg);
   if (FAILED(hr)) {
      free(ctx);
      return hr;
   }

   ctx->hContext = arg.hContext;
   ctx->base.device = dev;
   ctx->base.pCommandBuffer = arg.pCommandBuffer;
   ctx->base.CommandBufferSize = arg.CommandBufferSize;
   ctx->base.pAllocationList = arg.pAllocationList;
   ctx->base.AllocationListSize = arg.AllocationListSize;
   ctx->base.pPatchLocationList = arg.pPatchLocationList;
   ctx->base.PatchLocationListSize = arg.PatchLocationListSize;
   ctx->base.render = d3dddi_render;
   ctx->base.destroy = d3dddi_context_destroy;

   *out = &ctx->base;
   return hr;
}

/*
 * The drawable is the context DXGI gave with its present call (DXGI_DDI_ARG_PRESENT), which
 * knows the window or the screen the picture goes to; DXGI works out the rest.
 */
static NTSTATUS
d3dddi_present(struct gdikmt_context *ctx, D3DKMT_HANDLE hAllocation,
               void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(ctx->device);
   DXGIDDICB_PRESENT arg;

   (void)sub_box;

   /* Direct3D 9: the device's own present callback, with the destination its runtime named */
   if (ddi->isD3D9) {
      struct gdikmt_present_info *info =
         static_cast<struct gdikmt_present_info *>(winsys_drawable_handle);
      D3DDDICB_PRESENT present;
      HRESULT hr;

      memset(&present, 0, sizeof(present));
      present.hSrcAllocation = hAllocation;
      if (info && info->magic == GDIKMT_PRESENT_INFO_MAGIC)
         present.hDstAllocation = info->hDstAllocation;
      present.hContext = d3dddi_context(ctx)->hContext;
      const unsigned long long timed = D3D9TimesOn() ? D3D9TimesNow() : 0;
      hr = ddi->KTCallbacks.pfnPresentCb(ddi->hRTDevice, &present);
      if (timed)
         D3D9TimesCallback(D3D9_TIMED_PRESENT, timed);
      if (info && info->magic == GDIKMT_PRESENT_INFO_MAGIC)
         info->status = hr;
      return hr;
   }

   memset(&arg, 0, sizeof(arg));
   arg.hSrcAllocation = hAllocation;
   arg.pDXGIContext = winsys_drawable_handle;
   arg.hContext = d3dddi_context(ctx)->hContext;
   return ddi->pDXGIBaseCallbacks->pfnPresentCb(ddi->hRTDevice, &arg);
}

static NTSTATUS
d3dddi_set_display_mode(struct gdikmt_device *dev, D3DKMT_HANDLE hPrimaryAllocation)
{
   struct gdikmt_device_d3dddi *ddi = d3dddi_device(dev);
   D3DDDICB_SETDISPLAYMODE arg;

   memset(&arg, 0, sizeof(arg));
   arg.hPrimaryAllocation = hPrimaryAllocation;
   return ddi->KTCallbacks.pfnSetDisplayModeCb(ddi->hRTDevice, &arg);
}

/* The device belongs to the runtime and the bridge to the Direct3D device's own state. */
static void
d3dddi_destroy(struct gdikmt_device *dev)
{
   (void)dev;
}

void
gdikmt_d3dddi_fill_basefuncs(struct gdikmt_device_d3dddi *device)
{
   device->base.queryAdapterInfo = d3dddi_query_adapter_info;
   device->base.escape = d3dddi_escape;
   device->base.createAllocation = d3dddi_create_allocation;
   device->base.destroyAllocation = d3dddi_destroy_allocation;
   device->base.queryAllocation = d3dddi_query_allocation;
   device->base.openAllocation = d3dddi_open_allocation;
   device->base.lockAllocation = d3dddi_lock_allocation;
#ifdef RIPPLE_VISTA
   device->base.unlockAllocation = d3dddi_unlock_allocation;
#endif
   device->base.createContext = d3dddi_create_context;
   device->base.present = d3dddi_present;
   device->base.setDisplayMode = d3dddi_set_display_mode;
   device->base.destroy = d3dddi_destroy;
}
