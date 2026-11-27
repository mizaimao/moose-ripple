# Installing the Windows 2000/XP display driver

A display driver for QEMU's `virtio-vga-gl` device with host 3D acceleration
(virglrenderer), for x86 Windows 2000 and Windows XP guests. It provides the
desktop, DirectDraw, Direct3D 7, the Direct3D 8 and 9 driver interface and
OpenGL (Mesa 24.3.4), drawn by the host GPU. Releases are tagged
`win2k-vX.Y.Z`.

What's tested: version 0.4.4 on Windows XP SP3 (QEMU with KVM, Intel UHD
Graphics 630 host), with the project's pixel checks, which compare every
picture with Microsoft's reference rasterizer, and with 3DMark 2001 SE,
3DMark03 and 3DMark05. The package is install-tested on fresh Windows XP SP3
and Windows 2000 guests; on Windows 2000, which has no DirectX 8 or 9 runtime
of its own, the Direct3D 7 checks are the ones run.

## What works, and what not yet

Works:

- Direct3D 7 (on by default) with hardware transform and lighting.
- Direct3D 8 and 9, also through the Direct3D 9 driver interface (on by
  default):
  - pixel shaders 1.1 to 1.4 and 2.0, vertex shaders 1.1 and 2.0 with flow
    control on the host GPU, eight fixed-function stages;
  - stencil, 24-bit depth, user clip planes, the scissor test;
  - full-screen devices, back buffers with alpha (in a window and
    full-screen), render targets up to 2048x2048, also cube maps;
  - DXT textures (decoded by the host GPU), cube and volume textures, L8,
    A8L8, A8 and L16 textures, mipmaps of managed textures, and textures past
    the memory the host keeps at once (the least recently used are uploaded
    again);
  - plain surfaces in system memory and in the default pool, `StretchRect`,
    `ColorFill`, `UpdateSurface`, `GetFrontBufferData`;
  - vertex and index buffers kept on the host GPU, 32-bit index buffers,
    lines, points and point sprites.
- OpenGL through Windows' own `opengl32.dll` on the host GPU (Mesa 24.3.4).
- 3DMark 2001 SE, 3DMark03 and 3DMark05 run to the end on XP. Every 3DMark03
  test draws the right picture: seven frames match the reference rasterizer's,
  and its CPU tests match captures from a GeForce 9600 GT.

Not yet:

- Speed. On an Intel UHD 630 host 3DMark03 scores about a third of what a
  GeForce 9600 GT passed through to the same kind of guest does (see
  Measured, below). Where the host GPU does the work the driver is near the
  card, so the time is probably lost in the guest's draw path; that has not
  been profiled.
- 1920x1080 at 32 bits per pixel does not fit in the device's 8 MB of video
  memory ([#121](https://github.com/mizaimao/moose-ripple/issues/121)).
- Wireframe and point fill modes, and some fixed-function texture stage
  combinations, are not drawn
  ([#269](https://github.com/mizaimao/moose-ripple/issues/269)).
- A full-screen program that reads its back buffer before showing it gets an
  older picture ([#327](https://github.com/mizaimao/moose-ripple/issues/327)).
- Managed cube maps and volume textures may be drawn from their top mipmap
  level only; plain surfaces of another depth than the desktop's in the
  default pool are refused; no shader model 3.0
  ([#270](https://github.com/mizaimao/moose-ripple/issues/270),
  [#271](https://github.com/mizaimao/moose-ripple/issues/271)).
- Frames repeat in some benchmarks, and all of them stutter now and then
  ([#273](https://github.com/mizaimao/moose-ripple/issues/273)).
- On Windows 2000, Direct3D 8 and 9 need the DirectX 9.0c runtime (its
  February 2010 redistributable is the last that installs there).

Development notes: [development.md](development.md).

### Measured

Windows XP SP3, 1024x768, one processor, the guest alone on its host, QEMU
started with the clock option below, on 2026-11-27:

| | this driver, Intel UHD 630 host | GeForce 9600 GT passed through |
| --- | --- | --- |
| 3DMark03 | 11009 | 33348 |
| Wings of Fury | 187.2 fps | 974.0 |
| Battle of Proxycon | 113.4 | 290.6 |
| Troll's Lair | 76.4 | 194.9 |
| Mother Nature | 47.8 | 163.1 |
| CPU Test 1, CPU Test 2 | 64.5, 10.5 | 462.7, 101.4 |
| Fill rate, multi-texturing | 8922.9 MTexels/s | 10643.0 |
| Vertex Shader, Pixel Shader 2.0, Ragtroll | 83.7, 160.1, 72.7 fps | 95.8, 344.2, 103.8 |
| 3DMark 2001 SE | 7220 | 53913 |

The card's host has an Intel Core i9-9900K, this driver's an i9-10900K (QEMU
10.0 with KVM, virglrenderer 1.1.0, Mesa 25.0). The card's guest had a Penryn
processor model and 2 GB of memory with NVIDIA's driver 6.14.13.4052, this
driver's a Pentium III model and 512 MB. The card's 3DMark03 run had a small
screen capture every second running in the guest. The single-texturing fill
rate is left out: the guest's clock is not yet checked for that one test.
Scores of this driver published before 0.4.4 were measured without the clock
option and are too high.

## 1. Set up the VM

Give the guest QEMU's `virtio-vga-gl` device and a display with OpenGL, for
example:

```
qemu-system-x86_64 -accel kvm -machine pc -cpu pentium3 -m 512 \
    -rtc base=localtime,driftfix=slew \
    -vga none -device virtio-vga-gl -display sdl,gl=on \
    -drive file=winxp.qcow2,if=ide
```

`-display gtk,gl=on` works too on some hosts (it showed a black window on an
NVIDIA desktop where `sdl,gl=on` worked); `egl-headless` with a VNC display
runs without a window.

Keep `driftfix=slew` in the clock option. While the host draws a heavy scene
a Windows guest misses timer interrupts, and without it the lost time is never
made up: the guest's clock ran at 66 to 77 percent of real time through
3DMark03's game tests, and the benchmark printed a score 29 percent too high.

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
| `RippleTextureMB` | 256 | host memory for Direct3D textures, in megabytes (16 to 1024); past it the textures drawn longest ago are freed and uploaded again when next drawn |

`RippleHardwareCursor`, `RippleGpuDesktop`, `RippleDirectDrawGpuSurfaces` and
`RippleD3D7WaitCurrent` are for tests and stay 0.

`RipplePrimaryInMemory` 1 (being built, issue 121) keeps every 32-bit desktop in
guest memory instead of the 8 MB of VGA memory, which adds 1920x1080 and
1920x1200 (with page flipping, for full-screen programs, at every size). It
takes 18 MB of the guest's memory.

## Licences

The driver is MIT licensed (`LICENSE.TXT`). `ripplegl.dll` is Mesa 24.3.4,
built from `win9x/mesa` in this repository; its notices are in
`THIRDPTY.TXT`.
