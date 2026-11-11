# M5 plan: Quartz Extreme

Date: 2026-11-10. Go-ahead: 2026-11-10. Handoff acceptance: WindowServer
composites with the GPU, Exposé and Dashboard ripple work, no cursor tearing, apps that
require QE launch, and how QE status was verified is written down.

## What we know (M1 traces on the NVIDIA VM, [ioaccelerator-interface.md](ioaccelerator-interface.md))

- With QE, WindowServer composites with plain OpenGL through the GL driver bundle. Window
  contents become textures, and it re-attaches its GL context to the display surface every frame.
- It keeps display ("framebuffer") surfaces through the standard surface user client (type
  0, `IOAccelSurfaceConnect.h`): selectors 9 (`SetShape`) and 17 (`SetShapeBackingAndLength`).
- It also holds 2D contexts through the vendor's GA CFPlugIn (`IOCFPlugInTypes`).
- The GL bundle gets two callbacks from OpenGL (`gldInitializeLibrary`'s `ioFlush` and
  `ioBindSurface`). Hardware drivers use them to bind CGS surfaces to IOAccel surfaces
  (`CGSBindSurface` kind 2).

## Open questions

1. **QE decision:** what makes WindowServer turn QE on for a display (renderer properties,
   VRAM, extensions, a GA plugin, IOAccelerator properties)?
2. **WindowServer's calls under QE:** its exact GL driver calls (drawable kinds, `ioBindSurface`
   and `ioFlush` use), the surface client selectors with arguments, and what the GA plugin does.
3. **Getting composited frames to the screen:** read back into the 2D framebuffer, or scan out
   a virgl resource directly (zero copy), and how that coexists with the 2D framebuffer and cursor.
4. **Verification:** `CGDisplayUsesOpenGLAcceleration()`, System Profiler, Exposé and Dashboard
   ripple, apps that refuse to run without QE.

## Work split

**A second agent (parallel; its own new files, no VM 210):**
- **R1, static:** the QE decision in CoreGraphics and WindowServer (question 1), from the
  binaries mirrored on dev.lan. Report in `docs/re/qe-decision.md`.
- **R2, dynamic, on the 9600 GT VM (card windows while 210 is off):** trace WindowServer at
  login and during Exposé and Dashboard (question 2): `IOServiceOpen` types,
  `IOConnectCallMethod` selectors and arguments per client, GL driver calls, GA plugin calls.
  Raw traces in `docs/re/traces/`, summary in `docs/re/qe-windowserver-trace.md`.
- **R3, tooling:** `tools/qe-check` (QE on or off for each display, with a way to trigger and
  screenshot Exposé and Dashboard ripple from ssh), checked on the NVIDIA VM (QE on) and VM 210
  (QE off). Document the method in `docs/re/qe-verification.md`.
  Also a way to capture what VM 210's display really shows (the virtio scanout): QEMU's
  screendump says "no surface" with `virtio-gl` once the guest scans out.

**Driver work (VM 210, kext and GL bundle):**
- **K1:** the surface user client (type 0) in the kext, following the SDK header; surfaces backed by
  GPU buffers.
- **K2:** a GPU-neutral "show this buffer on the display" kext method (virtio `SET_SCANOUT` on a
  3D resource), prototyped with a test tool (question 3).
- **G1:** GL bundle: display and surface drawables, `ioBindSurface`/`ioFlush`, and offering the
  renderer to WindowServer behind a boot flag.
- **G2:** whatever R1 and R2 show WindowServer needs (GA plugin, properties).
- **V:** acceptance with R3's tools.
