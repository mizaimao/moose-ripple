/*
 * Copyright 2026 the moose-ripple authors
 * SPDX-License-Identifier: MIT
 *
 * The kernel bridge (gdikmt.h) through the D3DKMT functions of gdi32.dll, for drivers that are
 * not loaded by the Direct3D runtime: OpenGL.
 *
 * The functions are looked up when first needed instead of being imported, so the DLL this ends
 * up in loads on any Windows, also where the display driver is not a WDDM driver at all.
 */

#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "pipe/p_state.h"

#include "gdikmt.h"

#define KMT_UNSUCCESSFUL      ((NTSTATUS)0xC0000001L)
#define KMT_NO_MEMORY         ((NTSTATUS)0xC0000017L)
#define KMT_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define KMT_SUCCESS(status)   ((NTSTATUS)(status) >= 0)

/* Every D3DKMT function takes one pointer to its argument structure. */
typedef NTSTATUS(APIENTRY *kmt_function)(void *);

enum {
   KMT_OPEN_ADAPTER_FROM_HDC,
   KMT_CLOSE_ADAPTER,
   KMT_QUERY_ADAPTER_INFO,
   KMT_ESCAPE,
   KMT_CREATE_DEVICE,
   KMT_DESTROY_DEVICE,
   KMT_CREATE_CONTEXT,
   KMT_DESTROY_CONTEXT,
   KMT_CREATE_ALLOCATION,
   KMT_DESTROY_ALLOCATION,
   KMT_QUERY_RESOURCE_INFO,
   KMT_OPEN_RESOURCE,
   KMT_LOCK,
   KMT_RENDER,
   KMT_PRESENT,
   KMT_SET_DISPLAY_MODE,
   KMT_COUNT
};

static const char *const kmt_names[KMT_COUNT] = {
   [KMT_OPEN_ADAPTER_FROM_HDC] = "D3DKMTOpenAdapterFromHdc",
   [KMT_CLOSE_ADAPTER] = "D3DKMTCloseAdapter",
   [KMT_QUERY_ADAPTER_INFO] = "D3DKMTQueryAdapterInfo",
   [KMT_ESCAPE] = "D3DKMTEscape",
   [KMT_CREATE_DEVICE] = "D3DKMTCreateDevice",
   [KMT_DESTROY_DEVICE] = "D3DKMTDestroyDevice",
   [KMT_CREATE_CONTEXT] = "D3DKMTCreateContext",
   [KMT_DESTROY_CONTEXT] = "D3DKMTDestroyContext",
   [KMT_CREATE_ALLOCATION] = "D3DKMTCreateAllocation",
   [KMT_DESTROY_ALLOCATION] = "D3DKMTDestroyAllocation",
   [KMT_QUERY_RESOURCE_INFO] = "D3DKMTQueryResourceInfo",
   [KMT_OPEN_RESOURCE] = "D3DKMTOpenResource",
   [KMT_LOCK] = "D3DKMTLock",
   [KMT_RENDER] = "D3DKMTRender",
   [KMT_PRESENT] = "D3DKMTPresent",
   [KMT_SET_DISPLAY_MODE] = "D3DKMTSetDisplayMode",
};

static kmt_function kmt[KMT_COUNT];
static bool kmt_found;
static once_flag kmt_once = ONCE_FLAG_INIT;

static void
kmt_lookup(void)
{
   HMODULE gdi32 = LoadLibraryA("gdi32.dll");
   if (!gdi32)
      return;

   for (unsigned i = 0; i < KMT_COUNT; i++) {
      kmt[i] = (kmt_function)(void *)GetProcAddress(gdi32, kmt_names[i]);
      if (!kmt[i])
         return;
   }
   kmt_found = true;
}

struct d3dkmt_device {
   struct gdikmt_device base;
   D3DKMT_HANDLE hAdapter;
   D3DKMT_HANDLE hDevice;
   D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
};

struct d3dkmt_context {
   struct gdikmt_context base;
   D3DKMT_HANDLE hContext;
};

static inline struct d3dkmt_device *
d3dkmt_device(struct gdikmt_device *dev)
{
   return (struct d3dkmt_device *)dev;
}

static inline struct d3dkmt_context *
d3dkmt_context(struct gdikmt_context *ctx)
{
   return (struct d3dkmt_context *)ctx;
}

static NTSTATUS
d3dkmt_query_adapter_info(struct gdikmt_device *dev, KMTQUERYADAPTERINFOTYPE type, void *data,
                          UINT size)
{
   D3DKMT_QUERYADAPTERINFO arg;

   memset(&arg, 0, sizeof(arg));
   arg.hAdapter = d3dkmt_device(dev)->hAdapter;
   arg.Type = type;
   arg.pPrivateDriverData = data;
   arg.PrivateDriverDataSize = size;
   return kmt[KMT_QUERY_ADAPTER_INFO](&arg);
}

static NTSTATUS
d3dkmt_escape(struct gdikmt_device *dev, void *data, UINT size)
{
   D3DKMT_ESCAPE arg;

   memset(&arg, 0, sizeof(arg));
   arg.hAdapter = d3dkmt_device(dev)->hAdapter;
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   arg.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
   arg.pPrivateDriverData = data;
   arg.PrivateDriverDataSize = size;
   return kmt[KMT_ESCAPE](&arg);
}

static NTSTATUS
d3dkmt_create_allocation(struct gdikmt_device *dev, struct gdikmt_createallocation *create)
{
   D3DKMT_CREATEALLOCATION arg;
   NTSTATUS status;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   arg.pPrivateDriverData = create->pPrivateDriverData;
   arg.PrivateDriverDataSize = create->PrivateDriverDataSize;
   arg.NumAllocations = create->NumAllocations;
   arg.pAllocationInfo = create->pAllocationInfo;
   arg.Flags.CreateResource = 1;

   status = kmt[KMT_CREATE_ALLOCATION](&arg);
   if (KMT_SUCCESS(status))
      create->hResource = (HANDLE)(UINT_PTR)arg.hResource;
   return status;
}

static NTSTATUS
d3dkmt_destroy_allocation(struct gdikmt_device *dev, HANDLE hResource,
                          D3DKMT_HANDLE hAllocation)
{
   D3DKMT_DESTROYALLOCATION arg;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   if (hResource) {
      /* The resource takes its allocations with it. */
      arg.hResource = (D3DKMT_HANDLE)(UINT_PTR)hResource;
   } else {
      arg.phAllocationList = &hAllocation;
      arg.AllocationCount = 1;
   }
   return kmt[KMT_DESTROY_ALLOCATION](&arg);
}

static NTSTATUS
d3dkmt_query_allocation(struct gdikmt_device *dev, struct gdikmt_openallocation *open)
{
   D3DKMT_QUERYRESOURCEINFO arg;
   NTSTATUS status;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   arg.hGlobalShare = open->hGlobalHandle;

   status = kmt[KMT_QUERY_RESOURCE_INFO](&arg);
   if (KMT_SUCCESS(status)) {
      open->NumAllocations = arg.NumAllocations;
      open->PrivateDriverDataSize = arg.ResourcePrivateDriverDataSize;
      open->TotalBufferSize = arg.TotalPrivateDriverDataSize;
   }
   return status;
}

static NTSTATUS
d3dkmt_open_allocation(struct gdikmt_device *dev, struct gdikmt_openallocation *open)
{
   D3DKMT_OPENRESOURCE arg;
   NTSTATUS status;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   arg.hGlobalShare = open->hGlobalHandle;
   arg.NumAllocations = open->NumAllocations;
   arg.pOpenAllocationInfo = open->pOpenAllocation;
   arg.pResourcePrivateDriverData = open->pPrivateDriverData;
   arg.ResourcePrivateDriverDataSize = open->PrivateDriverDataSize;
   arg.pTotalPrivateDriverDataBuffer = open->pTotalBuffer;
   arg.TotalPrivateDriverDataBufferSize = open->TotalBufferSize;

   status = kmt[KMT_OPEN_RESOURCE](&arg);
   if (KMT_SUCCESS(status))
      open->hResource = (HANDLE)(UINT_PTR)arg.hResource;
   return status;
}

static NTSTATUS
d3dkmt_lock_allocation(struct gdikmt_device *dev, D3DKMT_HANDLE hAllocation,
                       D3DDDICB_LOCKFLAGS flags, void **ptr)
{
   D3DKMT_LOCK arg;
   NTSTATUS status;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   arg.hAllocation = hAllocation;
   arg.Flags = flags;

   status = kmt[KMT_LOCK](&arg);
   if (KMT_SUCCESS(status))
      *ptr = arg.pData;
   return status;
}

static NTSTATUS
d3dkmt_render(struct gdikmt_context *ctx, struct gdikmt_render *render)
{
   D3DKMT_RENDER arg;
   NTSTATUS status;

   memset(&arg, 0, sizeof(arg));
   arg.hContext = d3dkmt_context(ctx)->hContext;
   arg.CommandOffset = render->CommandOffset;
   arg.CommandLength = render->CommandLength;
   arg.AllocationCount = render->AllocationCount;
   arg.PatchLocationCount = render->PatchLocationCount;
   arg.NewCommandBufferSize = render->NewCommandBufferSize;
   arg.NewAllocationListSize = render->NewAllocationListSize;
   arg.NewPatchLocationListSize = render->NewPatchLocationListSize;
   arg.Flags.ResizeCommandBuffer = render->ResizeCommandBuffer;
   arg.Flags.ResizeAllocationList = render->ResizeAllocationList;
   arg.Flags.ResizePatchLocationList = render->ResizePatchLocationList;

   status = kmt[KMT_RENDER](&arg);
   if (!KMT_SUCCESS(status))
      return status;

   ctx->pCommandBuffer = arg.pNewCommandBuffer;
   ctx->CommandBufferSize = arg.NewCommandBufferSize;
   ctx->pAllocationList = arg.pNewAllocationList;
   ctx->AllocationListSize = arg.NewAllocationListSize;
   ctx->pPatchLocationList = arg.pNewPatchLocationList;
   ctx->PatchLocationListSize = arg.NewPatchLocationListSize;

   if (render->CompletionEvent)
      SetEvent(render->CompletionEvent);
   return status;
}

static void
d3dkmt_context_destroy(struct gdikmt_context *ctx)
{
   D3DKMT_DESTROYCONTEXT arg;

   arg.hContext = d3dkmt_context(ctx)->hContext;
   kmt[KMT_DESTROY_CONTEXT](&arg);
   free(ctx);
}

static NTSTATUS
d3dkmt_create_context(struct gdikmt_device *dev, struct gdikmt_context **out)
{
   struct d3dkmt_context *ctx = calloc(1, sizeof(*ctx));
   D3DKMT_CREATECONTEXT arg;
   NTSTATUS status;

   if (!ctx)
      return KMT_NO_MEMORY;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   arg.NodeOrdinal = 0;
   arg.EngineAffinity = 1;
   arg.ClientHint = D3DKMT_CLIENTHINT_OPENGL;

   status = kmt[KMT_CREATE_CONTEXT](&arg);
   if (!KMT_SUCCESS(status)) {
      free(ctx);
      return status;
   }

   ctx->hContext = arg.hContext;
   ctx->base.device = dev;
   ctx->base.pCommandBuffer = arg.pCommandBuffer;
   ctx->base.CommandBufferSize = arg.CommandBufferSize;
   ctx->base.pAllocationList = arg.pAllocationList;
   ctx->base.AllocationListSize = arg.AllocationListSize;
   ctx->base.pPatchLocationList = arg.pPatchLocationList;
   ctx->base.PatchLocationListSize = arg.PatchLocationListSize;
   ctx->base.render = d3dkmt_render;
   ctx->base.destroy = d3dkmt_context_destroy;

   *out = &ctx->base;
   return status;
}

/* The drawable is the HDC of the window to show the allocation in. */
static NTSTATUS
d3dkmt_present(struct gdikmt_context *ctx, D3DKMT_HANDLE hAllocation,
               void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   HWND hWnd = WindowFromDC((HDC)winsys_drawable_handle);
   D3DKMT_PRESENT arg;
   RECT rect;

   if (!hWnd || !GetClientRect(hWnd, &rect))
      return KMT_INVALID_PARAMETER;

   if (sub_box) {
      RECT part;

      part.left = sub_box->x;
      part.top = sub_box->y;
      part.right = sub_box->x + sub_box->width;
      part.bottom = sub_box->y + sub_box->height;
      if (!IntersectRect(&rect, &rect, &part))
         return 0;
   }

   memset(&arg, 0, sizeof(arg));
   arg.hContext = d3dkmt_context(ctx)->hContext;
   arg.hWindow = hWnd;
   arg.VidPnSourceId = d3dkmt_device(ctx->device)->VidPnSourceId;
   arg.hSource = hAllocation;
   arg.SrcRect = rect;
   arg.DstRect = rect;
   arg.SubRectCnt = 1;
   arg.pSrcSubRects = &rect;
   arg.FlipInterval = D3DDDI_FLIPINTERVAL_IMMEDIATE;
   arg.Flags.Blt = 1;
   return kmt[KMT_PRESENT](&arg);
}

static NTSTATUS
d3dkmt_set_display_mode(struct gdikmt_device *dev, D3DKMT_HANDLE hPrimaryAllocation)
{
   D3DKMT_SETDISPLAYMODE arg;

   memset(&arg, 0, sizeof(arg));
   arg.hDevice = d3dkmt_device(dev)->hDevice;
   arg.hPrimaryAllocation = hPrimaryAllocation;
   arg.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
   arg.DisplayOrientation = D3DDDI_ROTATION_IDENTITY;
   return kmt[KMT_SET_DISPLAY_MODE](&arg);
}

static void
d3dkmt_destroy(struct gdikmt_device *dev)
{
   D3DKMT_DESTROYDEVICE device;
   D3DKMT_CLOSEADAPTER adapter;

   device.hDevice = d3dkmt_device(dev)->hDevice;
   kmt[KMT_DESTROY_DEVICE](&device);
   adapter.hAdapter = d3dkmt_device(dev)->hAdapter;
   kmt[KMT_CLOSE_ADAPTER](&adapter);
   free(dev);
}

struct gdikmt_device *
gdikmt_create_from_hdc(HDC hDC)
{
   D3DKMT_OPENADAPTERFROMHDC adapter;
   D3DKMT_CLOSEADAPTER close;
   D3DKMT_CREATEDEVICE device;
   struct d3dkmt_device *dev;

   call_once(&kmt_once, kmt_lookup);
   if (!kmt_found)
      return NULL;

   dev = calloc(1, sizeof(*dev));
   if (!dev)
      return NULL;

   memset(&adapter, 0, sizeof(adapter));
   adapter.hDc = hDC;
   if (!KMT_SUCCESS(kmt[KMT_OPEN_ADAPTER_FROM_HDC](&adapter))) {
      free(dev);
      return NULL;
   }

   memset(&device, 0, sizeof(device));
   device.hAdapter = adapter.hAdapter;
   if (!KMT_SUCCESS(kmt[KMT_CREATE_DEVICE](&device))) {
      close.hAdapter = adapter.hAdapter;
      kmt[KMT_CLOSE_ADAPTER](&close);
      free(dev);
      return NULL;
   }

   dev->hAdapter = adapter.hAdapter;
   dev->hDevice = device.hDevice;
   dev->VidPnSourceId = adapter.VidPnSourceId;

   dev->base.queryAdapterInfo = d3dkmt_query_adapter_info;
   dev->base.escape = d3dkmt_escape;
   dev->base.createAllocation = d3dkmt_create_allocation;
   dev->base.destroyAllocation = d3dkmt_destroy_allocation;
   dev->base.queryAllocation = d3dkmt_query_allocation;
   dev->base.openAllocation = d3dkmt_open_allocation;
   dev->base.lockAllocation = d3dkmt_lock_allocation;
   dev->base.createContext = d3dkmt_create_context;
   dev->base.present = d3dkmt_present;
   dev->base.setDisplayMode = d3dkmt_set_display_mode;
   dev->base.destroy = d3dkmt_destroy;
   return &dev->base;
}
