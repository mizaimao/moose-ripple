# What turns Quartz Extreme on for a display (M5 R1)

Date: 2026-11-10. Static reading of the 10.6.8 (10K549) x86_64 slices on dev.lan:
CoreGraphics (WindowServer's code lives there; WindowServer itself is an 18 KB launcher),
OpenGL.framework and the guest's `CoreGraphics.framework/Resources/Configuration.plist`.
C = read from code, I = inferred. Addresses are CoreGraphics x86_64 VAs. Tool:
`tools/re/dis.py` (llvm-objdump plus stub, C string and CFString names).

## Short answer

`CGDisplayUsesOpenGLAcceleration(display)` returns byte 0x117 of WindowServer's display record
(C: 0x168d3c calls `boolForDisplayAndKey(display, 0x10)`, which calls `CGSDisplayStatusQuery`;
case 0x10 reads byte 0x117). WindowServer sets that byte for a display when all of these hold:

| # | Condition (C unless marked) | Ripple today (ce279dc, `ripple_qe=1`) |
|---|---|---|
| 1 | The display's framebuffer names an accelerator (`CGXAcceleratorForDisplayDevice`: `IOAccelTypes` and `IOAccelIndex`, as in `gl-plugin-interface.md`'s lookup chain). | Set by `RippleAccelerator` on `RippleFramebuffer`. |
| 2 | The accelerator's `AccelCaps` has bit 0. Bit 1 is also needed when the display record's flag 0x08 (at +0x54) is set; that flag's meaning is unknown. If every display's `AccelCaps` is 0, the renderer flag bit 12 (row 4) can stand in for bit 0, but only when an `IOAGPDevice` exists whose `model` doesn't start with `ATY,Rage128`; without an `IOAGPDevice`, QE stays off. | `AccelCaps` = 3, as NVKernel. |
| 3 | GLEngine (not CGL; see below) returns at least one pixel format for `{kCGLPFAWindow, kCGLPFAColorSize 24, kCGLPFADepthSize 0, kCGLPFAStencilSize 0}` (the list at 0x661f00). The node used for the display must have: the display's bit in its display mask (+0x34); flag bit 0 (Window) or 8 (Accelerated) (+0xc & 0x101); colour mode exactly 0x8000 (ARGB8888) or 0x4000 (RGB888) (+0x14); depth mode exactly 1 (kCGL0Bit) (+0x1c); stencil mode exactly 1 and aux buffers 0 (+0x20, +0x26). | `gldChoosePixelFormat` gives ARGB8888, kCGL0Bit, kCGL0Bit, Window + Accelerated. Meets it. |
| 4 | `gliQueryRendererInfo(1 << display)` succeeds. If its flags (+0xc) have bit 12, the display counts as accelerated (row 2). Its video memory in MB (+0x4c) is shifted left by 20 into 32 bits and compared with `GLCompositorMinimumVRAM` without scaling (16 bytes): any value from 1 to 4095 MB passes; 0 or 4096 MB and up fail. | Bit 12 set (`GLD_FLAG_QUARTZ_EXTREME`), 512 MB. |
| 5 | The accelerator or one of its parents in the IOService plane has the exact class name (`IOObjectGetClass`, no subclass match) `IOAGPDevice` or `IOPCIDevice` (`GLCompositorRequiredClasses`), and none has a registry name in `GLCompositorExcludedInstances`. | `RippleAccelerator` → `RippleFramebuffer` → ... → `IOPCIDevice` (virtio-gpu). Meets it. |
| 6 | The display depth is at least `GLCompositorMinimumDisplayDepth` (16 in the plist, 32 without it). | 32-bit modes. |
| 7 | The framebuffer's VRAM, which is the size of the `IOConnectMapMemory(kIOFBVRAMMemory = 110)` map (`IOFramebuffer::getVRAMRange()`), is at least `GLCompositorMinimumVRAM` MB (16). It must be at least twice that (32 MB) if another active display uses the same accelerator. Otherwise `updateGLVRAMFlag` (0xc6788) clears the display's "OK for OpenGL window accel" flag (0x40 at +0x54) and byte 0x117. | `kVRAMSize` = 32 MB. Meets it for one display, and just meets it for two. |
| 8 | `GLCompositorTileWidth` and `GLCompositorTileHeight` are nonzero (256 in the plist), and GLEngine creates the compositor context from the first matching pixel format (`cglsCreateContext`). | Needs `gldCreateContext` to succeed for WindowServer. |

Not part of the decision (C, nothing in the chain reads them): the 2D GA plugin
(`IOCFPlugInTypes`), the GL vendor, renderer and extension strings, `kCGLPFAAccelerated` (not
requested), `VRAM,totalsize` or `IOFBMemorySize` (CoreGraphics references neither), and
QuartzGL. `__CGXQGLCapable`, `QuartzGLMinimumVRAM` and `QuartzGLMinimumDRAM` decide QuartzGL,
which is per-application Quartz 2D drawing on GL and off by default; it is not QE.

## The code path (C)

1. **Who asks:** `__CGXGLDisplayContextForDisplayDevice` (0xe00b7) runs for each display. On
   the first call it runs `__CGXGLDisplayContextInitialize` (0xe01db). It then requires the
   tile size, `AccelCaps & 3 == 3` when display flag 0x08 is set, depth ≥ minimum, byte 0x113
   clear and the VRAM flag 0x40 (row 7). Then it finds the GL context entry whose display mask
   holds the display. If that entry has a context, it sets byte 0x117 = 1 and server operation
   state 7.
2. **Displays and accelerators** (0xe033e to 0xe0435): for each display,
   `CGXAcceleratorForDisplayDevice(display, &accel, &index)` and
   `IORegistryEntryGetProperty(accel, "AccelCaps", 8 bytes)`. The value is kept in the display
   record (+0x58) and ORed over all displays. With no accelerator on any display, QE is off.
   With all `AccelCaps` zero, the `IOAGPDevice`/`ATY,Rage128` test of row 2 runs.
3. **GL setup** (0xe0583): under root credentials, `cglsInitialize(services, fbindices, n)`
   dlopens `OpenGL.framework/Resources/GLEngine.bundle/GLEngine` and calls its
   `gliInitializeLibrary` with the accelerator services and framebuffer indices from step 2.
   GLEngine then loads the bundles named by `IOGLBundleName`. The shim table `__cgls_plugin`
   takes, in this order from +0x18: `gliChoosePixelFormat`, `gliDestroyPixelFormat`,
   `gliQueryRendererInfo`, `gliDestroyRendererInfo`, `gliCreateContext`, `gliDestroyContext`,
   `gliAttachDrawableWithOptions`, `gliSwapBuffers`, `gliSetInteger`, `gliGetInteger`,
   `gliGetVersion`. So WindowServer goes to GLEngine directly, not through CGL, and the records
   it reads are the `gld` layouts in `gl-plugin-interface.md`: renderer info +0x0c flags and
   +0x4c video memory, and the pixel format node (0x38 bytes, `next` at +0).
4. **Pixel formats** (0xe05e3): one `cglsChoosePixelFormat` for the whole list in row 3. No
   result means no QE.
5. **Per display** (0xe0745 to 0xe0bc7): renderer info (row 4), RequiredClasses and
   ExcludedInstances (row 5) on the display's accelerator, then each pixel format node (row 3).
   For a matching node it sets byte 0x117 if the depth passes (row 6). It then adds the
   display's bit to the entry with the same renderer ID and display mask, or starts a new
   0xb0-byte entry (display mask, VRAM bytes, framebuffer index, `AccelCaps`, a copy of the
   node).
6. **Context** (0xe0c3d): `cglsCreateContext` from the first entry's node; on failure, no QE.
   Then it logs (`CGSLogMessage`, always): `GLCompositor: GL renderer id 0x%08lx, GL mask
   0x%08lx, accelerator 0x%08lx, unit %ld, caps 0x%08lx, vram %ld MB`. After that come
   `cglsAttachDrawable`, `__CGXGLDisplayContextInformation`, and the server operation states:
   state 7, and states 0x12, 0xf, 0xd, 0x13, 0xe and 0x11 when `CGXGPUCapability` grants them.
   0x12 = a compositor context exists; 0xf = some entry has `AccelCaps` bit 0 and its +0x68
   bit 0 (set later, I); 0xd = QuartzGL capable; 0xe and 0x11 = the QuartzGL on/off
   preferences; 0x13 = the QuartzGL default.
7. **Later changes:** `updateGLVRAMFlag` (row 7) and `CGXGLCompositorSetTileSize` clear byte
   0x117. Mirroring and mux switching read it.

## How CoreGraphics calls GL (for the takeover; C unless marked)

- `cglsInitialize` (0xe0e28) calls `gliInitializeLibrary(services, fbindices, n, 0, 0, 0)` and
  requires `gliGetVersion` to report 2 and 3. It then asks `gliGetInteger(0, 0xe0, &size)` for
  the dispatch table size (`__cgls_gli_dispatch_size`; default 0x17e0 if the call fails).
- `cglsCreateContext` (0xe6bb3) mallocs `size + 0xb0` bytes and calls
  `gliCreateContext(&ctx[0], pixelFormat, share, &ctx[8], __cgls_dummy_disp, 0)`. The compositor's
  context pointer is kept in the CoreGraphics global `__cglsContext` (VA 0x7398e0, a local symbol
  in the nlist table; dlsym can't find it).
- The block has the same front layout as a `CGLContextObj`: +0 the GLEngine context and +8 the
  dispatch table GLEngine fills. GL calls are `ctx->disp[n](ctx->rend, ...)`, for example
  `call *0x348(%r12)` with `(%r12)` as the first argument for `glGetIntegerv`
  (`__CGXGLDisplayContextInformation`, 0xe7017). The table is in malloc'd memory, so it is
  writable. After it come 0xb0 bytes of cgls state, starting with the `__cgls_plugin` pointer.
- The 5th argument is a different, read-only table: `__cgls_dummy_disp_const` (VA 0x666ee0,
  `__TEXT,__const`) when the size is at most 0x2000, otherwise a calloc'd one. CoreGraphics never
  calls through it (I: it is GLEngine's stand-in or "front" table). This is probably the
  read-only table seen in the driver work. The one to patch is `__cglsContext + 8`.

## Configuration.plist on the guest (10.6.8)

`/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreGraphics.framework/Resources/Configuration.plist`
(SHA-256 `2de4fac1882591f4…`, read on VM 212), `GLCompositorConfiguration`:
`GLCompositorMinimumDisplayDepth` 16, `GLCompositorMinimumVRAM` 16, `GLCompositorRequiredClasses`
(`IOAGPDevice`, `IOPCIDevice`), `UseOnPortable` true, tile width and height 256. Without the file
the defaults are 32-bit depth, 16 MB and `IOAGPDevice` only (C, `_CGXCoreGraphicsConfiguration`).

## What this means for the driver

- With `ripple_qe=1`, the kext and GL bundle as of ce279dc meet every row on paper, and ripple
  saw `CGDisplayUsesOpenGLAcceleration` = 1 on VM 210 (f98c322). If it ever drops back to 0,
  check row 8 (`gldCreateContext` or the first attach failing for WindowServer) and row 7 first.
- Keep the framebuffer's `getVRAMRange()` at 32 MB or more, and the renderer's video memory
  below 4096 MB (row 4's 32-bit shift).
- Quick check on the guest: the `GLCompositor:` line above names the renderer ID, the display
  mask, `caps` (`AccelCaps`) and the VRAM WindowServer picked. Where `CGSLogMessage` writes on
  10.6 (through the `message_logger` hook) is for R3 to confirm.

## Open

- Meaning of display flag 0x08 (+0x54), which makes `AccelCaps` bit 1 required. `AccelCaps` = 3
  covers both cases.
- The exact use of renderer flag bit 12 beyond row 2 (GLEngine may use it elsewhere).
- Runtime confirmation on the NVIDIA VM and VM 210 (R3: `tools/qe-check`).
