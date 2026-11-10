# 0002: Dev VM setup (VM 210 on the retro rig)

Date: 2026-11-10. Status: in use.

- **Disk:** a qcow2 copy of the owner's Snow Leopard install (VM 200's `slhdd.raw`), so Proxmox
  snapshots work and VMs 200/201 stay untouched. Volume grown to 64 GiB.
- **Machine:** the same as VM 201, which already boots this install: `pc-q35-7.2`, SeaBIOS,
  Penryn CPU flags in `args`, `isa-applesmc` with the OSK from the host's VM config (never in
  the repo), e1000 NIC. 8 GB RAM, 4 cores. No GPU passthrough. `vga: virtio-gl`.
- **Bootloader: Chameleon 2.1 r2069** (the ISO VM 201 uses). It boots this install as is,
  takes boot flags at its prompt (`scripts/boot.sh` types them), and draws through
  virtio-vga's VGA BIOS. Not tried: iBoot (VM 200) and OpenCore (an image is on the rig).
  Known issue: a clean shutdown panics in AppleSMC with FakeSMC 2.7.2 loaded (most likely
  from the Chameleon ISO) next to QEMU's `isa-applesmc`; `scripts/stop.sh` syncs and powers
  off instead. Worth fixing before M2's 24-hour runs.
- **Input:** `usb-kbd` on `ehci.0` port 4: 10.6 has no PS/2 driver.
- **Reference VM 211 (`ripple-ref`):** full clone of VM 210 with the 9600 GT passed through
  (as VM 201). The real card drives the screen, so screenshots come from `screencapture` in
  the guest. Run it with `VMID=211 scripts/...`; only one of 210/211 at a time.
- **Access:** SSH key from the repo's ignored `.secrets/`, passwordless sudo for the guest user, set
  up in single-user mode (the owner's OK). Guest address found from the VM's MAC (`scripts/lib.sh`).
