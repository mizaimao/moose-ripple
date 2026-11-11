# 0002: Dev VM setup (VM 210 on the retro rig)

Date: 2026-11-10. Status: in use.

- **Disk:** a qcow2 copy of the existing Snow Leopard install (VM 200's `slhdd.raw`), so Proxmox
  snapshots work and VMs 200/201 stay untouched. Volume grown to 64 GiB.
- **Machine:** `pc-q35-7.2`, Penryn CPU flags in `args` (the SMC OSK never goes in the repo), e1000
  NIC, 8 GB RAM, 4 cores, no GPU passthrough, `vga: virtio-gl`. Firmware: **OVMF** since
  2026-11-10 (with `-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0`, see KI-2 in `docs/known-issues.md`);
  SeaBIOS before that, as VM 201, which boots the same install.
- **Bootloader: OpenCore 1.0.8 on OVMF since 2026-11-10** (decision 0007, built from
  `boot/opencore/`; boot flags in NVRAM via `scripts/bootargs.sh`, keep `npci=0x2000`). Snapshot
  `pre-opencore` holds the Chameleon setup. Before that: **Chameleon 2.1 r2069** (the ISO VM 201
  uses), which took boot flags at its prompt and drew through virtio-vga's VGA BIOS; FakeSMC
  2.7.2 and Disabler 1.0.1 came from the ISO's `Extra/Preboot.dmg` ramdisk.
- **One SMC only:** with QEMU's `isa-applesmc` and FakeSMC both present, every clean shutdown
  panicked in AppleSMC (recursion). VM 210 on OpenCore has no FakeSMC, so QEMU's `isa-applesmc`
  is back as its only SMC. VM 211 still boots Chameleon, so FakeSMC is its SMC and
  `isa-applesmc` stays removed there. Both power off in 2 to 4 s after `shutdown -h now`.
- **Input:** `usb-kbd` on `ehci.0` port 4: 10.6 has no PS/2 driver.
- **Reference VM 211 (`ripple-ref`):** full clone of VM 210 with the 9600 GT passed through
  (as VM 201). The real card drives the screen, so screenshots come from `screencapture` in
  the guest. Run it with `VMID=211 scripts/...`; only one of 210/211 at a time.
- **Access:** SSH key from the repo's ignored `.secrets/`, passwordless sudo for the guest user, set
  up in single-user mode (approved). Guest address found from the VM's MAC (`scripts/lib.sh`).
