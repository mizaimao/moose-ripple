/*
 * Copyright 2026 the moose-ripple authors
 * SPDX-License-Identifier: MIT
 *
 * The kernel bridge (gdikmt/gdikmt.h) for a Direct3D 10 device: everything goes through the
 * callbacks the Direct3D runtime hands the driver, with the runtime's handles.
 */

#ifndef GDIKMT_D3DDDI_H
#define GDIKMT_D3DDDI_H

#include "DriverIncludes.h"

#include "gdikmt/gdikmt.h"

/*
 * Part of the Direct3D device's state. The device fills in the runtime's handles and callbacks
 * and calls gdikmt_d3dddi_fill_basefuncs() once; the fields below those are set around the
 * calls into the pipe driver that end up here.
 */
struct gdikmt_device_d3dddi {
   struct gdikmt_device base;

   HANDLE hRTAdapter;
   HANDLE hRTDevice;
   D3DDDI_DEVICECALLBACKS KTCallbacks;
   D3DDDI_ADAPTERCALLBACKS *pAdapterCallbacks;
   const DXGI_DDI_BASE_CALLBACKS *pDXGIBaseCallbacks;

   /*
    * While a resource is being created for the runtime: the runtime's handle of it when the
    * allocations have to belong to that resource (NULL: allocations of the driver's own, which
    * it may keep and reuse after the runtime's resource is gone), and whether they are a
    * primary, for which display source.
    */
   HANDLE hRTResource;
   bool isPrimary;
   UINT allocationVidPn;

   /* While a shared resource is being opened: what the runtime said about it. */
   const D3D10DDIARG_OPENRESOURCE *pOpenResource;
};

void
gdikmt_d3dddi_fill_basefuncs(struct gdikmt_device_d3dddi *device);

#endif /* GDIKMT_D3DDDI_H */
