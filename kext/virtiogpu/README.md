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
| `ripple_async=1` | boot flag | not set | Experimental. A program's 3D submits and the copies of its frames to the screen are queued for the host and not waited for, so the program draws its next frame while the host runs the last one (never more than one frame ahead). Faster where the host's share of a frame is large, but timer-driven programs and WindowServer's animations then stall more often (`docs/re/perf-notes.md`), so it is off unless asked for. |

The current choice is shown as the registry property `RippleCursor` (`software` or `hardware`):
`ioreg -c RippleFramebuffer -l | grep RippleCursor`.

Refresh statistics: `RippleRefreshFramesSent`, `RippleRefreshPixelsSent`,
`RippleRefreshAverageMicroseconds` (over the last 600 refresh ticks).

## Gamma table

Mac OS X hands the display a gamma table (`IOFramebuffer::setGammaTable`; on 10.6.8 three
channels of 256 sixteen-bit entries) and uses it for more than calibration: `CGDisplayFade`
and the system's own fades (login and logout, display sleep, resolution changes, a program
capturing the display) are ramps of that table.

virtio-gpu has no gamma control, so the driver applies the table itself (`RippleGamma`). While
the table is the identity, nothing changes. While it is anything else, the screen is a second
image: what would be on screen (the framebuffer's picture, or the GPU buffer a client is
showing), drawn through the table by a small virgl context of the driver's own, and drawn
again whenever that picture or the table changes. It needs a host with virgl; on a 2D-only
host `setGammaTable` answers "unsupported", as before.

`ioreg -c RippleFramebuffer -l | grep RippleGammaTable` says `identity` or `applied`. After a
normal boot it is `identity`.

`tools/gamma-test` (`make gammatest`) sets a dimmed table for a while (`gamma-test hold 50 5`)
or runs a fade to black and back (`gamma-test fade 1`). A screenshot taken in the guest never
contains the table; look at the host's display (`scripts/screen-vnc.sh`, with `-n` and `-i`
for a burst of frames).

Checked on the Intel host (VM 210, 1280x800), with and without the GL driver: a table at half
brightness halves every sampled colour value exactly and leaves the picture in place, and a
one-second fade steps down to black and back up in frames taken every 0.1 s. In UTM on Apple
silicon (a trial session, the disk's changes discarded) the host accepted every command and
its display client logged about 27 draws a second through such a fade; what the screen itself
shows there has not been captured. The hardware pointer is drawn by the host and is not
dimmed.
