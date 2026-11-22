# Windows 2000 driver: development notes

How the Windows 2000 driver (`win2k/`) was built up and checked, step by step, with
the measurements behind each decision. The user-facing description is
[win2k/README.md](../../win2k/README.md). Unless a section says otherwise, the guest
is Windows 2000 Professional SP4 (build 2195), x86, in QEMU with `virtio-vga-gl`.

## The framebuffer, the GPU probe and DirectDraw

This x86 driver draws a Windows 2000 desktop on QEMU's `virtio-vga-gl`
adapter (PCI `1AF4:1050`). The miniport programs the compatibility display
registers and maps video memory. The display DLL gives GDI a top-down
32-bit bitmap backed by that memory. GDI draws directly into the visible
framebuffer. The driver performs DirectDraw colour fills and whole-surface
copies on the host for up to four XRGB32 offscreen surfaces, each up to
1024×768. Applications can copy a source rectangle from these surfaces
to a window using a DirectDraw primary surface and an HWND clipper.
In exclusive fullscreen mode the driver offers one back buffer when two
pages fit in the adapter's video memory. A flip changes the adapter's display-start
offset, and releasing the chain returns scanout to the desktop page.
A separate diagnostic draws one pixel on the host
through the same virtio control queue.

On Linux, `win2k/build.sh` cross-compiles the miniport and display DLL
with Clang targeting the x86 Microsoft ABI and links them against the
Windows 2000 DDK. The user programs use LLVM-MinGW with the system
msvcrt runtime. The script locates the DDK and VC6 headers under
`.tools/win9x` or `WIN9X_TOOLS`, and the compiler under
`.tools/llvm-mingw` or `LLVM_MINGW`. Loose files go to `build/win2k`
or the directory in `OUT`.

Copy the six output files into one directory the test guest can read.
Run `w2kprobe.exe` to see the hardware ID and current modes. Run
`nt5inst.exe` as a local administrator to install the driver, then restart
Windows. The helper obtains the adapter's complete PCI hardware ID from
SetupAPI before calling the native device installer.

The preferred desktop is 1024×768 at 32 bits and 60 Hz. The installer
supplies defaults for a new device and preserves an existing selection.
Use Display Properties to save 1024×768 on an upgraded guest. The driver
also supports 640×480 and 800×600. Run `w2kprobe.exe /pattern` to
sample four colours at 1024×768 and restore the original mode. Use
`/pattern800` to exercise the same change and restoration at 800×600.

Run `w2kprobe.exe /gpu` to exercise the host renderer. The miniport creates
a private 1×1 texture, clears it to orange, waits for completion, and
reads four bytes back. The expected BGRA pixel is `0xffff8000`. The tool
prints the stage and error code on failure and returns zero on success.
The desktop continues to use the mapped framebuffer during this test.

On Windows 2000 Professional SP4 with driver 0.0.6.0 and an Intel UHD 630 host, the adapter
initially had PCI bus mastering disabled. The miniport enables it for the
probe and restores the previous setting after resetting the queue. The guest
retrieved the exact orange pixel on two successive runs. QEMU traced both
command submissions and readbacks, and the Intel driver's per-process render
time advanced on each run. Afterward, the framebuffer still showed all four
test colours and the guest switched to 1024×768 and back to 800×600.

The GPU run used a previously installed Ripple service. The two driver
files were replaced on the powered-down test disk and verified after boot.
On a separate stock VGA installation, Windows Setup installed the same
build after the unsigned-driver prompt. Both files matched after reboot,
and Windows offered all three 32-bit modes. The first boot retained the
old four-bit setting. Select a 32-bit mode before running the GPU test.
An in-place upgrade from the previous driver still needs qualification.

`w2kprobe.exe /ddraw` tests the application-facing DirectDraw API. It
requests a 17×13 hardware-only offscreen surface, fills it with two
different colours, then takes one read-only lock and checks all 221 pixels.
It repeats the operation on an emulated system-memory surface. When the
driver exposes its transfer counters, the test checks for two GPU fills,
zero earlier readbacks, one readback at lock, and zero uploads. Run on an
idle desktop so unrelated DirectDraw clients do not contribute to the totals.

Run `w2kprobe.exe /ddraw-full` at 1024×768×32 to check a pair of
desktop-sized surfaces. It fills the source, copies it to the destination,
and overwrites the source before locking the destination. A pass requires
all 786,432 destination pixels to retain the first colour on both paths.
The hardware path must report two fills, one host-side copy, no uploads,
and a single readback at the lock.

On Windows 2000 Professional SP4 under Apple-silicon UTM, 0.0.8.0 passed
this test on a 1024×768×32 desktop at 60 Hz. QEMU recorded both textures,
the two fills and the copy, then one transfer from the destination when
the application locked it. A separate two-surface test verified edits
across scanlines at 1024×768 and isolation from a 1019×753 surface.
The guest retained its 1024×768 mode across a cold boot.

`w2kprobe.exe /window` opens a 640×480 test window and attaches a
DirectDraw clipper to it. It presents a solid colour, then a cropped
pattern from a host texture, first with another window overlapping it
and again after uncovering it. The probe checks the image, the window
margins and the overlapping window through GDI. It repeats the test
using DirectDraw software emulation as a control.

`w2kprobe.exe /flip` requests exclusive fullscreen mode, creates a primary
surface with one attached back buffer, and writes a different pattern to
each page. It flips five times and checks every front-buffer pixel after
each swap. On hardware it also reads the adapter's display-start register
to verify which page is visible. The hardware test runs at 640×480 and
1024×768, then checks that Windows has restored the original desktop mode.

`w2kprobe.exe /ddraw-pair` keeps one windowed DirectDraw surface alive
while a second process creates and reads its own. After the second process
exits, the first checks that its original colour is intact.

On Windows 2000 SP4 under UTM, the 32-bit hardware paths matched all
5,468,160 pixels checked across ten fullscreen flips. The adapter trace
recorded each page switch, the return to page zero, and zero resource
uploads or readbacks. Windows restored 1024×768×32 at 60 Hz. Only the
owner of a fullscreen chain maps both video pages. Two simultaneous
windowed clients retained their own offscreen pixels.

On Windows 2000 SP4 in UTM at 1024×768×32, both paths passed. Known solid
colours needed no GPU readback. For a patterned image the primary shares
GDI's framebuffer, so the driver reads back the visible rectangles. The
overlapping window left 286,833 pixels visible, requiring 1,147,332
bytes across four rectangles. Removing the overlap yielded one rectangle
of 298,936 pixels. The application uploaded its source pattern once
during setup. Presenting the window required no uploads.

When GDI or an application takes a writable lock after a solid fill, the
display DLL fills the application's staging buffer from the known colour.
A partial lock preserves pixels outside the locked rectangle. A patterned
surface still requires a readback if the caller needs its existing pixels.

Microsoft’s unmodified DirectX 7 Switcher sample displayed its text and
bouncing ball in a window on the 1024×768 desktop. Before this change,
each of 58 clears was followed by a full 640×480 readback for GDI. With the
change, 72 clears produced zero lock readbacks, saving 1,228,800 bytes per
frame. Each frame still uploaded the GDI edits and read its visible
rectangles for presentation.

## OpenGL

The display DLL answers `OPENGL_GETINFO` with the name `RIPPLEGL`, so
Windows' own `opengl32.dll` loads the driver registered under that name in
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers`. With the
Windows 98 build of `ripplegl.dll` (Mesa 24.3.4, `win9x/mesa`) copied to
`system32` and registered both as a string value and as a key (`Dll`,
`DriverVersion` 1, `Flags` 1, `Version` 2), it draws on the host GPU.

Mesa's virgl winsys uses the interface of the Windows 98 kernel driver
(`win9x/minivdd/rvd.h`). On Windows 2000 it sends the same calls to the
display DLL with `ExtEscape` (`rb2krvd.h`, `display_rvd.c`), and the miniport
carries them out (`gpu_rvd.c`): host contexts, resources, command batches of
up to 384 KB and transfers, one exchange with the host at a time. Each
buffer has guest pages of its own, attached to the host resource for its
whole life and mapped into the process that made it (`gpu_memory.c`). Mesa's
command stream moves data between the two by itself, so the pages cannot be
lent for single transfers. Mesa does not always destroy its context when a program ends, so the
miniport frees the contexts and buffers of ended processes at the next
creation and before the GPU self-test. While an
OpenGL context is open the transport stays up, and a mode change frees only
DirectDraw's surfaces.

On Windows 2000 SP4 in UTM, `gltest` reported `virgl` as the renderer and
passed all its pixel checks at about 590 frames a second at 320×240. The
scene is small, so this mostly measures a frame's way to the window: a
readback and a GDI copy. Microsoft's software OpenGL (`GDI Generic`) passed
at about 430 frames a second and Mesa's softpipe at about 9. A screen
capture of a 640×480 run showed the whole scene, clipped by an overlapping
window.

The Quake III Arena demo runs on the host GPU too, from its files copied
to `C:` (it needs no installer). `timedemo` with `demo001`, default
settings, 32-bit colour, one emulated Pentium III in UTM on Apple silicon:

| Mode | frames a second |
| --- | ---: |
| 640×480 in a window | 85 |
| 1024×768 full-screen | 82 |

    quake3.exe +set logfile 2 +set timedemo 1 +set s_initsound 0 +set r_mode 3
        +set r_fullscreen 0 +set nextdemo quit +demo demo001
    (the result is in demoq3\qconsole.log)

With 2.6 times the pixels the frame rate hardly changes, so on this host
the frame's readback and copy are not the limit (see below). Screen
captures of the windowed run showed the level's textures, lighting and
the game's text. Windows 98 reached about 300 frames a second in a window
on a Proxmox VE host whose CPU is not emulated; it has not been timed in
UTM.

`rvdstat.exe` prints the time spent in each request to the driver since
Windows started; run it before and after a program and subtract. In the
windowed timedemo each frame made one command submission (about 1.3 ms,
host included) and one readback of the frame (about 1.2 ms): about 2.5 ms
of a 11.8 ms frame. The rest is the emulated CPU running the game, Mesa
and GDI's copy of the frame to the window. The 685 textures and buffers
made and freed during the run belong to loading the level.

## Speed

`ddbench.exe` times the same DirectDraw frames through the driver
(hardware-only DirectDraw, video-memory surfaces) and through Windows'
software path (emulation-only DirectDraw, system-memory surfaces), for
three seconds each. It writes to standard output when that is redirected,
otherwise to COM1. Measured on Windows 2000 SP4 in UTM on Apple silicon
(one emulated Pentium III), 1024×768×32 desktop, 640×480 frames:

| Frame | Driver | Software |
| --- | ---: | ---: |
| Fill a surface, copy it to a window | 315 fps | 755 fps |
| CPU writes every pixel, copy it to a window | 92 fps | 454 fps |
| Fullscreen: fill the back buffer, flip | 845 fps | 914 fps |
| Fullscreen: fill, 100 64×64 sprites, flip | fails | 487 fps |
| Same with a source colour key | fails | 496 fps |

In a window the driver is two to five times slower than Windows' own
software path. The desktop is guest memory that the CPU draws into, so
each copy from a host texture to the screen is a GPU readback, and each
CPU-written frame is also an upload. Fullscreen fills and flips run on the
CPU in video memory on both paths. Copying part of a video-memory surface
into the back buffer fails with `DDERR_UNSUPPORTED`, and the driver offers
no colour keys. Microsoft's Donuts sample still draws its sprites
correctly with the driver: its frame matched the software path's.

The earlier 0.0.7.0 build supported full-surface fills on textures of
at most 4 KiB each. GDI continues to draw the primary desktop. On 0.0.6.0
the DirectDraw capability check reported no hardware acceleration and the
software control passed (exit 3).

On Windows 2000 Professional SP4 (build 2195), the stock VGA driver offered
640×480 and 800×600 at four bits. The Ripple miniport started after reboot,
and Windows switched to 800×600×32 at 60 Hz. Independent pixel samples
from GDI and the host display agreed on all four colours. The guest
restored its previous mode. Windows also cold-booted at the saved 800×600×32
setting, switched to 1024×768×32 and restored 800×600×32.

Console windows open on the 32-bit desktop. The display DLL reports
`TC_RA_ABLE` in its text capabilities, so GDI realizes raster fonts on the
framebuffer surface, as in the DDK's framebuffer sample. Builds without it
listed the raster fonts, Terminal among them, but `AllocConsole` failed with
Windows error 31; the console's default face is the raster Terminal font. On
Windows 2000 SP4 in UTM at 1024×768×32, the build with the flag allocated and
freed a console, and Command Prompt drew its banner in the Terminal font. The
miniport was byte-identical in both builds.

## Frame time

`rvdstat.exe` (the display DLL's per-request counters) during Quake III's
windowed timedemo: each frame made one command submission (about 1.3 ms,
host included) and one readback of the frame (about 1.2 ms), 2.5 ms of a
11.8 ms frame. The 685 textures and buffers made and freed during the run
belong to loading the level. Sending readbacks without a fence, as the
Windows 98 kernel driver does, made the timedemo slower (70 and 74 frames a
second against 85 and 82), so the fence stays. In UTM the emulated CPU
running the game, Mesa and GDI's copy of the frame is the limit.

## DirectDraw surfaces in system memory

With offscreen surfaces as host textures, `ddbench.exe` measured the driver
two to five times slower than Windows' own DirectDraw code in a window, and
blits from such a surface into the back buffer failed. The display driver
now declines offscreen surfaces in video memory with
`DDERR_OUTOFVIDEOMEMORY` unless `RippleDirectDrawGpuSurfaces` is 1. What a
program gets that lets DirectDraw choose (UTM, 640×480 frames):

| Frame | before | after |
| --- | ---: | ---: |
| Fill a surface, copy it to a window | 327 | 843 |
| CPU writes every pixel, copy it to a window | 90 | 450 |
| Full-screen: fill the back buffer, flip | 1077 | 1171 |
| Full-screen: fill, 100 64×64 sprites, flip | fails | 427 |
| Same with a source colour key | fails | 440 |

With the switch on (and a restart) the GPU-surface tests (`w2kprobe /ddraw`,
`/ddraw-full`, `/window`) still pass.

## Rebuilt flip chains

Releasing a flip chain revoked the owner's mapping of the two video pages,
but DirectDraw maps them once per session and keeps using the address. A
chain rebuilt in the same session, as games do after a mode change, then
failed every lock and blit into the back buffer with
`DDERR_CANTLOCKSURFACE` (`flipcycle.exe`: rounds 2 to 4). The view now stays
until DirectDraw unmaps it, another process acquires the pages, or the mode
changes; the display driver reuses it for the next chain and forgets the
views of processes whose mapping the miniport revoked. All four rounds then
passed, before and after `w2kprobe /flip`.

## OpenGL from several threads

Mesa's winsys kept the display DC of the thread that created the screen.
After that thread ended, Windows refused the escape on it: `threadtest`
failed 299 of 300 threads with black pixels, and the driver's counters
showed only the first thread's requests arriving. Each request now takes
`GetDC(NULL)`. `threadtest` then passed every pixel check. Its memory check
(at most 1 KB a thread after the first 50) measured 49 bytes for create and
delete, 147 with make-current, and about 3.2 KB a thread or 1.2 KB a context
in one thread when drawing; Microsoft's software OpenGL measured 442 bytes.
Over 100 contexts Mesa made 103 GPU buffers and destroyed 100, and the
driver held none afterwards, so the remaining growth is the program's heap.

## Installing through Setup

Windows 2000's `opengl32.dll` reads the `OpenGLDrivers\RIPPLEGL` key form
(`Dll`, `DriverVersion`, `Flags`, `Version`). With only a string value of
that name it used its own software OpenGL. The INF writes the key form and
copies `ripplegl.dll` to `system32`.

Updating the earlier driver with `NT5INST.EXE` and the 0.1.0.0 INF, after
removing the hand-made OpenGL registration and DLL: Setup copied
`rb2kmp.sys`, `rb2kdisp.dll` and `ripplegl.dll` and asked for a restart.
After it, both driver files matched the package and `gltest` ran on virgl.
Setup's unsigned-driver prompt had opened behind another window;
`NT5INST.EXE` now passes its console window as the owner of Setup's
prompts.

## Stability and the final run

An earlier boot ran five Quake III timedemos in a row (77 to 78 frames a
second), `threadtest` and a mode change while `gltest` drew; afterwards
`rvdstat` showed no contexts, buffers or pages held.

The final run installed the 0.1.0 package with `NT5INST.EXE` through Setup
over the earlier driver. After the restart both driver files and
`system32\ripplegl.dll` matched the package. On that boot: the mode,
colour, 800×600, GPU, flip and console tests, `flipcycle`, `ddbench`
(DirectDraw's choice: 923, 486, 1177, 426 and 435 frames a second),
`gltest` on virgl (653 frames a second), `threadtest` (every pixel right;
the heap check failed at 3.2 KB a thread, as above), two windowed
timedemos (89 frames a second) and one full-screen (89), and Donuts, whose
frame showed its sprites, score and counter. `rvdstat` showed nothing held
and the desktop was at 1024×768×32. With `RippleDirectDrawGpuSurfaces` set
and a restart, `/ddraw`, `/ddraw-full`, `/window`, `/ddraw-pair` and `/flip`
passed too.

## The framebuffer's memory type

On a Windows 11 host through WSL2 (QEMU 8.2.2 with KVM on an AMD Ryzen,
virgl on Mesa's Direct3D 12 driver and a GeForce RTX 4090), the 0.1.0
candidate passed every test but ran slowly: Quake III at 43 frames a second
against 89 in UTM, `gltest` at 120, and DirectDraw near 50 whatever the
frame held, Windows' own software path included. A test without any
graphics API (`memtest.exe`, now among the tools) narrowed it down. The
CPU and ordinary memory ran at full speed, and GDI drew 200 full-screen
fills into a DIB in memory in 16 ms. The same fills on the screen took
10.1 s, 50 ms each for 3 MB: about 60 MB/s into the framebuffer, and
reading it back was no faster.

QEMU's tracking of framebuffer writes for its display was not the cause:
with no display (`-display none`) the fills took 9.4 s. The page tables
were. QEMU's `info tlb` showed every framebuffer page mapped with both
cache-disable bits (PCD and PWT). VideoPort maps device memory uncached,
and asking it for write-combining (`VIDEO_MEMORY_SPACE_P6CACHE`) changed
nothing: Windows 2000 makes write-combining through the MTRRs, which KVM
on AMD processors does not apply, while it does honour the page
attributes. KVM on Intel processors ignores the guest's memory type for
memory like this, and UTM's emulated CPU has no caches to bypass, so
neither of the other test hosts showed it.

The framebuffer is ordinary memory inside the virtual machine, which the
host reads coherently, so the miniport now maps every view of it cached:
the display driver's (`MmMapIoSpace` with `MmCached`), its own scanout
view, and the DirectDraw flip chain's view in the program's process (a
view of `\Device\PhysicalMemory` without `PAGE_NOCACHE`, as VideoPort
makes it otherwise). `info tlb` then showed the pages without PCD and PWT,
and on the same host:

| Test | uncached | cached |
| --- | ---: | ---: |
| 200 full-screen GDI fills, 1024×768×32 | 10.1 s | 15 ms |
| 50 full-screen scrolls (screen to screen) | 6.2 s | 16 ms |
| Quake III `timedemo demo001`, 640×480 window | 43 | 323 |
| `gltest` | 120 | 353 |
| DirectDraw, fill and copy to a window (DirectDraw's choice) | 51 | 17 790 |
| DirectDraw, full-screen fill and flip | 50 | 7 110 |
| the same with 100 colour-keyed sprites | 28 | 4 440 |

On the Intel host (Proxmox VE 8.4, QEMU 9.2, UHD 630) both drivers fell
within that host's run-to-run spread: Quake III 204 to 311 frames a second
before and 213 to 242 after over four runs each, `gltest` 692 to 1005
before and 627 to 1522 after.

## Windows XP SP3 x86

### Desktop mode after a full-screen application

On a fresh driver install, XP initially showed 1024×768×32 at 60 Hz, but
`EnumDisplaySettings(ENUM_REGISTRY_SETTINGS)` returned 0×0. Exiting a
full-screen DirectDraw application left the desktop at 640×480 and the next
boot kept it there. Manually saving 1024×768 through `ChangeDisplaySettings`
fixed both. XP's display installer had written the INF defaults to the
adapter's `Control\Video\{GUID}\0000` key in HKLM. The matching key in
the current hardware profile had no saved resolution.

The INF now registers `rbntmode.dll` as a device co-installer. After Windows
Setup installs the adapter on XP, the DLL reads its `VideoID` and copies
the missing resolution, depth and refresh values from the INF defaults to
that adapter's hardware profile. Existing values survive an update. Both
`NT5INST.EXE` and Device Manager's **Have Disk** use this installation step.

The miniport supplies XP's size of `VIDEO_HW_INITIALIZATION_DATA` so
`\Device\Video0` maps to `Control\Video\{GUID}\0000`. It retries with
the Windows 2000 size on `STATUS_REVISION_MISMATCH`. On Windows 2000 SP4,
both the initial XP-sized miniport and the complete package with the
co-installer were installed from stock VGA on separate test disks. The
installed files matched, the GPU pixel was correct, and both DirectDraw
flip paths restored 1024×768. The complete package also passed
`regmode /check 1024x768` before and after the flip.

### Intel KVM results

The test started on the stock VGA driver in an existing XP Professional
SP3 x86 installation, using QEMU's Q35 machine, one Penryn vCPU, 2 GiB
and a virtio-vga-gl adapter on an Intel UHD 630.

- `NT5INST.EXE` installed the driver. Before the first full-screen test,
  the saved and active modes were 1024×768×32 at 60 Hz. Both DirectDraw
  flip paths restored that exact mode. It survived a cold boot.
- After selecting 800×600, a **Have Disk** reinstall from the CD root and a
  restart preserved 800×600×32 at 60 Hz. Both flip paths restored it. To
  repeat the regression test, run `regmode /check 1024x768` before and after
  `w2kprobe /flip` and after a reboot. Use `/check 800x600` for the update case.
  Checking the starting resolution catches a run that begins and ends at 640×480.
- The installed miniport, display DLL and OpenGL DLL matched the disc.
  Both GDI colour patterns, the console, the GPU pixel and all four
  `flipcycle` rounds passed.
- On virgl, `gltest` passed at 1,010 frames per second. Quake III's
  1,346-frame `demo001` timedemo ran at 233.8 fps in a 640×480 window
  and 163.9 fps at 1024×768 full-screen. The full-screen frame showed the
  textured map, player, weapon and HUD.
- DirectDraw's default path reached 11,848 fps for a windowed fill and copy,
  10,661 fps for a full-screen fill and flip, and 4,382 fps with 100
  colour-keyed sprites per frame. Stock DirectX 7 Donuts made 20,056 hardware
  flips, exited normally and restored 1024×768.
- `threadtest` passed 300 threads. After the games and thread test,
  `rvdstat` reported zero held contexts, buffers and pages.
- Enabling `RippleDirectDrawGpuSurfaces` through the live video device map
  and rebooting enabled the optional offscreen path on XP. The full-size
  hardware and software copies matched all 786,432 pixels. The clipped-window,
  two-process and flip tests also passed.

A second Intel test used a clean XP Professional SP3 install from the stock
CD on i440fx, one Penryn vCPU and 1 GiB. Stock VGA started at 640×480.
After installation and a cold boot, the saved and active desktops were
1024×768×32 at 60 Hz before any full-screen test. Both DirectDraw paths
restored that mode, and it survived another cold boot. Quake III completed
all 1,346 frames at 266.3 fps in a 640×480 window and 150.1 fps at
1024×768 full-screen.

### UTM on Apple silicon

When a guest keeps loading an older driver CD, follow the
[UTM test-media runbook](utm-test-media.md).

The clean XP SP3 disk booted on i440fx with one emulated Pentium III and
512 MiB. Stock VGA ran at 640×480 and Device Manager showed code 28 for
the display adapter. After the native installer finished and XP cold-booted,
Device Manager reported code 0. The installed miniport, display DLL, OpenGL
DLL and mode co-installer matched the test disc. The saved and active modes
were both 1024×768×32 at 60 Hz before the first full-screen test.

Both DirectDraw flip paths restored that exact desktop. The console test,
the four flip-chain rebuilds and the GPU pixel check passed. The OpenGL
renderer reported `virgl` and OpenGL 2.1. The 320×240 `gltest` scene
ran at 561 fps and passed all nine pixel checks. Donuts completed 977
hardware flips and restored the desktop. The 300-thread OpenGL test
passed, and the driver reported zero contexts,
buffers and pages held after the run.

On the next cold boot, the saved and active modes still matched. With
optional DirectDraw GPU surfaces enabled, hardware and software copies
each matched all 786,432 pixels at 1024×768. Clipped windows, two
simultaneous clients and both desktop-restore checks passed.

Quake III completed all 1,346 frames of `demo001` in each run. The repeat
used another cold boot after switching back to the default DirectDraw
settings. After the last full-screen run both desktop modes were again
1024×768×32 at 60 Hz, and the driver held no contexts, buffers or pages.

| Quake III mode | First run | Repeat |
| --- | ---: | ---: |
| 640×480 window | 65.6 fps | 76.6 fps |
| 1024×768 full-screen | 60.9 fps | 71.5 fps |

These XP game and benchmark results used the 32-bit display modes.

## 16-bit colour development

The miniport now offers RGB565 at 640×480, 800×600 and 1024×768 at 60 Hz.
The original 32-bit mode indices and preferred 1024×768×32 desktop carry
forward. GDI and DirectDraw use the same red, green and blue masks
(`0xf800`, `0x07e0`, `0x001f`). Full-screen DirectDraw can use two
framebuffer pages. In 16-bit mode Windows handles offscreen surfaces in
system memory. The optional host textures use 32-bit pixels.

An Apple silicon host test ran the driver's mode, scanout and rectangle-fill
routines against memory-backed registers and two framebuffer pages. It
checked the colour masks, pitch, page offsets, return to page zero and the
single-page fallback with insufficient memory. Rectangle fills at an odd
horizontal offset matched all 6,294,528 pixels checked across both pages,
three resolutions and two depths.

Windows 2000 SP4 and XP SP3 installed the same build in UTM. After
restart, each guest's miniport and display DLL matched the test files byte
for byte. `w2kprobe /pattern16` read the expected four GDI colours at
800×600. `w2kprobe /flip16` passed hardware page flips
at 640×480 and 1024×768 and software flips at 640×480. The hardware
rectangle fill, its untouched border and both page images matched. Each
path restored the 1024×768×32 desktop at 60 Hz. The 32-bit colour and GPU
pixel checks passed on both systems.

## 8-bit indexed colour

The development driver offers a 256-colour palette at the same three
resolutions. Each framebuffer byte is a palette index. GDI can update a
range of entries while the two flip pages retain their pixel data. The
display driver keeps the current table across mode resets. The miniport
programs full 8-bit red, green and blue values through the virtual VGA
colour registers.

On Apple silicon, a host test ran the display driver's palette updates,
the miniport's register writes and the framebuffer routines against a
simulated colour table and framebuffer. Nine modes at 8, 16 and 32 bits
matched all 9,441,792 pixels checked across both pages. All 256 colours,
a single-entry update, the neighbouring entries, the final table entry
and palette restoration passed.

Two failures appeared inside XP SP3. Its halftone service returned 216
colours. Requiring 256 made an enumerated mode fail during activation.
The driver now supplies all 256 entries and keeps the 20 Windows system
colours at their reserved indices. Advertising separate hardware palette
capabilities also made hardware-only DirectDraw surface creation fail.
The driver reports an indexed primary surface and lets Windows manage its
palette through GDI.

On Windows 2000 SP4 and XP SP3, all four GDI test colours matched at
800×600×8.
In `w2kprobe /flip8`, the default DirectDraw interface attached a 256-entry
palette to a video-memory flip chain. Changing entry 73 recoloured its
visible pixel without changing entry 74 or repainting either pixel.
At 640×480 and 1024×768, the scanout page and vertical offset followed
every hardware flip and the full frame matched its expected indices.
The software path at 640×480 also passed palette animation and all 81
visible samples on each of five flips. The saved and current desktops
returned to 1024×768×32 at 60 Hz.

Forcing `DDCREATE_HARDWAREONLY` passed the page and index checks with
the existing palette. Explicit palette creation on that interface returned
`E_NOTIMPL` on XP. The default interface used by the combined test can
create the palette and use hardware page flipping in the same session.

## Experimental GPU presentation for OpenGL

The ordinary renderer downloads a completed frame and passes it to GDI.
With the adapter DWORD `RippleGpuDesktop=1` set before boot, the driver
can display a 32-bit desktop texture up to 1024×768. Setting
`RIPPLE_GL_PRESENT=1` in an OpenGL application opts its swaps into that
path. GDI supplies the window's visible rectangles to `DrvDrawEscape`.
The miniport copies these rectangles into its own desktop texture. When
GDI needs desktop pixels, the miniport synchronizes the affected area
with the framebuffer. Software cursor drawing also uses this path.

On an M1 Max running UTM, each OS ran the same 969-frame OpenGL Quake
demo at 640×480 in a foreground window. The game, installed driver, Mesa
ICD, resolution, timer and demo settings were fixed. After a warmup, the
six trials used the ordinary, GPU, GPU, ordinary, GPU, ordinary paths.
Both sides used the same opt-in desktop. Only the process environment
variable changed between runs.
Host elapsed time runs from game launch to the serial score. Process CPU
includes kernel and user time. The synthetic test times 240 swaps with
a full-screen window, an unobstructed 640×480 window, a software cursor,
and a window partly covered by a green rectangle. Each trial checks its
own red and blue pixels and the cover's green pixel where applicable.

Keep the ordinary path as the default. In the initial Windows 2000
comparison, GPU presentation reduced Quake from 48.6 to 30.4 fps
despite reducing 240 full-screen test swaps from 1,186 to 710 ms.
Timing the individual driver operations reproduced that regression.
XP's three runs per setting showed no measurable game improvement.

| Test, median of three | Windows 2000 ordinary | Windows 2000 GPU | XP ordinary | XP GPU |
| --- | ---: | ---: | ---: | ---: |
| Windowed Quake (fps) | 47.5 | 30.8 | 46.3 | 46.1 |
| Host time from launch to score (s) | 22.61 | 34.98 | 23.77 | 23.72 |
| Game process CPU (s) | 7.03 | 19.41 | 7.91 | 8.01 |
| 240 full-screen swaps, cursor hidden (ms) | 1,176 | 714 | 1,212 | 714 |
| 240 windowed swaps, cursor hidden (ms) | 749 | 896 | 610 | 733 |
| Same window with a software cursor (ms) | 699 | 2,098 | 643 | 2,146 |
| Same window with a green window covering its centre (ms) | 615 | 1,964 | 626 | 2,056 |

The miniport measures elapsed time through each device reply. Each
rectangle copy attaches the source texture, waits for the copy, then
detaches it. Two `rvdstat.exe` snapshots around the game include
startup and teardown. Counts and seconds below are medians per game.
Time per call is total time divided by total calls across the three
GPU trials. XP uploaded 3, 3 and 20 times in those trials.

| Desktop operation | 2000 calls | 2000 seconds | 2000 µs/call | XP calls | XP seconds | XP µs/call |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU rectangle copy | 4,308 | 8.1551 | 1,890 | 1,079 | 1.9426 | 1,815 |
| Upload GDI pixels | 2,255 | 4.0561 | 1,843 | 3 | 0.0201 | 3,863 |
| Read partial desktop region | 1,117 | 0.7872 | 692 | 1 | 0.0006 | 559 |
| Flush desktop to scanout | 3,331 | 0.2676 | 81 | 1,081 | 0.0848 | 77 |
| Read whole GPU-owned region | 1 | 0.0024 | 2,441 | 1 | 0.0029 | 2,854 |

Windows 2000 copied four visible rectangles per presented frame. XP
copied one. The per-copy costs were similar. On the ordinary path,
Windows 2000 spent 1.306 s downloading 1,086 application frames and XP
spent 1.271 s downloading 1,101. GPU presentation eliminated those
downloads on both. The largest cost on Windows 2000 was the repeated
rectangle-copy sequence. Submitting a frame's rectangles together is
the next like-for-like presentation comparison.

All 24 measured synthetic trials on each OS passed the path, foreground
and pixel checks. The unmeasured first ordinary-path probe on each OS
returned a failure with matching sampled colours. Its executable did
not log the failing condition. All twelve measured game runs held the
foreground, released their GPU resources and restored the desktop.
One earlier Windows 2000 run also had an 8-bit page-flip mismatch at
1024×768. The immediate repeat and both later isolated repeats passed.

The tests sampled the program's own GDI pixels. The UTM viewer still
needs a visual check. On the opt-in XP desktop, set
`RIPPLE_GL_PRESENT=1` and run `glpresent.exe full hold`. At `GLP HOLD`
the 1024×768 client should show solid red in the lower-left 512×384
quarter and solid blue everywhere else. Display depth changes while a
GPU-presented OpenGL context is still live also remain unverified.

### Batching the visible rectangles

With `RIPPLE_GL_PRESENT=1`, the miniport attaches the application's
texture to the desktop context once, submits every visible rectangle
in one command buffer, waits for its fence, then detaches the texture.
It accepts up to 32 rectangles. GDI handles windows whose visible region
exceeds that limit. For a comparison on the same installed driver, use
`RIPPLE_GL_PRESENT=rect` to submit and fence each rectangle separately,
or `RIPPLE_GL_PRESENT=0` for ordinary GDI presentation.

`rvdstat.exe` reports cumulative rectangle and fence counts and the
elapsed ticks for attachment, fenced submission and detachment. The
fenced interval includes host execution and the guest waiting for the
device reply. The upload counters identify the GDI callback and, for a
bit-block transfer, its raster-operation code. The per-callback counts
also distinguish writes up to 64×64 pixels. `glpresent.exe window stripes`
places twenty narrow opaque bands over a window, checks an exposed pixel
between the bands and checks the cover's colour.
