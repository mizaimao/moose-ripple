# Windows 2000 display driver

A display driver for Windows 2000 (SP4, x86) on QEMU's `virtio-vga-gl`
adapter (PCI `1AF4:1050`), with OpenGL on the host's GPU.

The development build also runs on 32-bit Windows XP Professional SP3.
On Intel KVM and in UTM on Apple silicon, the driver installed over stock
VGA and restored the saved desktop after full-screen DirectDraw programs.
[Windows XP tests](../docs/win2k/development.md#windows-xp-sp3-x86)

- **Desktop:** 640×480 to 1680×1050 and 1600×1200 at 32-bit colour, 60 Hz:
  640×480, 800×600, 1024×768, 1152×864, 1280×720, 1280×800, 1280×960,
  1280×1024, 1440×900, 1600×900, 1600×1200 and 1680×1050.
  GDI draws into the adapter's framebuffer. Command Prompt windows,
  mode changes and the Display control panel work as usual. The development
  build also offers 16-bit RGB565 and 8-bit indexed colour at these sizes.
  QEMU's virtio-vga has 8 MB of framebuffer, so full-screen DirectDraw page
  flipping, which needs two screens of it, works at 32 bits up to 1280×800,
  and 1920×1080 at 32 bits does not fit.
  [Screen sizes](../docs/win2k/development.md#screen-sizes-above-1024768)
  Windows 2000 SP4 and XP SP3 passed native GDI and DirectDraw page-flip
  checks at both depths, including palette animation on an 8-bit
  video-memory surface.
  [Display-mode tests](../docs/win2k/development.md#16-bit-colour-development)
- **OpenGL:** Windows' own `opengl32.dll` loads `ripplegl.dll`, Mesa
  24.3.4 with its virgl driver. The host renders. It is the same DLL as
  the Windows 9x driver's. By default GDI displays the finished frames.
  An experimental GPU presentation path is opt-in. For Quake results and
  per-operation timings, see the [presentation tests](../docs/win2k/development.md#experimental-gpu-presentation-for-opengl).
- **DirectDraw:** full-screen programs flip between two pages in video
  memory. Other surfaces live in system memory, where Windows' own
  DirectDraw code draws them faster than the GPU could here (see Speed).
- **Direct3D 7** has an experimental native hardware driver for 32-bit
  render targets. Both Windows 2000 and XP passed pixel tests for triangle
  lists, indexed triangles, 16-bit depth testing, textures in four formats,
  texture modulation, alpha test, blending, culling and flat shading.
  [Texture and blending tests](../docs/win2k/development.md#native-textures-blending-and-culling) ·
  [Depth tests](../docs/win2k/development.md#native-16-bit-depth-rendering)

## The virtual machine

The driver was tested on QEMU's `pc-i440fx-2.12` machine with one
Pentium III CPU and 512 MB of memory on three hosts: UTM on Apple silicon
(emulated CPU), Proxmox VE 8.4 with an Intel UHD 630 (QEMU 9.2 with KVM,
`-display egl-headless`), and Windows 11 through WSL2 with an AMD CPU and
an NVIDIA GPU (QEMU 8.2.2 with KVM, virgl on Mesa's Direct3D 12 driver,
`GALLIUM_DRIVER=d3d12`). The display
adapter needs host 3D (virgl) and no 64-bit host-memory window, which
32-bit Windows 2000 cannot place:

    -device virtio-vga-gl,hostmem=0,blob=off,venus=off

with a display that has OpenGL turned on (for example `-display sdl,gl=on`
or SPICE with `gl=on`). The UTM test VM had the display card
`virtio-vga-gl,id=nt5-gl` and the QEMU arguments
`-set device.nt5-gl.hostmem=0 -set device.nt5-gl.blob=off`
`-set device.nt5-gl.venus=off -set device.nt5-gl.neptune=off`.

## Install

1. Start Windows 2000 SP4 or XP Professional SP3 on the stock VGA driver.
   In Device Manager, the adapter is a PCI display controller with vendor
   `1AF4` and device `1050`.
2. Open its properties, choose **Driver > Update Driver**, then **Have
   Disk**, and point to the folder containing `RB2K.INF`. For a driver CD
   with the INF at its root, enter the CD drive (for example, `D:\`).
   Accept the prompt about the missing digital signature.
   `NT5INST.EXE`, run as an administrator from the same folder, does the
   same without the wizard.
3. Restart. If Windows has not switched already, choose 1024×768 and
   32-bit colour in Display Properties. An update keeps the mode you had.

The INF installs `rb2kmp.sys` (the miniport), `rb2kdisp.dll` (the display
driver) and `ripplegl.dll` (OpenGL), and registers the OpenGL driver under
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\RIPPLEGL`.
On the XP development branch, keep `rbntmode.dll` alongside the INF.
Windows Setup uses it to initialize the new adapter's saved desktop mode on XP.

## Speed

The table shows Windows 2000 frame rates on the three test hosts. Each VM
used one Pentium III and a 1024×768×32 desktop.

| Program | UTM, Apple silicon | Intel UHD 630, KVM | WSL2, NVIDIA, KVM |
| --- | ---: | ---: | ---: |
| Quake III Arena demo, `timedemo demo001`, 640×480 in a window | 85 | 210 to 240 | 320 |
| the same, 1024×768 full-screen | 82 | not measured | not measured |
| `gltest` (a small OpenGL scene, 320×240) | 590 | 600 to 1500 | 350 |
| DirectDraw, fill a 640×480 surface and copy it to a window | 840 | 11 800 | 17 800 |
| DirectDraw, full-screen fill and flip, 640×480 | 1170 | 16 000 | 7 100 |
| the same with 100 colour-keyed 64×64 sprites a frame | 440 | 5 200 | 4 400 |

In UTM the emulated CPU running the program limits the frame rate; the
driver's requests take about a fifth of a Quake III frame there. The Intel
host's numbers vary from run to run; the ranges are four runs each. The
DirectDraw rows are what a program gets when it lets DirectDraw choose
where its surfaces live. [docs/win2k/development.md](../docs/win2k/development.md)
has the details and the earlier measurements behind these choices.

## How it works

The miniport (`rb2kmp.c`) sets the display mode through the adapter's
Bochs-compatible registers and maps the framebuffer. It also drives the
virtio-gpu side of the adapter through a 16-descriptor control queue in
`gpu_transport.c`. Each request waits for its descriptors to be returned.
With the adapter DWORD `RippleHardwareCursor=1`, 32-bit display modes
also use the virtio cursor queue. Supported pointer shapes occupy a
64×64 surface, and moving them leaves the desktop pixels alone. GDI
handles pointers with destination-dependent inverted pixels. The stock
installation uses the software pointer.

The display driver (`display.c`) gives GDI a bitmap in the selected colour depth over the
framebuffer and answers `OPENGL_GETINFO` with `RIPPLEGL`, the name of the
OpenGL driver. Mesa's virgl winsys (`win9x/mesa`) talks to the GPU through
the interface of the Windows 9x kernel driver (`win9x/minivdd/rvd.h`); on
Windows 2000 it sends those calls with `ExtEscape` to the display driver
(`display_rvd.c`, `rb2krvd.h`), and the miniport carries them out
(`gpu_rvd.c`). A buffer's guest pages stay attached to its host resource
and are mapped into the program that made it (`gpu_memory.c`). Contexts
and buffers that an ended program left behind are freed at the next
creation.

DirectDraw (`display_ddraw.c`) offers the second framebuffer page as a
back buffer for full-screen flipping. Offscreen surfaces as host textures
(`gpu_surface.c`) are switched off: copying them to the screen means a GPU
readback into the CPU-drawn desktop, which is two to five times slower
than Windows' own code, and they cannot be copied into the back buffer.
For tests, set the DWORD `RippleDirectDrawGpuSurfaces` to 1 in the adapter
key named by `HKLM\HARDWARE\DEVICEMAP\VIDEO` and restart. For this
driver the key is under `Services\rb2kmp\Device0` on Windows 2000 and
`Control\Video\{GUID}\0000` on XP. The optional host textures require a
32-bit display mode. In 8-bit and 16-bit modes, offscreen surfaces use system memory.
For native Direct3D 7 tests, set both `RippleDirectDrawGpuSurfaces` and
`RippleNativeD3D7` to 1 in that key and restart.

## Build

The Linux build uses the msvcrt variant of LLVM-MinGW, the Windows 2000
DDK headers and import libraries, and the Visual C++ 6.0 headers. Set
`LLVM_MINGW` and `WIN9X_TOOLS` if the tools are outside `.tools/`.

    win2k/build.sh                  # build/win2k: the driver files, the INF and the tools
    win9x/mesa/build.sh             # ripplegl.dll (docs/win9x/mesa.md)

## Tools

- `w2kprobe.exe`: hardware ID and modes; `/pattern` and `/pattern800`
  check colours and a mode change; `/gpu` draws one pixel on the host;
  `/flip` checks full-screen page flipping; `/ddraw`, `/ddraw-full` and
  `/window` check the GPU surfaces when they are switched on.
  Use `/pattern16` for GDI colours at 800×600 and `/flip16` for 16-bit
  DirectDraw fills, locks, page flips and desktop restoration.
  `/pattern8` and `/flip8` exercise 8-bit modes. The flip test changes a
  palette entry while two adjacent pixels keep their original indices.
  `/pattern WxHxB` and `/flipat WxHxB` (for example `/pattern 1280x1024x32`)
  run the colour check, or the flip test with that size as its larger case,
  in any offered mode.
- `ddbench.exe`: DirectDraw frame rates through the driver, Windows'
  software path and DirectDraw's own choice. The full-screen benchmark
  changes its fill colour every frame, so the display flashes during the test.
- `rvdstat.exe`: the time spent in each OpenGL request since Windows
  started, the requests that failed, and what programs hold now.
- `flipcycle.exe`: rebuilds a full-screen flip chain four times in one
  DirectDraw session and checks sprites, flips and locks each time.
- `memtest.exe`: times a compute loop, ordinary memory, GDI drawing into
  memory and on the screen, and copies to and from the screen, to tell a
  slow framebuffer from a slow CPU or a slow GPU path.
- `regmode.exe` reports the saved and current desktop modes.
  `/check 1024x768` returns success when both are 1024×768×32 at 60 Hz.
  `/set 800x600` saves and applies that resolution.
- `ipixels.exe hal` draws an indexed triangle and checks two frames against
  the expected colours. Use `ipixels.exe rgb` for Microsoft's software
  reference. `z16pixels.exe hal` and `z16pixels.exe rgb` check which
  pixels survive when two triangles overlap at different depths.

## Known limits

- Stock Direct3D Quake 0.73 draws its level, models and status bar through
  the driver's T&L device on Windows 2000 and XP, at about 130 and 123 fps
  in a 640×480 window with a 1 ms timer period; Microsoft's RGB device runs
  the same demo at about 85 fps. The T&L device also applies Direct3D lights,
  and fog, two texture stages and mipmaps are drawn. The Direct3D 8 and 9
  driver interfaces remain open.
  [Direct3D work](https://github.com/mizaimao/moose-ripple/issues/106)
- 8-bit and 16-bit full-screen game compatibility awaits guest testing.
- Each OpenGL context that draws leaves about 1.2 KB in the program's own
  heap (`threadtest`: 300 contexts in one thread); the GPU's buffers and
  contexts are all returned (`rvdstat` shows none held afterwards).
- OpenGL contexts and buffers of a program that ends without releasing
  them are freed at the next context or buffer creation, not at once.
- `ripplegl.dll` is shared with the Windows 9x driver; the Windows 2000
  path in its winsys is used only when Windows 98's kernel driver is absent.
