# M2 report: virtio-gpu kext, 2D

Date: 2026-11-10. Details: [notebook.md](notebook.md), driver docs in
[../../kext/virtiogpu/README.md](../../kext/virtiogpu/README.md).

## What works

- **Driver.** `RippleVirtioGPU.kext` (64-bit kernel, decision 0003) loads at boot, takes the
  virtio-gpu PCI device from Apple's boot framebuffer (and gives it back if virtio setup fails),
  speaks virtio 1.0 (control and cursor queues), and drives WindowServer through a 2D resource
  backed by 32 MB of contiguous guest RAM.
- **Native resolution.** The host's size from `GET_DISPLAY_INFO` is the preferred mode: 1280x800
  by default, 2560x1600 with `-global virtio-vga.xres=2560 -global virtio-vga.yres=1600`. WindowServer
  restores the user's last choice per display, as on a Mac.
- **Resolution changes.** 16 modes from 1024x768 to 2560x1600; switching through CoreGraphics (the
  Displays pane's API, `tools/setmode`) works at every size tried, and the host display follows.
- **Cursor.** Software cursor by default (Snow Leopard's own pointer, by choice); hardware
  cursor through the virtio cursor queue as an option (`RippleHardwareCursor`, `ripple_hwcursor=1`).
- **Pointer accuracy.** Fixed a 10.6 bug outside the display driver: `kext/qemutablet`, a codeless
  kext, removes IOHIDEventDriver's 15% trim of QEMU's tablet range ([known-issues](../known-issues.md)).
- **Cost.** Damage tracking (shadow frame, only the changed box sent), adaptive refresh (60 Hz
  while changing, 20 Hz when still): idle host CPU for the VM went from 96% to about 21%.
- **Clean shutdown.** The earlier shutdown panic (two SMCs) is fixed.

## Measured display lag (decision 0004: reported, not required)

Pointer move to QEMU's display surface, software cursor (`scripts/lag.sh`; figures include one
screendump of about 11 ms at 1280x800 and 35 ms at 2560x1600):

| Mode | Screen busy | After 2 s idle |
|---|---|---|
| 1280x800 | mostly 21 ms | 22-61 ms |
| 2560x1600 | 66-99 ms | 66-100 ms |

The Proxmox console (VNC) adds its own delay on top; not measured.

## Not done, and risks

- **Stability run**: deferred by decision 0006 (overnight, when decided).
- **Interrupts**: the driver still polls for command completion (device interrupts suppressed).
  Fine for 2D; M3's 3D transport needs real interrupts and fences, so they start M3.
- **One unexplained panic** right after the first switch to 2560x1600: "TLB invalidation IPI
  timeout", CPU 0 stuck for over 250 ms in Apple's USB EHCI driver, most likely in a slow QEMU
  exit. Not reproduced; QEMU's main loop never stalled more than 14 ms in the repeat.
- **Panics are invisible** with our driver: the refresh timer stops, so the panic text never
  reaches the host. It can be read with lldb (`panicstr`, `debug_buf`); a fix is open.
- **System Preferences** itself hasn't been clicked through by hand yet (same API as `setmode`).

## Recommended next step

Start M3: interrupts and fences first, then the 3D commands (capsets, contexts, resources,
SUBMIT_3D) behind a GPU-neutral user-client API (decision 0005), checked by a command-line tool
that clears a buffer through virgl and reads it back.
