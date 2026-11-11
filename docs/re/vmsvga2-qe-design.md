# How VMsvga2 approached Quartz Extreme (design notes for M5)

Date: 2026-11-10. Read by a helper agent from VMsvga2 svn r166 (`tags/1.2.5`, `branches/GL`) on
dev.lan (`.tools/src/vmsvga2-r166`). Behaviour and protocol only; no code copied. Its files say
"Copyright 2009-2011 Zenith432. All rights reserved." followed by an MIT-style permission
notice; whether that permits reuse is undecided, so we keep treating it as read-only
(HANDOFF rule 7). Caveat: VMsvga2's QE was opt-in and experimental (`-vmw_qe`, `-vmw_gl`), its
GL context mostly stubs in 1.2.5, and nothing says QE ever worked.

## The surface user client (type 0)

- Implemented on `externalMethod`, so selectors 6 and 17 get their 64-bit backing address
  whole (a comment says NVIDIA's IONVSurface does the same). 10.6 structure sizes:
  `IOAccelSurfaceInformation` 88 bytes, `IOAccelSurfaceScaling` 44, `IOAccelDeviceRegion` 12 + 8
  per rectangle.
- **SetIDMode (7)** names the surface: the window-server surface id and mode (needs the
  Windowed bit and a depth: 1555, 8888, BGRA32, YUV, YUV2). id 1 is WindowServer's own display
  surface.
- **SetShapeBackingAndLength (17)** (options, framebuffer index, backing address, row bytes,
  length + region; selectors 6 and 9 are the same without length or backing): keeps the region
  (refused above 4 KB; an empty region is a no-op) and the framebuffer index; client backing is
  checked against width x bpp and height x row bytes. With the identity-scale bit the buffer is
  the region size, except id 1, whose buffer is shaped like the whole screen.
- **Backing** is either the client's memory (wired for DMA; a lock returns the client's own
  address) or memory the kext allocates at the first lock and maps into the owner task.
  Quartz draws 2D into it with the CPU.
- **Locks (0, 1, 3, 4, 11-15)**: set or clear a read or write bit, return the surface
  information (address, buffer size, row bytes, pixel format). A write lock waits for pending
  device reads of the backing.
- **Flush (10)** copies the region's rectangles from the backing to the screen. For a
  GL-bound surface it presents the GL render target through the region instead, and a read lock
  first reads that render target back into the backing.
- **Stubs**: GetState (2) returns 0, Read (5) unsupported, Control (16) answers 1 for 1
  (`_CGXSynchronizeAcceleratedSurface`), 4 (`CGLSetPBufferVolatileState`) and 5 (surface
  volatility).

## Lifecycle

WindowServer creates every surface (an app's `CGSAddSurface` goes to it): id 1 for the display,
and one per CGS surface (GL windows, video, backing stores). Order (inferred): open type 0,
SetIDMode, optional SetScale, SetShape or SetShapeBackingAndLength on every move, write lock,
draw, unlock, Flush. That fits our NVIDIA trace (selectors 9 and 17; GL does the frames).

## GL binding

The GL bundle's `gldAttachDrawable` for a window calls OpenGL's `ioBindSurface` callback (from
`gldInitializeLibrary`), which creates a CGS surface and returns its id. The GL context client
is then told "bind to surface id"; the kernel finds the surface with that SetIDMode id and
gives it a GPU colour render target (plus depth/stencil) the size of its buffer, made again when
it grows. The GPU renders there; the surface's Flush presents it.

## Registry and other clients

- Accelerator: `IOCFPlugInTypes` (the GA plugin, also copied onto the framebuffer),
  `IOAccelRevision`, `AccelCaps` = 3 (only with QE on), `IOGLBundleName`, `IODVDBundleName` =
  `AppleVADriver` (AppleVA crashes without it). Framebuffer: `IOAccelTypes`, `IOAccelIndex`,
  `IOAccelRevision`. It also registers with IOSurfaceRoot.
- Client types: 0 surface, 1 GL context, 2 2D context (for the GA plugin), 3 DVD, 4 device
  (the GL bundle's config query), 5 OCD.
- The GA plugin (`IOGraphicsAcceleratorInterface` CFPlugIn) does 2D blits and video overlay
  surfaces; it is on whether or not QE is. Whether WindowServer needs it for QE: R1/R2.
- **Renderer flags:** its rebuilt `gldGetRendererInfo` documents bit 0x1000 as "supports
  Quartz Extreme" (next to Accelerated 0x100 and Compliant 0x400). Apple's GMA 950 driver ends
  with 0xb58b, NVIDIA's with 0x1000 set too. Our M4 bundle doesn't set it yet.

## What we take from this

1. A surface client on `externalMethod` with all 18 selectors, surfaces keyed by their
   SetIDMode id, regions, framebuffer index, client or kernel backing.
2. 2D Flush: copy the region from the backing into the framebuffer (or a GPU upload plus
   `DISPLAY`).
3. GL: bind a surface to a GPU render target that our GL bundle renders into (through
   `ioBindSurface` and a kext method), Flush presents it with `DISPLAY` (K2), read locks read it back.
4. Renderer flag 0x1000 and the registry properties above when the renderer is offered to
   WindowServer.
