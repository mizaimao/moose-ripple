# M1 report: reverse-engineering spike (go/no-go)

Date: 2026-11-10. Evidence: [gl-plugin-interface.md](gl-plugin-interface.md),
[ioaccelerator-interface.md](ioaccelerator-interface.md), [binaries.md](binaries.md),
[traces/](traces/), [notebook.md](notebook.md).

## Recommendation: go, with two scope changes

1. **Architecture: "Mesa takes over the dispatch table".** Our GL bundle implements the
   `gld*` management calls Apple's OpenGL needs (renderer info, pixel formats, context and
   drawable lifecycle, flush), and points each CGL context's public dispatch table (SDK
   `CGLContext.h`, `gliDispatch.h`) at Mesa, whose Gallium virgl driver talks to our kext.
   This avoids reverse-engineering GLEngine's private state structures, which a "native"
   driver under GLEngine would have to read.
2. **64-bit kernel only.** Clang can't build 32-bit C++ kexts (`-fapple-kext` unsupported for
   i386), and the VM already boots the 64-bit kernel. 32-bit *apps* stay supported: the GL
   bundle (plain C, userspace) builds for i386 and x86_64. Supporting the 32-bit kernel later
   would mean building the kext with Xcode 3.2.6's gcc in the guest.

## Answers to M1's questions

**Is the GL plugin ABI small enough to implement?** Yes for the "takeover" design.
- Every GL driver exports the same 80 `gld*` functions. OpenGL finds a driver through
  framebuffer `IOAccelTypes`/`IOAccelIndex` -> accelerator `IOGLBundleName` -> `dlopen`
  (disassembly of OpenGL, IOKit, libGFXShared).
- Proven by experiment: our kext made OpenGL load our own bundle on VM 210.
- First signatures decoded (`gldGetVersion`, `gldGetRendererInfo`, `gldGetString`,
  `gldInitDispatch`); a full ordered trace of NVIDIA's driver serves as the reference sequence.
- A "native" driver would also need GLEngine's private state layout: large and undocumented.
  That is the part the takeover design sidesteps.

**Can WindowServer be satisfied without a full IOAccelerator reimplementation?** Very likely.
- `IOAccelerator` itself is a tiny base class; each vendor defines its own user clients.
- The surface user client WindowServer drives is documented in the SDK
  (`IOAccelSurfaceConnect.h`, 18 selectors); traces show WindowServer using selectors 9 and
  17 for the display surfaces, numbered exactly as the header says.
- Quartz Extreme composites with plain OpenGL through the GL driver. So once our GL driver
  works, M5 needs the surface client plus whatever WindowServer's 2D plugin path requires.

## Estimate (rough; one agent working with the owner)

| Milestone | Size | Main risk |
|---|---|---|
| M2 2D kext | medium | virtio transport and IOFramebuffer details; useful on its own |
| M3 3D transport | medium | fences and interrupts |
| M4 GL bundle (takeover) | large | CGL paths that bypass the dispatch table (drawables, IOSurface textures, `CGLFlushDrawable`); building Mesa for 10.6 |
| M5 Quartz Extreme | medium to large | WindowServer's 2D plugin path and surface semantics |
| M6 Core Image, apps | medium | GL extensions Core Image expects |

## Risks found

- Apple's private paths that bypass the public dispatch table (drawable binding, IOSurface
  textures, swap) still go through `gld*` calls whose arguments are only partly decoded.
- WindowServer also used NVIDIA's 2D plugin (`GeForceGA._NVGetSurfaceInfo`): its role in
  Quartz Extreme is not yet clear.
- Tooling: modern clang/LLVM needs care for 10.6 (load commands, `__cxa_atexit`, no i386 C++
  kexts); all solved for x86_64 and recorded in `docs/decisions/0001-toolchain.md`.
- The guest panics on clean shutdown (FakeSMC plus QEMU's SMC); to fix before M2's 24-hour
  runs.

## Next step

M2: the virtio-gpu kext with a 2D framebuffer (native resolution, resizing, hardware
cursor). It's useful on its own, and it gives M4 the framebuffer that points at our
accelerator.
