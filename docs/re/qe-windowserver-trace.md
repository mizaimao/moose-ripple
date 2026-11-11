# WindowServer under Quartz Extreme: traces (M5 R2)

Date: 2026-11-10. Question 2 of [m5-plan.md](m5-plan.md): what WindowServer opens and
calls when Quartz Extreme is on.

## Setup

- VM 212 with the GeForce 9600 GT passed through, booted by OpenCore (OpenDuet on SeaBIOS,
  [decision 0007](../decisions/0007-opencore.md)); 10.6.8 10K549, GeForce 6.3.6, 1920x1080.
  QE on: `CGDisplayUsesOpenGLAcceleration(main)` = 1. VM 210 off.
- `tools/re/qe-kernel-iokit.d`: kernel fbt probes on `is_io_service_open_extended` and
  `is_io_connect_method` (xnu-1504), filtered by process name, so it also sees WindowServer
  starting at login (pid probes can't attach that early). Each user client's class comes from
  its vtable address, resolved afterwards with the kexts' symbols (GeForce, NVDAResman,
  IOGraphicsFamily, IOHIDFamily, KDK kernel; load addresses from `kextstat`).
- `tools/re/qe-user-calls.d`: pid probes on the running WindowServer: the GL driver bundle's
  `gld*`, the GA plugin (`GeForceGA`), IOKit's `IOAccel*`, OpenGL's `CGL*`; first call of each
  with arguments and caller, plus counts.
- Login: `killall -KILL WindowServer` in the guest; loginwindow restarts it and auto-logs in.
  Exposé: F9 twice, Dashboard: F12 twice, sent with `qm sendkey`.
- Raw traces (classes substituted for vtable addresses): `traces/2026-11-10-qe-ws-*.txt`.

## What WindowServer opens at login

| Service | Type | User client | Count |
|---|---|---|---|
| `NVKernel` (GeForce's `IOAccelerator`) | 0 | `nvTeslaSurface` (surface client, SDK `kIOAccelSurfaceClientType`) | 23 |
| `NVKernel` | 1 | `nvTeslaGLContext` (GL context) | 2 |
| `NVKernel` | 2 | `NV2DContext` (2D context, used by the GA plugin) | 2 |
| `NVKernel` | 4 | `nvDevice` | 2 |
| `NVDA` (framebuffer, one per head) | 0 | `IOFramebufferUserClient` | 2 |
| `IODisplayWrangler` | 0 | `IOAccelerationUserClient` (IOGraphicsFamily's, accel IDs) | 1 |
| `IOHIDSystem` | 0, 1, 3 | `IOHIDUserClient` (input, not graphics) | 5 |
| `IOPMrootDomain` | 0 | power | 1 |

So the accelerator's client types are 0 surface, 1 GL, 2 2D, 4 device. Types 1, 2, 4 are
NVIDIA's numbering (the SDK only fixes type 0). Surface IDs come from IOGraphicsFamily itself
(`IOAccelCreateAccelID` via `IODisplayWrangler`), not from the vendor.

## Method calls per client (login, about 85 s)

| Client | Selector | Inputs (scalars / struct bytes) | Calls | Reading |
|---|---|---|---|---|
| `nvTeslaSurface` | 7 | 2 / 0 | 23 | SDK `kIOAccelSurfaceSetIDMode` |
| `nvTeslaSurface` | 9 | 2 / 20-196 | 52 | SDK `kIOAccelSurfaceSetShape` (struct = `IOAccelDeviceRegion`, grows with rects) |
| `nvTeslaSurface` | 16 | 2 / 0 | 2 | SDK `kIOAccelSurfaceControl` |
| `nvTeslaSurface` | 17 | 5 / 20 | 21 | SDK `kIOAccelSurfaceSetShapeBackingAndLength` |
| `nvTeslaGLContext` | 0, 4, 11, 16, 25, 29 | see trace | 35, 27, 88, 60, 5, 2 | NVIDIA GL client (11 and 16 every frame, from `gldAttachDrawable` and `gldUpdateDispatch`, M1) |
| `nvDevice` | 0-3, 5, 6, 8, 10, 11, 13 | see trace | 5 = 221 calls (88-byte struct) | NVIDIA device client (5 and 6 from `gldModifyTexture`/`gldReclaimTexture`, M1) |
| `NV2DContext` | 0, 1, 7, 16, 17, 18 | 18 = 192-byte struct | 8, 2, 10, 2, 40, 19 | GA plugin (18 = `_NVGetSurfaceInfo`, M1) |
| `IOFramebufferUserClient` | 0, 1, 2, 4, 8-12, 15, 18, 19 | 1 = 3 scalars | 1: 779 calls | IOGraphicsFamily's own client (1 = `IOFBGetPixelInformation`, 12 = `IOFBSetCursorVisible`, M1; the others UNVERIFIED) |
| `IOAccelerationUserClient` | 0, 1 | 2 / 0 | 26, 18 | create / destroy accel ID (matches `IOAccelCreateAccelID`/`DestroyAccelID` counts) |

Memory mapped into WindowServer: `IOFramebufferUserClient` types 100 and 110,
`nvTeslaGLContext` types 3 and 4, `NV2DContext` 0 and 1, `nvDevice` 1, `IOHIDUserClient` 0.

## Exposé and Dashboard (WindowServer)

Kernel side, Exposé open and close (14 s): `nvTeslaSurface` 9 (SetShape) 746 calls,
`nvTeslaGLContext` 16 (426) and 11 (408), `nvDevice` 5 (52), plus single calls of
`IOAccelerationUserClient` 0/1. Dashboard (16 s): the same pattern, about half as many.
No surface opens or closes during either.

User side (counts for one open and close):

| Call | Exposé | Dashboard | Note |
|---|---|---|---|
| `gldUpdateDispatch` | 14755 | 10323 | every draw |
| `gldUnbindPipelineProgram` | 11332 | 13375 | |
| `gldGetError` | 4720 | 7933 | |
| `gldModifyTexture` | 785 | 481 | window contents uploaded as textures |
| `IOAccelSetSurfaceFramebufferShape` | 662 | 320 | display surface reshaped each frame |
| `gldAttachDrawable` | 454 | 193 | GL context attached to the display surface each frame |
| `gldModifyPipelineProgram` | 370 | 166 | shader uniforms |
| `gldWaitObject` / `gldTestObject` | 355 / 309 | 171 / 118 | fences on textures |
| `gldCreateTexture` / `gldDestroyTexture` | 52 / 52 | 59 / 54 | texture churn per animation |
| `gldCreateFramebuffer` | 2 | 0 | Exposé uses FBOs |
| `gldCreatePipelineProgram` | 0 | 1 | |
| GA plugin, `CGL*` | none | none | 2D plugin not used while compositing |

## Dock (Dashboard's host process)

Opening and closing Dashboard made no GL driver calls in the Dock: the GL driver isn't even
loaded there (QuartzCore is). The zoom-in is WindowServer's work. The ripple appears only when a
widget is added and is drawn by the Dock with Core Image; that wasn't triggered here (it needs a
mouse sequence; R3).

## What this means for M5 (K1/G1/G2 in m5-plan.md)

1. Surface client (type 0): WindowServer uses SDK selectors 7, 9, 16, 17 on it, 23
   surfaces at login, `SetShape` once per composited frame on the display surface.
2. The GL path does the compositing: `gldAttachDrawable` + `gldUpdateDispatch` every frame,
   window backings as textures (`gldModifyTexture`), FBOs for Exposé, shaders throughout. The GL
   context and device clients are private to the GL bundle, so ours can define its own API.
3. The 2D context and GA plugin are used at login (`NV2DContext`, 81 calls), not during the
   animations. Whether QE needs a GA plugin at all is for R1 (static).
4. `IOAccelerationUserClient` (accel IDs) and `IOFramebufferUserClient` are IOGraphicsFamily's
   and already work with RippleFramebuffer.
