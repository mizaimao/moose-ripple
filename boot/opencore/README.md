# OpenCore boot disk for Mac OS X 10.6.8 to 10.9.5 in QEMU

One image boots Mac OS X 10.6.8 through OpenCore 1.0.8 in every VM type tested, replacing the
Chameleon 2.1 ISO. The same image boots OS X 10.7.5 and 10.8.5 and their installers (VM 213); installers
and never-booted disks are in `/var/lib/vz/osx/` on the rig (status in the notebook):
- **virtio-gpu VMs** (VM 210): UEFI firmware (Proxmox's OVMF).
- **9600 GT passthrough VMs** (VM 211's setup): SeaBIOS, which starts the card from its BIOS
  ROM, then OpenDuet (OpenCore's UEFI layer for BIOS machines, on the same disk).

Why and what was tried: [decision 0007](../../docs/decisions/0007-opencore.md).

## Files

- `config.plist`: the OpenCore configuration (no kexts, no ACPI tables, MacPro3,1 SMBIOS, the
  9600 GT's device properties, `OpenPartitionDxe` for the installers' Apple partition maps,
  `ScanPolicy` 0: with 513 OpenCore didn't list volumes on Apple partition maps).
- `sources.txt`: pinned OpenCore releases with SHA-256. Nothing downloaded is committed.
- `stage.py`: runs on dev.lan in the repo's `.venv` (Python only from there):
  fetches the releases by SHA-256, runs `ocvalidate`, collects the files the config names and
  OpenDuet's loader (`bootX64-blockio`), adds `EFI/BOOT/.contentVisibility` (`Disabled`: keeps
  OpenCore's own disk out of its menu), and streams them as tar.
- `mkimage.sh`: builds the disk image from that stream, as root on the retro rig, with shell
  tools only: a FAT32 EFI partition with OpenCore plus OpenDuet's boot sectors.
- `build.sh`: runs both (the tar stream passes through the laptop's ssh) and with `-w VMID`
  writes the image to that VM's OpenCore disk.
- `coldboot-test.sh`: N cold boots with clean shutdowns, one line per boot.

## VM settings: virtio-gpu VMs

Compared with the Chameleon setup (VM 210 on 2026-11-10):

| Setting | Chameleon | OpenCore |
|---|---|---|
| Firmware | `bios: seabios` | `bios: ovmf`, plus `efidisk0: local:1,format=qcow2,efitype=4m,pre-enrolled-keys=0` (keeps NVRAM; Secure Boot off) |
| Boot disk | `ide0: local:iso/Chameleon-2.1-r2069.iso,media=cdrom` | `ide0: local:VMID/vm-VMID-opencore.qcow2` (made by `build.sh -w VMID`) |
| SMC | FakeSMC from Chameleon's ramdisk, no `isa-applesmc` | QEMU's `-device isa-applesmc,osk="..."` in `args` (Apple's key: copy it from VM 200's config on the rig; never put it in the repo) |
| CPU timing | | add `,vmware-cpuid-freq=off` to the `-cpu` list in `args` |
| PCI BARs | SeaBIOS puts them below 4 GB | add `-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0` to `args` |
| Boot flags | `npci=0x2000` from Chameleon's ISO | `npci=0x2000` in the default boot-args (config.plist) |

Everything else stays as it is (q35 7.2, Penryn args, `usb-kbd`, e1000, `vga`).
Check that `args` holds exactly one SMC device: `qm config VMID | grep -o isa-applesmc | wc -l`
must print 1. Two of them make 10.6 panic at once (`AppleACPIPlatformExpert::start failed`).

- `vmware-cpuid-freq=off`: otherwise OpenCore takes QEMU's 1 GHz APIC bus clock for the FSB
  and 10.6 sets the TSC to 4.0 GHz instead of 3.6 GHz; the guest clock runs 10% slow.
- `X-PciMmio64Mb=0` and `npci=0x2000`, together: 10.6 can't use a PCI BAR above 4 GB here.
  The virtio-gpu's BAR 2 landed at 0xff10100000 and RippleVirtioGPU hung on its first access.
  The fw_cfg keeps OVMF from placing BARs above 4 GB; `npci=0x2000` keeps 10.6 from moving
  them there. **Keep `npci=0x2000` whenever you change boot-args.**

Details and evidence: [decision 0007](../../docs/decisions/0007-opencore.md).

## VM settings: 9600 GT passthrough VMs

Same as VM 211 except the boot disk and the SMC, tested on VM 212:

| Setting | Value |
|---|---|
| Firmware | `bios: seabios` (OVMF can't start this card: its ROM has no UEFI driver) |
| Boot disk | `ide0: local:VMID/vm-VMID-opencore.qcow2`, boot order `ide0;sata0` |
| Card | `hostpci0: 0000:01:00,pcie=1,x-vga=1,romfile=9600gt-tpu.bin`, no emulated VGA |
| SMC | `-device isa-applesmc,osk="..."` in `args`, as for the virtio VMs |
| CPU timing | `,vmware-cpuid-freq=off`, as for the virtio VMs |

- The card's device properties (model, VRAM, NVCAP, display names, as Chameleon injected them)
  are in `config.plist` for the device path `PciRoot(0x0)/Pci(0x1C,0x0)/Pci(0x0,0x0)`, which
  is where Proxmox puts `hostpci0` with `pcie=1`. Pass the card as `hostpci0`.
- NVRAM isn't kept under OpenDuet: boot flags always come from `config.plist`.
- **Never run a 9600 GT VM while a virgl VM (`vga: virtio-gl`) runs**: the rig crashed once
  that way (2026-11-10 17:15).
- Open: the rig logs `irq 16: nobody cared ... Disabling IRQ #16` at most starts and stops
  of a VM with the card (IRQ 16 is shared with the host's SMBus). One boot in 12 hung while
  it happened. See decision 0007.

## Build and install

From the laptop (it only runs ssh/scp):

    boot/opencore/build.sh                 # validate and build only
    boot/opencore/build.sh -w 212          # ... and write to VM 212's OpenCore disk (VM stopped)
    OC_VARIANT=DEBUG boot/opencore/build.sh -w 212    # debug build of OpenCore
    OC_DUET=none boot/opencore/build.sh    # without OpenDuet (bootX64 = OpenDuet's own disk drivers)

The first `-w` creates `local:VMID/vm-VMID-opencore.qcow2` and sets it as `ide0`, boot order
`ide0;sata0`. Later runs overwrite its contents; Proxmox snapshots keep older versions.
The rest of the settings in the table above are set by hand with `qm set` (snapshot first).

## Day to day

- **Boot:** OpenCore shows its menu for 5 seconds and starts Macintosh HD by itself; no keys
  needed. The default boot-args are `-v npci=0x2000`.
- **Boot flags:** set them in the guest, as on a real Mac:
  `sudo nvram boot-args="-v npci=0x2000 foo=1"`. They stay in OVMF's NVRAM (the efidisk)
  until changed. OpenCore only fills in its default when `boot-args` is missing (for example
  after a NVRAM reset). OpenCore adds `arch=x86_64` in front.
- **Hotkeys in the OpenCore menu:** Cmd+S single user (tested: `qm sendkey VMID meta_l-s`
  from the rig within the 5 seconds). OpenCore's docs also list Cmd+V verbose and
  Shift+Enter safe mode (not tried).
- **Clear NVRAM:** OpenCore's docs: Space in the menu shows the hidden "Reset NVRAM" entry
  (not tried). Afterwards the default boot-args come back.
- **Update OpenCore:** new release URL and SHA-256 in `sources.txt`, compare the new
  `Docs/Sample.plist` and changelog with `config.plist`, then `build.sh` (ocvalidate reports
  any keys that changed).
