# Installing the Windows 2000/XP display driver

A display driver for QEMU's `virtio-vga-gl` device with host 3D acceleration
(virglrenderer), for x86 Windows 2000 and Windows XP guests. It provides the
desktop, DirectDraw, Direct3D 7, the Direct3D 8 and 9 driver interface and
OpenGL (Mesa 24.3.4), drawn by the host GPU. Releases are tagged
`win2k-vX.Y.Z`.

What's tested: version 0.4.0 installed from its package on fresh Windows XP
SP3 and Windows 2000 guests (QEMU 9.2 with KVM, virglrenderer 0.10.4, Intel
UHD Graphics 630 with Mesa 22.3), then the project's pixel checks: all pass on
XP; on Windows 2000, which had no DirectX 8 or 9 runtime, the Direct3D 7
checks pass.

## What works, and what not yet

Works: Direct3D 7 (on by default) with hardware transform and lighting;
Direct3D 8 and 9 through the DirectX 8 level interface with pixel shaders
1.1 to 1.3 and 2.0, fixed-function stages, stencil, 24-bit depth, back buffers
with alpha, full-screen devices, render target textures, DXT textures (decoded
by the host GPU), cube and volume textures, L8, A8L8 and A8 textures; OpenGL
through Windows' own `opengl32.dll`. 3DMark03 runs to the end on XP.

Not yet: many 3DMark03 pictures are wrong (dark scenes, white water); vertex
shaders are off by default and have no flow control when on; 1920x1080 at
32 bits per pixel does not fit in 8 MB of video memory; Direct3D 8 and 9 on
Windows 2000 are untested. Development notes: [development.md](development.md).

## 1. Set up the VM

Give the guest QEMU's `virtio-vga-gl` device and a display with OpenGL, for
example:

```
qemu-system-x86_64 -accel kvm -machine pc -cpu pentium3 -m 512 \
    -vga none -device virtio-vga-gl -display sdl,gl=on \
    -drive file=winxp.qcow2,if=ide
```

`-display gtk,gl=on` works too on some hosts (it showed a black window on an
NVIDIA desktop where `sdl,gl=on` worked); `egl-headless` with a VNC display
runs without a window.

## 2. Install

1. Download `moose-ripple-win2k-VERSION.zip` from
   [Releases](https://github.com/mizaimao/moose-ripple/releases) and copy its
   files to a folder in the guest (`SHA256SUMS.txt` has its checksum).
2. Device Manager, Display adapters, the adapter's Properties, Driver,
   Update Driver. Choose to pick the driver yourself, Have Disk, and select
   `RB2K.INF` in that folder. Confirm the unsigned driver.
3. Restart Windows. The desktop comes up at 1024x768, 32 bits per pixel.

## Removing it

In Device Manager, uninstall the display adapter and restart; Windows returns
to its standard VGA driver. `rb2kdisp.dll`, `rbntmode.dll` and `ripplegl.dll`
in `system32` and `rb2kmp.sys` in `system32\drivers` may then be deleted.

## Registry switches

DWORD values in the adapter's key under
`HKLM\SYSTEM\CurrentControlSet\Control\Video\{...}\0000`; restart after a
change.

| Value | Default | Meaning |
| --- | --- | --- |
| `RippleNativeD3D7` | 1 | Direct3D drawn on the host GPU |
| `RippleD3D8` | 1 | the Direct3D 8 and 9 driver interface |
| `RippleHostDxt` | 1 | DXT textures decoded by the host GPU; 0 decodes them in the guest |
| `RippleVertexShaders` | 0 | 1 runs vertex shaders on the host GPU (needed by DirectQ and 3DMark05) |
| `RippleD3D9` | 0 | 1 offers the Direct3D 9 driver interface (with `RippleVertexShaders` 1; being completed) |

`RippleHardwareCursor`, `RippleGpuDesktop`, `RippleDirectDrawGpuSurfaces` and
`RippleD3D7WaitCurrent` are for tests and stay 0.

## Licences

The driver is MIT licensed (`LICENSE.TXT`). `ripplegl.dll` is Mesa 24.3.4,
the same build as in the Windows 9x package; its notices are in
`THIRDPTY.TXT`.
