# Installing moose-ripple

What's tested: the steps below on Proxmox VE, including `install.sh` and `uninstall.sh` run from
a CD image in a copy of the test VM. Also tested under Chameleon 2.1 r2069 (SeaBIOS, FakeSMC,
`vga: virtio-gl`): the install, Quartz Extreme, the translucent menu bar, the Dashboard ripple,
gl-gears at 42-47 frames/s, and the uninstall; for its boot flags see the end of step 4. The
plain QEMU command line is the same VM written out for QEMU without Proxmox; it was run on
Windows 11 through WSL2, with the settings in "Windows 11 (WSL2)" below. In UTM the same disks
ran with the settings in "UTM on an Apple-silicon Mac".

## What you need

- [ ] A Linux x86_64 host with KVM, Windows 11 with WSL2 (KVM inside WSL), or an Apple-silicon
  Mac with UTM (x86 emulation, no KVM). Tested: Proxmox VE 8.4 (Debian 12.10, kernel 6.5.11),
  Windows 11 with WSL 3.0.5 and Ubuntu 24.04, and macOS 26.7 with UTM 5.0.8.
- [ ] QEMU with virgl: the `virtio-vga-gl` device and a display with OpenGL (`egl-headless`,
  `gtk,gl=on` or `sdl,gl=on`). Tested: QEMU 9.2.0 (Proxmox's `pve-qemu-kvm` 9.2.0-5), 8.2.2
  (Ubuntu 24.04's) and 10.0.12 (the one inside UTM 5.0.8).
- [ ] virglrenderer. Tested: 0.10.4, 1.0.0 and UTM's own build.
- [ ] A host GPU with an OpenGL driver. Tested: Intel UHD Graphics 630 with Mesa 22.3.6 (Debian
  12's), an NVIDIA GeForce RTX 4090 through WSL2 with Mesa 25.2's Direct3D 12 driver, and an
  Apple M1 Max through UTM's ANGLE (OpenGL ES on Metal).
- [ ] OVMF, QEMU's UEFI firmware. Tested: edk2 2025.02 (Proxmox's `pve-edk2-firmware`
  4.2025.02-3), 2024.02 (Ubuntu 24.04's `ovmf`) and the one inside UTM.
- [ ] Mac OS X 10.6.8 (build 10K549) installed in the VM, from your own copy, running the 64-bit
  kernel. Both boot loaders below start it that way.
- [ ] A boot loader: OpenCore, the boot disk from [boot/opencore/](../boot/opencore/README.md)
  (OpenCore 1.0.8), or Chameleon 2.1 (r2069) with SeaBIOS and FakeSMC. Others are untested.
- [ ] With OpenCore, the SMC key of a real Mac, for QEMU's Apple SMC device
  (`isa-applesmc,osk=...`). Chameleon's FakeSMC takes its place; don't add `isa-applesmc` then.

This repository contains no Apple software, firmware or keys. Mac OS X 10.6 (and, to build from
source, Xcode 3.2.6 and its SDK) must come from your own legal copies, and running Mac OS X in a
virtual machine is subject to Apple's license terms.

## 1. Get the files

Download `moose-ripple-VERSION.qcow2` from [Releases](https://github.com/mizaimao/moose-ripple/releases).
It is the OpenCore boot disk (the steps below call it `opencore.qcow2`), and its second
partition, RIPPLE, holds moose-ripple's installer, which Snow Leopard mounts on the desktop:

| File | What it is |
|---|---|
| `RippleVirtioGPU.kext` | the display and GPU driver (kernel) |
| `RippleGLDriver.bundle` | the OpenGL driver: Mesa's virgl driver for 10.6 |
| `RippleGA.plugin` | the 2D plugin WindowServer needs before it composites with OpenGL |
| `RippleQEMUTablet.kext` | optional: the pointer follows QEMU's USB tablet exactly |
| `qe-check` | prints whether each display has Quartz Extreme |
| `install.sh`, `uninstall.sh` | put the four bundles in place and set the boot flags, or take them out again |
| `INSTALL` | these steps, short |
| `LICENSE`, `LICENSE-Mesa.rst` | moose-ripple's license, and Mesa's for the Mesa code in the GL driver |
| `SHA256SUMS` | checksums of all the files (`install.sh` checks them first) |

The same files also come as `moose-ripple-VERSION.zip`, for a VM with another boot loader
(Chameleon). Hand those to the guest as a CD image:

```
unzip moose-ripple-VERSION.zip
genisoimage -V RIPPLE -r -J -o ripple.iso moose-ripple-VERSION   # Linux or WSL; or: xorriso -as mkisofs, same options
hdiutil makehybrid -iso -joliet -default-volume-name RIPPLE -o ripple.iso moose-ripple-VERSION   # on a Mac
```

To build them yourself instead (this needs the 10.6 SDK from Xcode 3.2.6): [Building from
source](building.md) for the zip (`make package`), then [boot/opencore/](../boot/opencore/README.md)
for the disk (`build.sh -p` adds the installer partition).

## 2. Set up the VM

### Proxmox VE

With the OpenCore disk as `ide0` and the Snow Leopard disk as `sata0`:

```
qm set VMID --machine pc-q35-7.2 --bios ovmf --cpu Penryn --cores 4 --memory 8192 \
    --vga virtio-gl --tablet 1 --net0 e1000,bridge=vmbr0 --ostype other \
    --efidisk0 local:1,format=qcow2,efitype=4m,pre-enrolled-keys=0
qm importdisk VMID opencore.qcow2 local --format qcow2   # attach the disk it prints as ide0
qm set VMID --boot order=ide0
qm set VMID --args '-device isa-applesmc,osk="YOUR-SMC-KEY" -smbios type=2 -cpu Penryn,kvm=on,vendor=GenuineIntel,+sse4.1,+sse4.2,+ssse3,+kvm_pv_unhalt,+kvm_pv_eoi,+hypervisor,+invtsc,vmware-cpuid-freq=off -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off -device usb-kbd,bus=ehci.0,port=4 -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0'
```

`vga: virtio-gl` needs `libgl1` and `libegl1` on the Proxmox host (`apt install libgl1 libegl1`).
The Proxmox console then shows the guest through VNC.

With Chameleon instead of OpenCore: `--bios seabios`, no efidisk, the Chameleon 2.1 CD as `ide0`
(`media=cdrom`), and `args` without the `isa-applesmc` device, the `-fw_cfg` option (SeaBIOS
keeps PCI memory below 4 GB by itself) and `,vmware-cpuid-freq=off` (an OpenCore setting); that is
how it was tested. `vga: virtio-gl` and the rest stay the same.

### Plain QEMU

```
cp /usr/share/OVMF/OVMF_VARS_4M.fd snowleopard-vars.fd    # OVMF's paths differ between distributions
qemu-system-x86_64 \
  -machine pc-q35-7.2,accel=kvm -m 8192 -smp 4 \
  -cpu Penryn,kvm=on,vendor=GenuineIntel,+sse4.1,+sse4.2,+ssse3,+kvm_pv_unhalt,+kvm_pv_eoi,+hypervisor,+invtsc,vmware-cpuid-freq=off \
  -device isa-applesmc,osk="YOUR-SMC-KEY" -smbios type=2 \
  -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=snowleopard-vars.fd \
  -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0 \
  -device virtio-vga-gl -display sdl,gl=on \
  -device ich9-usb-ehci1,id=ehci -device usb-kbd,bus=ehci.0 -device usb-tablet,bus=ehci.0 \
  -drive file=opencore.qcow2,if=none,id=oc,format=qcow2 -device ide-hd,bus=ide.0,drive=oc,bootindex=0 \
  -drive file=snowleopard.qcow2,if=none,id=sl,format=qcow2 -device ide-hd,bus=ide.1,drive=sl \
  -drive file=ripple.iso,if=none,id=cd,media=cdrom -device ide-cd,bus=ide.2,drive=cd \
  -netdev user,id=net0 -device e1000,netdev=net0
```

`-display gtk,gl=on` works too, but with display scaling above 100% (WSLg on a 4K screen, for
example) its pointer lands away from where you click. Proxmox uses `-display egl-headless,gl=core`
with a VNC console.

### Windows 11 (WSL2)

QEMU runs inside WSL2's Linux with KVM, and Mesa's Direct3D 12 driver hands the OpenGL work to
the Windows GPU driver. The VM's window appears on the Windows desktop through WSLg. Once, in an
administrator terminal, then restart Windows:

```
wsl --install --no-distribution
```

Then a Linux for the VM (its disk file goes where `--location` says) and, inside it, QEMU:

```
wsl --install Ubuntu-24.04 --location D:\moose-ripple\wsl --name moose-ripple
wsl -d moose-ripple
sudo apt install qemu-system-x86 qemu-system-gui qemu-utils ovmf genisoimage
```

Start the VM with the plain QEMU command line above, with these settings first:

```
export GALLIUM_DRIVER=d3d12                        # without it Mesa draws on the CPU (llvmpipe)
export MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA      # part of your GPU's name, if there are several
echo 1 | sudo tee /sys/module/kvm/parameters/ignore_msrs   # used on an AMD CPU; not tried without
```

- Open the Linux from the Windows desktop (Terminal), not over ssh: a WSL started from an ssh
  session has no desktop to put the VM's window on.
- Keep `-display sdl,gl=on`: with Windows display scaling above 100%, the GTK window misplaces
  the pointer.
- The guest takes the window's size as its screen size at first; pick a resolution in System
  Preferences > Displays.

Tested on Windows 11 with a Ryzen 7 9850X3D and a GeForce RTX 4090 (WSL 3.0.5, Ubuntu 24.04,
QEMU 8.2.2, virglrenderer 1.0.0, Mesa 25.2): the guest boots in about 25 s, Quartz Extreme is on
with the translucent menu bar and the Dashboard ripple, and gl-gears runs at about 55 frames/s.
The renderer shows up as `NVIDIA GeForce RTX 4090 OpenGL Engine`.

### UTM on an Apple-silicon Mac

UTM runs the VM under emulation (an x86 guest on an ARM Mac has no KVM), and its 3D display
hands the OpenGL work to the Mac's GPU through ANGLE, which turns OpenGL ES into Metal. Create a
VM with Emulate > Other, then set in its settings:

- System: architecture x86_64, system Q35, 4 GB of memory, 4 CPU cores.
- QEMU: UEFI Boot on.
- Display: emulated display card `virtio-vga-gl` (GPU Supported).
- Network: emulated network card `e1000`.
- Drives: the OpenCore disk, the Snow Leopard disk and `ripple.iso`, all on the IDE interface,
  the OpenCore disk first.
- QEMU > Arguments, one row each:

```
-device isa-applesmc,osk=YOUR-SMC-KEY
-smbios type=2
-cpu Penryn,vendor=GenuineIntel,+sse4.1,+sse4.2,+ssse3,vmware-cpuid-freq=off
-global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off
-device ich9-usb-ehci1,id=rehci
-device usb-kbd,bus=rehci.0
-device usb-tablet,bus=rehci.0
```

- In UTM, write the SMC key without quotes: UTM keeps quotes as part of the key, which is then
  wrong (see "Why these settings").
- The keyboard and tablet get their own USB 2.0 controller: UTM's USB bus is USB 3.0, which
  10.6 has no driver for.
- No `X-PciMmio64Mb`: UTM's 3D display adds an 8 GB memory region, which only fits above 4 GB.
  Under emulation 10.6 can use it there; the hang in KI-2 happens only with KVM.
- The CPU flags that only mean something to KVM (`kvm=on`, `+kvm_pv_*`, `+hypervisor`,
  `+invtsc`) are left out.
- Optional, System > Force Multicore: UTM normally runs all four emulated CPUs on one
  thread of the Mac; with this setting each gets its own. In our test it made no
  difference we could measure: the same scripted Real Racing 2 run reached the game's menu
  after 170 s with it and 175 s without, and its race ran at 29.7 against 28.2 frames/s.
  UTM leaves it off for an Intel guest on an ARM Mac because QEMU does not guarantee
  correct emulation with it: Intel CPUs promise a stricter order of memory writes between
  cores than ARM gives. We saw no trouble with it, but the numbers in this guide are
  without it, and if a program misbehaves in a way you can't explain, turn it off again.

Tested with UTM 5.0.8 (QEMU 10.0.12) on macOS 26.7 with an M1 Max: Quartz Extreme is on with the
translucent menu bar, and gl-gears runs at about 55 frames/s even under emulation. Chess and Real
Racing 2 run too: the game is playable, with its menu at 57 frames/s and a race at about 25-30, where the emulated CPU is the limit. This GPU path has no S3TC texture compression, so
textures in it are decoded on the CPU when an app loads them. The renderer
shows up as `Intel virgl OpenGL Engine`: UTM doesn't pass on the GPU's name, and Core Image only
uses the GPU for renderer names it knows. Install with `sudo sh install.sh --hwcursor` (step 4):
with Quartz Extreme on, UTM shows the pointer only as a hardware cursor.

The first boots are slow under emulation: Spotlight indexes the disk (`mds` and `mdworker`, about
145% CPU) and the kext cache is rebuilt. In a test VM, `sudo mdutil -a -i off` stops the indexing.

### Why these settings

- `vmware-cpuid-freq=off`: without it OpenCore reads the wrong bus clock and the guest's clock
  runs 10% slow.
- `X-PciMmio64Mb=0`, plus the boot flag `npci=0x2000` in step 4: they keep the GPU's registers
  below 4 GB. Without them 10.6 moves them to the top of the physical address range the CPU
  reports, and when that is wider than the host CPU's (QEMU's default 40 bits on a 39-bit CPU)
  KVM can't reach them and the guest hangs at boot (KI-2). `-cpu ...,host-phys-bits=on` instead
  of the two works too (tested on Proxmox).
- `usb-kbd`: 10.6 has no PS/2 keyboard driver.
- Exactly one `isa-applesmc`: two make 10.6 panic at boot.
- The SMC key must be exact, in straight quotes or none (curly quotes from a copied web page break
  it; in UTM, quotes become part of the key). With a wrong key QEMU silently uses its dummy key
  (`info qtree` in the monitor shows "This is a dummy key"): loginwindow, fontd, mds and configd
  crash every few seconds, and the verbose boot stops after `Ethernet [Intel8254x]: Link up on
  en0` with no desktop, which looks like a network hang.
- `pc-q35-7.2`: the machine version we tested.

More on the boot disk: [boot/opencore/README.md](../boot/opencore/README.md).

### Sound (optional)

moose-ripple has a driver for QEMU's virtio-sound device, `RippleVirtioSound.kext`, installed
with `sudo sh install.sh --sound` (step 4). Give the VM the device and an audio backend; in UTM,
as two more rows in QEMU > Arguments:

```
-audiodev coreaudio,id=rca,out.frequency=48000
-device virtio-sound-pci,audiodev=rca
```

- It needs QEMU 8.2 or newer (the first with virtio-sound-pci).
- `coreaudio` plays on the Mac's own output. On a Linux host, use its backend instead (`pa`,
  `pipewire` or `alsa`); on Proxmox, add both options to the VM's `args:` line.
- The driver plays 48 kHz, 16-bit stereo; `out.frequency=48000` keeps QEMU from resampling it.
- The guest shows it as "Virtio Sound" in System Preferences > Sound. Volume and mute work there;
  a sound starts about 0.1 s after it is played.

Tested on 2026-11-14 in UTM on an Apple-silicon Mac (system sounds and the Welcome music played
smoothly through the Mac's speakers) and under KVM on Proxmox (QEMU's `wav` backend, checked
for gaps).

Without our driver, QEMU's USB audio device works with Snow Leopard's own driver (AppleUSBAudio),
but it stutters. It needs a USB 1.1 controller (the EHCI one above is USB 2.0 only, and 10.6 has
no driver for UTM's USB 3.0 bus):

```
-device piix3-usb-uhci,id=ruhci
-audiodev coreaudio,id=rca
-device usb-audio,bus=ruhci.0,audiodev=rca
```

In eleven UTM boots with the USB audio device, one panicked early in the boot; the cause isn't
known.

## 3. Prepare Snow Leopard

- If 10.6.8 isn't installed yet: start the 10.6.3 retail DVD through OpenCore, install, then
  apply the 10.6.8 combo update. Until moose-ripple is in, the virgl card works as a plain
  display (Apple's boot framebuffer, no acceleration).
- Sleep has to stay off, because 10.6.8 in QEMU panics when it goes to sleep with more than one
  CPU. `install.sh` turns it off; by hand: `sudo pmset -a sleep 0 displaysleep 0 disksleep 0`.
- Take a snapshot of the VM.

## 4. Install the files in the guest

With the release disk the files are already there, on the RIPPLE volume. With the zip, attach
`ripple.iso` as a CD (on Proxmox: upload it to an ISO storage, then
`qm set VMID --ide2 local:iso/ripple.iso,media=cdrom`). Either way they appear as
`/Volumes/RIPPLE`. Then, in Terminal:

```
cd /Volumes/RIPPLE
sudo sh install.sh               # options: --no-tablet, --sound, --hwcursor (UTM), --logs
sudo reboot
```

`install.sh` stops with a message if the guest isn't 10.6.8 on the 64-bit kernel or a checksum
doesn't match. Otherwise it does what you would do by hand:

1. Copies `RippleVirtioGPU.kext`, `RippleGLDriver.bundle`, `RippleGA.plugin` and
   `RippleQEMUTablet.kext` (and with `--sound`, `RippleVirtioSound.kext`) into
   `/System/Library/Extensions`.
2. Makes them owned by `root:wheel` and read-only for others (`chmod -R u=rwX,go=rX`): 10.6
   refuses kexts with any other owner.
3. Checks the kernel drivers with `kextutil -n -t` ("appears to be loadable").
4. Rebuilds the kext cache with `kextcache -system-mkext`, run from `/` (from a folder on a CD
   it fails with "Couldn't lock").
5. Turns sleep off (`pmset -a sleep 0 displaysleep 0 disksleep 0`).
6. Adds `npci=0x2000 ripple_gl=1 ripple_qe=1` to the boot flags with `nvram boot-args`,
   keeping the flags already there except `-s` (left over from single-user mode, it would make
   every boot single-user).

Its options: `--no-tablet` leaves out `RippleQEMUTablet.kext`; `--sound` adds
`RippleVirtioSound.kext`, the driver for QEMU's virtio-sound-pci device ([Sound](#sound-optional));
`--hwcursor` adds
`ripple_hwcursor=1`; `--logs` creates `/Library/Logs/RippleGL`, where the GL driver then writes a
log for every program that uses it (remove the folder to stop them).

The boot flags:

| Flag | Meaning |
|---|---|
| `npci=0x2000` | keeps PCI memory below 4 GB. Always keep it. |
| `ripple_gl=1` | OpenGL through moose-ripple. Without it the kext only drives the display (any resolution, no 3D). |
| `ripple_qe=1` | Quartz Extreme. Needs `ripple_gl=1`. |
| `-v` | text instead of the grey Apple screen while booting. Optional. |
| `slto_us=4000000` | optional: add it if you take snapshots of the running VM ([KI-5](known-issues.md)). |
| `ripple_hwcursor=1` | optional: the pointer as a hardware cursor. QEMU's own window then draws it without lag; VNC consoles draw their own. |

The flags are kept in the VM's UEFI variables (Proxmox's efidisk, or the VARS file with plain
QEMU), so they survive restarts.

With Chameleon there are no UEFI variables, so what `install.sh` sets with `nvram` is gone after
the restart. Put the flags in the Kernel Flags of `/Extra/org.chameleon.Boot.plist` on the Snow
Leopard disk instead. Chameleon reads that file even when it starts from its CD, and its Kernel
Flags take the place of the CD's own (`npci=0x2000 -v`), so keep `npci=0x2000` in it:

```
sudo /usr/libexec/PlistBuddy -c 'Add :"Kernel Flags" string "npci=0x2000 -v ripple_gl=1 ripple_qe=1"' /Extra/org.chameleon.Boot.plist
```

(Use `Set` instead of `Add` if the file already has Kernel Flags.) For a single boot, type
`ripple_gl=1 ripple_qe=1` at Chameleon's boot prompt instead. `uninstall.sh` doesn't change that
file; take the `ripple_` flags out of it yourself after uninstalling.

### Before the first boot (single-user mode)

To install before Snow Leopard's first start, for example on a freshly installed disk, start it
in single-user mode (Cmd+S in OpenCore's menu) and mount the RIPPLE volume by hand:

```
/sbin/mount -uw /
kextload /System/Library/Extensions/msdosfs.kext
mkdir -p /Volumes/RIPPLE && mount_msdos /dev/diskNs2 /Volumes/RIPPLE
sh /Volumes/RIPPLE/install.sh
reboot
```

The RIPPLE volume's disk number (`N`) depends on how many drives the VM has; `ls /dev/disk*`
shows them, and the release disk is the one with two partitions. If `pmset` can't turn sleep
off in single-user mode, `install.sh` says so; run it again after the first start.

## 5. Check that it works

- The menu bar is translucent. Press F12 and drag a widget out of the widget bar: the water
  ripples.
- `/Volumes/RIPPLE/qe-check` prints `QE on` for each display.
- `ioreg -l -c RippleAccelerator | grep -E '"Ripple(GL|QE)"'` shows both as `on`.

If one app's OpenGL window looks wrong, start that app from Terminal with
`RIPPLE_GL_DIRECT=0` in front of its command (for example
`RIPPLE_GL_DIRECT=0 /Applications/Chess.app/Contents/MacOS/Chess`). Its frames then go through
WindowServer the older, slower way instead of straight to the screen. If that helps, please
open an issue.

## Removing it

```
cd /Volumes/RIPPLE
sudo sh uninstall.sh
sudo reboot
```

It removes the four bundles, rebuilds the kext cache and takes the `ripple_` flags out of the
boot flags (`npci=0x2000` stays). If the VM doesn't reach the desktop, go back to the snapshot,
or start it in single-user mode (Cmd+S in OpenCore's menu, or `-s` at Chameleon's prompt) and run `/sbin/mount -uw /`,
`rm -rf /System/Library/Extensions/Ripple*` and `reboot`.

## Troubleshooting

- **ssh into the guest:** 10.6 ships OpenSSH 5.2, which knows only SHA-1 RSA signatures. Use an
  RSA key, and on a current client add `-o PubkeyAcceptedAlgorithms=+ssh-rsa -o
  HostKeyAlgorithms=+ssh-rsa`.
- **A serial console in single-user mode:** there is no serial device until
  `kextload /System/Library/Extensions/Apple16X50Serial.kext/Contents/PlugIns/Apple16X50ACPI.kext`;
  after that, `command > /dev/cu.serial1` reaches the VM's serial port (in UTM, its serial pty).
  Don't add `serial=3` to the boot flags under UTM: 10.6.8 then hangs early in the boot.
- **GL driver logs:** `sudo sh install.sh --logs`, or create `/Library/Logs/RippleGL` (mode 1777)
  yourself; each program that uses OpenGL then writes `PROGRAM-PID.log` there.
