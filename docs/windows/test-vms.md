# Windows test VMs

The Windows 7 test VM on the Proxmox host, and what Windows 7 does on virtio-gpu without a
driver of ours: the baseline every later step is measured against. 2026-11-14.

## What the host has

Read only; none of it was changed.

- The owner's VMs 100 to 105: Windows 7, Vista, XP, 2000, ME and 98, each set up for the
  GeForce 9600 GT passed through (`hostpci0`, `x-vga=1`) and a USB receiver. They are the
  real-hardware reference and are never changed: tests run on clones under new VM IDs.
  VM 100 (Windows 7 Ultimate x64 SP1) is on `pc-q35-2.10`, SeaBIOS, 4 cores, 8 GB, one
  256 GB SATA disk (26.5 GB used), no snapshots.
- Install media on the host: Windows 7 SP1 x64 (an update rollup from 2020), Vista Ultimate
  SP2 x64, XP SP3 x86 and XP x64, Windows 10.
- virtio-win driver discs 0.1.285, 0.1.215 and 0.1.173. Only 0.1.285 has `viogpudo`, the
  virtio-gpu display-only driver, and only for Windows 8, 8.1, 10, 11 and Server 2012 and
  later: no `w7` folder. 0.1.215 and 0.1.173 have no virtio-gpu driver at all; their `qxl`
  folder (XDDM, Server 2008 R2) is for QEMU's QXL card, not virtio-gpu. So Windows 7 has no
  driver for virtio-gpu from virtio-win.
- QEMU `pve-qemu-kvm` 9.2.0-5, virglrenderer 0.10.4. The host has 15 GB of RAM: one 4 GB test
  VM fits next to VM 210 (8 GB).

## VM 220: `ripple-win7`

A full clone of VM 100's disk (`qm clone 100 220 --full`), with the 9600 GT, the USB receiver
and the CD removed, a new network card address, and the virgl display:

| Setting | Value |
|---|---|
| Machine | `pc-q35-2.10`, SeaBIOS, 4 cores (Penryn), 4 GB |
| Disk | SATA, `local:220/vm-220-disk-0.qcow2` |
| Display | `vga: virtio-gl`: QEMU's `virtio-vga-gl` at PCI 00:01.0, ID `1AF4:1050`, subsystem `1100:1AF4`, revision 1, class 0300 |
| Network | e1000 on the host bridge |
| Snapshots | `clone-fresh` (as cloned, before the first boot), `testsigning-on` (test mode on, the hello driver in `C:\rb`) |

The 9600 GT reference stays VM 100. Never run it while a virgl VM is drawing.

## Baseline: Windows 7 on virtio-gpu without our driver

Windows 7 Ultimate x64 SP1 (6.1.7601), DirectX 11.

- Display driver: Windows binds its own *Standard VGA Graphics Adapter* to the virtio-vga
  function (`PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01`) and reports it as working. That is
  the XDDM VGA driver, which draws through the VGA BIOS's VESA modes. dxdiag: DirectDraw,
  Direct3D and AGP acceleration "Not Available", DDI version and driver model "unknown".
  On the first boot Windows also reported that installing a driver failed, and lists two other
  devices without drivers (unrelated to the display).
- Resolutions: 800x600 at the first boot; Screen Resolution offers 800x600 up to 1920x1080
  (1024x768, 1280x720, 1280x800, 1280x1024, 1440x900, 1600x900, 1600x1200, 1680x1050 and
  others), all from the VGA BIOS.
- Aero: not available. Personalization still lists the Aero themes, but the desktop draws
  as Windows 7 Basic: no glass, no translucent taskbar, no Desktop Window Manager. Aero needs a
  WDDM driver.
- Windows Experience Index: cannot be computed. Refreshing it ends with "The Windows
  Experience Index for your system could not be computed ... an error being reported from the
  operating system, driver, or other component". The same disk's scores on record, from the
  9600 GT in VM 100 (2025-07-30), are the reference: processor 7.7, memory 7.9, graphics
  7.8, gaming graphics 7.8, disk 7.9 (base score 7.7).
- Driver signatures: KB4474419 (SHA-2 code signing support) and KB4490628 are installed,
  so the VM checks SHA-256 signatures. A test-signed kernel driver fails to start outside
  test mode (`StartService FAILED 577`, "Windows cannot verify the digital signature").

Screenshots (dxdiag, Device Manager, Personalization, the Experience Index error, the
resolution list) are kept out of the repository in `docs/re/img/win7-baseline/`, since they
show the owner's account and desktop.
