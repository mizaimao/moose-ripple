# moose-ripple

<!-- BANNER (to add, made separately): media/banner.png
     Donkey, the plush moose, as a flat cartoon with ripples, on an original purple aurora background:
![moose-ripple](media/banner.png)
-->

**Quartz Extreme and Core Image for Mac OS X 10.6 Snow Leopard in a virtual machine, on whatever
GPU your host has.**

Drop a widget onto the Dashboard and the water ripples. On a 2009 Mac that effect was the
handshake between Snow Leopard's window server and a real graphics card. In a virtual machine it
never happens: Snow Leopard has no driver for any virtual GPU, so the window server falls back to
drawing everything with the CPU. No ripple, no translucent menu bar, no Core Image effects, and
anything that insists on Quartz Extreme refuses to start.

moose-ripple brings it back. It's a guest driver for QEMU's standard **virtio-gpu** card with
**virgl**, the paravirtual 3D path that hands OpenGL to the host. Snow Leopard sees a graphics
card, OpenGL renders on the host GPU (Intel, AMD or NVIDIA, whatever the host drives), and
WindowServer composites the desktop on it. No period hardware, no GPU passthrough, no
vendor-specific host setup.

<!-- PREVIEW (to add): media/gears-menu-bar.png
     gl-gears running with the translucent menu bar in the same shot:
![gl-gears on the host GPU under Snow Leopard's translucent menu bar](media/gears-menu-bar.png)
-->

<!-- PREVIEW (to add): media/dashboard-ripple.png (still frame) and/or media/dashboard-ripple.gif (after the performance work)
![The Dashboard ripple, rendered through virgl on the host GPU](media/dashboard-ripple.png)
![The Dashboard ripple in motion](media/dashboard-ripple.gif)
-->

## Status

| Milestone | What it gives you | State |
|---|---|---|
| M0 | Build, deploy and kernel-debug loop from a Linux build host | done |
| M1 | Reverse-engineering spike: Apple's OpenGL plugin and accelerator interfaces | done |
| M2 | Display driver: virtio-gpu framebuffer, any resolution, hardware cursor | done |
| M3 | 3D transport: virgl contexts, resources and command submission from the guest | done |
| M4 | OpenGL 2.1 on the host GPU (Mesa's virgl driver, ported to Snow Leopard) | done |
| M5 | Quartz Extreme: WindowServer composites on the GPU (Exposé, Dashboard ripple, translucent menu bar) | done |
| M6 | Core Image and real apps (Quartz Composer, Photo Booth effects, iMovie, Front Row) | next |

Performance work is under way. Reports for each milestone are in [docs/re/](docs/re/), and
[docs/known-issues.md](docs/known-issues.md) lists what still trips it up.

## How it works

```
 Guest: Mac OS X 10.6.8
 ┌───────────────────────────────────────────────────────────────┐
 │ Apps, WindowServer, Core Image, Core Animation                │
 │      │ OpenGL / CGL                       │ 2D blits          │
 │ RippleGLDriver.bundle                RippleGA.plugin          │
 │  Mesa virgl driver + a CGL front end  (WindowServer's         │
 │      │ IOKit user client               graphics accelerator)  │
 ├──────┼────────────────────────────────────────────────────────┤
 │ RippleVirtioGPU.kext                                          │
 │  virtio transport, framebuffer and cursor, accelerator        │
 │  surfaces, virgl contexts, resources and fences               │
 └──────┬────────────────────────────────────────────────────────┘
        │ virtio-gpu (virgl)
 Host:  QEMU (virtio-vga-gl) ─► virglrenderer ─► host OpenGL ─► any GPU
```

- **RippleVirtioGPU.kext** drives the virtio-gpu card: display modes, scanout, the hardware
  cursor, and the 3D commands the GL driver sends. To Snow Leopard it looks like a graphics
  card with an accelerator.
- **RippleGLDriver.bundle** is an OpenGL renderer plugin, loaded by Apple's OpenGL framework
  like any vendor driver. Inside is Mesa's virgl Gallium driver, built for 10.6.
- **RippleGA.plugin** gives WindowServer the 2D blitter it asks for before it will composite
  the screen with OpenGL.
- **RippleQEMUTablet.kext** (optional) makes QEMU's USB tablet work as an absolute pointer.

The boundary between the GL bundle and the kernel is small, versioned and GPU-neutral:
[docs/kext-uapi.md](docs/kext-uapi.md). The design and the plan are in
[HANDOFF.md](HANDOFF.md), the decisions in [docs/decisions/](docs/decisions/).

## Requirements

- **Host:** Linux on x86_64 with KVM, QEMU with virgl (`virtio-vga-gl`), virglrenderer, OVMF
  (UEFI firmware), and a GPU with an OpenGL driver. Tested on Proxmox VE 8.4 (QEMU 9.2,
  virglrenderer 0.10.4) with an Intel UHD 630. Apple-silicon hosts through QEMU + virgl on
  macOS are a later target.
- **Guest:** Mac OS X 10.6.8 with the 64-bit kernel, booted by OpenCore.
- **Build host:** Linux with clang, cctools-port's ld64 and the Mac OS X 10.6 SDK from Xcode
  3.2.6 ([decision 0001](docs/decisions/0001-toolchain.md)).
- **Your own copy of Mac OS X.** This repository contains no Apple software, firmware or keys.
  Mac OS X 10.6, Xcode and the SDK must come from your own legal copies, and running Mac OS X
  in a virtual machine is subject to Apple's license terms.

## Quick start

1. **Boot Snow Leopard with OpenCore.** [boot/opencore/](boot/opencore/README.md) builds a
   small boot disk with OpenCore and documents the VM settings: OVMF, QEMU's Apple SMC device
   (you supply the SMC key), `vmware-cpuid-freq=off`, and PCI memory below 4 GB
   (`npci=0x2000`). The same disk boots OS X 10.7 and 10.8 too.
2. **Give the VM the virgl card:** `-device virtio-vga-gl` with a GL display in QEMU, or
   `vga: virtio-gl` on Proxmox.
3. **Build** on the Linux build host: `make virtiogpu glbundle gaplugin qemutablet`. Output
   goes to `build/`. `glbundle` cross-builds Mesa for 10.6 first (meson and ninja; see
   [gldriver/mesa/](gldriver/mesa/README.md)).
4. **Install into the guest** and switch it on:
   ```
   scripts/install-kext.sh RippleVirtioGPU
   scripts/install-kext.sh RippleQEMUTablet         # optional: absolute pointer
   scripts/install-glbundle.sh                      # RippleGLDriver.bundle and RippleGA.plugin
   scripts/bootargs.sh '-v npci=0x2000 ripple_gl=1 ripple_qe=1'
   ```
   then reboot. The scripts drive a Proxmox host and the guest over ssh; their settings
   (host, VM ID, guest user and key) are at the top of [scripts/lib.sh](scripts/lib.sh). On
   plain QEMU, copy the bundles into `/System/Library/Extensions` and set the boot flags with
   `sudo nvram boot-args=...` by hand.
5. **Check it:** `scripts/qe-check.sh` runs [tools/qe-check](tools/qe-check/qe-check.c) in
   the guest and says whether each display has Quartz Extreme
   ([docs/re/qe-verification.md](docs/re/qe-verification.md)). Then press F12 and drop a
   widget.

## Credits

moose-ripple stands on:

- [Mesa](https://mesa3d.org) and its virgl Gallium driver (MIT), the GL implementation inside
  RippleGLDriver.bundle.
- [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) (MIT), which runs the
  guest's GL on the host. Its protocol headers are in [third_party/virgl](third_party/virgl).
- [QEMU](https://www.qemu.org) and its virtio-gpu device. The virtio headers from Linux are in
  [third_party/virtio](third_party/virtio), with their own licenses.
- [OpenCore](https://github.com/acidanthera/OpenCorePkg) (BSD-3-Clause), the boot loader.
- [cctools-port](https://github.com/tpoechtrager/cctools-port), the Mach-O linker that lets a
  Linux box build 10.6 kexts.

Prior art, studied but no code used: VMsvga2 (VMware SVGA driver for OS X guests, by Zenith432)
and Virtual Intel Mac (OpenGL forwarding for Intel macOS guests on Apple-silicon hosts). Notes:
[docs/re/prior-art.md](docs/re/prior-art.md).

## License

MIT, see [LICENSE](LICENSE). Files in `third_party/` keep their own licenses.
