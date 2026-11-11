# Why WindowServer composites a display with GL, or not (M5 R4)

Date: 2026-11-10, for a question from the driver work: on VM 210 the compositor context is
taken over and `CGDisplayUsesOpenGLAcceleration` = 1, but WindowServer never acquires the
display surface (no `IOAccelSetSurfaceFramebufferShape`, selector 9) and keeps compositing the
old way. Static reading of 10K549 x86_64: CoreGraphics and IOKit.framework. C = read from code,
I = inferred. Addresses are VAs in the named binary.

## Answer

The GL extensions and limits don't gate it. The display surface needs a working **2D GA
plugin on the framebuffer**: WindowServer only takes the GL path for a display whose flags have
bits 0x80 and 0x08, and both come from `IOPSAllocateBlitEngine(framebuffer)` succeeding at
WindowServer start. Ripple has no GA plugin, so the flags stay clear and every update takes the
software path (C, chain below; that this is the condition the driver fails is I, from the
code plus "no GA plugin", and can be confirmed with the check at the end).

## The chain (C)

1. **Gate** in `CGXAcquireDisplayDeviceSurface` (CoreGraphics 0xf5c10, `Server/Windows/Surface.c`),
   at 0xf635a: display info flags (display record +0x10, field +0xf0) need bit 0x80 and bit 0x08.
   Debug option flags 0x01000000, 0x04010000 and screen recording also force software. Then:
   `CGXAccessSurfaceForDisplayDevice` → `MPAccessSurfaceForDisplayDevice` (0xf7225, rechecks
   32-bit, 0x08 and 0x80; `IOAccelFindAccelerator`, `IOAccelCreateSurface` with a surface ID),
   `CGXAcceleratorForDisplayDevice`, `IOAccelSetSurfaceScale` (only when zoomed),
   `IOAccelSetSurfaceFramebufferShape`, `IOAccelQueryLockSurface`, `IOAccelWriteLockSurface`.
   Any failure logs (trace only) "NO HW surface, using software path".
2. **Display flags** come from `__MPSetDisplayFlags` (0xc59f2), from the mapped display's flags
   (driver record +0xa8): only if that has 0x80 does it set 0x08, and 0x80 too when the
   framebuffer's `IOAccelRevision` is 2 or more. (0x20 in the driver flags adds 0x52, and 0x04
   when VRAM has room for two frames.)
3. **Driver flag 0x80** is set once, in `CGXMappedDisplayStart` (0xc43ad):
   `IOPSAllocateBlitEngine(framebuffer, &blit, &score)` succeeding sets 0xe0. It is then cleared
   unless the mode is deeper than 2 bytes per pixel (+0x1f8), and 0x60 is cleared when there is
   no blit engine.
4. **`IOPSAllocateBlitEngine`** (IOKit 0x10267) calls
   `IOCreatePlugInInterfaceForService(framebuffer, kIOGraphicsAcceleratorTypeID
   (ACCF0000-0000-0000-0000-000A2789904E), kIOGraphicsAcceleratorInterfaceID
   (6766E94A-0000-0000-0000-000A2789904E))`. Then, on the `IOGraphicsAcceleratorInterface`
   (`IOKit/graphics/IOGraphicsInterface.h` in the 10.6 SDK):
   - `SetDestination(this, 0, NULL)` if the slot is non-NULL: must return 0;
   - `GetBlitter(this, kIOBlitAllOptions, kIOBlitTypeCopyRects, kIOBlitSourceDefault, &b)`: must
     return 0;
   - `GetBlitter(this, kIOBlitAllOptions, kIOBlitTypeRects, kIOBlitSourceSolid, &b)`: must
     return 0;
   - `GetBlitter(..., kIOBlitTypeCopyRegion, kIOBlitSourceFramebuffer, ...)` and
     `GetBlitter(..., kIOBlitTypeCopyRects, kIOBlitSourceMemory, ...)`: optional (NULL on error).
5. **Finding the plugin** (`IOCreatePlugInInterfaceForService`, IOKit 0x63d6): it reads
   `IOCFPlugInTypes` from **the framebuffer service itself** (`IORegistryEntryCreateCFProperty`,
   no parent or child search), loads `/System/Library/Extensions/<value>` as a CFPlugIn, matches
   the bundle's optional Info.plist `Personality` against the service, creates the factory for the
   type, queries the interface, then calls `Probe` and `Start` (with the service). NVIDIA and
   VMsvga2 publish `IOCFPlugInTypes` on the framebuffer as well as on the accelerator
   (`vmsvga2-qe-design.md`).

The R2 trace fits: on the 9600 GT, WindowServer opened `NV2DContext` (accelerator client type 2)
only at login. That was `GeForceGA.plugin` starting for `IOPSAllocateBlitEngine`. The blitters are
then used only when no GL surface is available (I).

## GL extensions and limits (C; they don't gate the display surface)

`__CGXGLDisplayContextInformation` (CoreGraphics 0xe6fd0), once per compositor context:
- `glGetIntegerv`: `GL_MAX_TEXTURE_UNITS` (0x84e2), `GL_MAX_VIEWPORT_DIMS` (0xd3a, default 2048),
  `GL_MAX_TEXTURE_SIZE` (0xd33, default 2048). The largest compositor surface
  (`glSurfaceWidthMax`/`HeightMax`) is min(max texture size, viewport width/height). Keep both at
  or above the display size.
- `glGetString(GL_EXTENSIONS)`, whole-word matches, into a bit mask (entry +0x68):
  0x1 `GL_ARB_fragment_program`, 0x2 `GL_ARB_texture_non_power_of_two`,
  0x4 `GL_ARB_shading_language_100`, 0x8 `GL_APPLE_float_pixels` (without it
  `glFloatingPointCapable` = 0). Nothing returns early on a missing one. Bit 0x1 plus `AccelCaps`
  bit 0 grants `CGXGPUCapability(0xf)` (server state 0xf, I: Core Image on the GPU).
- It logs `GLCompositor: GL renderer id ..., GL mask ..., texture units N, texture max N, viewport
  max {W, H} extensions 0x...` to `/var/log/windowserver.log`. The 9600 GT logs 8, 8192,
  {8192, 8192} and 0x0000000f. Ripple's line will show the same mask unless one of the four is
  missing.
- Not checked here: `GL_APPLE_client_storage`, `GL_APPLE_texture_range`, `GL_APPLE_ycbcr_422`,
  `GL_APPLE_flush_render`, `GL_APPLE_fence`, `GL_APPLE_vertex_array_object`, vendor, renderer or
  version strings. The compositor's GL drawing code may still use the APPLE extensions once the
  surface path is on; R4 didn't look there.

## For the driver

- A minimal GA CFPlugIn (`IOGraphicsAcceleratorInterface`): `Probe` and `Start` succeed;
  `SetDestination` returns 0 (or leave it NULL); `GetBlitter` returns working blitters for
  CopyRects/Default and Rects/Solid (on the framebuffer: GPU-neutral, could start as CPU copies
  into the framebuffer through the 2D client). Publish
  `IOCFPlugInTypes = {ACCF0000-0000-0000-0000-000A2789904E = "RippleGA.plugin"}` on
  `RippleFramebuffer` (and on the accelerator, as NVIDIA does). Keep the framebuffer's
  `IOAccelRevision` at 2, and modes at 32 bits.
- Quick check that this is the gate: `ioreg -l -w0 -c RippleFramebuffer | grep IOCFPlugInTypes`
  prints nothing today, and `IOPSAllocateBlitEngine` can't succeed without it (step 5). It runs
  once when WindowServer starts, so a dtrace check needs WindowServer restarted (log out and
  back in) while `pid$target:IOKit:IOPSAllocateBlitEngine:return` is armed.
