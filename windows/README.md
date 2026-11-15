# Windows 7 on the host GPU

Windows 7's desktop, Aero and Direct3D drawn by the host's graphics card in a QEMU virtual
machine, through virtio-gpu and virglrenderer. Work in progress, not a release: the driver is
test-signed, so Windows has to run in test mode.

- `kmd/`: the WDDM 1.1 kernel driver for QEMU's virtio-gpu.
- `mesa/`: the user-mode driver, Mesa 24.3.4 built for Windows 7 with its Direct3D 10 and
  Direct3D 9 parts on Mesa's virgl driver, as patches and added files.
- `package/`: the INF that installs both.
- `umd9/`: the first, capability-only Direct3D 9 driver, kept for reference.

How it was built and what was found on the way: `docs/windows/` (start with
`kmd-port-notes.md` and `mesa-win7.md`). How the test machine is set up: `docs/windows/test-vms.md`.

## Credits

This part builds on code from these projects, each file keeping its notice:

- [virtio-win](https://github.com/virtio-win/kvm-guest-drivers-windows) (BSD-3-Clause, Red Hat):
  the kernel driver started as its `viogpu3d` driver for Windows 10, with the Mesa side of its
  pull request 943.
- [Mesa](https://mesa3d.org) (MIT): the Direct3D 10 driver, the virgl driver and the shader
  translator of its former Direct3D 9 state tracker "nine" (Joakim Sindholt, Christoph Bumiller).
- [Yttrium's Mesa fork](https://github.com/arehnman/virtio-win-mesa) (MIT, Ake Rehnman): the
  Direct3D 9 driver files (`mesa/overlay-virgl/src/gallium/frontends/d3d10umd/D3D9*`).
- [vmlab-win3d](https://github.com/VMLabDev/vmlab-win3d) (MIT, VMLabDev and contributors): its
  Direct3D 9 patches for DirectDraw, DirectX 7-era drawing and other fixes, merged into those
  files.
- [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) and
  [QEMU](https://www.qemu.org) on the host, unchanged.

Read but not used (their licences do not fit an MIT project): Yttrium's MPL-2.0 kernel driver
and renderer, vmlab-win3d's kernel driver patches, Wine, ReactOS, VirtualBox, qemu-3dfx.
