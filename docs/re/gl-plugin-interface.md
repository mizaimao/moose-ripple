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

All three GL drivers examined export the same 80 C functions, and nothing else of note:
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
through these 80 calls, and `gldInitDispatch`/`gldUpdateDispatch` let it install its own fast
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

## Trace 2: `cgl-info` on the NVIDIA driver (2026-11-10)

`tools/cgl-info` (our CGL test program) on reference VM 211, traced with
`sudo dtrace -q -Z -c ./cgl-info -s tools/re/gld-calls.d GeForceGLDriver`; raw, time-sorted
output in [traces/2026-11-10-cgl-info-geforce.txt](traces/2026-11-10-cgl-info-geforce.txt).
`cgl-info` saw two renderers: `0x0102260b` (NVIDIA, accelerated, display mask 0x3, 1024 MB)
and `0x01020400` (Apple software). GL 2.1, GLSL 1.20; the FBO clear read back 64 127 191 255.

Order of driver calls for "query renderers, make a context, clear an FBO, read a pixel":
1. `gldInitializeLibrary` once: opens an accelerator client (`IOServiceOpen` type 4), maps
   memory type 1, several calls (selectors 1, 3, 10, 11, 13).
2. `gldGetVersion` (4 pointer arguments, returns 1), `gldGetRendererInfo` (2nd argument 0x3 =
   the display mask), `gldChoosePixelFormat`, `gldDestroyPixelFormat`.
3. `gldCreateShared` (args include 0x3 and 0x4): another type-4 client, selectors 0 and 8.
4. `gldCreateContext`: a type-1 client (GL context), `IOConnectMapMemory` types 3 and 4, then
   `IOConnectAddClient(context, shared)`.
5. Many `gldCreateTexture`/`gldCreatePipelineProgram`/`gldCreateVertexArray` (GLEngine's own
   default objects), `gldGetString(0x3, 0x1f00/0x1f01/0x1f02)` for vendor/renderer/version,
   `gldCreateFramebuffer`, `gldAttachDrawable(ctx, 0x5b, ...)`, `gldInitDispatch`,
   `gldUpdateDispatch`, `gldFlush`, `gldGetError`, then teardown.

Readings (UNVERIFIED until disassembly confirms them):
- Client types used by the GL driver: 1 = GL context, 4 = device/shared. With
  `IOAccelClientConnect.h`'s type 0 = surface, that leaves 2 (2D) and 3 for WindowServer's
  other clients, matching the order of VMsvga2's user clients.
- The driver, not GLEngine, supplies `GL_VENDOR`/`GL_RENDERER`/`GL_VERSION` through
  `gldGetString`, keyed by the display mask.
- dtrace note: pid probes only worked with `-Z -c`. Attaching with `-p` to a process that
  had `dlopen`ed the driver itself matched no probes in any module.

## The lookup chain, from disassembly (2026-11-10)

1. `OpenGL` (x86_64, around 0x4dc3): for each display from `CGSGetDisplayList`, it takes the
   OpenGL display mask (`CGSGetDisplayOpenGLDisplayMask`) and the framebuffer service
   (`CGSServiceForDisplayNumber`), then calls `IOAccelFindAccelerator(framebuffer, &accel,
   &index)` and stores the accelerator per mask bit. Without a WindowServer connection it
   walks `IOServiceMatching("IOFramebuffer")` instead.
2. `IOKit` `IOAccelFindAccelerator` (0xef03): reads the framebuffer's `IOAccelTypes` (a
   registry path string), `IORegistryEntryFromPath`, requires `IOObjectConformsTo(...,
   "IOAccelerator")`, then reads the framebuffer's `IOAccelIndex`. Nothing else.
3. `libGFXShared` `gfxPluginConnectAll` (0xe7b): for each accelerator it reads
   `IOGLBundleName` and `dlopen`s `/System/Library/Extensions/<name>.bundle/Contents/MacOS/<name>`.
   The software renderer is added from `OpenGL.framework/Resources` (or `$GL_RESOURCES`).

## Experiment: OpenGL loads our bundle (2026-11-10, VM 210)

- `kext/accelprobe` (`RippleAccelProbe`, C++, x86_64): an `IOAccelerator` on the virtio-gpu
  PCI device (`IOPCIMatch` 0x10501af4, `IOMatchCategory` `IOAccelerator`, `IOGLBundleName` =
  `RippleGLDriver`); on start it sets `IOAccelTypes`/`IOAccelIndex`/`IOAccelRevision` on the
  `IONDRVFramebuffer` (`.Display_boot`) attached to the same device.
- `gldriver/shim` (`RippleGLDriver.bundle`, x86_64): all 80 `gld*` entry points as trampolines
  into `GLRendererFloat`, installed in `/System/Library/Extensions` of VM 210.
- Result: with the kext loaded, `cgl-info` logged "RippleGLDriver: loaded in pid 221,
  forwarding 80 gld entry points to GLRendererFloat (0 missing)". So OpenGL followed
  framebuffer -> our accelerator -> our bundle. `CGLQueryRendererInfo` then failed with
  10006 (invalid display): a bare forward to the software renderer doesn't describe a
  renderer for the accelerated display. Expected; `gldGetRendererInfo` is M4 work.
- Unloading the kext removed the properties and restored the normal state.

## First decoded signatures (GLRendererFloat 1.6.13, x86_64 disassembly, 2026-11-10)

Read from the code; still UNVERIFIED against a second driver unless noted.
- `int gldGetVersion(int *a, int *b, int *c, int *d)`: if initialized, writes 3, 1, 0, 0x400 and
  returns 1 (GeForceGLDriver also returned 1 in Trace 2).
- `int gldGetRendererInfo(void *info, uint32_t displayMask, ...)`: returns 10006
  (`kCGLBadDisplay`, "invalid display") unless `displayMask` is within the mask the driver was
  initialized with; otherwise fills a 0x88-byte record and returns 0. GLRendererFloat writes
  0x01000400 at +0x08 (renderer ID; CGL reported 0x01020400), the display mask at +0x24, and
  other flags/mode fields at +0x0c..+0x30; memory sizes are 0. Field meanings: to map by
  dumping NVIDIA's record next to CGL's renderer properties.
- `const char *gldGetString(uint32_t displayMask?, GLenum name)`: 0x1f00 vendor, 0x1f01
  renderer, 0x1f02 version, plus driver name and compute-device strings for 0x1f04/0x1f05.
- `gldInitDispatch(gldContext *ctx, void *renderOps, void *limits)`: fills a driver table of
  about 33 render operations (`gldAccum`, `gldClear`, `gldReadPixels`, `gldDrawPixels`, ...,
  table size 0x108, which matches the gap between the two pointers in Trace 2) and a limits
  record. It does **not** touch the public GL dispatch table.
- So under Apple's GLEngine a driver renders through those ~33 operations and reads the GL
  state from GLEngine's private context structures (e.g. GLRendererFloat's `gldUpdateDispatch`
  follows pointers at context offsets 0x740, 0x748, 0x778). Those structures are undocumented
  and large: this is the main reverse-engineering risk of a "native" driver.

## The public dispatch table (SDK headers)

- `OpenGL.framework/Headers/CGLContext.h`: `struct _CGLContextObject { GLIContext rend;
  GLIFunctionDispatch disp; CGLPrivateObj priv; void *stak; }`.
- `gliDispatch.h`: `GLIFunctionDispatch`, 764 function pointers, each taking the `GLIContext`
  first.
- `libGL.dylib` (10K549) uses it for every call: `glClear` loads the current context from
  `%gs:0x150`, passes `ctx->rend`, and jumps through `ctx + 0x58` (`disp.clear`);
  `glDrawArrays` through `ctx + 0x210` (`disp.draw_arrays`). Both offsets match the header.
- Consequence: a driver that, once its context exists, points `rend` and `disp` at its own
  GL implementation (Mesa) receives every GL call through a documented ABI, and needs from
  GLEngine only the `gld*` management calls (renderer info, pixel formats, context and drawable
  lifecycle, flush). See [m1-report.md](m1-report.md).

## Where the takeover hooks in (static disassembly, 10K549 x86_64, 2026-11-10)

From a read of OpenGL.framework and GLEngine (by a helper agent; not yet confirmed at runtime).
Offsets are for this build only; the bundle must check the magic values before using them.

- **`CGLCreateContext`** (OpenGL 0x15ad) mallocs one block: the public `CGLContextObject` at
  offset 0 (`rend` +0, `disp` +8, 764 slots; `priv` +0x17e8; magic 0x1b7f1843 at +0x18a0),
  then one 0x17f8-byte record per GL engine plugin, each with the plugin's `GLIContext` (+0),
  its `gli*` table (+0x10) and a second 764-slot "back" dispatch table (+0x18).
- It calls GLEngine's `gliCreateContext(&rec->rend, renderers, share, &ctx->disp, &rec->disp,
  flags)`. GLEngine keeps `&ctx->disp` at gc+0x6550 and `&rec->disp` at gc+0x6558 (gc =
  GLEngine's context, magic 0x3463a223 at gc+0x6588), calls `gldCreateContext` per renderer and
  fills the table. Every GLEngine slot write goes to back if back[i] != 0, else to front;
  OpenGL then copies back to front, zeroes back and sets `ctx->rend = gc`.
- At the end of `CGLCreateContext`, OpenGL always sends `gliSetInteger(0x2bd)`, which GLEngine
  forwards as `gldSetInteger(drvctx, 0x2bd, &v)` to every renderer: the first moment the
  table is final and the driver runs.
- **No `gld*` call receives the CGL object.** The way back: `gldCreateContext`'s 5th argument
  is gc+0x8b0, and `ctx = *(gc+0x6550) - 8`.
- **GLEngine rewrites the table later**: the whole table on `CGLEnable(kCGLCEMPEngine)`, draw
  slots on drawable attach, renderer switch, fallback and `CGLCopyContext`. A plain overwrite of
  `ctx->disp` would be undone.
- **`CGLSelectDispatch(ctx, table)`** (exported by OpenGL, not in the SDK headers; used by
  `GLProfilerFBDisp`) copies the live table to back and installs `table` in front. From then
  on GLEngine's writes land in back, so the takeover sticks. Only `CGLRestoreDispatch` undoes it.
- **Calls through the table with `ctx->rend`**: `CGLFlushDrawable` calls
  `disp.swap_APPLE(ctx->rend)` (double-buffered only); `CGLSetSurface`/`CGLSetOffScreen`/
  `CGLSetPBuffer` call `disp.scissor` and `disp.viewport`. Two calls pass GLEngine's gc
  instead: `flush_render_APPLE(gc)` from drawable attach (goes through back after the takeover),
  and `get_integerv(rec->rend)` through front when `kCGLCEStateValidation` is on, so our
  thunks must tolerate a `rend` that isn't ours.
- **`CGLSetCurrentContext`** is libGL's: `pthread_setspecific(0x1e, ctx)`, nothing else.
- `gldInitDispatch(drvctx, renderOps, limits)` runs only from `gliUpdateDispatchState` (drawable
  attach, `kCGLCERasterization`, fallback), never inside `CGLCreateContext`.

**Plan:** in the first `gldSetInteger` on a context of ours, find `ctx` through gc, check both
magic values, `ctx->rend == gc` and an all-zero back table, then `CGLSelectDispatch(ctx,
our_table)` and set `ctx->rend` to our context. Check at runtime with a dtrace probe on
`gldSetInteger` (0x2bd) once VM 210 is free.

**i386 (same build, same method):** CGL magic at ctx+0xc60 (`rend` +0, `disp` +4, 764 slots
of 4 bytes, `priv` +0xbf4); gc magic at gc+0x5844, `&ctx->disp` at gc+0x5820, back pointer at
gc+0x5824; `gldCreateContext`'s 5th argument is gc+0x890. `gldSetInteger(0x2bd)` and the
exported `CGLSelectDispatch`/`CGLBackDispatch`/`CGLFrontDispatch`/`CGLRestoreDispatch` behave
as on x86_64.

## The management calls a takeover driver must answer (static disassembly, 10K549 x86_64)

Read by a helper agent from libGFXShared (gfx), GLEngine (GLE), OpenGL (OGL), GLRendererFloat
and AppleIntelGMA950GLDriver; runtime confirmation pending. C = read from code, I = inferred.

- **Why the M1 forward-everything shim failed (C):** gfx calls `gldInitializeLibrary` then
  `gldGetVersion` per plugin, accelerator bundles first. The shim reported the float renderer's
  ID 0x400, so the real GLRendererFloat was disconnected as a duplicate; its
  `gldTerminateLibrary` zeroed the shared image's display mask, hence 10006 afterwards.
- `void gldInitializeLibrary(io_service_t accel[32], uint8 accelIndex[32], uint32 displayMask,
  void *ioFlush, void *ioBindSurface, uint32 flags)`: once per bundle, arrays indexed by display
  bit. `int gldGetVersion(int *major, int *minor, int *rev, uint32 *id)`: return nonzero with
  3, 1, 0 and a renderer ID with `(id & 0xffff00ff) == 0`, unique, `(id & 0x7f00) != 0x400`.
  We use **0x5000** (Apple uses 0x02/04/06, 0x10-0x1a, 0x20-0x26, 0x30, 0x40/0x42).
- `int gldGetRendererInfo(void *rec, uint32 displayMask)`: 0x88-byte record. +0x08 renderer ID
  (`0x01000000 | id`; GLE adds 0x20000), +0x0c flags (bit 0 Window, 1 FullScreen, 2 OffScreen,
  3 BackingStore, 6 Robust, 7 can-be-fallback (I), 8 Accelerated, 9 MultiScreen, 10 Compliant,
  13 PBuffer, 14 GPU vertex, 15 GPU fragment, 16 accelerated compute), +0x10 buffer modes,
  +0x14 color, +0x18 accum, +0x1c depth, +0x20 stencil, +0x24 display mask, +0x28/2a/2c s16 aux
  buffers, sample buffers, samples, +0x2e u8 sample alpha, +0x30 sample modes, +0x4c video and
  +0x54 texture memory in MB; the rest zero. Return 10006 unless `mask & init` is nonzero and
  `mask & ~init` zero.
- `int gldChoosePixelFormat(void **out, const uint32 *attrs)`: OGL strips the renderer-selection
  attributes (it filters on them itself) and appends attribute 4. Return 0 with `*out = NULL` if
  nothing matches (nonzero aborts the choice for all renderers). Otherwise one malloc'd list of
  0x38-byte nodes: +0 next, +8 renderer ID, +0xc flags, +0x10 buffer mode (0 or 8 = double,
  must equal the request), +0x14 one color-mode bit (0x8000 = ARGB8888), +0x18 accum, +0x1c
  depth, +0x20 stencil, +0x26 u16 aux, +0x28 u16 sample buffers, +0x2a u16 samples, +0x2c sample
  mode, +0x30 u8 sample alpha, +0x34 display mask. OGL copies the nodes and calls
  `gldDestroyPixelFormat(node)` at once: free our block, don't walk `next`.
- `int gldCreateShared(void **out, uint32 displayMask, int)`, `gldDestroyShared(shared)`.
- `int gldCreateContext(void **out, pfNode, shared, cfg, glState, aux)`: `cfg` (~0x1d0 bytes) is
  the one thing GLE needs filled: limits and capability bits, as GLRendererFloat's
  `_gldSetConfigData` writes them. GLE treats driver contexts, shared state and object handles
  as opaque (I: no dereference found; GLRendererFloat even stores placeholders like 4 and 0x123).
- `int gldInitDispatch(ctx, renderOps, limits)` / `gldUpdateDispatch(ctx, renderOps, dirty)`:
  return bit 0x4 = no fallback (GLRendererFloat and GMA return 4, NVIDIA 5; 0x1 = driver does
  vertex processing). 33 renderOps slots: fill all except byte offsets 0x80-0x98, 0xc8, 0xd0;
  limits = {drawable width, height, 0, 0, 0, 0}. Only called once a drawable is attached.
- `gldSetInteger(ctx, pname, const int *)` / `gldGetInteger(ctx, pname, int *)`: return 0 for
  unknown pnames. `const char *gldGetString(uint32 displayMask, GLenum)`: 0x1f00-0x1f02, 0x1f04
  driver name, else NULL. `gldGetError` returns and clears. `gldFlush`/`gldFinish`: return
  unused. `int gldAttachDrawable(ctx, type, data, opts)`: 0-3 OK, 0x2720 asks for the float
  fallback; types 0 detach, 0x35 offscreen, 0x36 fullscreen, 0x5b, 0x5c fallback probe.
- Object calls take `shared` first (`gldCreateTexture(shared, void **out, glTex)`, ...;
  `gldUnbind*(ctx, obj)`); dummy handles are fine.
- If no chosen pixel format node has flag bit 7 and recovery is allowed, OGL adds
  GLRendererFloat as a second virtual screen: `cgl-info` asks for `kCGLPFANoRecovery`.
