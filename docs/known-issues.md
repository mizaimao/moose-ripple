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

## KI-10: A wrong SMC key looks like a network hang

No GitHub issue: a setup pitfall, not a bug.

**Symptom.** The verbose boot stops after `Ethernet [Intel8254x]: Link up on en0` and no desktop
appears; loginwindow, fontd, mds and configd crash every few seconds. Seen in UTM on 2026-11-13.

**Cause.** QEMU didn't get the real SMC key and silently uses its dummy key (`info qtree` in the
monitor shows "This is a dummy key"). In UTM this happens when the key is written in quotes: UTM
keeps the quotes as part of the value. Curly quotes from a copied web page do the same anywhere.

**Fix:** write the key exactly; in UTM without quotes
([docs/install.md](install.md#utm-on-an-apple-silicon-mac)).

## KI-11: Changing the resolution with Quartz Extreme on crashed WindowServer (fixed in 0.6.0)

GitHub issue: [#161](https://github.com/mizaimao/moose-ripple/issues/161). Fixed on 2026-11-26; release 0.2.2
and earlier have it.

**Symptom.** With `ripple_gl=1 ripple_qe=1`, changing the display size while the system runs
(`tools/setmode`, from 1280x800 to 1920x1080 and to 1024x768) crashed WindowServer: the user was
logged out and the screen came back at the old size. Found on 2026-11-25 on the Proxmox host;
the same with release 0.2.2, and with queued submits on or off. System Preferences is not
tried yet.

**What is known.** WindowServer's main thread overflows its stack inside QuartzCore
(`fe_gl_texture_load` calling itself through `fe_buffer_texture`). Traced with dtrace over the
change: right after WindowServer has reconfigured its displays, attaching a new pbuffer fails
with 10005, the texture from that pbuffer with 10008 and a new context with 10007. All three
fail inside CoreGraphics before the driver is asked; the driver only sees its drawable taken
away.

**Cause.** While it reconfigures a display, WindowServer gives up its reference to the
accelerator service and looks the service up again, but its OpenGL side goes on using the port
it had. Nothing else in the process held a reference, so that port name died, and every
surface WindowServer asked for afterwards failed (`IOAccelCreateSurface` returned
0x10000003, an invalid destination).

**Fix.** The GL bundle keeps a reference to the accelerator service for as long as the process
lives (`cgl_device.c`). Checked with Quartz Extreme on: 1280x800 to 1920x1080, to 1024x768 and
back, no crash, the picture right each time.

**Workaround for 0.2.2 (checked):** boot once without the two flags, choose the size (it works
there), then put the flags back and restart. The size is kept, and Quartz Extreme runs at it.

## KI-12: In UTM, Core Image filled the area outside a picture with its edge pixels (fixed in 0.6.0)

GitHub issue: [#206](https://github.com/mizaimao/moose-ripple/issues/206). Found and fixed on 2026-11-26; release 0.2.2
and earlier have it.

**Symptom.** In UTM on an Apple silicon Mac, a Core Image filter that reads pixels from outside
its input picture gets the picture's edge pixels there instead of transparent black: a pinch
or twirl shows smeared streaks where it should be empty, a blurred picture does not fade out
at its edge. `tools/ci-effects filters` with a 960x600 picture: 7 of 24 filters differ from
the software renderer on more than 1% of the pixels (CITwirlDistortion 10.9%,
CIPinchDistortion 9.4%, CIGaussianBlur and CIGloom 8.0%, CIMotionBlur 6.2%, CIEdgeWork 1.7%,
CIPointillize 1.5%). On a host with desktop OpenGL the same run gives none.

**Cause.** Core Image sets `GL_CLAMP_TO_BORDER` with a border colour on its rectangle
textures. UTM's host renderer is OpenGL ES (ANGLE on Metal) and has no sampler border colours;
it logs "Sampler border color setting requested but not supported" for each such sampler, and
lookups outside the picture fall back to its edge.

**Fix.** On such a host the driver now does the border in the fragment program itself (Mesa
patch `0014`; [re/m6-notes.md](re/m6-notes.md), "Border colours on a host without them").
With it the same run gives 1 of 24 (edge work, 1.5%), and UTM differs from the software
renderer only where the Intel hosts do. It covers ARB and fixed-function fragment programs
with 2D and rectangle textures, which is what Core Image and WindowServer use; a GLSL
program that clamps to a border still gets the edge there.

## KI-13: A swap rectangle is ignored for windows the kernel shows

GitHub issue: [#235](https://github.com/mizaimao/moose-ripple/issues/235). Found on 2026-11-26; release 0.6.0 and
earlier have it.

**Symptom.** A program can ask that a swap shows only part of its window (`kCGLCPSwapRectangle`
with `kCGLCESwapRectangle` on; Core Animation presents only the changed part of a view that
way). With Quartz Extreme on, a GL window is shown by the kernel straight from the GPU, and
there the rectangle is ignored: every swap shows the whole back buffer.
`tools/gl-feature-check/swaprect-check` should end with a red window whose bottom quarter is
green and whose top right corner is blue; on this driver the window is blue all over.

**Who sees it.** No real program is known to show a difference: Photo Booth, the Finder's
animations and Real Racing 2, which all draw through Core Animation, looked right in earlier
checks. A program that leaves the rest of its back buffer stale and counts on the rectangle
would show the stale part.

**Workaround.** `RIPPLE_GL_DIRECT=0` in the program's environment shows its window the older
way (each frame read back into a private window), which applies the rectangle (checked
2026-11-26: `swaprect-check` then shows what it should); it is slower.

## KI-14: A GL call could crash after a context was destroyed from another thread (fixed in 0.6.0)

GitHub issue: [#240](https://github.com/mizaimao/moose-ripple/issues/240). Found and fixed on 2026-11-26; release
0.2.2 and earlier have it.

**Symptom.** A program crashes inside the OpenGL driver at the first GL call on a new context,
or when it switches contexts. Seen once: ScreenSaverEngine, as the screen saver started
(`EXC_BAD_ACCESS` in `update_framebuffer`, called from `glClear`).

**Cause.** The driver makes a context current for Mesa lazily, and Mesa's pointer to it stays
in the thread after the program has stopped using the context. A program that destroys the
context from another thread leaves the first thread pointing at freed memory; garbage-collected
programs do that all the time (the collector's thread destroys what the main thread used:
ScreenSaverEngine, System Preferences). The next context can be given the same address and
then looked "already current" without ever having been bound, and at the next switch Mesa
flushed the context it believed current, which was gone.

**Fix.** A context notes the thread it is current on, and an entry takes "already current"
only if both the address and the thread match; before a switch, a pointer that is not a living
context of this thread is dropped unfollowed. `tools/gl-feature-check stale` draws with a
context on one thread and destroys it from another, a hundred times: Apple's software renderer
passes, the driver as it was before the fix crashes in the second round, the fixed driver
passes.
