# OpenGL renderer plugin interface (10.6.8, 10K549)

Status: M1 in progress, first pass (2026-11-10). Static evidence only so far. Binary hashes
are in [binaries.md](binaries.md); everything here refers to the x86_64 slice unless noted.

## How OpenGL finds a hardware driver bundle

- The accelerator service publishes the bundle names as I/O Registry properties. On the
  reference VM (9600 GT, `ioreg -c IOAccelerator`), the `NVKernel` service
  (`IOMatchCategory` = `IOAccelerator`, from `com.apple.GeForce` 6.3.6) has:
  - `IOGLBundleName` = `GeForceGLDriver`
  - `IOVABundleName` = `GeForceVADriver`, `IODVDBundleName` = `GeForceVADriver`
  - `IOCFPlugInTypes` = `{ACCF0000-0000-0000-0000-000a2789904e = "GeForceGA.plugin"}` (2D)
  - `AccelCaps` = 3, `IOAccelRevision` = 2, `IOVARendererID` = `<02000401>`
- Each framebuffer (`IOClass` = `NVDA`) points at its accelerator with `IOAccelTypes` (the
  accelerator's registry path), `IOAccelIndex` (0, 1) and `IOAccelRevision` = 2.
- The bundles live in `/System/Library/Extensions/<name>.bundle`. The software renderer is
  `OpenGL.framework/Resources/GLRendererFloat.bundle`.
- UNVERIFIED: that OpenGL.framework (or GLEngine) reads exactly `IOGLBundleName` from the
  accelerator found through the display's `IOAccelTypes`. Next: trace file opens and
  IOKit lookups of a GL program (dtrace) on the reference VM.

## The plugin ABI: `gld*` entry points

All three GL drivers examined export the same 78 C functions, and nothing else of note:
`GLRendererFloat` 1.6.13 (software), `GeForceGLDriver` 6.3.6, `AppleIntelGMA950GLDriver`.

`gldAttachDrawable gldBufferSubData gldChoosePixelFormat gldCopyTexSubImage gldCreateBuffer
gldCreateComputeContext gldCreateContext gldCreateFence gldCreateFramebuffer
gldCreatePipelineProgram gldCreateProgram gldCreateQuery gldCreateShared gldCreateTexture
gldCreateVertexArray gldDestroyBuffer gldDestroyComputeContext gldDestroyContext
gldDestroyFence gldDestroyFramebuffer gldDestroyMemoryPlugin gldDestroyPipelineProgram
gldDestroyPixelFormat gldDestroyProgram gldDestroyQuery gldDestroyShared gldDestroyTexture
gldDestroyVertexArray gldDiscardFramebuffer gldFinish gldFinishObject gldFlush gldFlushBuffer
gldFlushMemoryPlugin gldFlushObject gldFlushVertexArray gldGenerateTexMipmaps gldGetError
gldGetInteger gldGetMemoryPlugin gldGetPipelineProgramInfo gldGetQueryInfo gldGetRendererInfo
gldGetString gldGetTextureLevelImage gldGetTextureLevelInfo gldGetVersion gldInitDispatch
gldInitializeLibrary gldIsTextureResident gldLoadBuffer gldLoadHostBuffer gldLoadTexture
gldModifyPipelineProgram gldModifyQuery gldModifyTexSubImage gldModifyTexture
gldModifyVertexArray gldObjectPurgeable gldObjectUnpurgeable gldPageoffBuffer gldReclaimBuffer
gldReclaimContext gldReclaimFramebuffer gldReclaimTexture gldReclaimVertexArray gldSetInteger
gldSetMemoryPlugin gldSyncBufferObject gldSyncTexture gldTerminateLibrary gldTestMemoryPlugin
gldTestObject gldUnbindBuffer gldUnbindFramebuffer gldUnbindPipelineProgram gldUnbindTexture
gldUnbindVertexArray gldUpdateDispatch gldWaitObject`

The two hardware drivers also export 28 `glrComp*` functions (OpenCL compute; out of scope).

Reading: the GL state machine lives in Apple's `GLEngine.bundle`; a driver plugs in below it
through these 78 calls, and `gldInitDispatch`/`gldUpdateDispatch` let it install its own fast
paths. The argument lists and structures are not in any SDK header. UNVERIFIED until
disassembled or traced.

## What the drivers import

- `GLRendererFloat` (software): no IOKit at all. It presents through private CoreGraphics
  window calls (`CGSNewWindow`, `CGSBindSurface`, `CGSLockWindowBits`, `CGSUnlockWindowBits`,
  `CGSFlushSurface`, `CGSSetWindowShape`, ...) and IOSurface (`IOSurfaceLockPlane`,
  `IOSurfaceGetBaseAddressOfPlane`, ...), plus Apple's shader/JIT libraries
  (`libGLProgrammability`, `libGLImage`, `glvm*`).
- `AppleIntelGMA950GLDriver`: talks to its kernel driver with only `IOServiceOpen`,
  `IOServiceClose`, `IOConnectCallMethod`, `IOConnectMapMemory`, `IOConnectAddClient`, plus
  `IOSurfaceBindAccel` and `IOSurfaceBindAccelOnPlane`; no CGS calls. 43 imports in total.
- `GeForceGLDriver`: 182 imports (not yet sorted).

So a hardware driver's kernel protocol is private to the vendor, and the shared pieces are
the `gld*` ABI above, IOSurface, and the surface user client WindowServer uses
([ioaccelerator-interface.md](ioaccelerator-interface.md)).

## Prior art: VMsvga2 (SourceForge, svn r166)

Read-only reference: its own code is "Copyright 2009-2011 Zenith432. All rights reserved."
(VMware-derived parts are MIT). Do not copy from it (HANDOFF.md rule 7).
- Its GL bundle (`GLD/VMsvga2GLDriver.c`, 810 lines with its entry-point table) does not
  render by itself: it `dlopen`s Apple's `AppleIntelGMA950GLDriver` and `GLRendererFloat` and
  forwards the `gld*` calls to them. Its kext provides user clients shaped like Intel's and
  NVIDIA's: Surface, GLContext, 2DContext, Device, DVDContext, OCDContext.
- The same six roles appear as classes in `GeForce.kext` 6.3.6: `IONVSurface`,
  `IONVGLContext`, `IONV2DContext`, `IONVDevice`, `IONVDVDContext`, `IONVOCDContext`.

## Open questions for the rest of M1

1. Signatures and structures of the `gld*` calls (`gldGetRendererInfo`, `gldChoosePixelFormat`,
   `gldCreateContext`, `gldAttachDrawable`, `gldFlush` first). Method: disassemble
   `GLRendererFloat` and its caller in `GLEngine`/`OpenGL`, confirm with dtrace `pid` probes.
2. Which renderer properties make apps and WindowServer treat a renderer as accelerated.
3. Whether a driver can present GL output the way `GLRendererFloat` does (CGS window
   surfaces) while rendering through virgl, which would avoid the private per-vendor kernel
   protocol for M4.
