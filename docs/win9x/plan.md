# Windows 98 and 2000: plan

Goal: hardware-accelerated 2D, OpenGL and Direct3D for Windows 98 SE and Windows 2000 in QEMU,
through the same virtual card as the Mac OS X and Windows 7 work: virtio-gpu with virgl
(`virtio-vga-gl`). No modified QEMU, so it works wherever that card does (Proxmox VE, plain QEMU,
UTM, WSL2). Our own code, MIT licensed. Windows 95 comes after 98 (same driver model, older
DirectX); a Windows 2000 driver should also run on XP.

## What exists, and how we use it

- **qemu-3dfx** (a modified QEMU that passes Glide and OpenGL calls from the guest to the host)
  and **SoftGPU** (DirectX and OpenGL drivers for Windows 9x, Mesa-based) are references and
  benchmarks: what a period game should look like and how fast it can run. Both are under
  licences we don't copy from (GPL parts); we read them, run them, compare against them.
- **Microsoft's DDKs** (driver development kits: headers, tools, sample drivers) are for reading
  and for building. Nothing from them is committed: no headers, no samples, no binaries. Our
  code is written from the documented interfaces.
- **Mesa** (MIT) is used as on the other systems: its virgl driver turns OpenGL into commands
  for the host's GPU.

## Driver model, in short

**Windows 98** (Windows 9x display model):
- a 16-bit display driver (`.drv`, built on Microsoft's DIB engine) for GDI, the Windows 2D
  drawing;
- a mini VDD (`.vxd`, a 32-bit kernel module) for mode switches and the VGA state of DOS boxes;
- a 32-bit DirectDraw and Direct3D HAL (the driver side of DirectX) in a DLL, called by the
  DirectX runtime;
- an OpenGL ICD (installable client driver: a DLL that `opengl32.dll` loads), here Mesa;
- our own VxD that owns the virtio-gpu device: virtqueues, interrupts, memory, and the command
  path from the user-mode drivers to the host.

**Windows 2000** (NT display model, "XDDM"): a video miniport (`.sys`, the hardware side), a
display driver (`.dll` in kernel mode, GDI and the DirectDraw/Direct3D HAL), and the same OpenGL
ICD. The miniport takes the virtio part of the 98 VxD's job.

## Steps

Each step ends with a report and side-by-side captures against the reference (Apple-style rule of
this project: nothing may look worse than without our driver).

1. **Groundwork:** toolchain, test VMs, baselines, this plan.
2. **2D on Windows 98:** display driver and mini VDD for virtio-vga: resolutions up to the host's,
   16- and 32-bit colour, the hardware pointer, DirectDraw basics (flip, blit to the screen).
   *State 2026-11-15:* the display driver runs the desktop at 8 to 32 bits and up to 1600x1200,
   changes modes without a restart and survives full-screen DOS sessions
   ([display-driver.md](display-driver.md)), and a mini VDD keeps DOS sessions in a window and
   BIOS calls from switching the card's mode. *2026-11-16:* DirectDraw uses the driver's HAL:
   video memory, page flipping, the vertical blank ([directdraw.md](directdraw.md)). Open: the
   hardware pointer, blits in the driver.
3. **The virtio-gpu path on 98:** our VxD: device setup, virtqueues, interrupts, resources, the
   3D commands (virgl) from user mode, fences.
   *State 2026-11-15:* the VxD brings the virtio device up (virtio 1.0 over PCI, two queues,
   answers polled), with the VGA frame buffer still the screen: QEMU keeps showing it until a
   scanout is set. Win32 programs send virtio-gpu requests through it
   ([vxd-interface.md](vxd-interface.md)). First 3D on a host with virgl (Proxmox VE, Intel
   UHD 630): `rvdtest clear` in Windows 98 had the host GPU clear a texture, read it back
   (the pixel value was right) and painted it on the desktop. The interface now has contexts,
   buffers with guest memory, command submission and transfers, and the VxD cleans up after a
   program that ends. Open: interrupts and fences (requests are polled and synchronous).
4. **OpenGL on 98:** Mesa (virgl) built for Windows 98 as the ICD. First target: the Quake III
   Arena demo, accelerated.
   *State 2026-11-15:* Mesa 24.3.4 builds for Windows 98 with a current clang, and its virgl
   driver draws with the host GPU through our VxD: a test program in OpenGL 1.1 style passes
   its pixel checks and shows its picture in a window, also when loaded by Windows' own
   `opengl32.dll` as its installable client driver, and the Quake III Arena demo plays its
   demos at about 300 frames a second at 640x480 and 155 at 1024x768, with a picture that
   matches Mesa's software renderer frame for frame ([mesa.md](mesa.md)). Windows 98's own
   OpenGL screen savers run, on 16-bit desktops too, and a new install from the INF sets
   everything up. Open: a faster way to the screen than reading frames back, a comparison
   against a card of the time, more programs.
5. **Direct3D on 98:** the HAL for DirectX 7 (3DMark 99 and 2000), then the DirectX 8 and 9
   command stream (3DMark 2001 SE), translated to Gallium (Mesa's driver interface).
   *State 2026-11-16:* DirectDraw uses the driver's HAL (video memory, page flipping, a 60 Hz
   vertical blank), and a Direct3D 7 HAL plays Direct3D's command buffers on OpenGL through our
   OpenGL driver: a test program's frame (shading, depth test, textures, blending) is right on
   the host GPU and runs at about 1,040 frames a second at 640x480 ([directdraw.md](directdraw.md)).
   3DMark 2000 runs its default benchmark to the end with a correct picture: 11816 3D marks at
   1024x768 16-bit on the host's Intel UHD 630, with DirectDraw surfaces that are never shown
   in a system memory heap; 3DMark 99 Max makes 21969 at 800x600. Restart and Shut Down work
   again ([display-driver.md](display-driver.md)). Next: a comparison against a card of the
   time, games.
   *State 2026-11-26:* the DirectX 8 and 9 driver interface, behind the registry switch
   `HKLM\Software\moose-ripple`, `D3D8` = `"1"` ([directdraw.md](directdraw.md#direct3d-8-through-its-own-interface-stage-1-2026-11-26-on-by-default-since-060)):
   stream draws, pixel shaders 1.1 to 1.3 and 2.0 (the Windows 2000 driver's translator, its
   TGSI text turned into ARB fragment programs), fog after the shader, eight samplers, DXT,
   cube, volume and bump map textures. `d8pixels` and `d9pixels` (DirectX 8.1 and 9.0c on
   Windows 98) pass every case they run, and 3DMark 2001 SE runs every test with correct pictures
   except Advanced Pixel Shader ([directdraw.md](directdraw.md#3dmark-2001-se-2026-11-26-rig-vm-230)).
   Since then (same day, `D3D8` = `"2"`): vertex processing on the GPU, fixed function and
   vertex shaders 1.1 and 2.0 ([directdraw.md](directdraw.md#vertex-processing-on-the-gpu-2026-11-26-d3d8--2)),
   and the first speed work, timed on the rig: 3DMark 2001 SE 12134, then 14716 (rendered textures
   stay on the GPU), then 30226 (vertex and index buffers in the driver's memory, kept in OpenGL
   buffer objects). On by default since 0.6.0 (`D3D8` = `"0"` turns it off). Next: 3DMark03
   (parked: its installers need a newer Windows or fail on 98).
6. **Windows 2000:** miniport and display driver; the OpenGL and Direct3D parts reused.
7. Glide stays out (2026-11-26): games that need it can use a separate wrapper such as OpenGlide
   ([display-driver.md](display-driver.md#no-3dfx-glide)). Windows 95 OSR2.5 is supported since 0.2.0.

## Windows 98 direct scanout

Full-screen Direct3D games can keep the finished picture on the GPU. Mesa copies it into a
display image on the host. The mini-VDD shows the latest frame at the console and switches
back to a CPU-drawn desktop when the game gives up the screen. It was opt-in at first and is
the default since 2026-11-26 (`RIPPLE_D3D_SCANOUT=0` switches it off), after the check in UTM
on Apple silicon. The pixel checks, frame counts and benchmark results are in
[DirectDraw and Direct3D](directdraw.md#full-screen-gpu-presentation).

On the test host's QEMU version (9.2.0), selecting a virtio display resource switches the
screen away from VGA. Selecting resource zero blanks the output. The mini-VDD therefore
retains virtio display ownership for the rest of the boot and keeps a desktop image
independent of any game. DOS works in a window and in standard full-screen 80-column
colour text. Full-screen DOS graphics after GPU presentation and support on other host
renderers remain open.
[QEMU scanout](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/virtio-gpu-virgl.c#L364-L422),
[VGA selection](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/virtio-vga.c#L20-L30),
[device reset](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/virtio-gpu-base.c#L21-L35).

## Toolchain (on the build host, under Wine; checked 2026-11-14)

Everything runs from the repo's `.tools/win9x/` (git-ignored); the kits are unpacked there with
`tools/win9x/ddk-unpack.py`, which lays out a DDK from its setup cabinets without running setup.

| Tool | From | Under Wine |
|---|---|---|
| C/C++ compiler, 32-bit (`cl` 12.00, VC6 SP6) | Visual C++ 6.0 Enterprise SP6 | works (a test program builds and runs) |
| Linker with VxD output (`link` 5.12) | Windows 98 DDK | runs |
| Assembler (`ml` 6.11d, Win32) | Windows 98 DDK | runs |
| 16-bit C compiler (`cl16`, C 8.00c, Win32-hosted) | Server 2003 DDK, 16-bit build environment | runs |
| 16-bit linker (`link16` 5.60, segmented executables) | same | runs |
| `mapsym`, `libw.lib` and the 16-bit headers and libraries | 98 DDK, 2003 DDK | runs |
| 16-bit resource compiler | Open Watcom v2 (`wrc`, a Linux program; the 98 DDK's `rc` needs DOS) | not needed: compiles 16-bit resources and binds them into the driver file |
| Windows 2000 build environment (`build`, `cl` 13.10) | Server 2003 DDK (builds for 2000) | to check in step 6 |

Proof for the 16-bit side: the 98 DDK's own sample display driver (`framebuf`, 22 assembly files)
assembles with `ml` and links with `link16` into a 16-bit driver file (`.drv`, an NE-format DLL),
and `mapsym` makes its symbol file, all under Wine; `wrc` adds resources to the result. The DDK's
help files unpack to HTML with `7z` (2700 pages on display drivers, VxDs and DirectX drivers).

Kits on hand: Windows 98, ME and 2000 DDKs, the Server 2003 SP1 DDK (3790.1830), the DirectX 7
DDK (Direct3D 7 HAL; the DirectX 8 and 9 driver interface is in the 2003 DDK's Direct3D
headers), MASM 6.11, Visual C++ 1.52c and 6.0 SP6. Test programs: 3DMark 99 MAX, 2000 and
2001 SE, the Quake III Arena demo, Unreal Tournament GOTY.

## Test setup (Proxmox host)

| VM | What |
|---|---|
| 230 `ripple-win98` | full clone of the owner's Windows 98 SE VM (105), the 9600 GT removed, COM1 on a socket |
| 231 `ripple-win2k` | full clone of the Windows 2000 SP4 VM (103), the 9600 GT and USB receiver removed, standard VGA, COM1 on a socket |

Both have a `clone-fresh` snapshot. The originals stay untouched.

References for the comparisons:
- software drawing: the clones as they are (no 3D);
- Windows 2000 on the passed-through GeForce 9600 GT (its own VM, driver 258.96), never while a
  virgl VM runs;
- qemu-3dfx, when built (its own QEMU binary next to Proxmox's, not replacing it).
