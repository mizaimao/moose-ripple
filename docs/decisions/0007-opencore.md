# 0007: OpenCore on OVMF boots the 10.6.8 VMs (replacing Chameleon 2.1)

Date: 2026-11-10. Status: tested on VM 212; the driver work switches
VM 210. The owner asked for it: Chameleon is unstable for him.

## Decision

Boot Mac OS X 10.6.8 through **OpenCore 1.0.8 on Proxmox's OVMF (UEFI)**, from a small qcow2
boot disk built by [`boot/opencore/`](../../boot/opencore/README.md). No third-party kexts and
no ACPI tables are injected. Four VM settings change, all in the VM config:

1. `bios: ovmf` with an `efidisk0` (`efitype=4m,pre-enrolled-keys=0`), which keeps NVRAM.
2. QEMU's `isa-applesmc` is the one SMC (with Apple's key in `args`, never in the repo).
   FakeSMC came from Chameleon's ramdisk and is gone with it.
3. `-cpu ...,vmware-cpuid-freq=off`, so the kernel gets the right TSC frequency.
4. `-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0`, so OVMF puts every PCI BAR below 4 GB.

And one boot argument, `npci=0x2000`, the default in OpenCore's NVRAM settings (Chameleon's
ISO passed the same one).

## Why each piece

- **OVMF, not OpenCore's legacy loader on SeaBIOS.** OpenCore's OpenDuet (UEFI emulated on top
  of SeaBIOS, which would have kept VM 210's firmware) boots the kernel, but 10.6.8 panicked
  10 s into every boot (3 of 3) in `machine_stack_handoff`: a `vm_map` was all zeros. The
  zeroed memory was the first 1 KB at guest physical 0x100000000 (checked with lldb and QEMU's
  `gva2gpa`), so something under OpenDuet writes there after the kernel has taken the memory.
  Not chased further. With OVMF the same disk and config boot cleanly.
- **`isa-applesmc` alone.** One SMC, as decision 0002 requires. AppleSMC 3.1.0d5 attaches to
  it, DSMOS gets its keys, and `shutdown -h` powers off in 2-4 s. The kernel logs
  `smcWriteKey NTOK failed (kSMCBadCommand)` because QEMU's SMC has no writable keys; harmless
  so far. VirtualSMC (Lilu plugin) was the fallback and wasn't needed.
- **`vmware-cpuid-freq=off`.** With it on, OpenCore reads QEMU's CPUID leaf 0x40000010 and
  passes its 1 GHz bus clock (the local APIC timer's) as the FSB; 10.6 multiplies that by the
  Penryn bus ratio 4 from MSR 0x198 and sets the TSC to 4.0 GHz instead of 3.6 GHz, so the
  guest clock runs 10% slow. With it off, OpenCore measures the TSC (3.6 GHz, FSB 900 MHz, the
  same as Chameleon reported).
- **BARs below 4 GB (`X-PciMmio64Mb=0` plus `npci=0x2000`).** Under OVMF the virtio-gpu's
  64-bit BAR 2 ended up at 0xff10100000, and the first access to it from RippleVirtioGPU
  (`negotiate()`, first `device_status` write) never completed: the kernel took the page fault
  through the device pager again and again (`vm_fault` > `pmap_map_block`, kernel_task at
  100%), the display never came up (also with `cpus=1`), and one run later ended in a kernel
  pmap spinlock panic (after lldb had been attached).
  Under Chameleon/SeaBIOS the same BAR is at 0xfd000000 and the kext works. Both settings are
  needed: without the fw_cfg, OVMF puts the BAR at 0x800100000 and 10.6 moves it to
  0xff10100000; without `npci=0x2000`, 10.6 moves it there from wherever OVMF put it.
  `npci` clears bits in IOPCIFamily's configurator flags (IOPCIFamily-162.8 source,
  `IOPCIBridge.cpp`: `gIOPCIFlags &= ~npci`); 0x2000 is `kIOPCIConfiguratorPFM64`, its
  64-bit prefetchable window. Which IOPCIFamily source release matches 10.6.8's IOPCIFamily
  2.6.5 is UNVERIFIED; the behaviour above is what we measured.
- **MacPro3,1 SMBIOS.** Same model as Chameleon reported, so the guest looks the same to
  the driver work. With the real MacPro3,1 board ID, Apple's AppleTyMCEDriver now loads (it
  didn't under Chameleon's made-up board ID); on OVMF it has caused no problems.
- **`DummyPowerManagement`.** Does what Chameleon's Disabler.kext did: keeps
  AppleIntelCPUPowerManagement from starting (KVM here has `ignore_msrs` off).
- **No kexts.** Nothing to keep up to date besides OpenCore itself, and nothing from outside
  Apple runs in the kernel.

## Results (VM 212, 4 GB, 4 vCPUs, 2026-11-10)

- 10 of 10 cold boots without RippleVirtioGPU on `vga: virtio`: 64-bit kernel, TSC 3.59998 GHz,
  no panics, clean power-off 2-4 s after `shutdown -h now`. `shutdown -r` comes back.
- With RippleVirtioGPU from `build/` installed and `vga: virtio-gl` (as VM 210), final
  settings: 10 of 10 cold boots with RippleFramebuffer registered each time (1280x800, 32 MB
  VRAM, both VIRGL capsets, RippleAccelerator up), 64-bit kernel, no panics, power-off 3-4 s.
  Details: [notebook](../../boot/opencore/notebook.md).
- Cmd+S in the OpenCore menu (`qm sendkey VMID meta_l-s`) gives single-user mode.
- `sudo nvram boot-args=...` in the guest survives reboots (OVMF NVRAM).
- `arch=i386` in boot-args still booted the 64-bit kernel, with `KernelArch` both `x86_64`
  and `Auto`. Not pursued: decision 0003 says 64-bit kernel only.

## Update 2026-11-10 evening: the 9600 GT passthrough VM (KI-4)

The owner's decision: one OpenCore image for every VM type (virtio-gpu, 9600 GT passthrough,
later QEMU/UTM on Apple silicon); per-machine differences go in the VM settings. Tested on
VM 212 with VM 210 stopped.

**Result: the same image boots the 9600 GT VM, through OpenDuet on SeaBIOS.**
- Picture: SeaBIOS starts the card from its ROM; OpenDuet draws through the card's VESA modes,
  so the OpenCore menu and the verbose boot show on the monitor at 1024x768 (OpenCore debug
  log: `GOP exists on ConsoleOutHandle and has 6 modes`, FB at 0xF9000000, mode 1024x768 set).
- macOS: GeForce 6.3.6, NVDAResman, nv50hal; System Profiler "GeForce 9600 GT", 1024 MB, the
  VS2247 at 1920x1080@60. Quartz Extreme on (`CGDisplayUsesOpenGLAcceleration` = 1), OpenGL
  "NVIDIA GeForce 9600 GT OpenGL Engine" 2.1 NVIDIA-1.6.36, accelerated, and the repo's
  `cgl-info` clear test reads back 64 127 191 255 (correct). Core Image renders through the GL
  context (`CI_PRINT_TREE`: `fe-context-gls`). The Dashboard ripple itself wasn't watched
  (that needs someone at the monitor).
- 11 of 12 boots with the card fine (shutdown 3-4 s each). One hung early in boot (ssh died,
  no WindowServer); at that moment the rig logged `irq 16: nobody cared ... Disabling IRQ #16`.
- The same image still boots the virtio VM on OVMF (3 of 3 cold boots, RippleFramebuffer up),
  and the virtio VM also boots on SeaBIOS + OpenDuet (3 of 3).

What it took:
- **OpenDuet's BIOS-disk build (`bootX64-blockio`) instead of the default `bootX64`.** With the
  default build 10.6 panicked every boot (zeroed memory at 4 GB, above); with the block-I/O
  build, which reads disks through the BIOS instead of its own drivers, 0 panics in 15 boots.
  So the corruption most likely came from one of OpenDuet's own disk drivers leaving the
  controller running after the hand-over to macOS (UNVERIFIED which one).
- **The card's device properties in `config.plist`**, as Chameleon's GraphicsEnabler adds them
  (Chameleon source, `i386/libsaio/nvidia.c`): `@0/@1,compatible` NVDA,NVMac,
  `@0/@1,device_type` display, `@0/@1,name` NVDA,Display-A/B, `@0/@1,display-cfg`, NVCAP
  (Chameleon's default), NVPM, `VRAM,totalsize` 1 GB, `device_type` NVDA,Parent, `model`,
  `rom-revision` 62.94.11.00.03. Without them NVIDIA's driver loads but the card gets Apple's
  generic framebuffer and no display. Keyed to `PciRoot(0x0)/Pci(0x1C,0x0)/Pci(0x0,0x0)`
  (Proxmox `hostpci0`, `pcie=1`); on the virtio VMs nothing sits there.

Why not OVMF for the card: all three ROM files on the rig for it are BIOS-only (no UEFI
driver), so nothing starts the card. With the properties, macOS on OVMF did report the card
and the monitor at 1920x1080, but the card wasn't initialised: `cgl-info` read back
0 0 0 0 and screenshots were black. (Without `x-vga=1` on OVMF; that run caused no host
messages.)

The rig crash at 17:15 came about 60 s into a SeaBIOS + Chameleon run with the card
(`x-vga=1`) while VM 210 ran virgl on the UHD 630: soft lockups in host register accesses,
softdog reboot. Not reproduced since; all later card runs were with 210 stopped. Rule: no
9600 GT VM while a virgl VM runs.

Open:
- `irq 16: nobody cared` on most starts and stops of a VM with the card (IRQ 16 is shared
  with the host's SMBus controller; the card's own interrupts go through MSI once macOS's
  driver runs). Not seen in the boot before the crash, which included VM 211's runs. Possible
  fixes are host changes (for example vfio-pci's `nointxmask`), so the owner's call.
- NVCAP is Chameleon's default, not computed from this card's output table; both outputs and
  the monitor are found, so left as is.

## Not done / open

- VMs 200/201 (the owner's) still have FakeSMC (via their boot loaders) plus `isa-applesmc`;
  moving them to this setup is the owner's call.
- The OpenDuet memory corruption may be worth an upstream report to acidanthera, once it is
  narrowed down further. The owner decides (rules: report, draft, review, send).
