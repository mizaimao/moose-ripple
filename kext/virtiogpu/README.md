# RippleVirtioGPU.kext

Display driver for QEMU's virtio-gpu (PCI 1af4:1050) on Mac OS X 10.6.8, 64-bit kernel.

- `VirtioGPUDevice`: virtio 1.0 PCI transport, control and cursor queues, 2D commands.
- `RippleFramebuffer`: the `IOFramebuffer`. It outranks Apple's boot framebuffer on the same
  device (`IOProbeScore` 30000 vs 20000) and falls back to it if virtio setup fails.

Install it in `/System/Library/Extensions` (it has to load at boot): `scripts/install-kext.sh
RippleVirtioGPU` does that on the dev VM.

## Settings

| Setting | Where | Default | Meaning |
|---|---|---|---|
| `RippleHardwareCursor` | `Info.plist` personality (rebuild the kext cache after editing) | `false` | `false`: the guest draws Snow Leopard's pointer into the picture, so it looks exactly as on a real Mac and scales with the screen in VNC consoles; moves with the screen updates (about 16 ms when active, up to 50 ms on the first move after idle). `true`: hardware cursor through virtio's cursor queue; QEMU's own window draws it at the guest's scale with no lag, while VNC clients (the Proxmox console) draw it themselves at the client's pointer size. |
| `ripple_hwcursor=0` / `=1` | boot flag (Chameleon prompt or `Kernel Flags`) | not set | Overrides `RippleHardwareCursor` for one boot. |
| `RippleGLBundle` | `Info.plist`, `RippleAccelerator` personality | `false` | `true`: the accelerator names `RippleGLDriver` as its OpenGL driver (`IOGLBundleName`) and the framebuffer points at it (`IOAccelTypes`), so OpenGL loads `/System/Library/Extensions/RippleGLDriver.bundle`. Off until GL output reaches windows (M4/M5), because WindowServer uses OpenGL too. The registry property `RippleGL` shows `on` or `off`. |
| `ripple_gl=0` / `=1` | boot flag (`scripts/bootargs.sh`) | not set | Overrides `RippleGLBundle` for one boot. |

The current choice is shown as the registry property `RippleCursor` (`software` or `hardware`):
`ioreg -c RippleFramebuffer -l | grep RippleCursor`.

Refresh statistics: `RippleRefreshFramesSent`, `RippleRefreshPixelsSent`,
`RippleRefreshAverageMicroseconds` (over the last 600 refresh ticks).
