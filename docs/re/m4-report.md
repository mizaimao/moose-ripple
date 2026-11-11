# M4 report: GL renderer bundle

Date: 2026-11-10. Details: [gl-plugin-interface.md](gl-plugin-interface.md), decisions
[0003](../decisions/0003-architecture.md) and [0009](../decisions/0009-mesa-for-10.6.md).

## Accepted

The handoff's test: a CGL/NSOpenGLView app renders through the virgl path, `GL_RENDERER`
identifies our driver, it works in 32- and 64-bit processes, and OpenGL 2.1 is reported.

`tools/gl-gears` (our own spinning-gears demo in an NSOpenGLView, accelerated, no recovery,
double-buffered) on VM 210:
- `GL_RENDERER virgl (Mesa Intel(R) UHD Graphics 630 (CFL GT2))`, `GL_VERSION 2.1 Mesa
  24.3.4`, GLSL 1.20.
- Frames appear in the window, 64-bit ([screenshot](img/m4-gl-gears-64bit.png)) and 32-bit,
  including a live resize from 400x400 to 640x480 ([screenshot](img/m4-gl-gears-32bit-resized.png)).
- 45-59 frames per second at 400x400 and 640x480.
- `tools/cgl-info` (FBO clear and readback) matches exactly: 64 128 191 255.

## How it works

- **`RippleGLDriver.bundle`** in `/System/Library/Extensions`: Mesa 24.3.4 (GL state tracker,
  GLSL compiler, NIR) with its virgl Gallium driver, cross-built for 10.6 on dev.lan.
  Universal (i386 + x86_64), 31 MB, it needs only OpenGL, IOKit, libSystem and libstdc++.
- **Our winsys** gives Mesa's virgl driver the GPU through our kext's user client (M3's API,
  with limits raised for Mesa: 65,536 buffers, 4,096 handles and 320 KB per submit).
- **The 80 `gld*` entry points** answer Apple's OpenGL as its plugin interface expects
  (renderer info, pixel formats, contexts, drawables), as read from 10.6.8's binaries.
- **Dispatch takeover:** when a context is complete, OpenGL's own (undocumented)
  `CGLSelectDispatch` installs our 764-entry table, so every GL call goes to Mesa. GLEngine's
  later rewrites land in the back table and can't undo it. Confirmed by `tools/takeover-probe`.
- **Windows:** like Apple's software renderer, a private window-server window is bound to the
  app's surface. Each frame is read back from the GPU, copied into its locked bits, and the
  surface is flushed.
- **Opt-in for now.** The accelerator names the bundle only with the boot flag `ripple_gl=1`.
  Even then only processes started with `RIPPLE_GL=1` see the renderer. WindowServer, the Dock
  and all other apps keep Apple's software path until M5.

## Limits and risks

- **Frames are copied:** GPU to guest memory through virtio, then into the window. That's
  fine at window sizes like these; zero-copy presentation belongs with Quartz Extreme (M5).
- **Not yet supported:** off-screen memory drawables (`CGLSetOffScreen`), pbuffers, IOSurface
  drawables, full screen, multisampling, stereo, float colour, accumulation buffers. 22 rare
  Apple and ATI extension entry points are logged no-ops (Mesa doesn't advertise them).
- **Build-specific offsets:** the takeover uses offsets in Apple's private structures of
  10.6.8 (10K549). Every step checks Apple's magic values first. If a check fails, the context
  stays with GLEngine and renders nothing.
- **Static reading:** the plugin interface was read from disassembly. Paths beyond these tests
  (shared contexts across share groups, many contexts, multithreaded engine with real apps) are
  not exercised yet.
- **Guest boot flag `slto_us=4000000`:** live snapshots could panic the guest
  ([known-issues.md](../known-issues.md)); VM 210 carries the fix.

## Next

M5 (Quartz Extreme): offer the renderer to WindowServer and all apps, which needs the
IOAccelerator surface support WindowServer composites with (and gives zero-copy presentation).
