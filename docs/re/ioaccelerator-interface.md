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

## Step 4: WindowServer draws the window itself (a drag, Exposé) (2026-11-12)

While a window moves, WindowServer goes through this for every step (kext log and dtrace,
gl-gears shown the new way): `SetShape(options 0, the new place)`, `Flush(framebuffer mask 1)`,
`SetShape(options 2, non-simple)`, then for each of its own redraws of the window
`ReadLockOptions(2)` / `ReadUnlock` from `_CGXRedrawWindow` → `CGXNextSurface`, with
`CGXGLCreateSurfaceTextureReference` on what the lock returned. After the drag the surface
stays non-simple and WindowServer keeps drawing the window from read locks; Exposé shapes it
plain again while it shows, and non-simple after.

What the kext and the bundle do for it now:
- `SURFACE_PRESENT` records the client's buffer as the surface's frame even when it answers
  0. The bundle then flushes the surface to WindowServer (`CGSFlushSurfaceWithOptions`), which
  redraws the window, and flips its two buffers so the recorded one is not drawn into.
- A read lock hands out that frame. The client's buffer has no memory in the guest (Mesa
  gives textures a one-byte backing when the host can copy between buffers), so the kext
  keeps a GPU buffer of its own over the lock memory, copies the frame into it on the GPU
  (one BLIT in the client's context) and reads it back, which fills the memory WindowServer
  has mapped. Top row first.
- **The pixel format a read lock returns has to be a depth code.**
  `CGXGLCreateSurfaceTextureReference` (CoreGraphics 10.6.8 at 0x2bce9f) switches on the
  structure's `pixelFormat`: 3, 4 (32 bit), 5, 6, `'2vuy'`, `'yuvs'`; anything else and it
  returns nothing, and the window is drawn blank. The kext answered `'BGRA'`. It now answers
  4 for a read lock of a surface that has a client frame (the others are left as they
  were). The second address stays 0, which makes the texture opaque (`GL_RGB8`).

Result: gl-gears stays visible and animated while its window is dragged, after the drag and
in Exposé, at 57-58 frames/s (7-11 ms per present on this path against 4-5 ms when the kernel
shows the frame itself).

Open: what brings a surface back to a plain region after a move on Apple's driver (ours
stays on the slower path until Exposé or a new attach), the `Flush` call (the kext still
answers it from the lock memory, the 2D way), minimising, covered windows, a program that
draws once, surfaces that are not opaque.

## Trace 5: a GL window that moves or is covered, on Apple's driver (2026-11-12)

Reference VM 211 (9600 GT), gl-gears moving its own window (`-m` up and to the right, `-M`
down until it lies partly under the Dock) and with a second window crossing it (`-c`), dtrace
on WindowServer. The question was when a window leaves the path where the kernel shows its
frames, and how it gets back.

- **Each step of a move**: `SetShape(options 2, non-simple)` at the old place, and about 2 ms
  later `SetShape(0)` at the new place. The program's driver does not flush to WindowServer
  during those moments (no `CGXFlushSurface` in a move of 60 steps). After the last step the
  surface is plain and WindowServer again does nothing per frame.
- **Under something that is drawn over it** (the Dock with `-M`, another window with `-c`) the
  surface stays non-simple for as long as that lasts. The program's driver then flushes the
  surface to WindowServer at every swap (`CGXFlushSurface` 59 times a second), and WindowServer
  draws the window itself each time (`CGXRedrawSpecifiedWindowsToDisplayAndRegion`). When the
  other window has left, the surface is shaped plain within the second and the flushes stop.
  Under the Dock it never ends. (The first version of this trace only had the move under the
  Dock and concluded that a moved window never returns to the kernel-shown path. Wrong.)
- **WindowServer draws with a texture it made of the surface once**, when the window appeared:
  one `IOAccelReadLockSurfaceWithOptions(2)`, which returned an address, 1600 row bytes,
  400x400 and pixel format 4, then `CGXGLCreateSurfaceTextureReference`, which calls
  `cglsSetInteger(997, {surface ID, GL_TEXTURE_RECTANGLE, GL_RGB8, 400, 400,
  GL_UNSIGNED_INT_8_8_8_8_REV, 0x400, 0})` on its GL context. That is the only read lock and
  the only 997 in the window's life. `CGXNextSurface` keeps the texture with the surface and
  takes the read lock only when it has none (`CGXGLGetSurfaceTextureInformation` answers from
  the texture). The texture's data pointer is a dummy (`0xdeadbeef`): WindowServer never
  uploads anything into it. No `IOAccelFlushSurfaceOnFramebuffers` in any of these runs.
- The program keeps 58.7-58.9 frames/s through all of it.

Who decides: WindowServer, in `_CGXActivateSurfaces` (CoreGraphics 10.6.8), run when a window
or surface is bound, ordered, shaped or moved and at the end of every geometry change. A
surface is shaped plain only if every visible part of it is unobscured (nothing drawn over it,
shadows and the Dock included), the window is not transformed, zoomed or translucent, no
screen watcher is active and its colours need no conversion; otherwise non-simple. While a
geometry change is under way (`CGXPrepareGeometryChange` → `_CGXSynchronizeSurfaces`) it is
non-simple too. So parameter 997 ties the texture to the surface for good, and a driver has
two jobs: show the frame itself while the surface is plain, and tell WindowServer about each
frame while it is not.

## Step 5: the window's surface as WindowServer's texture, kept live (2026-11-12)

The move under the Dock on our kext (VM 210, `RIPPLE_GL_DIRECT=1`) showed the same calls from
WindowServer as trace 5: one read lock and one 997 when the window appears, shapes alternating
plain and non-simple for the steps of the move until the window reaches the Dock, then
non-simple, a `CGXFlushSurface` from the program for every frame, no kernel flush. But the
picture stopped: under the Dock the window showed the frame it had when WindowServer made its
texture.
Our GL bundle answered 997 with a copy of the surface's memory, once. Step 4's check, a drag
with the pointer, did not catch this.

What the kext and the bundle do now:
- `SURFACE_TEXTURE` (docs/kext-uapi.md): WindowServer's GL bundle, at 997 for a surface that a
  program presents on (`RIPPLE_GPU_SURFACE_FRAMES` in `SURFACE_INFO`), gives the bound texture
  storage of its own and names that buffer to the kext as the surface's texture. The kext
  copies the latest frame into it at once and every later frame at its present, on the GPU, in
  the program's context, top row first, in the same submit as the copy to the screen. The link
  ends when WindowServer deletes the texture (the buffer's destruction) or names another.
  998 and 999 for such a surface copy the latest frame once.
- The copies moved into the surface client (`RippleSurfaceClient::pushFrame`); `SURFACE_PRESENT`
  only hands it the frame.
- Flush on such a surface is answered the same way as a present: the latest frame is copied to
  the screen if the surface is a plain region of it now, instead of the old copy from the lock
  memory. WindowServer did not send one in these runs.

Checked on VM 210 with captures: the gears keep turning after a self-move and after a drag
with the pointer (both ended partly under the Dock), during them, and in Exposé; a window partly under a dialog is clipped right; resize,
two windows at once and killing the program in the middle of a move leave the desktop clean.
Numbers (gl-gears, frames/s and time per present):

| | 400x400 | 1280x686 |
|---|---|---|
| kernel shows the frame | 58.7, 4.1 ms | 58.3-58.8, 4.8-5.5 ms |
| under the Dock, WindowServer draws it | 58.4, 13.3 ms | 55-58, 12-14 ms |

While WindowServer draws the window the frame rate holds, but each present waits for its
redraw (the flush to it is a call that returns when the window is drawn): a program that needs
most of a frame's time to draw loses frames there (step 7 has numbers). Apple's driver stays
at 58.7 frames/s on that path; what its flush costs the program was not measured.

Open: a window whose first frames arrive while WindowServer still has the new surface
non-simple can start on the slower path and stay there (seen once in five starts of the big
window); minimising; a program that draws once; surfaces that are not opaque.

## Step 6: minimising, a program that draws once, surfaces that are not opaque (2026-11-12)

Checked on VM 210 with the kernel-shown path switched on (`RIPPLE_GL_DIRECT=1`), against
Apple's software renderer in the same VM (`gl-gears -s`) and our default path, with captures.
gl-gears got options for it that need no pointer: `-f N` stops drawing after N frames, `-c`
sends a second window across the GL window, `-z` minimises and restores, `-o` makes the
surface and the window see-through.

| Case | Result with the kernel-shown path |
|---|---|
| Draws 3 frames, then another window crosses it | the picture is clipped right while covered and whole again after |
| Draws 3 frames, then the window moves | the picture goes along and stays |
| Minimise and restore, animating | the genie effect carries the gears both ways, the Dock shows the thumbnail, animation resumes (58.8 frames/s) |
| Draws 3 frames, then minimise and restore | the picture comes back |
| See-through surface, standing and moving | the desktop shows through around the gears, as on the two references; 49 frames/s while moving against 40-41 on both |

No driver change was needed after step 5: in all of these WindowServer draws the window with
its texture of the surface, which now always holds the latest frame, and while the surface is
a plain region of the screen the frame the kernel copied there stays. A see-through surface
is never shaped plain; WindowServer asks for an RGBA texture for it (it sets the read lock's
second address itself when the surface is not opaque) and blends.

## Step 7: when a window leaves the kernel-shown path, and what the other path costs (2026-11-12)

Two things looked like weaknesses after step 5: a present on the path through WindowServer
took about 13 ms against 4-5 ms, and one start of a big window had stayed on that path.

- **Ours follows trace 5.** The same three runs on VM 210 (`RIPPLE_GL_DIRECT=1`, clear
  screen): moving clear of the Dock, no flush from the program during the move and a plain
  surface after it, 58.8 frames/s; under the Dock, a flush per frame for good; with a window
  crossing, flushes while it is there and none after. The only difference is one or two flushes
  when the window appears, where the reference has none.
- **A window that starts or stays on the other path has something drawn over it.** In these
  tests that was a dialog left on the screen after a restart, or the Dock: both runs under a
  dialog stayed non-simple from the first frame to the last. That is WindowServer's rule
  (trace 5), not a state the driver gets stuck in. The start of the big window that prompted
  the question (it ends a few pixels above the Dock) did not come back in 22 more starts.
- **The 13 ms are mostly a wait for WindowServer's next update, not work.** gl-gears with
  `-l MS` keeps the CPU busy for MS milliseconds in every frame, like a program that needs
  that long to draw (frames/s, single runs, they vary by a few):

  | busy per frame | default path | kernel shows the frame | WindowServer draws it (under the Dock) |
  |---|---|---|---|
  | 0 ms | 57-58 | 58.7 | 58.2-58.4 |
  | 10 ms | 44-49 | 50-59 | 48-53 |
  | 13 ms | 45-50 | 59.4 | 42-48 |

  So a program whose window has something drawn over it runs about as fast as on the default
  path today, and faster when nothing is. The real extra cost while WindowServer draws the
  window is its redraw, about 7 ms a frame, which the program's flush waits for.

No driver change in this step. Not done: taking that wait off the program's thread (a flush
from a second thread), which would only help a window that is busy and covered at once.

A trap for traces: `dtrace -p` on WindowServer with a script file that does not exist leaves
WindowServer suspended (dtrace stops the process before it reads the script), and every
program then hangs at its first window-server call. Check that the file is there first.

## Step 8: real apps with the kernel-shown path switched on (2026-11-12)

VM 210, every app once on the default path and once with the file
`/Library/Logs/RippleGL/direct` present (the switch for all programs), same steps, captures of
both, swaps counted with dtrace on `CGLFlushDrawable`, time per present from the bundle's log.
The pictures were the same in every pair.

| App | default path | kernel-shown path |
|---|---|---|
| Angry Birds Rio, title screen (32-bit, full screen) | 32-39 frames/s, WindowServer 17% of a CPU | 58 frames/s, WindowServer 1% |
| Flurry screen saver, full screen | 30 frames/s (its cap), 22-24 ms a present, WindowServer 32%, ScreenSaverEngine 11% | 30 frames/s, 4.5-5 ms, WindowServer 0%, ScreenSaverEngine 3% |
| Quartz composition (Arabesque) in a Quick Look window | 25 frames/s, 7-8 ms | 32 frames/s, 4.2 ms |
| Quartz Composer, viewer behind the editor window | 24 frames/s, 7-8 ms | 23 frames/s, 5.1 ms (WindowServer draws it: covered) |
| QuickTime Player, a movie playing | 30 frames/s, 12-13 ms, WindowServer 15%, the player 29% | 30 frames/s, 7.5 ms, WindowServer 5%, the player 18% (WindowServer draws it: the surface is not opaque) |
| Screen saver preview in System Preferences (RSS Visualizer, Flurry) | 30 frames/s, 9-10 ms, WindowServer 6% | 30 frames/s, 4 ms, WindowServer 0% |
| Real Racing 2 (its intro, black on our driver either way) | 30 frames/s, 9-12 ms | 30 frames/s, 3.1 ms |
| Front Row, Core Image Fun House with a picture | right | the same |
| Chess | board black (a known fault) | the same |

Dashboard opened over a running gl-gears window: the window dims under it and keeps turning
(WindowServer draws it meanwhile), and the kernel shows it again when Dashboard closes.

Not tried: Grapher's 3D examples and Keynote transitions (need a document or menu steps),
iTunes' visualiser, DVD Player, Photo Booth and video chat (no disc, no camera), the same
switch on a host without KVM.

## Step 9: the default (2026-11-12, f1e00d8)

The kernel-shown path is now what every window gets when the kext runs Quartz Extreme work
(`gld_window.c`, `direct_wanted`). The old path stays for the configuration without it and
behind `RIPPLE_GL_DIRECT=0` or the file `/Library/Logs/RippleGL/no-direct`. The user-client
API version went to 2 with it, so a bundle never meets a kext without `SURFACE_PRESENT` and
`SURFACE_TEXTURE`. Numbers and checks: docs/re/perf-notes.md, "App GL windows shown by the
kernel".
