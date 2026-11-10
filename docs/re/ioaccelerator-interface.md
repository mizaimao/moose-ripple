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

## Trace 1: WindowServer while a TextEdit window opens and closes (2026-11-10)

Reference VM 211 (9600 GT, GeForce 6.3.6). `tools/re/iokit-calls.d` on WindowServer (pid 69)
for 15 s; raw output in [traces/2026-11-10-windowserver-textedit.txt](traces/2026-11-10-windowserver-textedit.txt).
User clients WindowServer holds (`ioreg`, `IOUserClientCreator` = "pid 69, WindowServer"):
5 `nvTeslaSurface`, 2 `nvTeslaGLContext`, 2 `NV2DContext`, 1 `nvDevice`,
1 `IOAccelerationUserClient`, 2 `IOFramebufferUserClient`.

| Connection | Selector | Called from | Count | Reading |
|---|---|---|---|---|
| 0xb207 | 9 | `IOKit`IOAccelSetSurfaceFramebufferShape` | 188 | surface `kIOAccelSurfaceSetShape` (SDK enum index 9) |
| 0xb123 | 17 | `IOKit`IOAccelSetSurfaceFramebufferShapeWithBackingAndLength` | 39 | surface `kIOAccelSurfaceSetShapeBackingAndLength` (index 17) |
| 0x5733 | 11 | `GeForceGLDriver`gldAttachDrawable` | 164 | GL context client |
| 0x5733 | 16 | `GeForceGLDriver`gldUpdateDispatch` | 165 | GL context client |
| 0x5733 | 25, 30 | `GeForceGLDriver`gldFinish` | 4 each | GL context client |
| 0x562f | 5 | `GeForceGLDriver`gldModifyTexture` | 21 | probably the device/shared client (UNVERIFIED) |
| 0x562f | 6 | `GeForceGLDriver`gldReclaimTexture` | 10 | same |
| 0x371f | 18 | `GeForceGA`_NVGetSurfaceInfo(...)` | 39 | 2D context client (GA plugin) |
| 0x3403 | 1, 12 | `IOKit`_IOFBGetPixelInformation`, `IOFBSetCursorVisible` | 265, 2 | framebuffer client |

No `IOServiceOpen` during the window's life, and no surface for the TextEdit window itself.

Reading (to confirm with more traces): with Quartz Extreme, WindowServer composites with
plain OpenGL through the GL driver plugin (`gldAttachDrawable` to the display surface every
frame, window contents as textures via `gldModifyTexture`), and uses the standard surface
client only for the display ("framebuffer") surfaces. The two surface selectors seen match the
SDK header's numbering exactly. Implication: Quartz Extreme needs a working GL driver (M4) plus
the surface client for framebuffer surfaces and whatever the GA plugin's 2D client is used for.
