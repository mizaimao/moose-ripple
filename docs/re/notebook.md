# Lab notebook

Dated entries: hypothesis, what was done, result, next step (HANDOFF.md rule 4).

## 2026-11-10: dev VM on the retro rig, first boot on virtio-vga

**Hypothesis.** A copy of the owner's Snow Leopard disk boots in QEMU without the 9600 GT, on
`virtio-vga` (no GL yet), using the boot loader's VESA framebuffer.

**Host state.**
- Retro rig: PVE 8.4.0, running kernel 6.5.11-8-pve (pinned by the owner; never change it),
  pve-qemu-kvm 9.2.0-5, libvirglrenderer1 0.10.4-1, libepoxy0 1.5.10-1.
- UHD 630 switched on in the BIOS by the owner today: `00:02.0 [8086:3e98]`, driver i915,
  `/dev/dri/card0` and `renderD128`.
- Mesa's GL/EGL libraries are not installed (only libgbm1), so `vga: virtio-gl` can't run
  yet. `apt-get -s install libgl1 libegl1` adds 28 Debian packages and updates libgbm1
  22.3.6-1+deb12u1 to +deb12u2; no Proxmox package changes. Waiting on the owner's OK.

**Done.**
- VM 210 `ripple-dev`: `qm disk import` of `local:200/slhdd.raw` into
  `local:210/vm-210-disk-0.qcow2` (qcow2, so Proxmox snapshots work). Snapshot `baseline` is the
  untouched copy. VMs 200/201 are unchanged.
- Same machine settings as VM 201 (q35 7.2, SeaBIOS, Penryn args, Chameleon 2.1 r2069 ISO),
  minus the 9600 GT and the host USB devices; 4 GB RAM, new e1000 MAC, `vga: virtio`.
- Added `-device usb-kbd,bus=ehci.0,port=4`: 10.6 has no PS/2 driver, so QEMU's default
  keyboard never reached the guest. The USB keyboard works; keys are sent in chunks of 20
  because bursts drop keys (and once left Shift stuck).

**Result.**
- Chameleon draws at 1024x768 through virtio-vga's VGA BIOS. With `-v`, 10.6 boots to the
  desktop in about 25 s and logs in as the guest user automatically.
- Kernel: `Darwin 10.8.0 xnu-1504.15.3~1/RELEASE_X86_64 x86_64`, so this setup boots the
  64-bit kernel by default.
- System Profiler, Graphics/Displays: Type GPU, Bus PCI, VRAM 3 MB, Vendor ID 0x1af4,
  Device ID 0x1050, Revision 0x0001, "No Kext Loaded", 1024 x 768, 32-bit ARGB8888.
  This confirms the modern virtio-gpu PCI ID from the handoff on this QEMU.
- Guest IP 192.0.2.10 (DHCP). No `/Developer`, so no Xcode in this install.
  Remote Login is off; `sudo` asks for a password.

**Next.** Remote Login and root access for the scripts (needs the owner), then Mesa on the host
(needs the owner) and switch VM 210 to `vga: virtio-gl`.

## 2026-11-10: virtio-gl on the UHD 630; shutdown panic

**Host.** the owner installed `libgl1 libegl1` (Mesa 22.3.6-1+deb12u2, `iris_dri.so`). VM 210 set to
`vga: virtio-gl`: Proxmox runs `-device virtio-vga-gl -display egl-headless,gl=core`. QEMU holds
`/dev/dri/renderD128` (the UHD 630), maps `iris_dri.so` and `libvirglrenderer.so.1.7.7`, and
`info virtio-status` offers `VIRTIO_GPU_F_VIRGL` and `VIRTIO_GPU_F_EDID`. The guest boots to the
desktop as before (VESA framebuffer, 1024x768; no driver yet).

**Shutdown panic.** `osascript ... to shut down` in the guest panicked the kernel (launchd thread):
a backtrace of one repeated return address in `com.apple.driver.AppleSMC` 3.1.0d5, with
`org.netkas.FakeSMC` 2.7.2 also loaded. The install has FakeSMC while the VM also has QEMU's
`isa-applesmc`; the two SMCs are the likely cause (UNVERIFIED). Same disk and args as VMs 200/201.
Until fixed, power VM 210 off with `qm stop` after the disk is idle.

Also seen: the guest volume has only 5.79 GB free of 64 GB.

## 2026-11-10: root and SSH in VM 210

**Done** (the owner's OK; snapshot `pre-root-setup` first). Booted Chameleon with `-s -v`, ran
`fsck -fy` and `mount -uw /`, then in VM 210 only:
- `overrides.plist`: `com.openssh.sshd` Disabled = false (Remote Login on).
- `/etc/sudoers`: appended `<user> ALL=(ALL) NOPASSWD: ALL`; `visudo -c` parsed OK.
- the guest user's `~/.ssh/authorized_keys`: the RSA key from the repo's git-ignored `.secrets/`;
  MD5 fingerprint matched the local copy.
- `exit` continued to a normal boot (no reboot, so no shutdown panic).

**Result.** `scripts/guest.sh` logs in over SSH (10.6 runs OpenSSH 5.2, so the client needs
`+ssh-rsa`), and `sudo -n` works. Third-party kexts loaded: `org.netkas.FakeSMC` 2.7.2 and
`net.osrom.kext.Disabler` 1.0.1. Neither is in `/System/Library/Extensions` and `/Extra` holds
only Themes, so they most likely come from the Chameleon ISO's `/Extra` (UNVERIFIED). No panic
report was saved for the shutdown panic. Disk: GPT, `Macintosh HD` is 34.0 GB (5.4 GB free)
on the 68.7 GB disk, plus an empty `disk0s3`.

## 2026-11-10: more room in VM 210

**Why.** `Macintosh HD` had 5.8 GB free; Xcode 3.2.6, the KDK and test apps need more.
`diskutil resizeVolume disk0s2 limits` said max = current: the GPT still ended at the old
32 GiB mark (last usable LBA 67108830) and held an empty entry 3 ("Untitled", 0 bytes,
start 67108824), left over from an earlier grow of the image.

**Done** (snapshot `pre-disk-resize` first). Booted the Arch Linux ISO in VM 210 with
`nomodeset` (with KMS the Linux virtio-gpu driver takes the scanout and `screendump` says
"no surface"), then `sgdisk -e -d 3 /dev/sda`: last usable LBA 134217694, `sgdisk -v` found
no problems. Back in 10.6, `diskutil resizeVolume disk0s2 68.3G` grew the volume live, and
`diskutil verifyVolume /` says OK. RAM raised from 4 to 8 GB (`hw.memsize` 8589934592).

**Result.** 64 GiB volume, 37 GiB free. Snapshot `resized`. New scripts: `boot.sh` (Chameleon
menu, boot flags, waits for ssh) and `stop.sh` (sync, then `qm stop`).

## 2026-11-10: hello-world kext loads on both kernels

**Hypothesis.** clang 19 + cctools-port ld64-609 + the open-source xnu-1504.15.3 headers make
a universal kext that 10.6.8 loads. Toolchain details: `docs/decisions/0001-toolchain.md`.

**Done.** `kext/hello` (C kmod, logs on start/stop). `scripts/loop.sh RippleHello` builds on
dev.lan, copies to VM 210, takes the rolling snapshot `pre-load`, runs `kextutil`, and shows
the kernel log.

**Result.**
- x86_64 kernel: loads (`kextstat` index 82, 0xffffff7f8098c000), logs "64-bit kernel",
  unloads.
- i386 kernel (`arch=i386`): first attempt failed in kxld, "Invalid segment type in MH_OBJECT
  kext: 36" (LC_VERSION_MIN_MACOSX in the `ld -r` output). After stripping it (and
  LC_DATA_IN_CODE): loads at 0x7a19f000, logs "32-bit kernel", unloads.
- The whole loop takes 6 s (M0 target: under a minute).

**Next.** M0 still lacks the hello-world dylib (needs SDK headers or libSystem stubs) and
kernel debugging with symbols (needs the 10.6.8 KDK, and lldb or a Mach-O-capable gdb on
dev.lan).

## 2026-11-10: hello-world dylib runs in 32- and 64-bit processes

**Done.** `tools/hello-dylib`: a universal i386 + x86_64 dylib that prints from a constructor,
linked against the guest's libSystem (`docs/decisions/0001-toolchain.md`, userspace stopgap).
Test: `DYLD_INSERT_LIBRARIES=/tmp/ripple/hello.dylib arch -x86_64 /usr/bin/true`, then
`arch -i386`.

**Result.** "hello.dylib: loaded in pid 297, 64-bit process" and "... pid 298, 32-bit
process", both exit 0.

**Next.** The last M0 item is kernel debugging with symbols: waiting for the 10.6.8 KDK
(build 10K549) from the owner.

## 2026-11-10: retro rig CPU governor (fan noise)

The owner: the CPU fan was loud. The rig was 99.7% idle but every core sat at ~4975 MHz under
Proxmox's default `performance` governor, and VM 210's small, constant activity kept it at
full turbo. With the owner's OK, switched all 16 CPUs to `powersave` (intel_pstate, EPP
`balance_performance`), live only; a reboot reverts it. Idle: 800 MHz, package 52°C -> 48°C.
Persistent setting: pending the owner's verdict on the noise.

Also: copied into VM 210's `/Users/Shared/ripple-media/` from the owner's Macintosh Repository
downloads (never committed):
- `kernel_debug_kit_10-6-8_10k549.dmg`, 93429770 bytes, SHA-256
  `8dc391def99bc69700bdc1a5e91d4e88451f60c16241ce1940954fb22e018f0a`
- `xcode_3.2.6_and_ios_sdk_4.3.dmg`, 4443150993 bytes, SHA-256
  `47f6fecdce63bb00640a9f99c2a65778c1356630286d8003d597ffb83dfcdf3b`
The KDK image has a license agreement, so `hdiutil attach` asks "Agree Y/N?" and loops at
end of input when run over ssh.

## 2026-11-10: Xcode 3.2.6 in VM 210; builds switch to the 10.6 SDK

**Done** (snapshot `pre-xcode` first). `installer -applyChoiceChangesXML` with the iOS SDK
(`iPhoneSDKMeta`) and the documentation download (`DocDownload`) turned off.

**Result: partial install.** `/Developer` (1.7 GB) arrived: Xcode 3.2.6 (build 10M2518),
`gcc-4.2` (Apple build 5666.3), gdb, MacOSX10.5 and 10.6 SDKs. Then the installer stopped:
"Failed install preflight: The package iPhoneSystemComponents.pkg is untrusted
... CSSMERR_TP_CERT_EXPIRED". Apple's 2011 package-signing certificate has expired, so the
packages checked after that point (iPhoneSystemComponents, DeveloperToolsSystemSupport,
UNIXDevelopmentSupport, which puts `gcc` etc. in `/usr/bin`) didn't install, and no package
receipts were written. Not worked around: that is a macOS security check, so it's the owner's
call (setting the guest clock back to 2011 for the install is the usual workaround).

The SDK (`/Developer/SDKs/MacOSX10.6.sdk`, 240 MB, with `Kernel.framework` headers,
`libkmod.a`, `libkmodc++.a`, `crt1.10.6.o`, OpenGL/IOKit/IOSurface frameworks) is complete.
Copied to dev.lan `.tools/sdk`; the Makefile now uses it (`docs/decisions/0001-toolchain.md`).
RippleHello and hello.dylib rebuilt against it: kext loads/unloads on the i386 and x86_64
kernels, dylib runs in 32- and 64-bit processes.

**KDK.** `hdiutil attach` shows Apple's license first. Accepting it automatically was refused
by Claude Code's safety check, so it waits for the owner to accept it himself. Debugging also
needs lldb on dev.lan (its gdb can't read Mach-O: "not in executable format").

## 2026-11-10: kernel debugging with symbols (M0's last item)

**Setup.** KDK `kernel_debug_kit_10-6-8_10k549.dmg` (license accepted by the owner) copied to
dev.lan `.tools/kdk/10.6.8-10K549`. Its `mach_kernel` and `mach_kernel.dSYM` UUIDs match the
guest's `/mach_kernel`: x86_64 `90350675-2812-3D2D-9223-BBEDAC4C77C0`, i386
`C033B86D-01F9-A03D-E20D-AB676392175C` (`dwarfdump --uuid`). The owner installed lldb 19.1.7 on
dev.lan. `scripts/debug.sh` opens QEMU's gdbstub on the rig's 127.0.0.1:1234 and tunnels it
to dev.lan through the laptop (dev.lan has no ssh access to the rig).

**Result.**
- Attach: lldb finds the kernel at 0xffffff8000200000, slide 0 (no KASLR on 10.6), and gives a
  symbolic backtrace (`machine_idle` < `processor_idle` < `thread_select` ...).
- Kext breakpoint: hardware breakpoint on `ripple_hello_stop` (kext symbols added with
  `target modules load --slide <kextstat address>`), then `kextunload` in the guest: hit on
  thread #3, called from `OSKext::stop() + 316`; detached, the unload finished normally.

**Two traps, both handled in `scripts/debug.sh`.**
- The first resume after attaching returns at once with a stray SIGTRAP from another vCPU
  (`legacy_write+18`, every time). The helper `ripple-continue` (`tools/lldb/ripple_lldb.py`)
  resumes until a real breakpoint stop.
- lldb in batch mode treated that stray stop as a crash, skipped to the end and quit with a
  kill; QEMU's gdbstub obeys a kill by exiting, so VM 210 powered off at 13:18:11 ("QEMU:
  Terminated via GDBstub"). After a reboot `diskutil verifyVolume /` was OK. The script now
  always ends in `process detach`, also on unexpected stops (`-k`).

## 2026-11-10: M1 starts; reference VM 211 with the 9600 GT

**Done** (the owner's go for M1). VM 210 stopped; `qm clone 210 211 --full` (`ripple-ref`), then
`hostpci0: 0000:01:00,pcie=1,x-vga=1,romfile=9600gt-tpu.bin` (as VM 201) and no emulated VGA.
Snapshot `base`. Only one of 210/211 runs at a time (shared card, 16 GB host).

**Result.** Boots through Chameleon (keys sent blind; the display is the real card). System
Profiler: GeForce 9600 GT, 1024 MB, device 0x0622, driving the owner's VS2247 at 1920x1080.
Kexts: `com.apple.GeForce` 6.3.6, `NVDAResman` 6.3.6, `nv50hal` 6.3.6, `IOGraphicsFamily` 2.2.
`screencapture` over ssh works (`scripts/screen.sh -g`): full Quartz Extreme desktop.

**First static pass.** 43 Apple binaries copied to dev.lan `.tools/re/10K549`
([binaries.md](binaries.md)). Findings in [gl-plugin-interface.md](gl-plugin-interface.md) and
[ioaccelerator-interface.md](ioaccelerator-interface.md): 80 shared `gld*` entry points; the
surface user client is in the SDK's own headers; VMsvga2 r166 fetched for reading (not
copying; its own code is all-rights-reserved).

**Next.** dtrace on VM 211: what WindowServer opens and calls on the accelerator, and how a GL
program finds and drives `GeForceGLDriver`.

## 2026-11-10: dtrace traces of WindowServer and of a GL program

- `tools/re/iokit-calls.d` on WindowServer while TextEdit opened and closed a window:
  Quartz Extreme composites through the GL driver plugin; the standard surface selectors
  (9, 17) are used for display surfaces. Details: [ioaccelerator-interface.md](ioaccelerator-interface.md).
- New `tools/cgl-info`: lists renderers, makes a context, clears an FBO, reads a pixel. Works
  on VM 211 in 64- and 32-bit processes and with the software renderer.
- 10.6's dtrace could not see our binaries' modules at first. Userspace links now leave out
  LC_FUNCTION_STARTS, LC_DATA_IN_CODE and LC_VERSION_MIN_MACOSX like Apple's 10.6 programs
  (Makefile `ULD`/`strip_vmin`). The remaining blocker was attaching with `-p` after loading the
  driver; `dtrace -Z -c` works.
- `tools/re/gld-calls.d` on `cgl-info`: full ordered `gld*` sequence with the driver's own
  IOKit calls. Details: [gl-plugin-interface.md](gl-plugin-interface.md), Trace 2.

## 2026-11-10: loader experiment: OpenGL loads our GL bundle

See [gl-plugin-interface.md](gl-plugin-interface.md), "The lookup chain" and "Experiment".
First C++ kext (`RippleAccelProbe`) built with clang 19 after two fixes (C++98 for the SDK
headers; keep global destructors out of `__cxa_atexit`). On VM 210 it pointed the boot
framebuffer at itself, and OpenGL `dlopen`ed `RippleGLDriver.bundle` in a new process.
Renderer queries then failed (expected for a bare pass-through). Kext unloaded cleanly.
Also: the gld* count is 80, not 78 (earlier hand count was wrong; corrected).

## 2026-11-10: shutdown panic fixed

FakeSMC 2.7.2 (and Disabler 1.0.1) load from the Chameleon ISO's `Extra/Preboot.dmg` ramdisk,
and VM 210 also had QEMU's `isa-applesmc`. Removed `isa-applesmc` from VM 210's `args`
(snapshot `pre-smc-fix` first): boots fine, AppleSMC 3.1.0d5 attaches to FakeSMC, and both
`sudo shutdown -h now` and System Events "shut down" power off in about 3 s. Same change on
VM 211. `scripts/stop.sh` now shuts down cleanly and only powers off as a fallback.
VMs 200/201 (the owner's) have the same pair and most likely the same panic; not changed.

## 2026-11-10: M2 step 1: virtio-gpu transport and a 2D test pattern

`kext/virtiogpu` (`RippleVirtioGPU`, own `IOMatchCategory`, so it loads at runtime next to the
boot `IONDRVFramebuffer`). Protocol definitions: Linux v6.12's BSD-licensed virtio headers in
`third_party/virtio`.

- Capabilities all in BAR 2: common 0x1000, ISR 0x1800, device 0x2000, notify 0x3000.
- Features offered: 0x0000010130000003 on `virtio-gl` (VIRGL, EDID, VERSION_1, ...);
  0x...0002 on plain `virtio` (no VIRGL). We accept VERSION_1 only.
- Two queues (control: device max 256, we use 64; cursor: 16). Status 0xf (DRIVER_OK).
  1 scanout, 2 capsets. `GET_DISPLAY_INFO`: scanout 0 1280x800, enabled.
- Test pattern: `RESOURCE_CREATE_2D` (B8G8R8X8), `ATTACH_BACKING` (1000 page entries),
  `SET_SCANOUT`, `TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`: all OK; the screen shows the bars
  ([img](img/2026-11-10-virtiogpu-testpattern.png)), colours right, so B8G8R8X8 matches the
  Mac's 32-bit pixel layout. Unload resets the device and QEMU shows the VGA desktop again.
- Display note: with `vga: virtio-gl` (egl-headless), QEMU's `screendump` says "no surface"
  once the guest scans out through virtio. M2 (2D) is developed on `vga: virtio`; VM 210 is
  switched back to `virtio-gl` for M3.

## 2026-11-10: M2 step 2: RippleFramebuffer drives the desktop

`kext/virtiogpu` is now a boot-time kext (`scripts/install-kext.sh`): `VirtioGPUDevice` (transport,
queues, commands) plus `RippleFramebuffer` (`IOFramebuffer`, `IOMatchCategory` IOFramebuffer,
`IOProbeScore` 30000, which beats `IONDRVFramebuffer`'s 20000 on the same PCI device).

- Works: WindowServer runs on it at the host's preferred 1280x800 (was 1024x768 VESA);
  System Profiler shows 32 MB VRAM, 1280 x 800, 32-bit. [img](img/2026-11-10-framebuffer-desktop-1280x800.png)
  16 modes offered (host size + standard sizes up to 2560x1600). Hardware cursor via the
  cursor queue (`convertCursorImage` to a 64x64 B8G8R8A8 resource); not yet seen on screen,
  since `screendump` doesn't include QEMU's cursor.
- The first screenshot after boot was black only because WindowServer hadn't drawn yet.
- `CurrentPowerState` = 0 on the framebuffer and `AppleDisplay` turned out to be harmless.

**Host CPU, idle desktop** (QEMU process on the retro rig):
| Change | Host QEMU | Guest |
|---|---|---|
| Full 1280x800 transfer at 60 Hz | 96% | idle |
| Damage tracking (shadow frame, send only the changed box) | 113% | 1% sys, but vCPU 0 at 92% on the host |
| Device interrupts suppressed while polling (`VRING_AVAIL_F_NO_INTERRUPT`), ISR read | 58-65% | 89% idle; refresh() 11-13 ms |
| 8-byte compare instead of the kernel's byte-wise `memcmp` | 29% | 94.75% idle; refresh() 1.3 ms |
| Adaptive refresh: 60 Hz while changing, 20 Hz after 30 still frames | 21% | 98.3% idle |

Causes found: (1) the device kept its legacy INTx asserted because the polled driver never
read the ISR, so vCPU 0 serviced interrupts constantly (invisible to the guest's `top`);
(2) the 10.6.8 kernel's `memcmp` is a byte loop (`_memcmp` at 0xffffff800022ac6c, KDK
10K549). The VRAM is also handed out as RAM (`IOSubMemoryDescriptor` of the buffer) rather
than `IODeviceMemory`, which would map it uncached when the guest's cache attributes are
honoured (KVM with a passed-through device).

Statistics are published as `RippleRefresh*` registry properties, not logged.

## 2026-11-10: pointer bugs (the owner's report from the Proxmox console)

The owner: two squashed pointers side by side, and clicks land left of the pointer.

1. **Cursor image: our bug, fixed.** `convertCursorImage` writes the image packed at its own
   width (logged: 8x8 and 32x32, hot spot 4,4), not at the descriptor's 64. Now converted into
   a scratch buffer and re-strided; UPDATE_CURSOR gets the real hot spot and the pointer
   position (top-left + hot spot).
2. **Click offset: not our driver.** New tool `tools/ptrpos` prints the guest's pointer
   position; positions were injected from the rig with QMP `input-send-event` (abs, 0..32767).
   With our framebuffer (1280x800): 0 -> 0, 4096 -> 75.3, 8192 -> 263.6, 12288 -> 451.8,
   16384 -> 640.0, 24576 -> 1016.5, 32767 -> 1279.9 (y the same way). That is a linear map
   stretched 1.176x (about 1/0.85) around the screen centre and clamped at the edges, and
   repeatable (no history, so not acceleration). With our kext removed (Apple's
   `IONDRVFramebuffer`, 1024x768): 8192 -> 210.8, 16384 -> 512.0, 24576 -> 813.2: the same
   1.176x. So it's how 10.6's HID stack handles QEMU's `usb-tablet` (`HIDPointerConvertAbsolute`
   = 0). Root cause not yet found.

## 2026-11-10: resolution switching

New `tools/setmode` (CoreGraphics display configuration, as the Displays pane uses): lists
16 modes from our framebuffer (1024x768 up to 2560x1600; refresh shows as 0 Hz) and switches
permanently. 1280x800 -> 1920x1200 -> 1024x768 -> 1280x800 all OK; the host display follows
each time, and `RippleFramebuffer: mode WxH set` is logged.
[img at 1920x1200](img/2026-11-10-framebuffer-1920x1200.png)
