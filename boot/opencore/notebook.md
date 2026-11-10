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
