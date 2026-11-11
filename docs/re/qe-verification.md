# Checking Quartz Extreme from ssh (M5 R3)

Date: 2026-11-10. How to tell whether QE is on in a 10.6.8 VM, and how to see
Exposé and Dashboard, without anyone at the screen. What WindowServer checks:
[qe-decision.md](qe-decision.md) and [qe-display-surface.md](qe-display-surface.md).

## One command

```
scripts/build.sh qecheck                       # once: build/qe-check (x86_64)
VMID=210 scripts/qe-check.sh                   # report; exit 0 only if every display has QE
VMID=210 scripts/qe-check.sh -x /tmp           # plus /tmp/qe-expose.png and /tmp/qe-dashboard.png
CAPTURE=guest VMID=211 scripts/qe-check.sh -x  # passed-through GPU: capture inside the guest
```

It prints three things.

1. **`tools/qe-check`, per active display:** QE on or off (`CGDisplayUsesOpenGLAcceleration`,
   which returns WindowServer's own per-display flag), mode and pixel encoding, and the GL display
   mask. Then what WindowServer's decision reads: the framebuffer's `IOAccelIndex`, the
   accelerator from `IOAccelTypes` with its `AccelCaps` and `IOGLBundleName`, the first
   `IOPCIDevice`/`IOAGPDevice` ancestor (the RequiredClasses test), and the GL renderers for the
   display mask (ID, accelerated, VRAM).
2. **WindowServer's own lines** from `/var/log/windowserver.log`, for the running WindowServer
   only (its pid in brackets; the file keeps earlier runs, and `windowserver_last.log` has the
   previous file):
   `GLCompositor: GL renderer id ..., GL mask ..., accelerator ..., unit ..., caps ..., vram ... MB`
   and `... texture units ..., texture max ..., viewport max {...} extensions 0x...`.
   WindowServer writes them whenever it creates the GL compositor, so no line means no QE.
3. With `-x`: F9 (Exposé, all windows) and F12 (Dashboard) through `qm sendkey`, a picture of
   each after 2 s, then the same key again to close it.

## Results so far

| VM | Display | qe-check | GLCompositor lines |
|---|---|---|---|
| 212, 9600 GT boots (R2, 2026-11-10 18:22 to 19:28) | NVIDIA | (tool not yet run there) | renderer 0x0102260b, mask 0x3, caps 0x3, vram 1024 MB; texture units 8, texture max 8192, viewport {8192, 8192}, extensions 0xf |
| 212, virtio (OVMF), 2026-11-10 19:52 | `RippleFramebuffer`, no accelerator (no `ripple_gl`) | QE off, renderer 0x01020400 (software), exit 1 | none |
| 210, `ripple_qe=2`, RippleGA.plugin, 2026-11-10 20:08 | `RippleFramebuffer` 1280x800 | QE on, `RippleAccelerator` `AccelCaps` 3, `RippleGLDriver`, `IOPCIDevice` S08, renderer 0x01025000 (accelerated, 512 MB), exit 0 | renderer 0x01025000, mask 0x1, caps 0x3, vram 512 MB; texture units 8, texture max 16384, viewport {16384, 16384}, extensions 0x7 (no `GL_APPLE_float_pixels`) |

On 212 (virtio), F9 and F12 through `qm sendkey` opened Exposé and Dashboard, and
`scripts/screen-vnc.sh` captured both at 2560x1600. Exposé works without QE on 10.6, so a
picture of it doesn't prove QE. Compare the two VMs instead, or use the Dashboard ripple.

## Dashboard ripple

The ripple shows only when a widget is dropped onto the Dashboard: F12, click "+" (bottom left,
about x 27, y 1572 on a 2560x1600 screen), then click a widget in the bar. Clicks need an
absolute pointer the guest can use: QMP `input-send-event` (abs, 0..32767; `scripts/lag.sh`
shows how) together with `RippleQEMUTablet` on VM 210. On 212, which has no tablet
driver, the click test didn't move the pointer, and the guest hung soon after (3 vCPUs
spinning, no ping; stopped with `qm stop`). The cause isn't known; don't repeat the test on a VM
without the tablet driver.

On 210 (with the tablet driver) it worked with `scripts/qmp-input.sh` (one QMP session per call: `send-key` and
`input-send-event`, nothing else on the socket): F12, click "+" at abs (358, 32193), then click
a widget in the bar (Dictionary at abs (12467, 30105) on 1280x800). To catch the ripple, start
`scripts/screen-vnc.sh` about 1 s before the widget click, because the VNC connection takes
about 1.5 s to set up. 2026-11-10 20:09 on 210, with the GL compositor running through
RippleGA.plugin:
- During the ripple the whole screen was black apart from a white 732x400 rectangle at the bottom
  left holding the new widget ([frame](img/m5-r3-dashboard-ripple-frame.png)).
- Afterwards only that 732x400 region kept updating and the rest stayed black
  ([after](img/m5-r3-after-ripple.png)), until Dashboard was closed. Closing it brought the
  desktop back.
- The Dashboard had no dimming layer, and copies of widgets from the fly-in stayed on screen.

The Dictionary widget is still on 210's Dashboard from this test.

## Pitfalls

- `screencapture` writes nothing over ssh on VM 210 (QEMU's `screendump` says "no surface" once
  virtio-gl scans out). Use `scripts/screen-vnc.sh` there for what the display really shows;
  `tools/grab` (`CGWindowListCreateImage`) works over ssh but builds its picture from windows.
- `qm sendkey` and the QMP socket: while another QMP client holds `/var/run/qemu-server/VMID.qmp`
  (a `socat` session), `qm sendkey` times out. Send keys through the same session
  (`{"execute":"send-key",...}`) or close it first.
- `/var/log/windowserver.log` is mode 640 root:admin; the guest user must be an admin (the guest user is).
