# 0002: Dev VM setup (VM 210 on the retro rig)

Date: 2026-11-10. Status: in use.

- **Disk:** a qcow2 copy of the owner's Snow Leopard install (VM 200's `slhdd.raw`), so Proxmox
  snapshots work and VMs 200/201 stay untouched. Volume grown to 64 GiB.
- **Machine:** the same as VM 201, which already boots this install: `pc-q35-7.2`, SeaBIOS,
  Penryn CPU flags in `args` (QEMU's `isa-applesmc` removed, see below; the OSK never goes in
  the repo), e1000 NIC. 8 GB RAM, 4 cores. No GPU passthrough. `vga: virtio-gl`.
- **Bootloader: OpenCore 1.0.8 on OVMF since 2026-11-10** (decision 0007, built from
  `boot/opencore/`; boot flags in NVRAM via `scripts/bootargs.sh`, keep `npci=0x2000`). Snapshot
  `pre-opencore` holds the Chameleon setup. Before that: **Chameleon 2.1 r2069** (the ISO VM 201 uses). It boots this install as is,
  takes boot flags at its prompt (`scripts/boot.sh` types them), and draws through
  virtio-vga's VGA BIOS. Not tried: iBoot (VM 200) and OpenCore (an image is on the rig).
  FakeSMC 2.7.2 and Disabler 1.0.1 come from the ISO's `Extra/Preboot.dmg` ramdisk.
- **One SMC only:** QEMU's `isa-applesmc` is removed from VMs 210 and 211, so FakeSMC is the
  only SMC. With both, every clean shutdown panicked in AppleSMC (recursion); with FakeSMC
  alone, `shutdown -h now` and the GUI shut-down both power off in about 3 s (2026-11-10).
- **Input:** `usb-kbd` on `ehci.0` port 4: 10.6 has no PS/2 driver.
- **Reference VM 211 (`ripple-ref`):** full clone of VM 210 with the 9600 GT passed through
  (as VM 201). The real card drives the screen, so screenshots come from `screencapture` in
  the guest. Run it with `VMID=211 scripts/...`; only one of 210/211 at a time.
- **Access:** SSH key from the repo's ignored `.secrets/`, passwordless sudo for the guest user, set
  up in single-user mode (the owner's OK). Guest address found from the VM's MAC (`scripts/lib.sh`).
