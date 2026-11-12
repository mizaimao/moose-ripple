# Known issues

Things we found that others running Mac OS X 10.6 in QEMU may run into too. Each entry has a
number (KI-1, KI-2, ...) that the other documents refer to, and a GitHub issue with the same
number where it is followed up.

## KI-1: The pointer doesn't follow QEMU's USB tablet exactly (10.6 + `usb-tablet`)

GitHub issue: [#1](https://github.com/mizaimao/moose-ripple/issues/1)

**Symptom.** With QEMU's virtual USB tablet (`-device usb-tablet`, Proxmox `tablet: 1`), 10.6
places the pointer about 1.18x too far from the screen centre: the centre is exact, the error
grows towards the edges, and the pointer clamps at the screen edge early.

**Why it often goes unnoticed.** With the default display (Apple's boot framebuffer, no
hardware cursor) the guest draws its own pointer into the picture. Clicks always match that
pointer, so everything looks right; the pointer just doesn't sit under your real mouse and
runs ahead of it towards the edges. With a driver that has a hardware cursor (ours), a VNC
console such as Proxmox's draws the pointer under your real mouse, and the guest's error then
shows up as clicks landing somewhere else (2026-11-10: pointer at the centre, click on
the left half).

**Measurement** (2026-11-10, 10.6.8 10K549, QEMU 9.2, Proxmox 8.4). Absolute positions were
injected with QMP `input-send-event` (range 0..32767) and the guest's pointer position read
with `tools/ptrpos` (`CGEventGetLocation`):

| Injected (x and y) | Expected at 1280x800 | Guest pointer | With Apple's boot framebuffer at 1024x768 |
|---|---|---|---|
| 0 | 0, 0 | 0.1, 0.4 | |
| 4096 | 160, 100 | 75.3, (y alike) | |
| 8192 | 320, 200 | 263.6, 164.7 | 210.8, 158.1 (expected 256, 192) |
| 12288 | 480, 300 | 451.8, (y alike) | |
| 16384 | 640, 400 | 640.0, 400.0 | 512.0, 384.0 |
| 24576 | 960, 600 | 1016.5, 635.3 | 813.2, 609.9 (expected 768, 576) |
| 32767 | 1280, 800 | 1279.9, 799.6 | |

That is a linear map stretched by 1.176 (about 1/0.85) around the centre, then clamped. It is
repeatable and doesn't depend on the previous position, so it isn't pointer acceleration, and
it is the same with or without our framebuffer, so it comes from 10.6's HID stack, not the
display.

**Cause.** Apple's `IOHIDEventDriver` (IOHIDFamily 1.6.6, open source as IOHIDFamily-315.7.16)
trims 15% off the range of every absolute pointing device, half at each end
(`IOHIDEventDriver.cpp`: `absoluteAxisRemovalPercentage = 15`), then scales the remaining 85%
to the screen: 1/0.85 = 1.176. The driver reads an override from its own property
`AbsoluteAxisBoundsRemovalPercentage`.

**Fix.** [`kext/qemutablet`](../kext/qemutablet/Info.plist), a codeless kext (only an
Info.plist): a personality that has Apple's `IOHIDEventDriver` drive QEMU's tablet (vendor
0x0627, product 1, Generic Desktop / Mouse) with `AbsoluteAxisBoundsRemovalPercentage` = 0.
Its vendor/product keys outscore Apple's generic personality (7200 vs 1200). After installing
it in `/System/Library/Extensions` and rebuilding the kext cache:

| Injected | Guest pointer at 1280x800 |
|---|---|
| 0 | 0, 0 |
| 4096 | 160, 100 |
| 8192 | 320, 200 |
| 16384 | 640, 400 |
| 24576 | 960, 600 |
| 32767 | 1279, 799 |

It works with any display driver, not only ours, so it is useful on its own for 10.6 in QEMU
(and likely 10.5, which has the same HID code; untested).

## KI-2: A PCI BAR above 4 GB hangs 10.6.8 when the guest's address width exceeds the host CPU's

GitHub issue: [#2](https://github.com/mizaimao/moose-ripple/issues/2)

Found while setting up OpenCore (decision 0007). Under OVMF, QEMU's virtio-gpu 64-bit BAR 2 can
land above 4 GB, and 10.6 moves it to the top of the address space the CPU reports. QEMU's
default for that is 40 bits; on a host CPU with fewer (the Core i9-9900K has 39) 10.6 put the BAR
at 0xff10100000, beyond what KVM can back, and the first access from a driver never completed
(`vm_fault` again and again, kernel_task at 100%, no display).

Two ways out, both tested (2026-11-11):
- Give the guest the host's address width: `-cpu ...,host-phys-bits=on`. 10.6 then puts the BAR
  at 0x7f10000000 and the driver works with it (Quartz Extreme on). Under emulation (UTM), where
  there is no host limit, the default width works as is.
- Keep every BAR below 4 GB: `-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0` for OVMF and the boot
  flag `npci=0x2000` for 10.6. SeaBIOS places them below 4 GB already. This doesn't work when a
  device needs more than the 32-bit window holds, such as UTM's 3D display with its 8 GB
  host-memory region (#18).

RippleVirtioGPU logs a BAR above 4 GB and goes on (until 2026-11-11 it refused one).

## KI-3: configd's PowerManagement plugin crashes at boot (VM 210)

GitHub issue: [#3](https://github.com/mizaimao/moose-ripple/issues/3)

`configd` crashes with `EXC_BAD_ACCESS` in `PowerSourcesHaveChanged` (PowerManagement
plugin, `CFGetTypeID` on a bad object), usually a few times per boot; launchd restarts it and
nothing visible breaks. Seen in `/Library/Logs/DiagnosticReports` since VM 210's first boot
(2026-11-10 12:34) under both Chameleon and OpenCore, so it has nothing to do with our kexts.
Likely the VM's missing or odd power-source (battery/UPS) information. Not investigated.

## KI-4: 9600 GT passthrough VM: occasional hang at boot, host logs `irq 16: nobody cared`

GitHub issue: [#4](https://github.com/mizaimao/moose-ripple/issues/4)

Found while setting up OpenCore (decision 0007). On the retro rig the 9600 GT's legacy
interrupt shares the host's IRQ 16 with the SMBus controller. With a VM that has the card, the
host logs `irq 16: nobody cared ... Disabling IRQ #16` at most VM shutdowns, and with the
OpenCore setup also at most VM starts; twice it came during macOS's boot, and once the VM then
hung (no WindowServer, no ssh). With Chameleon (3 boots) it came only at shutdown. Once macOS's
NVIDIA driver runs, the card uses MSI and the shared line no longer matters. Decision
(2026-11-10): accepted as a known issue, no host setting changes; a hung 9600 GT VM is simply
restarted.

## KI-5: Live snapshots can panic the guest ("TLB invalidation IPI timeout")

GitHub issue: [#5](https://github.com/mizaimao/moose-ripple/issues/5)

Seen on VM 210 on 2026-11-10, right after `qm snapshot` paused the running VM (a disk-only
snapshot pauses the vCPUs briefly). The panic: `TLB invalidation IPI timeout: CPU(s) failed to
respond to interrupts` (pmap.c:2741, CPU 1 in AppleUSBEHCI). A CPU waiting for another CPU's
acknowledgement when the pause starts sees the whole pause as waiting time after the resume,
and 10.6 gives up after its spin limit, `LockTimeOut`, 250 ms by default. The same panic once
followed a 2560x1600 mode switch in M2; a long host-side stall would cause it the same way.

**Workaround:** the guest boot flag `slto_us=4000000` raises the limit to 4 s (the value is
kept in 32 bits of nanoseconds, so about 4.29 s is the most it can hold). VM 210 has it in its
NVRAM flags since 2026-11-10 (`scripts/bootargs.sh`). Snapshots of a stopped VM are always safe.

## KI-6: The guest panics when it goes to sleep (10.6.8 in QEMU, more than one CPU)

GitHub issue: [#6](https://github.com/mizaimao/moose-ripple/issues/6)

**Symptom.** After 10 minutes without input (Energy Saver's default), the VM stops answering:
the screen keeps its last picture, ssh and ping are dead, QEMU spins on all CPUs. The kernel has
panicked: `"Error 4 trying to transition CPU 1 to OFF"` at `osfmk/i386/acpi.c:182`, with
AppleACPIPlatform in the backtrace (`scripts/panic.sh` reads it from guest memory; VM 210,
2026-11-11 12:20).

**Workaround:** turn sleep off in the guest: `sudo pmset -a sleep 0 displaysleep 0 disksleep 0`
(set on VM 210 on 2026-11-11). Not the GPU driver: the panic is in the CPU power-down for
sleep.

## KI-7: configd crashes twice at every boot (VM 210)

GitHub issue: [#7](https://github.com/mizaimao/moose-ripple/issues/7)

`configd` dies in its PowerManagement plugin (`PowerSourcesHaveChanged`, a NULL CFType) twice
within ten seconds of booting, and `loginwindow` sometimes follows (a NULL run loop source).
Both restart by themselves; ssh sessions opened in that window can be reset. Seen on every boot
of VM 210 since at least 2026-11-11 03:41. A power-source (battery/ACPI) matter of the VM, not
the GPU driver.

## KI-8: In UTM the pointer was invisible (fixed)

GitHub issue: [#18](https://github.com/mizaimao/moose-ripple/issues/18)

**Symptom.** In UTM 5.0.8 with the 3D display (`virtio-vga-gl`) and the hardware cursor
(`ripple_hwcursor=1`), the pointer was invisible. It moved, and clicks landed where they should.

**Cause: our driver.** Its pointer moves (`MOVE_CURSOR`) carried resource id 0. QEMU takes the
pointer's visibility from that field on a move too, so displays that honour it, such as UTM's
SPICE, hid the pointer on every move. Proxmox's VNC console ignores the field, which is why it
never showed there. A first report to UTM (UTM issue 8139) took it for a UTM bug; it was ours.

**Fixed** in 3d4a5c5 (2026-11-12, checked in UTM). In UTM, keep `ripple_hwcursor=1`: with
Quartz Extreme on, the default software pointer doesn't show there.

## KI-9: Under emulation (UTM on Apple silicon) 10.6.8 sometimes panics early at boot

GitHub issue: [#24](https://github.com/mizaimao/moose-ripple/issues/24)

**Symptom.** The verbose boot stops after `mig_table_max_displ = 73`, and nothing more appears.
The kernel has panicked with `commpage no match for last, next address ffff1780` (read from
guest memory: the panic text doesn't reach the screen this early). Not every boot: on one day
11 boots in a row came up, then 3 in a row stopped here.

**Cause: the emulated TSC runs at exactly 1 GHz.** Under QEMU's TCG on an ARM host the guest's
TSC comes from QEMU's nanosecond clock. OpenCore measures it at 1.00005 to 1.00023 GHz, and the
10.6.8 kernel treats a TSC at or below 1.0000678 GHz as slow; the 64-bit commpage has no routine
for a slow TSC, so the kernel panics. Our drivers aren't loaded yet at that point.

**Fixed** in the OpenCore boot disk: two kernel patches move the threshold to exactly 1 GHz
([boot/opencore/README.md](../boot/opencore/README.md#kernel-patches-1068)). Rebuild the boot
disk (`boot/opencore/build.sh`) to get them. Chameleon has no equivalent.
