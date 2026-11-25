# Installing the Windows Vista and Windows 7 display driver

A display driver for QEMU's `virtio-vga-gl` device with host 3D acceleration
(virglrenderer), for 64-bit Windows Vista SP2 and Windows 7 guests. The
desktop, Aero and Direct3D are drawn by the host's graphics card. Releases
are tagged `vista7-vX.Y.Z`.

The driver is test-signed: **Windows has to run in test mode**
([below](#2-switch-test-mode-on)). Only 64-bit Windows is supported; there is
no 32-bit kernel driver. 32-bit programs on 64-bit Windows are served.

## What works, and what not yet

Works on both systems: the desktop at the host window's size, the hardware
pointer, Aero (glass, Flip 3D, thumbnails), Direct3D 9 for 64-bit and 32-bit
programs with shaders up to model 3, multisampling, DXT textures, DirectDraw
and Direct3D 7-era programs through Windows' own layers, video playback
(Windows Media Player, DreamScene on Vista), gamma ramps on Windows 7, and
recovery after a GPU timeout. 3DMark06 runs all six of its tests on both.

Windows 7 also gets Direct3D 10 where the host's renderer offers OpenGL 3.2
or later (a Linux host with a desktop GPU). On a host that offers OpenGL ES
(UTM on Apple silicon) there is no Direct3D 10, and Aero runs on the
Direct3D 9 part. Vista has Direct3D 9 only.

Not yet:

- OpenGL: Windows' own software OpenGL 1.1 is all a program gets.
- Direct3D 10 on Vista, Direct3D 11 on either.
- Sleep and hibernation: the picture does not come back. The Windows 7
  package switches both off for the machine; on Vista switch them off
  yourself.
- More than one screen.
- 32-bit Windows.

On UTM for Apple silicon the guest's processor is emulated, so everything is
much slower than on a host that runs the guest on its own processor, and
that host's renderer takes no DXT textures (the driver unpacks them itself).

## 1. Set up the VM

Give the guest QEMU's `virtio-vga-gl` device and a display with OpenGL, for
example:

```
qemu-system-x86_64 -accel kvm -machine q35 -cpu host -smp 4 -m 4G \
    -vga none -device virtio-vga-gl -display sdl,gl=on \
    -drive file=win7.qcow2,if=ide
```

`egl-headless` with a VNC display runs without a window. On Proxmox VE the
display type is `virtio-gl` (`vga: virtio-gl`). In UTM choose the display
card `virtio-vga-gl (GPU Supported)`.

Two settings make Windows' clock keep time while a game runs, which
otherwise shows as stutter and as benchmark scores that are too high:
`-machine hpet=off` and `-rtc driftfix=slew` (Proxmox VE sets both for its
Windows guests; in UTM add them as extra QEMU arguments).

Install Windows with its standard VGA driver first.

## 2. Switch test mode on

64-bit Windows loads a kernel driver only when it carries a signature that
chains to a certificate Microsoft issued for that purpose. This project has
no such certificate. The kernel driver is signed with the project's own test
certificate instead, and Windows accepts a test signature only in test mode.

Test mode lets every test-signed kernel driver load, not only this one. Use
it in a virtual machine kept for this purpose, not on a machine whose safety
matters to you.

In a Command Prompt started with **Run as administrator**:

```
bcdedit /set testsigning on
```

Restart Windows. "Test Mode" then shows in the corners of the desktop.

The signature is SHA-1, which Vista SP2 and every Windows 7 can check; no
Windows update is needed for it. Nothing has to be added to Windows'
certificate stores.

## 3. Install

1. Download `moose-ripple-vista7-VERSION.zip` from
   [Releases](https://github.com/mizaimao/moose-ripple/releases) and unpack
   it into a folder in the guest (`SHA256SUMS.txt` has its checksum).
2. Device Manager, Display adapters, right-click the adapter (Standard VGA
   Graphics Adapter), Update Driver Software, Browse my computer for driver
   software, Let me pick from a list of device drivers on my computer, Have
   Disk, and choose `viogpu3d.inf` in the `vista` or the `win7` folder.
3. Windows warns in red that it cannot verify the publisher of the driver
   software. Choose **Install this driver software anyway**.
4. Restart Windows.

From an administrator's Command Prompt the same is
`pnputil -i -a win7\viogpu3d.inf` (or `vista\viogpu3d.inf`), with the same
warning to confirm.

After the restart Device Manager lists the adapter as "moose-ripple
virtio-gpu 3D". Choose an Aero theme in Personalization if Windows did not
switch to one itself; on Windows 7, re-running the Windows Experience Index
assessment turns Aero on.

If test mode is off, the driver does not start: Device Manager shows the
adapter with error code 52 and Windows stays on its VGA driver.

## Removing it

1. Device Manager, Display adapters, right-click "moose-ripple virtio-gpu
   3D", Uninstall, tick **Delete the driver software for this device**, OK.
2. Restart Windows. It returns to the Standard VGA Graphics Adapter.
3. Switch test mode off if nothing else needs it, and restart:
   `bcdedit /set testsigning off`.
4. Windows 7 only: to allow sleep and hibernation again, as administrator:

   ```
   reg delete "HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings\abfc2519-3608-4c2a-94ea-171b0ed546ab" /f
   powercfg -h on
   ```

If the screen stays black after an install, start Windows in Safe Mode (F8
before Windows starts). Safe Mode uses the VGA driver; remove the driver
there.

## Switches

DWORD values in the adapter's driver key,
`HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\000N`;
restart after a change.

| Value | Default | Meaning |
| --- | --- | --- |
| `HWCursor` | 1 | the pointer drawn by the host over the picture; 0 lets Windows draw it |
| `FlexResolution` | 1 | the desktop follows the size of the host's window |

The user-mode driver reads environment variables, for finding faults:

| Variable | Meaning |
| --- | --- |
| `RIPPLE_UMD_LOG` | a file the driver writes its messages to |
| `RIPPLE_UMD_TRACE` | 1 to 3: how much of each Direct3D 9 call goes to that file |
| `RIPPLE_UMD_TIMES` | 1: a report of where each frame's time went |
| `RIPPLE_NO_D3D10` | 1: no Direct3D 10 device (Windows 7) |
| `RIPPLE_NO_MSAA` | 1: no multisampling |

The kernel driver writes its warnings to the guest's first serial port
(COM1). `-serial file:ripple-com1.log` on QEMU's command line keeps them in
a file on the host, which is what a bug report needs.

## Licences

The driver is MIT licensed (`LICENSE.txt`). The kernel driver is derived from
virtio-win's `viogpu3d` (BSD-3-Clause). The user-mode driver is Mesa 24.3.4
(MIT) with Direct3D 9 files from Yttrium's Mesa fork and vmlab-win3d (both
MIT) and statically linked runtime code from MinGW-w64 and LLVM. Their
notices are in `THIRD-PARTY-NOTICES.txt`. How the package is built, signed
and packed: `windows/package/make-release.sh`.
