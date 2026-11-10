# OpenCore lab notes

Dated entries: what was tried, what happened. The conclusions are in
[decision 0007](../../docs/decisions/0007-opencore.md).

## 2026-11-10: the OpenCore image already on the rig

`local:iso/1920x1080-OpenCoreSLeopard.img` (SHA-256 `b5d41fa5…21bbbc`), read-only inspection:
OpenCore from about Oct 2021 (Lilu 1.5.6, VirtualSMC 1.2.7, WhateverGreen 1.5.4), set up for a
MacBookPro13,3 under Hyper-V (SSDT-HV-*, MacHyperVSupport), VoodooHDA, AGPMInjector, ru-RU
keyboard, an empty `SSDT-PLUG.aml` that the config enables, Hackintool.app and old logs on the
volume, no legacy boot files. `local:200/opencore-SLeopard.qcow2` is a truncated copy (87.7 of
189 MiB). Verdict: not usable; built new from upstream.

## 2026-11-10: VM 212 and the Chameleon baseline

`qm disk import` of `local:200/slhdd.raw` to qcow2; hardware as VM 210 (4 GB). Snapshot
`baseline`. Remote Login, NOPASSWD sudo for the guest user and the repo's guest key set up in single-user
mode (as for VM 210; fingerprint checked), snapshot `ssh-ready`.

Under Chameleon 2.1 the guest reports: MacPro3,1 with board-id Mac-F4208DC8 (not the real
MacPro3,1 board-id), serial SOMESRLNMBR, TSC 3.6 GHz with FSB 900 MHz, boot-args
`npci=0x2000 -v`, third-party kexts Disabler 1.0.1 (blocks AppleIntelCPUPowerManagement by
taking its IOMatchCategory) and FakeSMC 2.7.2, both from the ISO's `Extra/Preboot.dmg`.
virtio-gpu BAR 2 at 0xfd000000.

## 2026-11-10: OpenCore 1.0.8 through OpenDuet on SeaBIOS (dropped)

Same disk image with OpenDuet's boot sectors (OpenCore's `Utilities/LegacyBoot`, as its
`QemuBuild.command`). OpenDuet starts, the picker works once the OpenCore volume itself is
hidden (`ScanPolicy` 513). The kernel boots, then panics about 10 s in, 3 runs of 3:
`machine_stack_handoff+203`, CR2 0x80, a thread's `vm_map` with a NULL pmap. With lldb over
QEMU's gdbstub: the `vm_map` at 0xffffff80020081f0 was all zeros, as was most of the 1 KB from
0xffffff8002008000, which `gva2gpa` puts at guest physical 0x100000000, the first page above
4 GB. Blocking AppleTyMCEDriver and fixing the TSC (below) didn't change it. Not chased
further; OVMF works.

## 2026-11-10: TSC 4.0 GHz instead of 3.6 GHz

OpenCore debug log: `OCCPU: VMWare TSC: 3600000000Hz, 3600MHz; FSB: 1000000000Hz, 1000MHz`,
so OpenCore used QEMU's CPUID leaf 0x40000010 bus clock (1 GHz, the APIC timer). The kernel
(lldb): `tscFreq` 4000000000, `busFreq` 1000000000, because xnu multiplies the FSB by the bus
ratio 4 from MSR 0x198. `-cpu ...,vmware-cpuid-freq=off`: `tscFreq` 3599967520, `busFreq`
899991880.

## 2026-11-10: OVMF

`bios: ovmf`, efidisk with no Secure Boot keys. Boots on the first try. 10 cold boots of 10:
64-bit kernel, TSC 3.59998 GHz, no panics, no third-party kexts, power-off 2-4 s after
`shutdown -h now`; `shutdown -r` works. AppleTyMCEDriver loads (real board-id Mac-F42C88C8)
without problems. `nvram boot-args` set in the guest survives a reboot. `arch=i386` in
boot-args still gave the 64-bit kernel (KernelArch `x86_64` and `Auto`).

## 2026-11-10: RippleVirtioGPU on OVMF

`build/RippleVirtioGPU.kext` (built 15:54) installed in `/System/Library/Extensions`,
`vga: virtio-gl`. The kext loaded but RippleFramebuffer never registered; kernel_task at 100%,
the screen stayed on the verbose console. lldb: `RippleFramebuffer::start` >
`VirtioGPUDevice::negotiate` (first write to BAR 2) > page fault > `vm_fault` >
`memory_object_data_request` / `pmap_map_block`, over and over; the same with `cpus=1`. BAR 2
was at 0xff10100000 (`info pci`; `IODeviceMemory` 1095486144512). Same disk under Chameleon:
BAR 2 at 0xfd000000 and the kext works.

| OVMF window (`X-PciMmio64Mb`) | `npci=0x2000` | OVMF put BAR 2 at | 10.6 left it at | Kext |
|---|---|---|---|---|
| default | no | 0x800100000 (as in row 3) | 0xff10100000 | hangs |
| 0 | no | 0x81b40000 | 0xff10100000 ("device=3" moved) | hangs |
| default | yes | 0x800100000 | 0xff10100000 | hangs |
| 0 | yes | 0x81b40000 | 0x81b40000 ("device=0") | works: 1280x800, 32 MB VRAM, both VIRGL capsets |

Then 10 cold boots of 10 with the kext, `vga: virtio-gl`, boot-args from OpenCore's default
(`nvram -d boot-args` first): RippleFramebuffer registered every time, 64-bit kernel, TSC 3.6
GHz, no panics, power-off 3-4 s. `shutdown -r` works. Cmd+S in the OpenCore menu
(`qm sendkey 212 meta_l-s`) gives single-user mode. Snapshot `opencore-final`.

## 2026-11-10: 9600 GT passthrough with the same image (stopped: rig crash)

The owner's decision: one OpenCore image for virtio-gpu, 9600 GT passthrough and later Apple
silicon. VM 212 got `hostpci0: 0000:01:00,pcie=1,x-vga=1,romfile=9600gt-tpu.bin` (as VM 211),
no emulated VGA, OVMF kept. Snapshot `pre-gpu` first.

- The ROM file (`/usr/share/kvm/9600gt-tpu.bin`, 62976 bytes, SHA-256 `4c4e2165…d331`) has
  one image only, code type 0 (x86 legacy): no UEFI driver. OVMF can't light the card, so the
  OpenCore menu and the boot are invisible on the monitor; the first picture is macOS's.
- OVMF + OpenCore, 17:11:42: boots (ssh after 22 s, `npci=0x2000`), NVDAResman, nv50hal and
  GeForce 6.3.6 load, but the card gets IONDRVFramebuffer, not NVIDIA's framebuffer, and
  System Profiler shows no model name, no VRAM and no display.
- Same VM on SeaBIOS + Chameleon (as VM 211), 17:12:56: "GeForce 9600 GT", 1024 MB, two
  connectors, the monitor found. So Chameleon injects the NVIDIA properties (`@0,compatible`,
  NVCAP, `VRAM,totalsize` ...) that OpenCore would have to supply as DeviceProperties. The
  dump of Chameleon's `device-properties` didn't finish.
- About 60 s after that start, with VM 210 running virgl on the UHD 630, the rig stalled
  (pve-firewall/pvestatd updates 21-34 s from ~17:14:00, NMI handler 1.5 s at 17:14:36,
  soft lockups at 17:14:53 in e1000e register reads and the CMOS clock port write) and
  rebooted at 17:15:02. No vfio, AER or DMAR messages. Cause UNVERIFIED; leading idea: legacy
  VGA routing for `x-vga=1` against i915, which was busy with 210's virgl. Earlier 9600 GT runs
  had either 210 stopped (VM 211) or i915 not loaded (VMs 200/201, before the iGPU was on).
- VM 212 rolled back to `pre-gpu` (no card). No VM with the 9600 GT until the owner decides.

## 2026-11-10 evening: 9600 GT under OpenCore (KI-4), VM 210 stopped

1. OVMF, no `x-vga`, `vga: none`, NVIDIA properties added to `config.plist` (values from
   Chameleon's `nvidia.c`, ROM version string from the ROM): GeForce 6.3.6 attaches, System
   Profiler shows the card, 1024 MB and the VS2247 at 1920x1080, Quartz Extreme reported on,
   but `cgl-info` reads 0 0 0 0 and `screencapture` is all black: the card was never started
   (its ROM is BIOS-only). No host messages.
2. OpenDuet again, now the `bootX64-blockio` build, virtio VM on SeaBIOS: 3 of 3 boots fine
   (the default build had panicked 3 of 3).
3. SeaBIOS + OpenDuet + card (`x-vga=1`, as VM 211): desktop on the 9600 GT at 1920x1080,
   QE 1, `cgl-info` 64 127 191 255, Core Image on the GL path. Debug build: OpenCore finds
   OpenDuet's GOP on the card (6 modes) and sets 1024x768, so the menu shows on the monitor.
4. Cold boots with the card: 5 + 5, plus 2 single boots: 11 fine, 1 hung (boot 1 of the
   first series: no WindowServer, ssh died, no power-off; the rig logged `irq 16: nobody
   cared` at 17:37:28, 8 s after the guest kernel started). `irq 16: nobody cared` appears at
   most VM starts and stops with the card since the 17:15 reboot (17:30:00, 17:36:55, ...).
5. Same image back on the virtio VM with OVMF: 3 of 3, RippleFramebuffer registered.
6. VM 212 left without the card, stopped; snapshot `one-image`.

## 2026-11-10 18:05-18:25: Chameleon vs OpenCore, IRQ 16 messages

VM 212 set up as VM 211 (SeaBIOS, Chameleon on ide0, the card with `x-vga=1`, FakeSMC, no
`isa-applesmc`, OpenCore disk detached). Note: my first 4 Chameleon boots were reported as
failures by my own test script (it used the wrong ssh key path) and were hard-stopped; the
guests had in fact booted (checked by hand on the 4th: Disabler and FakeSMC loaded, up).
With the script fixed, 3 boots: QE 1, 1920x1080, `cgl-info` 64 127 191 255, power-off 2-3 s;
`irq 16: nobody cared` 0 times from start to shutdown, 1 time at each shutdown.
With OpenCore/OpenDuet earlier (same host boot): at most starts (during QEMU's start, before
the firmware runs) and shutdowns, twice during macOS's boot (17:37:28 hang, 17:46:53 no harm).
The owner stopped the series here: accepted as a known issue, no host changes, at most 3 VM boots
per check from now on. The rig gets one clean reboot (by the coordinator), then at most 3
card boots check whether the messages persist.

## 2026-11-10 18:32-18:50: the two card-boot panics after the rig reboot: my config error

The owner saw a kernel panic on the 9600 GT monitor in both card boots after the clean rig reboot
(18:32:56 and the next one). Diagnosed without the card (VM 212 on virtio):
- OpenCore/OVMF without the card panicked too, 0.18 s into the kernel:
  `AppleACPIPlatformExpert::start failed`, `Unable to find driver for this platform: "ACPI"`.
  Chameleon still booted the same disk.
- Cause: VM 212's `args` held **two** `-device isa-applesmc` entries. When I put the SMC back
  for the card test, a command of mine that had been interrupted had already added one, and
  my check (`grep -c applesmc`) counted lines, not devices. Two SMC devices in QEMU's ACPI
  tables make 10.6's ACPI platform driver fail. The card boots at 18:32 ran with this config.
  (The Chameleon boot at 18:41 then had one `isa-applesmc` plus FakeSMC, which is why its
  shutdown hung: the known two-SMC problem.)
- Fixed: exactly one `isa-applesmc` (`grep -o isa-applesmc | wc -l` = 1). One boot without
  the card: fine, AppleACPIPlatformExpert up. So the panics were not the card's or OpenCore's.

## 2026-11-10 18:50-18:53: card window (210 off), 2 boots

1. **Apple EFI test ROM on OVMF** (`romfile=9600gt-macefi-gt120.rom`, no `x-vga`, `vga: none`,
   OpenCore debug log): OpenCore finds neither GOP nor UGA (`Missing compatible GOP`,
   `Found 0 handles with UGA draw`), so no UEFI picture. But macOS came up with the card
   working: QE 1, 1920x1080, `cgl-info` 64 127 191 255. On OVMF with the BIOS-only ROM (17:2x)
   the card had stayed unstarted (0 0 0 0, black screenshots). So something in Apple's driver
   most likely started the card without publishing a GOP (UNVERIFIED: no proof yet that OVMF
   ran the driver). `irq 16: nobody cared` once, at shutdown.
2. **OpenDuet card boot, corrected settings** (one `isa-applesmc`, release image): fine, QE 1,
   1920x1080, GL correct. `irq 16: nobody cared` 0 times from start to shutdown, once at
   shutdown: the same pattern as Chameleon. The start-time messages seen before the clean rig
   reboot didn't appear (one sample).
VM 212 left without the card.
