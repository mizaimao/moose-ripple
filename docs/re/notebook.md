# Lab notebook

Dated entries: hypothesis, what was done, result, next step (HANDOFF.md rule 4).

## 2026-11-10: dev VM on the retro rig, first boot on virtio-vga

**Hypothesis.** A copy of the owner's Snow Leopard disk boots in QEMU without the 9600 GT, on
`virtio-vga` (no GL yet), using the boot loader's VESA framebuffer.

**Host state.**
- Retro rig: PVE 8.4.0, running kernel 6.5.11-8-pve (pinned by the owner; never change it),
  pve-qemu-kvm 9.2.0-5, libvirglrenderer1 0.10.4-1, libepoxy0 1.5.10-1.
- UHD 630 switched on in the BIOS by the owner today: `00:02.0 [8086:3e98]`, driver i915,
  `/dev/dri/card0` and `renderD128`.
- Mesa's GL/EGL libraries are not installed (only libgbm1), so `vga: virtio-gl` can't run
  yet. `apt-get -s install libgl1 libegl1` adds 28 Debian packages and updates libgbm1
  22.3.6-1+deb12u1 to +deb12u2; no Proxmox package changes. Waiting on the owner's OK.

**Done.**
- VM 210 `ripple-dev`: `qm disk import` of `local:200/slhdd.raw` into
  `local:210/vm-210-disk-0.qcow2` (qcow2, so Proxmox snapshots work). Snapshot `baseline` is the
  untouched copy. VMs 200/201 are unchanged.
- Same machine settings as VM 201 (q35 7.2, SeaBIOS, Penryn args, Chameleon 2.1 r2069 ISO),
  minus the 9600 GT and the host USB devices; 4 GB RAM, new e1000 MAC, `vga: virtio`.
- Added `-device usb-kbd,bus=ehci.0,port=4`: 10.6 has no PS/2 driver, so QEMU's default
  keyboard never reached the guest. The USB keyboard works; keys are sent in chunks of 20
  because bursts drop keys (and once left Shift stuck).

**Result.**
- Chameleon draws at 1024x768 through virtio-vga's VGA BIOS. With `-v`, 10.6 boots to the
  desktop in about 25 s and logs in as the guest user automatically.
- Kernel: `Darwin 10.8.0 xnu-1504.15.3~1/RELEASE_X86_64 x86_64`, so this setup boots the
  64-bit kernel by default.
- System Profiler, Graphics/Displays: Type GPU, Bus PCI, VRAM 3 MB, Vendor ID 0x1af4,
  Device ID 0x1050, Revision 0x0001, "No Kext Loaded", 1024 x 768, 32-bit ARGB8888.
  This confirms the modern virtio-gpu PCI ID from the handoff on this QEMU.
- Guest IP 192.0.2.10 (DHCP). No `/Developer`, so no Xcode in this install.
  Remote Login is off; `sudo` asks for a password.

**Next.** Remote Login and root access for the scripts (needs the owner), then Mesa on the host
(needs the owner) and switch VM 210 to `vga: virtio-gl`.
