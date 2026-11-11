# M5 working notes: Quartz Extreme on virgl

Date: 2026-11-10 (evening). Status: QE on and the desktop GL-composited on VM 210; Exposé and
Dashboard still show placement errors. Plan: [m5-plan.md](m5-plan.md); research:
[qe-decision.md](qe-decision.md) (R1), [qe-windowserver-trace.md](qe-windowserver-trace.md)
(R2), [qe-verification.md](qe-verification.md) (R3), [qe-display-surface.md](qe-display-surface.md)
(R4); design notes from VMsvga2: [vmsvga2-qe-design.md](vmsvga2-qe-design.md).

## How to run it (VM 210)

- Boot flags: `-v npci=0x2000 ripple_gl=1 ripple_qe=2 ripple_hwcursor=1 slto_us=4000000`
  (`ripple_qe=2` also logs every surface call to the kernel log; `ripple_qe=1` without).
- Installed: `RippleVirtioGPU.kext` (`scripts/install-kext.sh`), `RippleGLDriver.bundle` and
  `RippleGA.plugin` (`scripts/install-glbundle.sh`; checks: compare `md5` of the installed and
  built binary, a half-copied bundle crashed WindowServer once).
- Logs: the directory `/Library/Logs/RippleGL` (world-writable) turns on the bundle's log per
  process; the file `/Library/Logs/RippleGL/trace` (present when a process starts) adds a GL
  call trace. Checks: `scripts/qe-check.sh`, `build/grab` (prints
  `CGDisplayUsesOpenGLAcceleration`), `VMID=210 scripts/screen-vnc.sh` (the real display).

## What it took (in order)

1. Accelerator properties `AccelCaps` = 3 and `IOAccelRevision` = 2: WindowServer then loads
   our GL bundle and turns QE on (`CGDisplayUsesOpenGLAcceleration` = 1).
2. The surface user client (type 0) for WindowServer's surfaces.
3. Taking over CoreGraphics' own GL contexts ("cgls", not CGL): GLEngine gets a writable
   shadow back table at gc+0x6558, our table goes in front, the first word stays GLEngine's
   context (cgls passes it to GLEngine) and the thunks map it to ours.
4. Render-op slots 27-29 filled (GLEngine calls flush/swap there).
5. `RippleGA.plugin` (copy and fill blitters): without a GA plugin on the framebuffer,
   WindowServer never composites the display itself with GL (R4).
6. The display surface (id 1) is shaped to each frame's dirty rectangle; WindowServer renders
   that rectangle with `glViewport(0, 0, w, h)` and `glOrtho(x, x+w, y+h, y)`. The frontend
   keeps one persistent screen texture on the GPU, copies each presented rectangle into place
   and shows it with `DISPLAY`. One drawable per surface for the whole process (persistent).
7. Apple hint enums (`GL_TEXTURE_STORAGE_HINT_APPLE`, `GL_TRANSFORM_HINT_APPLE`) accepted as
   no-ops. `GL_UNPACK_CLIENT_STORAGE_APPLE` must keep failing until real client storage
   exists: accepting it made WindowServer reuse texture memory (wrong contents everywhere).

## Open problems

- **Large rectangles in Exposé:** some frames' content belongs to the screen's top-left
  instead of the rectangle (screenshot in the session: a 500x278 rectangle at 390,305 showing
  the dimmed menu bar). Either some frames are drawn in full-screen coordinates while the
  surface is shaped to a sub-rectangle (the rectangle then clips rather than translates), or
  a frame is presented with another frame's rectangle. Next: log viewport/ortho per present
  next to the shape, and handle a full-screen viewport by copying the rectangle from its
  screen position instead of the drawable's origin.
- Small leftovers on the desktop: a "Finder" menu-title box at a button-sized rectangle, a black
  box under the clock, a faint bar next to the dialog.
- Dashboard: no dimming layer, stale widget copies; the ripple (Dock + Core Image) left a
  732x400 viewport (R3 run, before the per-surface drawables).
- WindowServer crashed in `_mesa_init_remap_table` with a half-installed bundle (fixed by
  reinstalling); keep the md5 check.
- Performance not measured; per-frame cost is one GPU copy plus a `DISPLAY` flush.
