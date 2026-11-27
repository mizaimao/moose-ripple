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
Direct3D 8 and 9, also through the Direct3D 9 driver interface (on by
default), with pixel shaders 1.1 to 1.4 and 2.0, vertex shaders 1.1 and 2.0
with flow control on the host GPU (on by default), eight fixed-function
stages, stencil, 24-bit depth, user clip planes, the scissor test,
StretchRect, back buffers with alpha, full-screen devices, render targets up
to 2048x2048 (also cube maps), DXT textures (decoded by the host GPU), cube
and volume textures, L8, A8L8, A8 and L16 textures, 32-bit index buffers,
lines, points and point sprites, and textures past the 64 MB the host keeps
at once (the least recently used are uploaded again); OpenGL through Windows'
own `opengl32.dll` on the host GPU. 3DMark 2001 SE, 3DMark03 and 3DMark05
run to the end on XP.

Not yet: 1920x1080 at 32 bits per pixel does not fit in the device's 8 MB of
video memory ([#121](https://github.com/mizaimao/moose-ripple/issues/121));
point sizes from fixed-function vertices and `POINTSCALEENABLE`; some
fixed-function texture stage combinations are not drawn. On Windows 2000,
Direct3D 8 and 9 need the DirectX 9.0c runtime (its February 2010
redistributable is the last that installs there). Development notes: [development.md](development.md).

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
| `RippleVertexShaders` | 1 | vertex shaders on the host GPU (DirectQ and 3DMark05 need them); 0 leaves them to the runtime |
| `RippleD3D9` | 1 | the Direct3D 9 driver interface (with `RippleVertexShaders` 1); 0 leaves Direct3D 9 programs on the DirectX 8 level interface |
| `RippleHostBuffers` | 1 | vertex and index buffers kept on the host GPU for draws with a vertex shader (with `RippleVertexShaders` 1); 0 sends the vertices with every draw |

`RippleHardwareCursor`, `RippleGpuDesktop`, `RippleDirectDrawGpuSurfaces` and
`RippleD3D7WaitCurrent` are for tests and stay 0.

`RipplePrimaryInMemory` 1 (being built, issue 121) keeps every 32-bit desktop in
guest memory instead of the 8 MB of VGA memory, which adds 1920x1080 and
1920x1200 (with page flipping, for full-screen programs, at every size). It
takes 18 MB of the guest's memory.

## Licences

The driver is MIT licensed (`LICENSE.TXT`). `ripplegl.dll` is Mesa 24.3.4,
the same build as in the Windows 9x package; its notices are in
`THIRDPTY.TXT`.
