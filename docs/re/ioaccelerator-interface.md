# IOAccelerator interface (10.6.8, 10K549)

Status: M1 in progress, first pass (2026-11-10). Binary hashes in [binaries.md](binaries.md).

## Public part: in the 10.6 SDK

`MacOSX10.6.sdk` (Xcode 3.2.6) ships these headers, in both `IOKit.framework` (userspace) and
`Kernel.framework` (kernel), under `IOKit/graphics/`:
- `IOAccelerator.h` (42 lines): `class IOAccelerator : public IOService` with only
  `createAccelID`, `retainAccelID`, `releaseAccelID`.
- `IOAccelClientConnect.h`: `kIOAcceleratorClassName` = `"IOAccelerator"`; one standard
  client type, `kIOAccelSurfaceClientType` (= 0).
- `IOAccelSurfaceConnect.h` (182 lines): the surface user client's 18 method selectors, in
  order from 0: `kIOAccelSurfaceReadLockOptions`, `ReadUnlockOptions`, `GetState`,
  `WriteLockOptions`, `WriteUnlockOptions`, `Read`, `SetShapeBacking`, `SetIDMode`, `SetScale`,
  `SetShape`, `Flush`, `QueryLock`, `ReadLock`, `ReadUnlock`, `WriteLock`, `WriteUnlock`,
  `Control`, `SetShapeBackingAndLength`; plus the mode, shape, scale and lock option bits and
  `IOAccelDeviceRegion`.
- `IOAccelTypes.h`: shared types (`IOAccelID`, bounds, sizes).

These are Apple's own headers for the 10.6 SDK, so the surface protocol is documented, not
reverse-engineered. Still to confirm with traces: argument shapes per selector as used.

## Who uses it

- `IOKit.framework` exports the client side: `IOAccelFindAccelerator`, `IOAccelCreateSurface`,
  `IOAccelDestroySurface`, `IOAccelSetSurfaceFramebufferShape*`, `IOAccelSetSurfaceScale`,
  `IOAccel{Read,Write}{Lock,Unlock}Surface*`, `IOAccelQueryLockSurface`, `IOAccelWaitForSurface`,
  `IOAccelFlushSurfaceOnFramebuffers`, `IOAccelReadSurface`, `IOAccelSurfaceControl`,
  `IOAccelCreateAccelID`, `IOAccelDestroyAccelID`, `IOAccelReadFramebuffer`.
- `CoreGraphics` (the library WindowServer runs on) imports 21 of them. `WindowServer` itself
  imports none directly. So window surfaces are created by CoreGraphics through this
  standard interface, whichever vendor's accelerator is present.

## Vendor part

- `IOGraphicsFamily` 2.2 exports only two accelerator base classes: `IOAccelerator` and
  `IOAccelerationUserClient`.
- `GeForce.kext` 6.3.6 subclasses `IOAccelerator` (`IONVAccelerator`, service `NVKernel`) and
  defines its own user clients: `IONVSurface`, `IONVGLContext`, `IONV2DContext`, `IONVDevice`,
  `IONVDVDContext`, `IONVOCDContext`, `IONVShared`, `IONVGraphicsClient`.
- Live counters on the reference VM (`PerformanceStatistics` of `NVKernel`): `contextGLCount`
  = 2, `context2DCount` = 2, `surfaceCount` = 5, and `SurfaceList` shows a `gl` surface owned
  by pid 69 (WindowServer). So WindowServer holds GL and 2D contexts on the accelerator.
- The 2D path is a CFPlugIn named by the accelerator's `IOCFPlugInTypes`
  (`ACCF0000-0000-0000-0000-000a2789904e` = `GeForceGA.plugin`).

## What M5 will need (working view, to be confirmed)

1. The surface user client (type 0) per `IOAccelSurfaceConnect.h`.
2. Whatever GL and 2D context clients WindowServer opens (types 1, 2, ... are vendor
   conventions; VMsvga2 mirrors the NVIDIA/Intel set). To find out: dtrace
   `IOServiceOpen` types and `IOConnectCallMethod` selectors from WindowServer on the
   reference VM.
3. Possibly a GA plugin, if WindowServer requires one for Quartz Extreme.
