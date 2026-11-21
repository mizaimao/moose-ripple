# Windows 2000 display driver

A display driver for Windows 2000 (SP4, x86) on QEMU's `virtio-vga-gl`
adapter (PCI `1AF4:1050`), with OpenGL on the host's GPU.

- **Desktop:** 640×480, 800×600 and 1024×768 at 32-bit colour, 60 Hz.
  GDI draws into the adapter's framebuffer. Command Prompt windows,
  mode changes and the Display control panel work as usual.
- **OpenGL:** Windows' own `opengl32.dll` loads `ripplegl.dll`, Mesa
  24.3.4 with its virgl driver. The host renders. It is the same DLL as
  the Windows 9x driver's.
- **DirectDraw:** full-screen programs flip between two pages in video
  memory. Other surfaces live in system memory, where Windows' own
  DirectDraw code draws them faster than the GPU could here (see Speed).
- **Direct3D** is not accelerated: Windows 2000 runs a Direct3D driver
  inside its kernel, where Mesa cannot run.

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

1. Start Windows 2000 on the stock VGA driver. In Device Manager, the
   adapter is a PCI display controller with vendor `1AF4` and device
   `1050`.
2. Open its properties, choose **Driver > Update Driver**, then **Have
   Disk**, and point to `RB2K.INF` on the CD or in the folder with the
   driver files. Accept the prompt about the missing digital signature.
   `NT5INST.EXE`, run as an administrator from the same folder, does the
   same without the wizard.
3. Restart. If Windows has not switched already, choose 1024×768 and
   32-bit colour in Display Properties. An update keeps the mode you had.

The INF installs `rb2kmp.sys` (the miniport), `rb2kdisp.dll` (the display
driver) and `ripplegl.dll` (OpenGL), and registers the OpenGL driver under
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\RIPPLEGL`.

## Speed

Frames a second on the three test hosts (see The virtual machine), one
Pentium III, 1024×768×32 desktop:

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
virtio-gpu side of the adapter (`gpu_transport.c`): one queue of 16
descriptors, one request at a time, each waited for.

The display driver (`display.c`) gives GDI a 32-bit bitmap over the
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
For tests, set the DWORD `RippleDirectDrawGpuSurfaces` to 1 in the
miniport's `Device0` key and restart.

## Build

On the Linux build host, with the Windows 2000 DDK and Visual C++ 6.0 under
Wine in `.tools/win9x` (or `WIN9X_TOOLS`):

    win2k/build.sh                  # build/win2k: the driver files, the INF and the tools
    win9x/mesa/build.sh             # ripplegl.dll (docs/win9x/mesa.md)

## Tools

- `w2kprobe.exe`: hardware ID and modes; `/pattern` and `/pattern800`
  check colours and a mode change; `/gpu` draws one pixel on the host;
  `/flip` checks full-screen page flipping; `/ddraw`, `/ddraw-full` and
  `/window` check the GPU surfaces when they are switched on.
- `ddbench.exe`: DirectDraw frame rates through the driver, Windows'
  software path and DirectDraw's own choice.
- `rvdstat.exe`: the time spent in each OpenGL request since Windows
  started, the requests that failed, and what programs hold now.
- `flipcycle.exe`: rebuilds a full-screen flip chain four times in one
  DirectDraw session and checks sprites, flips and locks each time.
- `memtest.exe`: times a compute loop, ordinary memory, GDI drawing into
  memory and on the screen, and copies to and from the screen, to tell a
  slow framebuffer from a slow CPU or a slow GPU path.
- `regmode.exe`: the desktop mode Windows has saved and the one in use;
  `/set 1024x768` saves a mode.

## Known limits

- Direct3D is not accelerated (see above).
- Only 32-bit colour: 640×480, 800×600 and 1024×768.
- Each OpenGL context that draws leaves about 1.2 KB in the program's own
  heap (`threadtest`: 300 contexts in one thread); the GPU's buffers and
  contexts are all returned (`rvdstat` shows none held afterwards).
- OpenGL contexts and buffers of a program that ends without releasing
  them are freed at the next context or buffer creation, not at once.
- `ripplegl.dll` is shared with the Windows 9x driver; the Windows 2000
  path in its winsys is used only when Windows 98's kernel driver is absent.
