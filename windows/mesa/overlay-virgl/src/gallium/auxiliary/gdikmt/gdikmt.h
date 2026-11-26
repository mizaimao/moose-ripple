/*
 * Copyright 2026 the moose-ripple authors
 * SPDX-License-Identifier: MIT
 *
 * The bridge between Mesa's Windows user-mode drivers and the kernel half of a WDDM display
 * driver.
 *
 * A user-mode driver reaches the kernel driver in one of two ways. A Direct3D driver is handed
 * callbacks by the Direct3D runtime and has to use those. Everything else (OpenGL here) calls
 * the D3DKMT functions of gdi32.dll itself. A winsys should not care which, so both are put
 * behind the same table of operations: gdikmt_d3dkmt.c fills it with the gdi32 calls, the
 * Direct3D frontend (gdikmt_d3dddi.cpp) with the runtime's callbacks.
 *
 * Written for moose-ripple from the way Mesa's virgl winsys for Windows and the Direct3D 10
 * frontend call it. The names of the operations and fields are theirs.
 */

#ifndef GDIKMT_H
#define GDIKMT_H

#include <stdbool.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_box;
struct gdikmt_device;
struct gdikmt_context;

/* Allocations for one resource. */
struct gdikmt_createallocation {
   UINT NumAllocations;                    /* in */
   D3DDDI_ALLOCATIONINFO *pAllocationInfo; /* in: private data; out: hAllocation */
   const void *pPrivateDriverData;         /* in: for the resource as a whole */
   UINT PrivateDriverDataSize;
   HANDLE hResource;                       /* out: what destroyAllocation wants back */
};

/*
 * A shared resource, opened from its global handle. queryAllocation fills in the three sizes,
 * the caller provides memory of those sizes, openAllocation fills the memory.
 */
struct gdikmt_openallocation {
   D3DKMT_HANDLE hGlobalHandle;                /* in */
   UINT NumAllocations;                        /* out of the query */
   D3DDDI_OPENALLOCATIONINFO *pOpenAllocation; /* NumAllocations entries */
   void *pPrivateDriverData;                   /* the resource's private data */
   UINT PrivateDriverDataSize;                 /* out of the query */
   void *pTotalBuffer;                         /* holds the private data of all allocations */
   UINT TotalBufferSize;                       /* out of the query */
   UINT RuntimeDataSize;                       /* out of the query: what the runtime that made
                                                * the resource stored with it */
   HANDLE hResource;                           /* out of the open */
};

/* One submission of the context's command buffer. */
struct gdikmt_render {
   UINT CommandOffset;
   UINT CommandLength;
   UINT AllocationCount;
   UINT PatchLocationCount;

   /* Sizes wanted for the buffers the kernel hands back, where the flag is set. */
   bool ResizeCommandBuffer;
   bool ResizeAllocationList;
   bool ResizePatchLocationList;
   UINT NewCommandBufferSize;
   UINT NewAllocationListSize;
   UINT NewPatchLocationListSize;

   /*
    * An event to set when the commands have run. Neither way to the kernel can do that on
    * every Windows version this is built for (it takes WDDM 1.2), so the bridge sets the event
    * as soon as the commands are submitted. A driver that has to know when they have really
    * run puts a command of its own into the buffer instead and leaves this NULL.
    */
   HANDLE CompletionEvent;
};

/*
 * A rendering context of the kernel driver, with the buffers the next submission is written
 * into. After render() these point at the buffers the kernel handed back.
 */
struct gdikmt_context {
   struct gdikmt_device *device;

   void *pCommandBuffer;
   UINT CommandBufferSize;
   D3DDDI_ALLOCATIONLIST *pAllocationList;
   UINT AllocationListSize;
   D3DDDI_PATCHLOCATIONLIST *pPatchLocationList;
   UINT PatchLocationListSize;

   NTSTATUS (*render)(struct gdikmt_context *ctx, struct gdikmt_render *render);
   void (*destroy)(struct gdikmt_context *ctx);
};

/* One adapter of the kernel driver with a device on it. */
struct gdikmt_device {
   NTSTATUS (*queryAdapterInfo)(struct gdikmt_device *dev, KMTQUERYADAPTERINFOTYPE type,
                                void *data, UINT size);
   /* A private packet for the kernel driver, answered in place. */
   NTSTATUS (*escape)(struct gdikmt_device *dev, void *data, UINT size);

   NTSTATUS (*createAllocation)(struct gdikmt_device *dev,
                                struct gdikmt_createallocation *create);
   NTSTATUS (*destroyAllocation)(struct gdikmt_device *dev, HANDLE hResource,
                                 D3DKMT_HANDLE hAllocation);
   NTSTATUS (*queryAllocation)(struct gdikmt_device *dev, struct gdikmt_openallocation *open);
   NTSTATUS (*openAllocation)(struct gdikmt_device *dev, struct gdikmt_openallocation *open);

   /* Map allocation memory for CPU access. */
   NTSTATUS (*lockAllocation)(struct gdikmt_device *dev, D3DKMT_HANDLE hAllocation,
                              D3DDDICB_LOCKFLAGS flags, void **ptr);

#ifdef RIPPLE_VISTA
   NTSTATUS (*unlockAllocation)(struct gdikmt_device *dev, D3DKMT_HANDLE hAllocation);
#endif

   NTSTATUS (*createContext)(struct gdikmt_device *dev, struct gdikmt_context **ctx);

   /*
    * Show an allocation. What the drawable is depends on who made the device: a window's HDC
    * for one made from an HDC, DXGI's present context for the Direct3D runtime's.
    */
   NTSTATUS (*present)(struct gdikmt_context *ctx, D3DKMT_HANDLE hAllocation,
                       void *winsys_drawable_handle, struct pipe_box *sub_box);
   /* Make an allocation the one the display shows (full-screen swap chains). */
   NTSTATUS (*setDisplayMode)(struct gdikmt_device *dev, D3DKMT_HANDLE hPrimaryAllocation);

   void (*destroy)(struct gdikmt_device *dev);

   /* Set while the runtime destroys the device this one serves (the Direct3D 9 code sets it) */
   bool runtime_destroying;
};

/*
 * A device on the adapter that shows the given window or screen DC, through gdi32's D3DKMT
 * functions. NULL if the display driver is not a WDDM driver or refuses.
 */
struct gdikmt_device *
gdikmt_create_from_hdc(HDC hDC);

#ifdef __cplusplus
}
#endif

#endif /* GDIKMT_H */
