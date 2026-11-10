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

## 2026-11-10: virtio-gl on the UHD 630; shutdown panic

**Host.** the owner installed `libgl1 libegl1` (Mesa 22.3.6-1+deb12u2, `iris_dri.so`). VM 210 set to
`vga: virtio-gl`: Proxmox runs `-device virtio-vga-gl -display egl-headless,gl=core`. QEMU holds
`/dev/dri/renderD128` (the UHD 630), maps `iris_dri.so` and `libvirglrenderer.so.1.7.7`, and
`info virtio-status` offers `VIRTIO_GPU_F_VIRGL` and `VIRTIO_GPU_F_EDID`. The guest boots to the
desktop as before (VESA framebuffer, 1024x768; no driver yet).

**Shutdown panic.** `osascript ... to shut down` in the guest panicked the kernel (launchd thread):
a backtrace of one repeated return address in `com.apple.driver.AppleSMC` 3.1.0d5, with
`org.netkas.FakeSMC` 2.7.2 also loaded. The install has FakeSMC while the VM also has QEMU's
`isa-applesmc`; the two SMCs are the likely cause (UNVERIFIED). Same disk and args as VMs 200/201.
Until fixed, power VM 210 off with `qm stop` after the disk is idle.

Also seen: the guest volume has only 5.79 GB free of 64 GB.

## 2026-11-10: root and SSH in VM 210

**Done** (the owner's OK; snapshot `pre-root-setup` first). Booted Chameleon with `-s -v`, ran
`fsck -fy` and `mount -uw /`, then in VM 210 only:
- `overrides.plist`: `com.openssh.sshd` Disabled = false (Remote Login on).
- `/etc/sudoers`: appended `<user> ALL=(ALL) NOPASSWD: ALL`; `visudo -c` parsed OK.
- the guest user's `~/.ssh/authorized_keys`: the RSA key from the repo's git-ignored `.secrets/`;
  MD5 fingerprint matched the local copy.
- `exit` continued to a normal boot (no reboot, so no shutdown panic).

**Result.** `scripts/guest.sh` logs in over SSH (10.6 runs OpenSSH 5.2, so the client needs
`+ssh-rsa`), and `sudo -n` works. Third-party kexts loaded: `org.netkas.FakeSMC` 2.7.2 and
`net.osrom.kext.Disabler` 1.0.1. Neither is in `/System/Library/Extensions` and `/Extra` holds
only Themes, so they most likely come from the Chameleon ISO's `/Extra` (UNVERIFIED). No panic
report was saved for the shutdown panic. Disk: GPT, `Macintosh HD` is 34.0 GB (5.4 GB free)
on the 68.7 GB disk, plus an empty `disk0s3`.

## 2026-11-10: more room in VM 210

**Why.** `Macintosh HD` had 5.8 GB free; Xcode 3.2.6, the KDK and test apps need more.
`diskutil resizeVolume disk0s2 limits` said max = current: the GPT still ended at the old
32 GiB mark (last usable LBA 67108830) and held an empty entry 3 ("Untitled", 0 bytes,
start 67108824), left over from an earlier grow of the image.

**Done** (snapshot `pre-disk-resize` first). Booted the Arch Linux ISO in VM 210 with
`nomodeset` (with KMS the Linux virtio-gpu driver takes the scanout and `screendump` says
"no surface"), then `sgdisk -e -d 3 /dev/sda`: last usable LBA 134217694, `sgdisk -v` found
no problems. Back in 10.6, `diskutil resizeVolume disk0s2 68.3G` grew the volume live, and
`diskutil verifyVolume /` says OK. RAM raised from 4 to 8 GB (`hw.memsize` 8589934592).

**Result.** 64 GiB volume, 37 GiB free. Snapshot `resized`. New scripts: `boot.sh` (Chameleon
menu, boot flags, waits for ssh) and `stop.sh` (sync, then `qm stop`).
