# 0003: Mesa takes over the GL dispatch table; 64-bit kernel only

Date: 2026-11-10. Status: approved ("Sounds brilliant. Move on"), after the M1 report.

## Decisions

1. **GL architecture.** Our GL driver bundle implements the `gld*` management calls Apple's
   OpenGL needs (renderer info, pixel formats, context and drawable lifecycle, flush) and points
   each CGL context's public dispatch table (`rend` + `disp`, SDK `CGLContext.h` and
   `gliDispatch.h`) at Mesa. Mesa's Gallium virgl driver renders through our kext's user client.
   We don't implement a "native" driver under GLEngine, which would need GLEngine's private
   state structures.
2. **64-bit kernel only.** Our kexts are x86_64; the guest boots the 64-bit kernel
   (`arch=x86_64` if needed). The GL bundle and user-space tools stay universal (i386 + x86_64)
   so 32-bit apps keep working. Supporting the 32-bit kernel later would need the kexts built
   with Xcode 3.2.6's gcc-4.2.

## Why

See [../re/m1-report.md](../re/m1-report.md). In short: the dispatch table is documented and
libGL calls through it for every GL function; clang can't build i386 C++ kexts.

## Changes to the handoff's plan

- HANDOFF.md section 3 calls for universal i386 + x86_64 kexts: now x86_64 only.
- M4's "port Mesa's virgl Gallium driver with a macOS winsys" stays; the GL plugin glue is the
  takeover design above.
