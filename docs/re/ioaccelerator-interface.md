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
| 0xb207 | 9 | `IOKit: IOAccelSetSurfaceFramebufferShape` | 188 | surface `kIOAccelSurfaceSetShape` (SDK enum index 9) |
| 0xb123 | 17 | `IOKit: IOAccelSetSurfaceFramebufferShapeWithBackingAndLength` | 39 | surface `kIOAccelSurfaceSetShapeBackingAndLength` (index 17) |
| 0x5733 | 11 | `GeForceGLDriver: gldAttachDrawable` | 164 | GL context client |
| 0x5733 | 16 | `GeForceGLDriver: gldUpdateDispatch` | 165 | GL context client |
| 0x5733 | 25, 30 | `GeForceGLDriver: gldFinish` | 4 each | GL context client |
| 0x562f | 5 | `GeForceGLDriver: gldModifyTexture` | 21 | probably the device/shared client (UNVERIFIED) |
| 0x562f | 6 | `GeForceGLDriver: gldReclaimTexture` | 10 | same |
| 0x371f | 18 | `GeForceGA: _NVGetSurfaceInfo(...)` | 39 | 2D context client (GA plugin) |
| 0x3403 | 1, 12 | `IOKit: _IOFBGetPixelInformation`, `IOKit: IOFBSetCursorVisible` | 265, 2 | framebuffer client |

No `IOServiceOpen` during the window's life, and no surface for the TextEdit window itself.

Reading (to confirm with more traces): with Quartz Extreme, WindowServer composites with
plain OpenGL through the GL driver plugin (`gldAttachDrawable` to the display surface every
frame, window contents as textures via `gldModifyTexture`), and uses the standard surface
client only for the display ("framebuffer") surfaces. The two surface selectors seen match the
SDK header's numbering exactly. Implication: Quartz Extreme needs a working GL driver (M4) plus
the surface client for framebuffer surfaces and whatever the GA plugin's 2D client is used for.

## Trace 3: an app's GL window on Apple's driver (2026-11-12)

Reference VM 211 (9600 GT), Chess and gl-gears, dtrace on the app and on WindowServer. This is
how an accelerated window is shown without going through WindowServer for every frame, and
what our driver would have to do to keep app windows on the GPU (today it shows them like
Apple's software renderer, through a private window and two copies in memory).

- **Binding.** The app's driver, in `gldAttachDrawable(ctx, 0x50, {cid, wid, sid})`, calls
  `CGSBindSurface(cid, wid, sid, 2, 0x24, DISPLAY)` through a callback of OpenGL.framework
  (the software renderer passes 1, 0 and its private window instead). 0x24 is the accelerator
  surface mode; the last argument looked like a display ID (0x18dba7c1; the main display was
  0x18db8fc1).
- **WindowServer then makes the kernel surface.** `_XBindSurface` → `CGXBindSurface` →
  `IOAccelCreateSurface(accelerator, id = the CGS surface ID, mode 0x24)`, then
  `_CGXSynchronizeSurfaceVisibility` → `IOAccelSetSurfaceFramebufferShape(conn, region,
  options 0, framebuffer 0)`: the surface's visible region on screen. The app never calls
  `IOAccelCreateSurface` itself (only `IOAccelFindAccelerator`).
- **The app's GL context attaches to that surface by ID**: one call on its GL context client
  (selector 11, a 40-byte structure) from `gldAttachDrawable`.
- **Per frame WindowServer does nothing for the window.** With gl-gears animating at 59
  frames/s, WindowServer made no GL, CGL, accelerator or kernel call for it; the only per-frame
  call was `CGXGLInvalidateTextureRegion`, from the Dock's mirror update. So the kernel driver
  puts each swapped frame on screen itself, inside the surface's visible region.
- **When WindowServer has to draw the window itself** (redraws, the Dock's mirror, and
  presumably Exposé), it takes the surface as a texture:
  `_CGXRedrawWindow` → `CGXWindowBackingGet` → `CGXBackingStoreTextureGet` →
  `CGXGLSetTextureSurface(ctx, surface ID, ...)`.

What that means for a driver of ours: the kext shows a swapped frame itself (a copy on the
host GPU from the app's buffer into the screen texture, clipped to the surface's region), the
app's GL bundle binds with kind 2 and attaches its drawable to the kernel surface, and
WindowServer's GL bundle answers the surface-texture request with the app's buffer. No read
back, no private window, no upload.

## Trace 4: the same binding on our kext (2026-11-12)

`tools/surface-probe` makes a window, adds a surface and calls
`CGSBindSurface(cid, wid, sid, 2, 0x24, CGMainDisplayID())`, then asks the kext about that
surface ID (`RIPPLE_GPU_SURFACE_INFO`). On VM 210, with the kext logging surface calls
(`ripple_qe=2`):

- Before the bind the kext has no such surface. The bind returns 0, and WindowServer has
  opened a surface client on our accelerator: `SetIDMode(id = the CGS surface ID, mode 0x24)`,
  then `SetShape(options 0, framebuffer 0, bounds 300,178 400x300, 1 rectangle)`: the
  surface's place on the screen (the window's frame, title bar included, for bounds 0,0 in the
  window). So the main display's ID works as the last argument, and the kext's existing
  surface client is enough for this part.
- When the window is moved: `ReadLockOptions(2)` and `ReadUnlock` (WindowServer reads the
  surface's pixels to draw the moving window itself), then
  `SetShape(options 2 (non-simple), bounds 0,0 400x300)`: no longer a plain region of the
  screen. The probe never draws or flushes, so what brings the surface back to a screen
  region is not seen yet.
- `CGSRemoveSurface` makes WindowServer close the surface client.

Next: let an app attach a GPU buffer to such a surface and have the kext copy it into the
screen texture inside the surface's region when the app swaps.

## Step 2: the kext shows a buffer on such a surface (2026-11-12)

New GPU client call `SURFACE_PRESENT` (docs/kext-uapi.md): a copy on the host GPU from the
caller's buffer into the buffer on the display, one per visible rectangle of the surface, then
a flush. `tools/surface-probe -p` binds a window's surface, uploads a test picture into a GPU
buffer and presents it ten times a second: the picture appears in the window's content area,
upright and in place, a moving bar in it updates, and nothing of it is left when the program
ends. WindowServer makes no call for any of these frames.

Found on the way:
- The host's copy coordinates in the buffer on the display count rows from the top: a copy to
  row 300 lands 300 rows from the top of the screen.
- The host drops a transfer for a buffer its context has not seen, without an error. The
  kext's `TRANSFER` now attaches such a buffer first (`surface-probe -t` checks upload, read
  back and both kinds of copy in a new context).
- A boot-time dialog over the window made WindowServer shape the surface non-simple; the kext
  then answers 0 (not shown), as intended.

Not done yet: what WindowServer paints over the region when it redraws that part of the
screen, partly covered windows, a moving window, and the GL bundle using any of this.

## Step 3: the GL bundle shows a window this way, on request (2026-11-12)

With `RIPPLE_GL_DIRECT` in the environment (or the file `/Library/Logs/RippleGL/direct`), a
window's surface is bound as accelerated instead of getting a private window, and each frame
is handed to the kext with `SURFACE_PRESENT` (`gld_window.c`, `cgl_drawable.c`). Without the
switch nothing changes.

gl-gears on VM 210, frames/s and the bundle's time per present:

| Window | read back (as before) | on the GPU (the switch) |
|---|---|---|
| 400x400 | 59, 8.8 ms | 57-59, 3.9 ms |
| 800x600 | 53, 16.3 ms | 57, 4.6 ms |
| 1280x720 | 40, 22.4 ms | 57, 5.5 ms |

gl-gears redraws from a timer through AppKit, which holds it to about 60 frames/s, so the time
per present is the better measure: a quarter of what it was for a large window, and no longer
growing much with the size. The picture is upright and in place (the window's texture has its
top row first), the Dock is drawn over a window that reaches it, and with a swap interval the
bundle itself keeps presents 1/60 s apart, since nothing else holds the program back here.

Still missing before this can be the default: a window that is covered, moving, minimised or
shown by Exposé (WindowServer shapes the surface non-simple then and wants its pixels: read
lock, or the surface as a texture), what WindowServer paints into the region when it redraws
that part of the screen while the program is not drawing, and surfaces that are not opaque.
