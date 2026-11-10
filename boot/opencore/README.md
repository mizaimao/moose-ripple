# OpenCore boot disk for Mac OS X 10.6.8 in QEMU

Boots the Snow Leopard dev VMs through OpenCore 1.0.8 on UEFI firmware (Proxmox's OVMF),
replacing the Chameleon 2.1 ISO. Why and what was tried: [decision 0007](../../docs/decisions/0007-opencore.md).

## Files

- `config.plist`: the OpenCore configuration (no kexts, no ACPI tables, MacPro3,1 SMBIOS).
- `sources.txt`: pinned OpenCore releases with SHA-256. Nothing downloaded is committed.
- `mkimage.sh`: builds the disk image (runs as root on the retro rig).
- `build.sh`: validates `config.plist` with `ocvalidate` on dev.lan, builds the image on the
  rig, and with `-w VMID` writes it to that VM's OpenCore disk.
- `coldboot-test.sh`: N cold boots with clean shutdowns, one line per boot.

## VM settings it needs

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

- `vmware-cpuid-freq=off`: otherwise OpenCore takes QEMU's 1 GHz APIC bus clock for the FSB
  and 10.6 sets the TSC to 4.0 GHz instead of 3.6 GHz; the guest clock runs 10% slow.
- `X-PciMmio64Mb=0` and `npci=0x2000`, together: 10.6 can't use a PCI BAR above 4 GB here.
  The virtio-gpu's BAR 2 landed at 0xff10100000 and RippleVirtioGPU hung on its first access.
  The fw_cfg keeps OVMF from placing BARs above 4 GB; `npci=0x2000` keeps 10.6 from moving
  them there. **Keep `npci=0x2000` whenever you change boot-args.**

Details and evidence: [decision 0007](../../docs/decisions/0007-opencore.md).

## Build and install

From the laptop (it only runs ssh/scp):

    boot/opencore/build.sh                 # validate and build only
    boot/opencore/build.sh -w 212          # ... and write to VM 212's OpenCore disk (VM stopped)
    OC_VARIANT=DEBUG boot/opencore/build.sh -w 212    # debug build of OpenCore

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
