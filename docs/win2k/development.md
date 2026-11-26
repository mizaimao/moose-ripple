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

## Screen sizes above 1024×768

The miniport also offers 1152×864, 1280×720, 1280×800, 1280×960,
1280×1024, 1440×900, 1600×900, 1600×1200 and 1680×1050 at 8, 16 and 32
bits. They follow the earlier modes, so those keep their mode indices. A
mode is offered when one screen fits in the framebuffer. DirectDraw page
flipping needs a second screen; where it does not fit, the miniport
reports a single page and the display driver offers no flip chain. QEMU's
virtio-vga has an 8 MB framebuffer, and QEMU 9.2 has no option to change
it. So 32-bit flipping works up to 1280×800, 16-bit and 8-bit flipping
up to 1680×1050 and 1600×1200, and 1920×1080 at 32 bits needs 8.3 MB and
is not offered. Direct3D and OpenGL textures and render targets live in
host GPU memory and are not limited by it. Host images, the miniport's
own framebuffer view and its readback staging grow to the largest mode;
a readback of a 1680×1200 image takes 124 staging segments of 64 KB,
which still fit one 2 KB request.

`w2kprobe /pattern WxHxB` checks four GDI colours in any offered mode.
On Windows 2000 SP4 and XP SP3 under KVM on the Intel host:

| Check | Windows 2000 SP4 | Windows XP SP3 |
| --- | --- | --- |
| GDI colours at 1280×1024×32 and 1680×1050×32 | pass | pass |
| GDI colours at 1440×900, 1600×900 and 1600×1200 at 32 bits; 1600×1200 and 1680×1050 at 16 bits; 1600×1200 at 8 bits | pass | not run |
| DirectDraw flip chain at 1280×800×32 (`/flipat`) | pass | pass |
| Direct3D pixel tests on a 1280×1024×32 desktop, HAL and T&L | pass | pass |
| Quake 0.73 `demo1` in an 800×600 window on that desktop | 103.7 fps | 98.9 fps |

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

OpenGL Quake ran the same 969-frame demo three times per setting, in
rotating order, at 640×480 on a 1024×768 desktop on an M1 Max. The
host timer measured game launch to score. All measured runs held focus.

| Guest | GDI fps | Per-rectangle fps | Batched fps | Host seconds, GDI / per rectangle / batched |
| --- | ---: | ---: | ---: | --- |
| Windows 2000 | 48.4 | 30.4 | 32.8 | 22.56 / 35.84 / 32.72 |
| Windows XP SP3 | 46.8 | 45.6 | 45.3 | 23.70 / 23.83 / 24.69 |

On Windows 2000, batching changed the median from 4,352 rectangles
with 4,352 waits to 4,348 rectangles with 1,087 waits. Copy time fell
from 8.21 to 2.63 s. On XP both paths already had one rectangle per
present, and copy time was 1.98 and 1.96 s. Both systems passed the 26
measured image checks and seven legacy tests. The cold-start probe on
each lost foreground despite matching colours and was excluded.

In a Windows 2000 per-rectangle run, 2,189 of 2,270 uploads came from
bitmap-to-desktop `DrvCopyBits` calls. In the synthetic cursor test, each
swap generated two 31×27 uploads and one partial read. With Quake's
mouse grab disabled, placing the cursor inside, outside and back inside
gave 33.0, 45.5 and 33.7 fps with batched copies. Desktop uploads were
2,527, 204 and 2,204. On XP the stock game showed no visible pointer
inside its client in any timed sample and made a median of three uploads.
Disabling Quake's mouse grab on XP reproduced the Windows 2000 effect.
Inside, outside and back inside gave 33.0, 46.6 and 33.3 fps. XP recorded
2,135, 62 and 2,191 uploads, respectively. In the two inside runs only the
initial upload came from anything other than bitmap-to-desktop copies.
Ordinary GDI remains the default on both systems.

### On the Intel host: keep it off

The GPU path stays opt-in. On the Intel UHD 630 host under KVM, every
case was several times slower with it than with the ordinary download to
GDI. Both paths ran alternately in the same boot of each system, with the
adapter DWORD `RippleGpuDesktop=1` set and only `RIPPLE_GL_PRESENT`
changed between runs. The host GPU's minimum clock was held at 1200 MHz.
The Quake III Arena demo ran `timedemo demo001`, three runs per path; the
ranges are the lowest and highest. `glpresent` timed 240 swaps, two or
three runs per path.

| Test | Windows 2000, ordinary | Windows 2000, GPU | XP, ordinary | XP, GPU |
| --- | ---: | ---: | ---: | ---: |
| Quake III, 640×480 window (fps) | 466.6 to 467.8 | 28.8 to 29.9 | 443.8 to 469.6 | 28.7 to 28.9 |
| Quake III, 800×600 window (fps) | 418.8 to 422.7 | 29.9 | 392.2 to 392.8 | 28.5 to 28.6 |
| Quake III, 1024×768 full screen (fps) | 374.2 to 375.9 | 84.3 to 84.4 | 373.5 to 376.0 | 83.0 to 83.3 |
| 240 swaps, 640×480 window (ms) | 204 to 210 | 2,693 to 2,765 | 224 to 227 | 8,243 to 8,292 |
| 240 swaps, full screen (ms) | 406 to 408 | 2,688 to 2,721 | 401 to 408 | 2,707 to 2,728 |
| 240 swaps, window with a software pointer (ms) | 203 to 209 | 8,008 to 8,067 | 206 to 228 | 29,696 to 29,919 |

Each GPU-presented frame waits for its fence, and QEMU checks virgl
fences on a 10 ms timer, so a full-screen frame costs at least about
11 ms. A windowed frame also synchronizes desktop pixels around the
window and costs about three such waits. On this host the ordinary path's
download and GDI copy cost under 1 ms a frame. The earlier comparison that
favoured the GPU path at 1024×768 ran under UTM on Apple silicon, where
the CPU copy is far slower. Guest screenshots showed Quake III correct on
the ordinary path in a window and at full screen. XP's `glpresent` pixel
checks failed on both paths in this run, because the test runner left
the Start menu holding the focus. To make the GPU path worth turning on,
its present would first have to stop waiting for each frame's fence, as
the native Direct3D 7 present does.

### Waits no longer sit out the fence timer

QEMU answers a fenced request when it next looks at its fences: right
after it handles a notification of the control queue, or on its 10 ms
timer. A notification with no new request is enough to make it look
again, so while the miniport waits for a reply it rings the control
queue's doorbell again after 0.2 ms, then at doubling intervals up to
2 ms. [QEMU control handler](https://github.com/qemu/qemu/blob/v10.0.0/hw/display/virtio-gpu-gl.c) · [QEMU fence timer](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/virtio-gpu-virgl.c)

One run each on XP SP3 under KVM on a second Intel UHD 630 host (QEMU
10.0, virglrenderer 1.1.0, Mesa 25.0; its numbers are not comparable with
the table above), only the miniport changed. The OpenGL runs set
`RippleGpuDesktop=1`; 3DMark03 ran with the default settings, and in both
of its runs its message about a volume texture it could not create was
answered from the host:

| Test | before | after |
| --- | ---: | ---: |
| Quake III, 640×480 window, GPU path (fps) | 28.8 | 158.5 |
| Quake III, 640×480 window, ordinary path (fps) | 509.7 | 519.9 |
| Quake III, 1024×768 full screen, GPU path (fps) | 83.1 | 290.7 |
| Quake III, 1024×768 full screen, ordinary path (fps) | 379.7 | 384.9 |
| 240 swaps, 640×480 window, GPU path (ms) | 8,306 | 1,282 |
| 240 swaps, full screen, GPU path (ms) | 2,695 | 454 |
| 240 swaps, full screen, ordinary path (ms) | 378 | 378 |
| 3DMark03 3.60, 1024×768, Direct3D (3DMarks) | 1,133 | 2,099 |
| 3DMark03: time waiting for frame N−2 (s) | 143.1 | 9.8 |

Direct3D gains as well: a draw that finds both frame slots in flight
waits for the older one, and those waits no longer last until the timer.
The GPU path is five times faster in a window and three and a half times
at full screen, but still slower than the ordinary path, so it stays
opt-in. On the ordinary path the window swaps took 174 ms before; after,
the warm-up run took 171 ms and the timed one 279 ms in the same boot.
`d9pixels` (41 of 41) and `tpixels hal` (18 of 18) pass with both builds.

### Full-screen windows shown from the GPU

As on Windows 9x (`docs/win9x/mesa.md`), a full-screen OpenGL window can be
shown the way a Direct3D flip chain is: when its client area is the whole
screen, nothing covers it and the pointer is hidden, Mesa copies the frame
on the host GPU into a scanout image of its own and asks the display driver
to show that image (`RVD_PRESENT`). The miniport puts the image on the
display and flushes it, with no copy into the desktop image and no wait for
a fence. When a frame does not qualify, or the program lets go of the image,
the display returns to the desktop image (`RVD_DESKTOP`), which kept
receiving GDI's writes meanwhile.

On Windows 2000 and XP this needs the GPU desktop (`RippleGpuDesktop=1`):
once a guest has shown a resource, virtio-vga keeps the display on
virtio-gpu until a reset, so the display can only come back to the desktop
image, not to VGA. `RVD_DESKTOP` without input tells Mesa whether that
desktop is on; without it every frame stays on the ordinary path.
`RIPPLE_GL_SCANOUT=0` keeps every frame on the ordinary path too.

One boot of XP SP3 under KVM on the second Intel UHD 630 host (QEMU 10.0),
with `RippleGpuDesktop=1`; only the program's environment changed between
the Quake III runs (`timedemo demo001`, 1024×768 full screen):

| Path | fps |
| --- | ---: |
| Ordinary (download, GDI) | 370.4 |
| GPU presentation (`RIPPLE_GL_PRESENT=1`) | 285.4 |
| Scanout (on by default with the GPU desktop) | 414.2 |

All 1,346 frames of the scanout run went through the scanout path (one
flush each, no rectangle copies). Each costs about 1.8 ms in the flush,
which here includes QEMU's egl-headless display reading the image back for
its VNC view. In a 640×480 window the frames are not eligible and take the
ordinary path (484.2 fps).
Picture check: QEMU's egl-headless display cannot dump the screen while a
program's image is shown ("no surface"), so the demo was played at normal
speed in a second boot with an SDL window on the host instead; captures of
that window during the scanout frames show the game correctly (level,
lighting, status bar, the right way up), and after the game the display is
back on the desktop.

### Small desktop writes and the hardware pointer

`RIPPLE_GL_PRESENT=inline` uses the same per-frame copy as `1`.
When GDI finishes drawing, the miniport puts the pixels of up to three
pending rectangles of 64×64 or smaller in one virgl command stream.
It waits for the device to return the command buffer so the guest can
reuse the bytes. It requests no GPU completion fence for those writes.
Larger rectangles retain the fenced transfer. `glpresent.exe window uploads`
writes new colours twice to 13×17, 64×64 and 65×1 rectangles in its own
window and reads both ends and the neighbouring pixel after each write.
`rvdstat.exe` reports the numbers of inline rectangles and submissions.

The independent adapter setting `RippleHardwareCursor=1` enables the
virtio cursor queue in 32-bit modes. The display driver translates GDI's
AND/XOR shape or premultiplied BGRA image into a padded 64×64 resource.
A fenced upload completes before the cursor queue selects it. Mouse
movement uses the existing resource. GDI draws a shape that inverts
whatever lies underneath. [Windows pointer callbacks](https://learn.microsoft.com/en-us/windows/win32/api/winddi/nf-winddi-drvsetpointershape) · [Virtio GPU cursor protocol](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html) · [SPICE cursor pixel format](https://www.spice-space.org/spice-protocol.html)

`cursorqa.exe` checks the monochrome mask, hotspot, movement, hiding and
inversion fallback. For alpha pointers it alternates two known hotspot
colours and returns to the first. Windows can expand the application's
bitmap and move its hotspot when adding a shadow. The probe asks the
display driver to checksum the next alpha bitmap Windows supplies, with
transparent padding to 64×64. It compares that value with the independent
checksum of the entire resource submitted by the miniport. The test also
checks the hotspot colour and counts partially transparent pixels.

Windows 2000 supplied a 41×39 image with hotspot (8,14) for the 32×32
application cursor with hotspot (5,11). Both versions contained 61
partially transparent coloured pixels. The source and submitted checksums
on red, green and red again were `6699b8a1`, `9044e1de` and `6699b8a1`.
Windows rounded each opaque hotspot channel down by one before the driver
received it. The test checks for consistent channel rounding and requires
an exact checksum match for each submitted shape.

### Quake with the hardware pointer

The native OpenGL build of Quake ran the same 969-frame shareware demo on
an M1 Max in UTM. Each figure below is the median of three runs per setting
after warm-up. The game used `timedemo demo1`, `gl_texsort 0`,
`host_framerate 0.05` and `r_norefresh 0`, with sound disabled.
The GDI path downloads the rendered frame to guest memory.
Both GPU paths copy it to the desktop on the host. The inline setting also
changes how the driver submits small GDI writes.

| Guest and rendering area | GDI fps | Batched GPU fps | GPU with inline writes, fps | Host seconds to score, GDI / batched / inline |
| --- | ---: | ---: | ---: | --- |
| Windows 2000 SP4, 640×480 window | 49.5 | 47.6 | 47.7 | 21.67 / 22.74 / 22.92 |
| Windows 2000 SP4, 1024×768 decorated window | 39.7 | 47.8 | 48.2 | 27.08 / 23.02 / 22.86 |
| Windows 2000 SP4, 1024×768 exclusive fullscreen | 40.2 | 47.9 | 47.9 | 26.96 / 22.86 / 22.99 |
| Windows XP SP3, 640×480 window | 48.6 | 47.1 | 47.2 | 22.70 / 23.65 / 23.73 |
| Windows XP SP3, 1024×768 decorated window | 38.3 | 46.2 | 46.5 | 28.40 / 24.36 / 24.17 |
| Windows XP SP3, 1024×768 exclusive fullscreen | 39.1 | 47.4 | 46.6 | 28.28 / 23.09 / 24.07 |

At 640×480 on Windows 2000, GDI frame downloads took 1.24 seconds.
Eliminating them introduced 2.04 seconds of fenced GPU copies on the
inline path. At 1024×768 windowed, GDI made 1,113 downloads in 2.53
seconds. The inline path made one in 0.002 seconds and spent 2.06 seconds
copying on the GPU. Game-process CPU time fell from 11.56 to 6.97 seconds.
The device timers cover only part of the 4.22-second improvement measured
on the host. In exclusive fullscreen the inline path made zero downloads.

The same size effect appeared on XP. GDI's full-frame downloads took
1.25 seconds at 640×480 and 2.46 seconds in the 1024×768 window.
GPU copy time was 1.99 and 2.02 seconds, respectively. In the larger
window, game-process CPU time fell from 12.55 to 7.94 seconds.

With mouse capture disabled on XP, placing the visible software pointer
inside, outside and back inside the 640×480 game window gave 33.5, 48.4
and 33.5 fps. The driver counted 2,213, 88 and 2,207 desktop uploads.
With the hardware pointer the same placements gave 47.2, 47.6 and 50.7
fps, with three, four and four uploads. Both inside runs performed a
single desktop read when returning to GDI.

The 240-frame visible-pointer test used the same driver binaries and
changed only the adapter's cursor setting. On both systems the software
pointer caused a median of 240 desktop reads and 480 uploads. The
hardware pointer reduced both counts to zero. Median swap time fell from
2,101 to 720 ms on Windows 2000 and from 2,181 to 738 ms on XP.

Windows 2000's natural 640×480 game also exposed the pointer cost.
With the software pointer, GDI / batched / inline scored 49.6 / 32.9 /
38.6 fps. Enabling the hardware pointer gave 49.5 / 47.6 / 47.7 fps.
In the inline case, median desktop uploads fell from 2,263 to 95,
partial reads from 1,124 to 39, and host time from 27.70 to 22.92 s.

The launcher checks the game's actual client rectangle, foreground
ownership, 32-bit display mode and window style. Quake first received a
1024×755 client when it requested a decorated 1024×768 window. The
launcher sizes the client before starting the timedemo and requires the
engine to acknowledge the command. Fullscreen uses the game's native
display-mode switch. Every larger run in the table kept the verified size
and foreground, processed 969 frames and released its resources.

An independent Windows 2000 fullscreen series gave 40.3 / 47.7 / 48.4
fps. Its companion windowed series missed a timedemo command and is
excluded. The Windows 2000 640×480 run used the earlier alpha probe that
expected the unrounded application colour. The corrected probe passed in
both larger Windows 2000 runs and on XP with identical graphics binaries.
On XP one 640×480 ordinary-GDI synthetic case lost foreground.
Its pixels matched. Two independent 240-frame repeats and both indexed
page-flip repeats passed. On Windows 2000, the 8-bit test after Quake
returned palette index 255 throughout page zero with the hardware pointer
enabled and disabled. Of 786,432 pixels, 774,144 differed from the
expected pattern. Both isolated repeats on each setting passed. The
source of that repaint remains open. A subsequent GDI-only boot reproduced
the same locked page on both immediate repeats, after ten Quake launches
and zero GPU desktop frames. [Indexed page issue](https://github.com/mizaimao/moose-ripple/issues/114)

## Native Direct3D 7 pixel test

The first test uses the system DirectDraw and Direct3D 7 runtimes. It opens
a 128×128 offscreen surface on a 1024×768×32 desktop. Run
`d3d7probe rgb` for the Microsoft software renderer, `d3d7probe target`
to check the host render target, and `d3d7probe hal` to request the hardware
device. `d3d7probe window` also presents the rendered triangle to a clipped
window using a primary surface marked for Direct3D. It samples the triangle,
the clear colour and two untouched pixels outside the destination rectangle.
Exit code 0 means the pixels and counters matched, 1 means a check
failed, and 2 means the requested hardware was unavailable.

`z16pixels rgb` and `z16pixels hal` request a separate 16-bit depth
surface on the software and hardware device respectively. Two overlapping
triangles are drawn in both orders. The nearer red triangle must win both
times. With depth writes disabled, the blue triangle drawn last must win.
Each draw also checks a blue pixel outside the overlap and a black pixel
outside both triangles.

The software reference clears all 16,384 pixels to dark blue, then draws a
triangle with red, green and blue vertices. It checks 4,095 interior pixels
against the interpolated colours and 11,233 background pixels against the
clear. It repeats the draw for 120 frames and locks the target before ending
the timing.

| Test, three runs per system | Windows 2000 SP4 | Windows XP SP3 |
| --- | --- | --- |
| Original driver, software device | 3 passes | 3 passes |
| Original driver, hardware device | 3 unavailable | 3 unavailable |
| Host render targets enabled, before accepting the 3D surface flag | 3 rejected | 3 rejected |
| Same setting, after accepting the flag | 3 passes | 3 passes |

The last two rows used the same guest, driver configuration and executable
on each system. Each passing target check recorded one GPU fill, one readback
and zero uploads. All pixels matched. The target check calls DirectDraw
`Blt` and verifies the storage used by a later Direct3D device.

The historical Direct3D 8 Quake `demo1` comparison used 969 frames at
640×480. The optimized medians were 24.2 fps on Windows 2000 and 24.1 fps
on XP. Version 0.81 requires Direct3D 8. The earlier 0.73 Direct3D 7 renderer
can play the same demo. [Port and game test plan](https://github.com/mizaimao/moose-ripple/issues/106)

### Native hardware device and full-target clear

Set both `RippleDirectDrawGpuSurfaces` and `RippleNativeD3D7` to 1 on the
test adapter. The native option applies to the 32-bit display mode. Run
`d3d7probe clear` to request the stock Direct3D 7 hardware device and check
a full 128×128 blue clear. It checks the returned device GUID, hardware
rasterization flag, every target pixel, one completed native clear and the
live context and surface counts after release.

On XP SP3, supplying the texture-stage and driver-state callbacks let the
runtime request the driver's depth formats and extended capabilities.
Returning the extended capability record then changed hardware device
creation and clear from zero successes in three tries to three in three.
Windows 2000 SP4 also passed all three hardware clears. Each clear had
zero pixel mismatches. Ordinary DirectDraw hardware and
software fills passed, and a separate 16×16 XRGB texture check returned
all 256 uploaded pixels on each of three readbacks. In the clear-only revision the
native draw stopped at its first triangle-list command.

### First native triangle

The same test now draws transformed, untextured Gouraud triangle lists.
On XP SP3 all three hardware runs and three RGB software runs passed. Each
hardware run counted 121 triangles and 122 clears. Both sampled triangle
frames matched all 4,095 interior and 11,233 background pixels checked.
After release the driver counted zero live contexts and targets.

The median time for 120 frames at 128×128, including the final synchronous
read, was 431.0 ms on the hardware device and 47.2 ms on the RGB device.

### Submission timing on XP

The instrumented test used the same executable before and after batching.
Each timed block drew 120 frames at 128×128 and waited for its final pixel
read. The hardware clear and Gouraud checks passed in all six runs.

| XP SP3 | Three hardware runs, ms | Median, ms | Render submissions / fences per frame |
| --- | --- | --- | --- |
| Separate clear and draw | 451.0, 445.0, 452.2 | 451.0 | 2 / 2 |
| Batched at scene end | 865.2, 264.5, 261.2 | 264.5 | 1 / 1 |

When a scene exceeds one command packet, intermediate packets carry no
GPU fence. A surface read synchronizes earlier drawing to that target.

Time in the render queue fell from about 395 ms to 200 ms per block.
The slowest batched run also spent about 200 ms in that queue. The other
delay occurred outside it. The software control medians were 44.7 ms
and 44.6 ms. No texture uploads occurred in these timed blocks.

The original Direct3D Quake 0.73 window reached 640×480 with both drivers.
The game reported `DDERR_INVALIDCAPS` and never recorded a hardware
context or draw. Neither run produced a `demo1` score for comparison with
the historical 0.81 renderer. [Author's game releases](https://dxquake.sourceforge.net/)

### Two frames in flight on stock QEMU

The opt-in native driver keeps up to two scenes in flight. When submitting
frame N it waits only if frame N−2 still owns the command buffer it needs.
A lock, copy or destruction of a render target waits for outstanding GPU
work on that target. The test setting `RippleD3D7WaitCurrent=1` selects the
synchronous control in the same driver binary.

Linux QEMU 9.2.0 with KVM and Intel UHD 630 ran a 128×128 render target on a
1024×768, 32-bit desktop. Each timing covers 120 clear-and-triangle scenes
through the final synchronized surface lock. Each row reports the median of
three runs per mode. The RGB column uses the software device in the
two-frame driver's guest.

| Guest | Wait for each frame | Two frames in flight | RGB software |
| --- | ---: | ---: | ---: |
| Windows 2000 SP4 | 1,200.869 ms | 55.789 ms | 8.400 ms |
| Windows XP SP3 | 1,200.930 ms | 64.165 ms | 11.243 ms |

XP's three two-frame runs took 65.063, 27.259 and 64.165 ms. All reached
two outstanding scenes, completed all 120 fences by the final read and
matched both triangle samples. Waiting after each scene cost about 1.194 s
per block. The two-frame runs recorded zero waits for N−2 and one
9.50–9.82 ms wait at the final read.

Windows 2000's two-frame runs took 103.018, 41.512 and 55.789 ms. Each
completed 120 fences and passed the pixel test. Only the second run waited
for N−2, twice for 19.241 ms in total. All three waited for the target at
the final read, for 9.37–9.92 ms. On both guests the hardware device
completed 121 triangles and 122 clears per test and released its context
and render target.

QEMU checks for completed GPU work after processing new control commands.
If work remains, it schedules another check 10 ms later. The measured
9.95 ms per synchronous fence is consistent with waiting for that timer.
Submitting the next scene gives QEMU another opportunity to retire the
previous fence. [QEMU control handler](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/virtio-gpu-gl.c#L92-L102) · [QEMU fence timer](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/virtio-gpu-virgl.c#L977-L986)

The separate XRGB texture test wrote and read back 20 images at each size.
On both guests, 16×16 and 64×64 writes used 20 inline submissions and zero
upload fences. At 65×65 the existing transfer path used 60 submissions and
20 fences. All texels matched.

Direct3D Quake 0.73 reported `DDERR_INVALIDCAPS` before creating a native
device on all four hardware boots. Two further boots disabled the native
Direct3D device, one per OS. Both reproduced the same game error and passed
all three RGB software pixel tests. Each of the original game's four D3DX7
windowed context requests returned the same error in a separate test.
Explicit requests for a hardware device without depth and for the RGB
software device also failed. DirectDraw successfully created a plain
primary surface and a 640×480 offscreen hardware render target. Creating
a 16-bit depth surface returned a separate error, `DDERR_NOZBUFFERHW`.
The game recorded no frame rate in any of the six boots. The historical XP
score on this Intel system was 57.3 fps for the same 969-frame demo in the
0.81 Direct3D 8 version.

A separate trace captured the DirectDraw calls made by the stock D3DX7
library when given the game's four windowed context requests. On both guests
each attempt reached `CanCreateSurface` once. The driver returned
`DDERR_INVALIDCAPS` for `0x6200`, a primary surface in video memory marked
`DDSCAPS_3DDEVICE`. Requests explicitly selecting hardware with or without
depth and RGB software with 16-bit depth reached the same rejection. The
ordinary primary (`0x4200`) and the 640×480 offscreen hardware render
target (`0x6040`) succeeded in the direct control. [Microsoft DirectDraw surface flags](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/ns-ddraw-ddscaps)

### Windowed primary and game startup

Four fresh Intel QEMU boots compared the original display driver and the
change that accepts the 3D flag on a primary surface. The miniport and the
other 24 test-drive files, including the game and pixel test, matched.
On both systems the original driver rejected the primary in all three
window tests. The changed driver accepted it in all three. All six changed
driver tests rendered the offscreen triangle correctly, counted 122 clears
and 121 triangles, and released their contexts and targets. The final copy
to the window returned `E_FAIL` before the test could sample a visible pixel.

All four windowed startup requests from Direct3D Quake 0.73 advanced from
`DDERR_INVALIDCAPS` to `D3DXERR_CAPSNOTSUPPORTED` on both guests. The
same library error occurred when the test explicitly requested hardware
with depth disabled. Explicitly requesting the Microsoft RGB device with
16-bit depth succeeded and returned its software device identifier
(`a4665c60`). Direct3D's hardware depth-format enumeration returned
`DDERR_NOZBUFFERHW` and zero formats.

The original game made five surface-registration calls with the changed
driver on each OS. It made zero native context or draw calls and returned
no timedemo score. The offscreen controls below are medians of three
120-frame runs at 128×128, including the final read. The changed boots
also rendered three extra window-test sequences before these timings.

| Guest | Hardware before | Hardware after | RGB before | RGB after |
| --- | ---: | ---: | ---: | ---: |
| Windows 2000 SP4 | 81.212 ms | 44.658 ms | 8.835 ms | 11.177 ms |
| Windows XP SP3 | 66.008 ms | 35.603 ms | 11.063 ms | 4.899 ms |

Two further fresh boots traced the copy into the test window. Both
systems returned `E_FAIL` for all three tagged-primary cases before
entering the display driver's `Blt` callback. Releasing the window's
device context first and copying the entire target both reproduced it.
Changing only the primary to an ordinary one let the full 128×128 and
cropped 96×96 copies reach the miniport. Both copies matched all four
visible samples on each OS. The ordinary DirectDraw hardware and
software window controls also passed, including the covered regions.
Each tagged test recorded two surface registrations, compared with one
for an ordinary primary. The driver's registration callback treated a
mapped primary without an offscreen slot as `DDERR_SURFACELOST`. The
follow-up change acknowledges that registration and keeps hardware render
targets in their existing offscreen slots. Fourteen DDK descriptor checks
per driver verified the callback result and whether it assigned a surface
handle.

Four fresh boots compared the old and new registration callbacks. Each pair
used the same test program, miniport, Quake executable and settings. Only
the display DLL differed. A passing copy required the display callback, a
successful miniport copy and all four expected pixels in the test window.

| Window copy | Windows 2000 before | Windows 2000 after | XP before | XP after |
| --- | ---: | ---: | ---: | ---: |
| 3D-tagged primary, three cases | 0/3 | 3/3 | 0/3 | 3/3 |
| Ordinary primary, two controls | 2/2 | 2/2 | 2/2 | 2/2 |

The three tagged cases covered a 96×96 crop with the window device context
held, the same crop after releasing it and a full 128×128 copy. The old
driver received zero copy callbacks for these cases. The changed driver
received one per case. The hardware and software DirectDraw window controls
also preserved the regions obscured by another window.

Each of the five cases also timed 120 offscreen frames at 128×128 through
the final surface read. The window copy followed the timed interval. Each
case completed all 120 submitted frames with zero pending or failed.

| Guest | Old registration, median of five | New registration, median of five |
| --- | ---: | ---: |
| Windows 2000 SP4 | 31.616 ms | 33.783 ms |
| Windows XP SP3 | 32.771 ms | 60.541 ms |

On XP the first case took 32.771 ms before and 31.950 ms after. All four
later cases took longer with the new driver. Each XP boot waited once for
a two-frame queue slot across 600 frames. Each case's final GPU wait was
9.28–9.93 ms. Those waits do not account for the change in median. A repeat
with the offscreen control run before any windows is needed to isolate it.

Stock Direct3D Quake 0.73 at 640×480 returned
`D3DXERR_CAPSNOTSUPPORTED` on every boot. It made five surface registrations
and zero native context, clear or draw calls per launch. It produced no
`demo1` score to compare with the historical Direct3D 8 result.

At the current 1024×768×32 display mode, direct hardware queries
enumerated one texture format and zero depth formats on both systems.
The stock D3DX7 library returned `D3DXERR_CAPSNOTSUPPORTED` for hardware
device caps and reported zero depth, colour-texture and render-target
formats, whether given that mode explicitly or asked for the current mode.
For the RGB device, the same library reported two depth, seven colour-texture
and six render-target formats. Its hardware capability query enumerates
depth before texture formats and skips collecting the rest when depth
enumeration fails. The 16-bit depth test draws overlapping triangles in
both orders and repeats with depth writes disabled. On both revisions of
each guest, Microsoft's RGB device matched all nine pixel samples. The
hardware device returned `DDERR_NOZBUFFERHW` at enumeration, before drawing.

### Native 16-bit depth surface storage

Four fresh Windows 2000 SP4 and XP SP3 boots compared the production driver
with and without Z16 surface storage. The test programs, desktop mode and
driver settings matched. The previous driver returned `DDERR_NOZBUFFERHW`
for both requested sizes on each OS. The new driver accepted both sizes.
`zstore.exe` wrote a pattern, overwrote one pixel through a partial lock,
then read the whole surface via DirectDraw.

| Z16 video-memory surface | Row pitch | Windows 2000 values matched | XP values matched | Miniport transfers per guest |
| --- | ---: | ---: | ---: | --- |
| 3×3 | 6 bytes | 9/9 | 9/9 | 2 reads, 2 writes |
| 257×129 | 514 bytes | 33,153/33,153 | 33,153/33,153 | 2 reads, 2 writes |

The colour controls passed on both revisions and systems, including all
four pixels in the test window. Each boot ran one 120-frame block per mode
after the storage check. The candidate had just completed four depth
uploads and four reads. The control had rejected both allocations.

| Colour control, 120 frames | Windows 2000 before | Windows 2000 after | XP before | XP after |
| --- | ---: | ---: | ---: | ---: |
| Software RGB | 11.157 ms | 13.539 ms | 2.365 ms | 6.099 ms |
| Native offscreen | 32.777 ms | 41.063 ms | 21.941 ms | 31.477 ms |
| Native offscreen before window copy | 23.802 ms | 92.270 ms | 72.688 ms | 77.284 ms |

The separate `z16pixels` overlapping-triangle
test matched nine samples with Microsoft's RGB device on every boot.
Hardware depth enumeration on this storage-only revision still returned
`DDERR_NOZBUFFERHW`.

With Microsoft's RGB device forced, the original Direct3D Quake 0.73
completed all 969 frames of `demo1` at 640×480 on Windows 2000. Both the
control and new driver measured **57.6 fps**. All four matching D3DX7
startup requests selected the RGB device. The driver's counters recorded
zero native contexts, clears and draws during the game. Both XP boots
started the demo, then the game exited with `0xc0000005` before scoring.
The test restored the software-selection registry setting on all four
boots. The XP wrapper reported success despite the game's failure. The
game process exit and timedemo output determine the result.

Two separate XP boots isolated timing around the first attempted visible
copy. Each ran three 120-frame offscreen blocks, one tagged-primary
window copy and three more offscreen blocks. Only the registration DLL
differed. Times below are medians of the three blocks in each group.

| XP registration | Before the copy | Visible pixels | After the copy |
| --- | ---: | ---: | ---: |
| Previous | 58.178 ms | 0/4 | 57.303 ms |
| Fixed | 58.930 ms | 4/4 | 81.489 ms |

Both drivers completed all 120 frames in every block. After the successful
copy, the fixed driver's three runs took 76.444–81.733 ms. The measured
miniport operations accounted for 22.921–23.693 ms per block. None waited
for an older frame's queue slot. The next timing check is at the Direct3D
runtime calls and final surface lock.

### Native 16-bit depth rendering

The native Direct3D 7 device now attaches a Z16 surface to its colour
target. The GPU clears depth, compares incoming triangle depth and
honours the depth-write enable state. A CPU lock on depth waits for the
colour context that last wrote it. Reusing the same attachment adds no
queue submission or fence wait.

Four fresh Intel UHD 630 guests compared the storage-only driver with
the depth renderer. The display DLL and miniport were the only two
files that changed. On both systems the control returned
`DDERR_NOZBUFFERHW` before the first hardware depth pixel. The new HAL
matched Microsoft's RGB reference in all three cases. Each case also
checked a far-only pixel and untouched background.

| Hardware depth test | Expected overlap | Windows 2000 SP4 | Windows XP SP3 |
| --- | --- | ---: | ---: |
| Near triangle first, then far | Red | 3/3 | 3/3 |
| Far triangle first, then near | Red | 3/3 | 3/3 |
| Near triangle first, depth writes disabled | Blue | 3/3 | 3/3 |

Before Quake started, each candidate had counted six more native
triangles and three more clears than its control, matching the three
new depth cases. Microsoft's RGB device matched all nine
pixels on all four boots. The existing Z16 lock test still matched
33,162 values per boot. The native window test matched all four pixels.

The stock Direct3D Quake 0.73 startup changed on both systems. All four
game-matched D3DX7 requests had previously returned
`D3DXERR_CAPSNOTSUPPORTED`. All four selected the hardware device
(`84e63de0`) with the new driver. Quake then finished `demo1` at
640×480 and exited normally on each system.

| Stock Quake, 969 frames | Windows 2000 SP4 | Windows XP SP3 |
| --- | ---: | ---: |
| Timedemo rate printed by the game | 39.7 fps | 39.1 fps |
| Native GPU clears during the game | 970 | 970 |
| Native GPU triangles during the game | **0** | **0** |
| Game observed in the foreground | 25/25 samples | 0/26 samples |

**Game rendering remains incomplete.** Both drivers last reported
command 26, `D3DNTDP2OP_INDEXEDTRIANGLELIST2`, and
`D3DNTERR_COMMAND_UNPARSED` (`0x88760bb8`). The game loop advanced
without the driver drawing its triangles. A performance comparison with
software or the historical game baseline needs confirmed scene geometry.

Each guest also ran one 120-frame colour block in each configuration.
The offscreen timer ends on the final lock, before the visible copy in
the window case. These single observations show the variation across
boots. The dedicated repeated XP timing experiment is recorded above.

| Colour control, 120 frames | Windows 2000 storage | Windows 2000 rendering | XP storage | XP rendering |
| --- | ---: | ---: | ---: | ---: |
| Microsoft RGB | 13.491 ms | 13.594 ms | 10.136 ms | 6.537 ms |
| Native offscreen | 21.968 ms | 32.034 ms | 84.156 ms | 76.255 ms |
| Native offscreen before window copy | 47.692 ms | 48.967 ms | 20.967 ms | 31.572 ms |

### Native indexed triangle lists

The display driver now handles `D3DNTDP2OP_INDEXEDTRIANGLELIST2`
(command 26). It adds each 16-bit index to the command's base vertex,
checks the referenced vertex range and submits the vertices in draw
order. [Indexed triangle list format](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dhal/ns-d3dhal-_d3dhal_dp2indexedtrianglelist2).

The `ipixels.exe` test draws the reference red, green and blue triangle
using vertices 4, 1 and 3 from a five-vertex buffer. The other two
vertices make a sequential draw produce the wrong image. Four fresh
Intel UHD 630 guests compared the previous driver with the indexed
implementation. Only the display DLL differed between the two inputs.

| Indexed test at 128×128 | Windows 2000 SP4 | Windows XP SP3 |
| --- | ---: | ---: |
| Native triangles completed, previous driver | 0 | 0 |
| Native triangles completed, indexed driver | 121 | 121 |
| Incorrect pixels in either of the two native reads | 0/15,328 | 0/15,328 |
| Native time for 120 indexed frames, including final read | 21.905 ms | 21.866 ms |
| Microsoft RGB time for the same 120 frames, previous / indexed driver | 10.977 / 11.179 ms | 10.649 / 11.013 ms |

Each native read checked 4,095 interior pixels and 11,233 background
pixels. The driver completed 120 queued frames after the warm-up, with
one wait at surface access and zero queue-slot waits. Of the timed block,
the read took 10.212 ms on Windows 2000 and 10.326 ms on XP. The ordinary
triangle, window copy, nine hardware depth samples and all 33,162 depth
storage values also passed on every boot.

The original Direct3D Quake 0.73 completed the 969-frame `demo1` at
640×480 on all four boots. Each boot's four matching D3DX7 requests
selected the hardware device. The driver counters immediately before
and after the game isolate its work from the earlier pixel test.

| Quake on the previous / indexed driver | Windows 2000 SP4 | Windows XP SP3 |
| --- | ---: | ---: |
| Rate printed by the game (one run per driver) | 37.8 / 38.8 fps | 39.7 / 39.3 fps |
| Completed native clears | 970 / 970 | 970 / 970 |
| Completed native triangles | 0 / 0 | 0 / 0 |
| Game observed in the foreground | 26 of 26 on both | 0 of 25 / 0 of 26 |

After the game, both previous drivers reported an unparsed command 26
(`0x88760bb8`). Both indexed drivers reached the draw handler and
reported `DDERR_UNSUPPORTED` (`0x80004001`) on that command at byte
offset zero. No game triangle completed, so these frame rates cannot
establish rendering performance against software or the historical
baseline. The next diagnostic records the vertex format and active
render settings at each rejected draw.

### Native textures, blending and culling

The native path now draws texture stage zero. A texture lives in system
memory, or in "video memory" that the runtime allocates in the program's
own memory, in one of six RGB formats (32-bit with and without alpha,
5-6-5, 1-5-5-5 with and without alpha, 4-4-4-4). Programs and the
runtime's texture manager write textures without telling the driver, so
each texture used in a scene is read once, compared with its last upload
by checksum, and converted to B8G8R8A8 for a host texture. The texture
manager's copies arrive as texture blits between the two kinds of memory.
Stage-zero colour and alpha operations become fragment shaders generated
per combination. Alpha test, blending, cull modes, flat shading, the
sampler state, triangle strips and fans, and immediate fans are handled.
A draw this path cannot reproduce is skipped and counted; the rest of the
command buffer is still drawn.

`tpixels.exe hal|rgb` draws each case on a 128×128 target and checks
pixels well inside the shapes. Every case passed on both the hardware
device and Microsoft's RGB device, on a fresh Windows 2000 SP4 and a
fresh Windows XP SP3 guest on the Intel UHD 630. The earlier colour,
indexed and depth tests also passed on both.

| Case | Checks |
| --- | --- |
| Select texture, 8888 / 565 / 1555 / 4444 | four quadrant colours and the background each |
| Texture × diffuse colour | four half-bright quadrants |
| Alpha test (greater than 0x80) | two quadrants kept, two discarded |
| Blend source alpha / additive | blended colour within 3 levels |
| Cull none / counter-clockwise / clockwise | which of two triangles remains |
| Flat shading | colour of the first vertex |
| Texture rewritten between scenes | the new colour in the next scene |
| Video-memory and managed textures (hardware only) | four quadrant colours |

Microsoft's RGB device widens 5-bit red to 248; the hardware path
replicates the high bits to 255. Video-memory and managed textures need
texture hardware by design, so the RGB device reports `DDERR_NOTEXTUREHW`.

### T&L device and mipmapped textures

The driver also offers the T&L device (`D3DDEVCAPS_HWTRANSFORMANDLIGHT`).
The runtime then sends object-space vertices together with the world,
view and projection matrices, the viewport and the depth range, and a
second vertex shader applies them on the host. Transformed draws are
clipped to the Direct3D viewport with a scissor rectangle. Lights are not
applied yet: a lit draw keeps its vertex colours and is counted.
`tpixels.exe tnl` runs the cases above through this device, and `xyz` or
`persp` sends object-space vertices with an orthographic or a right-handed
perspective projection. All cases pass on both guests.

Video-memory textures may be mipmap chains. NT creates such a chain one
level per `CreateSurface` call, each call carrying the top level's
description, so every level is sized from its own surface. Draws sample
the top level only; mipmapped draws are counted.

Before this, Quake 0.73 drew no level at all. Its world and model
textures are managed mipmapped textures, the runtime's texture manager
could not create their video-memory copies, and it dropped every draw
that used one without an error. The geometry seen in earlier screenshots
was not the level either. On the hardware device the runtime transforms
vertices itself into a 64 KB buffer, and with a driver that never swaps
vertex buffers it indexes vertices it has not written once that buffer
fills: on XP it starts batches past the end, on Windows 2000 it states
more vertices than the buffer holds. The driver skips triangles outside
the stated window on that device. The T&L device does not use this
buffer, and guest screenshots now show the level, models, lighting and
status bar as expected.

Three changes then made it faster. A frame's commands went to the host in
2 KB packets, each but the last a request that waited for the host; they
now go in packets of up to 64 KB, queued without waiting. A draw request
now carries up to 32 triangles instead of 8. A windowed present waited for
the frame's fence and then read the image back with a fenced transfer,
and QEMU checks virgl fences on a 10 ms timer; a present now only queues
the frame and reads the image back unfenced in the same context, which the
host runs after the frame's drawing. Locks and blits still wait.

| 969-frame `demo1` at 640×480 | Windows 2000 SP4 | Windows XP SP3 |
| --- | ---: | ---: |
| First correct picture, one run each | 18.0 fps | 16.7 fps |
| After the three changes | 45.6 fps (one run) | 36.3 and 39.7 fps (two runs) |
| Native triangles drawn | 2,195,812 | 2,185,962 |
| Triangles skipped | 0 | 0 |
| Draw requests | 98,265 | 97,864 |
| Waits for a frame's fence | 97 | 94 |

Runs of the same XP build differ by about 3 fps. Four scene slots instead
of two, and unfenced texture uploads, measured 36.5 and 37.4 fps against
36.3 and 39.7, so neither was kept. The earlier, faster-looking numbers on
the software vertex path drew about a tenth of the triangles.

### Lighting

The T&L device applies Direct3D 7 fixed-function lighting. The display
driver keeps the material, the lighting render states and up to 32 lights
by index, and sends the first eight enabled lights with each lit draw; the
driver offers eight active lights. One generated vertex shader loops over
the lights in camera space: point, spot and directional lights with range
and attenuation, global and per-light ambient, emissive, specular with a
local or distant viewer, normalized normals, and material colours taken
from the vertex where its format has them.

`tlight.exe rgb|tnl` lights a quad facing the camera in fifteen cases and
compares pixels inside it with Direct3D's lighting equations evaluated for
a corner vertex. The quad and every light are symmetric about the view
axis, so all corners light alike. All fifteen cases pass on Microsoft's RGB
device, which checks the expected values, and on the T&L device, on both
Windows 2000 SP4 and XP SP3.

| Case | Checks |
| --- | --- |
| Directional diffuse, two directional lights | colour times N·L, sums |
| Global ambient, light ambient, emissive | ambient and emissive terms |
| Point light: attenuation 0, linear attenuation, out of range | 1/(a0 + a1·d), range cut |
| Spot light inside the falloff band, outside the outer cone | falloff factor, cut-off |
| Diffuse colour from the vertex | material source |
| Specular with local and distant viewer | halfway vector, power |
| Normal of length 2 with and without normalizing | NORMALIZENORMALS |

### Fog

The native path fogs draws. Table fog is computed per pixel from the depth,
or from w when the runtime's WINFO command says the projection is
perspective. On the T&L device a vertex fog mode is computed from each
vertex's camera-space depth, or its distance with range fog. Otherwise the
fog factor is the specular alpha, which the runtime fills when it does the
vertex processing. Linear, exponential and squared exponential modes are
generated into the fragment shader. The driver offers vertex, table, range
and w-based fog.

`tfog.exe rgb|hal|tnl` fogs a blue quad toward red at one depth and
compares pixels inside it with Direct3D's fog formulas. Microsoft's
software vertex path ignores range fog, and its RGB device has no w-based
fog; there the results match z-based fog, so those cases run only where the
device offers them. Every case that runs passes on Windows 2000 SP4 and XP
SP3.

| Case | Vertices | RGB | HAL | T&L |
| --- | --- | :---: | :---: | :---: |
| Specular-alpha factor | transformed | pass | pass | pass |
| Table fog from z: linear, exponential, squared | transformed | pass | pass | pass |
| Vertex fog: linear, exponential | object space | pass | pass | pass |
| Vertex fog with range | object space | n/a | n/a | pass |
| Table fog from z under an orthographic projection | object space | pass | pass | pass |
| Table fog from w under a perspective projection | object space | n/a | pass | pass |

### Two texture stages

The native path draws two texture stages, and the driver offers two
stages with two textures. Stage one combines over stage zero's result with
the same operations, where an argument may also be the current colour; it
samples its own texture with its own coordinate set, which travels in the
otherwise unused half of each vertex's texture-coordinate constant. A third
enabled stage is still skipped and counted.

The renderer keeps only the sampler and view slots up to the last one set,
so setting slot zero alone drops slot one. A draw with stage one therefore
sets both slots in one command each. Quake 0.73 found this at once: with
two stages offered it draws its lightmaps in stage one, and with stage
zero's texture changing between draws the lightmaps came out as noise.

`tstage.exe rgb|hal|tnl` draws a quad with two 8×8 textures of four flat
quadrants. Stage one reads the second coordinate set, mirrored left to
right, so each screen quadrant combines a known texel of each texture.
Microsoft's RGB device draws stage zero alone correctly but no texture at
all once stage one is enabled, so the expected values follow Direct3D's
documented operations and the RGB device runs the two-stage cases for
reference only. All nine cases pass on the HAL and T&L devices on Windows
2000 SP4 and XP SP3, and Quake's lightmapped walls look as they did with
one stage (44.3 and 39.7 fps).

| Case | Checks |
| --- | --- |
| Stage zero only | control |
| Modulate, add, select current | stage one over stage zero |
| Blend by texture alpha, by current alpha | stage-one alpha sources |
| Texture times diffuse | stage one ignoring the current colour |
| First coordinate set | TEXCOORDINDEX |
| Stage zero's texture changes between two draws | both slots stay bound |

### Mipmaps

Textures are drawn with all their mipmap levels. The display driver walks
each texture's mipmap chain when the texture is registered, reads and
checksums every level once a scene, and uploads every level; texture blits
copy the matching rectangle of each level. Host textures carry all their
levels, their views cover them, and the stage's point or linear mip filter
becomes the sampler's. A sampler's maximum level of detail must be set:
left at zero it clamps sampling to the top level.

`tmip.exe rgb|hal|tnl` draws a 64×64 texture of seven flat-coloured levels
on quads sized to pick a given level. Sizes sit a quarter level inside each
level: Microsoft's RGB device rounds the level of detail down, hardware to
the nearest level, and exact powers of two fall on the boundary between
them. The HAL and T&L devices also draw a texture the runtime manages,
whose video-memory copy is a separate chain. Every case passes on all three
devices on Windows 2000 SP4 and XP SP3, and Quake runs at 43.0 and 38.4
fps with its mipmaps drawn.

| Case | Expected |
| --- | --- |
| No mip filter | level 0 |
| Point filter, quads for levels 0, 1 and 3 | that level |
| Linear filter at level 2.5 | half level 2, half level 3 |
| The same five cases on a managed texture (HAL and T&L) | as above |

### Speed against Microsoft's RGB device

Three more changes took cost out of each Quake frame. The display driver
now gathers a scene's draws into one miniport request of up to 64 KB
instead of one request per draw: 129,000 requests became about 12,000. A
windowed present read its rectangle back from the host and waited for it,
and Windows presents each visible rectangle of a partly covered window
separately; the miniport now reads the whole image back once and copies
later rectangles of the same frame from staging, until any other request
touches an image. Every texture a scene used was read and checksummed
once a scene; a video-memory texture, which includes the runtime's
managed copies, changes only through DirectDraw locks and blits, so those
now mark it dirty and a clean one is used as it is. System-memory
textures are still checked once a scene. `tpixels` rewrites its video and
managed textures between two draws to check the second change arrives.

On the Intel host, Quake's 969-frame `demo1` at 640×480 in a window ran
the native T&L device and then Microsoft's RGB device in the same Windows
2000 SP4 boot (`SoftwareOnly` set for the second run). The host GPU's
minimum clock was held at 1200 MHz and the CPU governor at `performance`
for these runs; at the host's default 350 MHz minimum, runs of one build
varied from 36 to 50 fps.

| Windows 2000 SP4, three boots each | Native T&L device | RGB device, same boot |
| --- | ---: | ---: |
| Before the three changes | 121.0, 121.1, 121.4 fps | 91.7, 85.7, 88.1 fps |
| After them | 130.1, 130.4, 130.6 fps | 83.8, 86.0, 85.1 fps |
| After them, Windows' default timer period | 60.6, 61.0 fps | 54.4, 53.9 fps |

The test launcher sets a 1 ms system timer period with `timeBeginPeriod`
for the first two rows. With Windows' default 15.6 ms period both devices
lose time to a wait on the system timer that the driver does not make;
the base build also ran 60.7 fps there, so that row cannot show the
driver's own changes. On XP SP3 the native device ran 123.1 fps with the
1 ms period. In the same native run on Windows 2000 the display driver
spent 5.3 s of 7.4 s in its `DrawPrimitives2` handler, the miniport about 0.85 s
presenting and 1.4 s uploading textures; texture checks took 5 ms where
they had taken 1 to 3.5 s. All pixel tests pass on both systems.

### Page flipping at 8 and 16 bits with native Direct3D on

With native Direct3D switched on, every full-screen DirectDraw flip chain
at 8 or 16 bits failed with `DDERR_UNSUPPORTEDMODE` on both systems, and
DirectDraw's software fallback then broke the test program. The flip
probe now prints the capabilities DirectDraw reports after each mode
change, and `d7draw` counts the display driver's DirectDraw setup per
depth. The driver's setup succeeded in the lower depths, yet DirectDraw
reported no hardware there (`DDCAPS_NOHARDWARE`). With native Direct3D
off, the same build reported the usual blit and flip capabilities and
every flip passed. The only difference was that the 32-bit desktop
described a Direct3D HAL and the lower depths described none.

Every depth now gives the same Direct3D description while native
Direct3D is on. Below 32 bits it cannot be used: the device renders only
to 32-bit targets, and textures and host surfaces are refused there. On
Windows 2000 SP4 and XP SP3 the 8-bit and 16-bit hardware, software and
default flip cases pass with native Direct3D on, including palette
animation, and the Direct3D pixel tests still pass afterwards on the
32-bit desktop.

### On by default

Native Direct3D 7 is on unless the adapter key has the DWORD
`RippleNativeD3D7` set to 0. Its render targets (offscreen surfaces with
the 3D-device flag), 16-bit depth buffers and textures are host images.
Plain 2D offscreen surfaces in video memory are still declined with
`DDERR_OUTOFVIDEOMEMORY`, so DirectDraw keeps them in system memory as
before (see DirectDraw surfaces in system memory).
`RippleDirectDrawGpuSurfaces` set to 1 still moves them to the host for
tests. Without the native device, stock Quake 0.73 runs on Microsoft's
RGB device, which drew the level as a flat colour in guest screenshots.

Each system was installed from the stock INF with neither switch set,
tested, then restarted with `RippleNativeD3D7` set to 0 and tested
again. KVM on the Intel UHD 630 host, GPU minimum clock at 1200 MHz:

| Check | Windows 2000 SP4 | Windows XP SP3 |
| --- | --- | --- |
| Direct3D 7 pixel tests on the RGB, HAL and T&L devices: colour, depth, indexed triangles, textures, lighting, fog, two stages, mipmaps | pass | pass |
| Quake 0.73 `demo1`, 640×480 window, 1 ms timer | 127.2 fps | 121.6 fps |
| Guest screenshots during the demo | textured level, weapon and status bar | the same |
| DirectDraw full-screen flips at 8, 16 and 32 bits | pass | pass |
| Plain offscreen surface asked for in video memory | declined; system memory passes | the same |
| After `RippleNativeD3D7` = 0: hardware device | not listed | not listed |
| After `RippleNativeD3D7` = 0: RGB device and 32-bit flips | pass | pass |

The Quake rates match the earlier runs with both switches set, 130 fps
on Windows 2000 and 123 fps on XP. With native Direct3D switched off, XP
gave the same plain-surface results as with it on: the hardware window
and fill cases declined the video-memory surface, and the system-memory
cases passed.

The default also gives Direct3D 8 and 9 programs a hardware device on XP
SP3: Microsoft's own `d3d8.dll` and `d3d9.dll` runtimes drive this
Direct3D 7 level driver for fixed-function drawing. Before, both failed
the hardware capability query with `D3DERR_NOTAVAILABLE`. In one boot
from the stock INF, the Direct3D 8 and 9 textured-scene tests matched
all 4,096 pixels in both frames, and the Direct3D 8 build of Quake (0.81)
ran `demo1` in a 640×480 window at 57.2 fps with a 1 ms timer; guest
screenshots showed the textured level, weapon and status bar. The test
launcher could not close the game afterwards with its bound key. Vertex
and pixel shaders are not offered (the capabilities report shader
version 0), and a Direct3D 9 vertex declaration test failed with
`E_FAIL`. Windows 2000 ships with Direct3D 7 only and was not tested here.

## Direct3D 8 driver interface

With native Direct3D 7 on, the display driver also offers the Direct3D 8
driver interface. Pixel and vertex shaders can only be reported through
it, so the shader work builds on it. To switch it off, set the DWORD
`RippleD3D8` to 0 in the adapter key and restart; Microsoft's Direct3D 8
and 9 runtimes then translate their calls to the Direct3D 7 interface, as
they did before.

The driver answers the GetDriverInfo2 requests with a D3DCAPS8 record
and a list of surface formats with the operations each allows. It accepts
the DirectX 8 DrawPrimitives2 commands: vertex stream zero, index
buffers, `DrawPrimitive` and `DrawIndexedPrimitive` with their byte-offset
forms, and the clipped triangle fans of software vertex processing. The
record copies what the runtime itself builds for this Direct3D 7 driver,
except that it offers one vertex stream, which marks a DirectX 8 driver.
Shaders are reported as version 0.0, so the runtime still runs vertex
shaders in software.

Four details mattered on XP SP3:

- GetDriverInfo2 requests share their GUID with stereo-mode queries. On
  NT the outer expected size is always 24 bytes; the real size is in the
  request itself, as the DDK says. Copying only 24 bytes of the record made
  the runtime report no hardware device (`D3DERR_NOTAVAILABLE`).
- The runtime marks the blits of `Present` with `DDBLT_PRESENTATION` and
  `DDBLT_LAST_PRESENTATION`, but only for DirectX 8 drivers. The display
  driver refused them as an unknown operation, and the runtime then locked
  the back buffer and copied it with the CPU: Direct3D 8 Quake ran at
  29.6 fps instead of 58.7. The flags are ignored now; each present is
  finished before `Blt` returns.
- `DrawPrimitiveUP` vertices arrive through a vertex buffer the runtime
  owns and registers like any other, set as stream zero.
- The runtimes create a Direct3D 8 or 9 context with 4 as the input
  handle value; Direct3D 7 passes 3. Direct3D 8 numbers the first world
  matrix 256, and its mipmap filter values are one lower than Direct3D 7's.

Installed from the stock INF under KVM on the Intel UHD 630 host:

| Check | Windows XP SP3 | Windows 2000 SP4 |
| --- | --- | --- |
| `d8pixels`, hardware and software vertex processing: clear, triangles from user memory, vertex buffer, strip, indexed, indexed with a base vertex, managed texture, untransformed vertices | 8 of 8 pass each way | no Direct3D 8 runtime installed |
| Direct3D 8 and 9 textured scenes from Microsoft's runtimes | pass | not run |
| Direct3D 7 pixel tests on the RGB, HAL and T&L devices | pass | pass |
| DirectDraw flips at 8, 16 and 32 bits | not rerun | pass |
| Quake 0.73 `demo1`, 640×480 window, 1 ms timer | 122.0 fps | 128.1 fps |

Direct3D 8 Quake (0.81), the same `demo1` in a 640×480 window, ran three
times with the interface on and three times after a restart with
`RippleD3D8` set to 0, in one installation:

| Interface | Run 1 | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| Direct3D 8 | 57.6 fps | 58.7 fps | 58.6 fps |
| Direct3D 7 (switched off) | 58.4 fps | 58.6 fps | 58.7 fps |

Guest screenshots showed the level, weapon and status bar drawn the same
way. `d8pixels.exe` is built against the Direct3D 8 headers of the
Windows 7 WDK (`WDK_ROOT`).

## Direct3D 8 pixel shaders

The device reports pixel shader version 1.4 (2.0 to a Direct3D 9 runtime). The display driver
translates each shader into Gallium TGSI text (`d3d8_ps.c`) when the
runtime creates it, and the miniport keeps the text and creates the host
shader in each render target's context on first use. A pixel shader draw
carries the coordinates of texture stages 0 to 3 as four components each,
the constants c0 to c7 and up to four textures.

The translator covers the arithmetic instructions with their source
modifiers (negation, bias, sign scale, complement) and result modifiers
(shifts, saturation), co-issued pairs, `cnd` and `cmp`, constants given
by `def`, and the texture instructions `tex`, `texcoord`, `texkill`,
`texreg2ar`, `texreg2gb`, `texreg2rgb`, the `texm3x2` pair, `texm3x3`
without sampling, `texdp3` and `texdp3tex`. Results are clamped to
[-1, 1] after each instruction, matching the maximum pixel shader value
of 1 the device reports, so negative intermediate values survive as they
do on ps 1.1 hardware. A shader using bump mapping, cube maps or depth
output is refused; the runtime still creates it, and draws with it are
counted and skipped. `d7draw` reports shaders created and refused.

During development `d3d8_ps.c` was also built natively on the Linux
build host, and Mesa's TGSI text parser accepted each test shader's
translation before it ran in a guest.

Not yet drawn with a pixel shader: texture transforms and generated
coordinates. Version 1.4 is not offered.

Installed from the stock INF under KVM on the Intel UHD 630 host:

| Check | Windows XP SP3 | Windows 2000 SP4 |
| --- | --- | --- |
| `d8pixels`: 8 fixed-function and 11 pixel shader cases, hardware and software vertex processing | 19 of 19 pass each way | no Direct3D 8 runtime |
| The same pixel shader cases one per process, plus five single-modifier variants | 16 of 16 pass | not run |
| Direct3D 8 and 9 textured scenes | pass | not run |
| Direct3D 7 pixel tests on the RGB, HAL and T&L devices | pass | pass |
| DirectDraw flips at 8, 16 and 32 bits | not rerun | pass |

The pixel shader cases cover colour and texture inputs, `def` and program
constants, the modifiers including a negative intermediate result,
co-issue with alpha blending, `cnd`, per-channel `cmp`, `dp3`,
`texcoord`, `texkill` and two textures, each against colours computed on
the CPU.

One bug only showed with several shaders in one device: when a stage's
sampler and view were already current, the draw queued an empty command,
which the miniport treats as a failure, and the transport stopped. Each
case passed in its own process, which pointed at state carried between
draws. `d7draw` now prints the step at which the miniport last refused a
draw.

Quake 0.73 `demo1` in a 640×480 window, three runs on each build:

| Build | Windows XP SP3 | Windows 2000 SP4 |
| --- | --- | --- |
| Direct3D 8 interface only | 121.9, 123.0, 129.7 fps | 128.1 fps (one run) |
| With pixel shaders | 122.0, 122.1, 119.8 fps | 127.7, 128.6, 127.5 fps |

Direct3D 8 Quake (0.81) ran at 57.9 and 58.7 fps with pixel shaders
offered, as before.

### Fog and lighting with pixel shaders

Fog now blends the result of a version 1 pixel shader as it does the
fixed-function colour: the factor comes from the specular alpha, from
the depths the untransformed vertex shader computes, or from the pixel
depth, with the same choice as the fixed-function draws. The translator
writes one program per fog setting a shader is drawn with and keeps up to
four. Untransformed vertices with lighting on are now drawn with a pixel
shader too, through a variant of the lighting vertex shader that passes
on all four coordinate sets.

On XP SP3, `d8pixels` gained four cases: alpha fog from the specular
alpha, linear table fog, a quad lit by a directional light from its
material, and the same quad with linear vertex fog. All 23 cases passed
with hardware vertex processing, where the host lights and fogs the
vertices, and with software vertex processing, where the runtime does.
Direct3D 8 Quake ran at 58.8 fps.

## Pixel shader 2.0 for Direct3D 9 programs

Direct3D 9 programs now get pixel shader 2.0 on the hardware device. The
display driver translates ps_2_0 shaders into TGSI as it does version 1
shaders: declared samplers and coordinates, `def` constants, c0 to c31,
and the shader model 2 instructions with negation and absolute value,
computed unclamped as Direct3D 9 does. The expanded instructions (`nrm`,
`crs`, `dp2add`, `sincos` and the matrix forms) go through scratch
registers, and the result is written after fog. Only 2D samplers are
accepted, because the device has no cube or volume textures yet, and
texture coordinates t0 to t7 and samplers s0 to s7 are available (see the
next section).

No new driver interface was needed. Microsoft's Direct3D 9 runtime asks
a driver of the Direct3D 8 interface for its Direct3D 8 record, and it
announces itself as version 9 just before. The record reports pixel
shader 2.0 to that runtime and 1.3 to the Direct3D 8 runtime. Declaring
the Direct3D 9 interface itself (version 4 in GetDriverInfo2) made the
runtime report no hardware device, both with a Direct3D 9 record that
matched what the runtime builds and with pixel shader 2.0 in it; the
interface would need more than these replies. Once 2.0 is offered, the
runtime also sets states for samplers 8 to 15, which are now ignored
instead of refused.

On XP SP3 under KVM on the Intel UHD 630 host, `d9pixels` (the
`d8pixels` cases through the Direct3D 9 interfaces, plus eight shader
model 2.0 cases) passed every case with hardware and with software
vertex processing:

| Case | Result |
| --- | --- |
| Fixed-function, version 1 pixel shaders, fog and lighting through Direct3D 9 | pass |
| ps_2_0 texture lookup times the diffuse colour | pass |
| ps_2_0 `texkill` | pass |
| ps_2_0 `pow`, `rcp`, `rsq`, `nrm` and `dp2add` with values above one | pass |
| `pow`, `rcp`, `rsq`, `nrm`, `dp2add` each alone | pass |

`d8pixels` still saw version 1.3 and passed its 23 cases, the Direct3D 8
and 9 textured scenes passed, and Direct3D 8 Quake ran at 58.8 fps. The
runtime refuses a shader that writes `oC0` with `_sat`, and a version 1.1
shader in the Direct3D 8 test with `_bias` on a texture register and a
negated `def` constant; `d9pixels` skips that one unless asked for it.
Windows 2000 has no Direct3D 9 runtime of its own and was not rerun then; its
Direct3D 7 path is unchanged. (Later, with the DirectX 9.0c runtime installed,
Direct3D 8 and 9 pass there too: see "Direct3D 8 and 9 on Windows 2000".)

## Eight coordinate sets and samplers for pixel shader 2.0

Shader model 2 programs can read texture coordinates t0 to t7 and sample
s0 to s7. A pixel shader draw carries only the coordinate sets up to the
last one its program reads: each vertex in the record is 9 + 4 x sets
words (position, colours, normal, then the sets), and the vertex shaders
step through the vertex constants by a stride passed in `CONST[0].z`
instead of a fixed seven or eight constants. They output all eight sets
as `GENERIC[2..9]`, so a set the draw does not carry holds words of the
next vertex, which the program never reads; the fog depths moved from
`GENERIC[6]` to `GENERIC[10]`. Version 1 programs keep t0 to t3, and the
device still reports four simultaneous textures, the limit for them and
for the fixed-function stages.

On XP SP3 under KVM on the Intel UHD 630 host, a new `d9pixels` case draws
with eight coordinate sets, samples s7 with t7 and s6 with t5 and adds
t6, while sets 0 to 4 run the other way so that a wrong set would show.
`d9pixels` passed all 31 cases with hardware and with software vertex
processing, `d8pixels` its 23, the Direct3D 8 and 9 textured scenes
passed, and Direct3D 8 Quake ran at 58.7 fps (58.6 before).

## Vertex shaders on the host GPU

With the `RippleVertexShaders` registry value set to 1, the driver reports
vertex shader 2.0 to Direct3D 9 programs (256 constants) and 1.1 to
Direct3D 8 programs (96), and runs the shaders on the host GPU instead of
leaving them to the runtime's software vertex processing. The value is off
by default until real programs have been checked with it.

Microsoft's Direct3D 9 runtime passes vs_2_0 shaders through the Direct3D 8
interface: each CreateVertexShader record holds a Direct3D 8 declaration
(stream 0 only while the driver offered one stream, the registers already
assigned from the shader's `dcl` statements, which are removed from the
code) followed by the code. The
display driver keeps both and translates them at the first draw:

- The vertices travel as the stream holds them. Each one is padded to whole
  constants and placed after the shader's constants; the vertex program
  fetches its inputs from those words itself, unpacking `D3DCOLOR`,
  `UBYTE4`, `SHORT2` and `SHORT4` with integer operations, so the display
  driver needs no floating point.
- Instructions of vs_1_1 and vs_2_0 share the pixel shader 2.0 rules where
  they agree; the vertex-only ones (`slt`, `sge`, `dst`, `lit`, `expp`,
  `logp`, `sgn`, `mova`) translate to their TGSI counterparts. Relative
  addressing (`c[a0.x + n]`) loads the address register just before the
  read. Flow control (`rep`, `loop`, `if`, `call`) is not translated yet;
  such shaders are refused and their draws skipped.
- oPos goes through the Direct3D viewport into the render target's clip
  space with w kept, so the host clips as Direct3D does. oD0 and oD1 are
  saturated, oT0 to oT7 reach the pixel program as `GENERIC[2..9]`, and oFog
  becomes the fog factor through the specular alpha when vertex fog is on.
- Without a pixel shader, the texture stages after the vertex shader become
  a pixel program of their own, made from the colour and alpha operations
  and arguments of up to eight stages (bump mapping, premodulate and the
  three-argument operations are not translated).

State blocks are recorded and replayed as Direct3D expects: commands
between STATESETBEGIN and STATESETEND go into the block instead of taking
effect, STATESETEXECUTE replays them, CAPTURE refreshes their values from
the current state, and CREATE makes a block of the whole current state.
The driver used to skip these commands. Direct3D 9 effects (D3DX) keep the
states of their passes in state blocks, so before this DirectQ drew its
text, particles and screen flashes without blending.

On XP SP3 under KVM on the Intel UHD 630 host, with `RippleVertexShaders`
set:

| Check | Result |
| --- | --- |
| `d9pixels`, hardware vertex processing: the pixel cases, five vertex shader cases (position and viewport, byte, short and colour inputs with relative addressing, oFog, the stages after a vertex shader) and blending after shaders | 38 of 38 |
| The same with software vertex processing (Microsoft's runtime runs the shaders) | 38 of 38 |
| DirectQ 1.5 (a Direct3D 9 Quake engine with vs_2_0 and ps_2_0 effects), `timedemo demo1` at 640x480 in a window | renders the scene, text, particles and flashes; 55.1 fps |

Without the value, `d9pixels` passed its 31 cases, `d8pixels` its 23, the
Direct3D 7 checks passed, and Direct3D 8 Quake ran at 58.7 fps, as before.
DirectQ cannot run without hardware vertex shaders. 3DMark03 starts but
refuses every game test: it needs cube textures, DXT1 and DXT3 compressed
textures, a back buffer with alpha and a stencil buffer, none of which the
device offers yet.

## DXT compressed textures

Direct3D 8 and 9 programs can create DXT1 to DXT5 textures. The device
reports the five DXT FourCC codes and lists them as texture formats. A
compressed level holds rows of 4x4 blocks: that is the pitch the driver
gives video-memory levels, and how it reads them back. Before each upload
the display driver decodes every level to ARGB with integer arithmetic,
so the host needs no compressed formats. DXT2 and DXT4 decode as DXT3 and
DXT5. Texture blits copy whole blocks.

On XP SP3 under KVM on the Intel UHD 630 host, three new `d9pixels` cases
draw a DXT1 texture with four solid blocks and one interpolated block,
and show the alpha of a DXT3 and a DXT5 block as grey. All passed, with
`d9pixels` at 34 of 34 with hardware vertex processing and 41 of 41 with
software vertex processing. `d8pixels` (23 of 23), the Direct3D 7 checks
and Direct3D 8 Quake (58.6 fps) were unchanged.

### DXT blocks decoded by the host

The miniport asks the host once, through its virgl capability set, which
DXT formats it can sample. Where it can, compressed textures are created
in that format and their levels go to the host as rows of 4x4 blocks,
exactly as the program wrote them; the host GPU decodes them. Elsewhere,
or with the `RippleHostDxt` registry value set to 0, the miniport answers
texture id zero and the display driver decodes the blocks itself, as
above. (The refusal cannot be an error code: the video port reports every
failure status of a request as the same one.)

`d9texload` creates 32 managed textures of 512x512 with every mipmap level
in DXT1 and in DXT5, fills them, and times drawing each once. On XP under
KVM on the Intel UHD 630 host (which samples DXT1, DXT3 and DXT5):

| Path | DXT1, 32 textures | DXT5, 32 textures |
| --- | --- | --- |
| Blocks decoded by the host | 170 to 199 ms | 193 to 199 ms |
| Decoded in the guest (`RippleHostDxt` 0) | 1995 to 1997 ms | 2011 to 2012 ms |

`d9pixels`, including its DXT cases, passed all 34 cases on both paths.
In a guest whose processor is emulated the difference is larger still.

## Stencil, 24-bit depth and render targets with alpha

Depth buffers can now have 24 bits of depth with 8 of stencil: Direct3D 8
and 9 list D24S8 and D24X8 next to D16, and Direct3D 7 enumerates a 32-bit
depth buffer with stencil. The host keeps them as S8_UINT_Z24_UNORM (the
byte order of D24S8); the host's renderer here does not know the other
order, Z24_UNORM_S8_UINT, and reports it as an unknown format. These
buffers are not read by the processor (Direct3D does not offer locking
them either).

The stencil render states (enable, comparison, reference, read and write
masks, and the fail, depth-fail and pass operations) travel with each draw
of every path: fixed-function, pixel shader and vertex shader draws. The
miniport puts them in the depth-stencil object, both faces alike (the
one-sided stencil of Direct3D 7 to 9), and sets the reference value apart.
Clears take the stencil value too. Draws with stencil on used to be
skipped; now only a stencil test without a stencil buffer is ignored, as
in Direct3D, where it always passes.

A render target may keep alpha: an A8R8G8B8 back buffer is accepted over
the X8R8G8B8 desktop. The host image always stored alpha; what changes is
blending. Destination alpha, its inverse and the saturated source alpha
now use the stored value on such targets, while targets without alpha
keep Direct3D's rule that destination alpha reads as one.

On XP SP3 under KVM on the Intel UHD 630 host, the new `d9stencil` draws
through the Direct3D 9 hardware device with each back buffer and depth
buffer: it marks an area in the stencil and draws only there, clears the
stencil and draws everywhere, counts two passes with INCRSAT, blends with
the back buffer's alpha, and tests depth on the 24-bit buffer.

| Back buffer, depth buffer | Result |
| --- | --- |
| X8R8G8B8, D16 | 2 of 2 (destination alpha reads as one) |
| X8R8G8B8, D24S8 | 5 of 5 |
| A8R8G8B8, D16 | 2 of 2 (destination alpha from the clear) |
| A8R8G8B8, D24S8 | 5 of 5 |

`d9pixels` (34 of 34), `d8pixels` (23 of 23), the Direct3D 7 checks and
Direct3D 8 Quake (58.7 fps) were unchanged.

## Cube textures

Direct3D 8 and 9 programs can create cube maps in every texture format
(the device reports cube maps with mipmaps). The display driver finds the
six faces among the attachments of the top face, each with its mipmap
sublevels, then reads, decodes and uploads every face; texture blits copy
all six. The miniport makes a cube host texture of six layers, uploads
each face into its layer, and gives its views all six layers. Pixel
shaders sample cube maps: shader model 2 by its `dcl_cube` declaration,
version 1 by the texture bound to the stage at the draw, which selects the
translated variant. The fixed-function stages do not sample cube maps
(generating their coordinates is not done); such draws are skipped.

On XP SP3 under KVM on the Intel UHD 630 host, two new `d9pixels` cases
draw six quads that look up a cube map with one colour per face in each of
the six directions, with ps_2_0 and with ps_1_1. Both passed, `d9pixels`
at 36 of 36 with hardware vertex processing and 43 of 43 with software
vertex processing; `d9stencil` (5 of 5), `d8pixels` (23 of 23), the
Direct3D 7 checks and Direct3D 8 Quake (58.6 fps) were unchanged.
3DMark03 now starts without warnings and offers all four game tests.

## Full-screen devices, render target textures, volume textures: 3DMark03 completes

**Full screen.** A flip chain that is a 3D device is accepted in 32-bit
modes. Its two surfaces keep their frame buffer pages, and each also gets a
host image, because the Direct3D 8 and 9 runtimes draw to the two surfaces
in turn (they swap the surfaces after each flip, and NT does not swap their
memory). Direct3D registers only the chain's front surface; the back
buffer's handle is found among its attachments. A flip copies the image of
the surface about to be shown into its page, then shows that page.

Once a full-screen chain has mapped the frame buffer into a process,
Windows adds that view's address to every lock it completes for the
driver. That pointed texture locks at unmapped memory (3DMark03 crashed in
d3d9.dll), so the driver now answers video-memory texture locks itself.

**Render target textures.** A texture that is also a 3D device (32-bit,
one level) is a host image: Direct3D draws into it by its handle and
samples it as it is, through a texture id that names the image. The
miniport attaches the image to each sampling target's context and submits
its own draws first. A depth buffer may be larger than its target, as
Direct3D 9 allows. Up to 16 host images may exist at once (was 4).

**Volume textures** (uncompressed, up to 256 per side, with mipmaps) are
read and uploaded slice by slice into 3D host textures, and copied for the
volume blit command (managed volumes). Pixel shaders sample them (`dcl_volume`,
or a version 1 stage bound to a volume); the fixed-function stages do not.

Checked on XP SP3 under KVM on the Intel UHD 630 host:

- `d9full` (new) opens full-screen X8R8G8B8 and A8R8G8B8 devices, draws
  three frames and reads the front buffer back: both pass.
- `d9pixels` gained render target texture cases (fixed function and
  ps_1_1) and volume cases (ps_2_0 and ps_1_1): 40 of 40 with hardware
  and 47 of 47 with software vertex processing.
- Unchanged: `d9stencil` 5 of 5, `d8pixels` 23 of 23, the Direct3D 7
  checks, 32 DXT textures in 170 to 200 ms, Direct3D 8 Quake 58.4 to
  58.6 fps.
- 3DMark03 3.60 runs to the end and scores **1157 3DMarks** at
  1024x768. Most of its pictures are still wrong (white sky, black
  characters): 4.8 million triangles were skipped because a shader or a
  texture stage combination is not translated (most often `t0_bx2`, a
  texture register with the sign-scale modifier), and one volume texture
  could not be created. Those are the next fixes.

`guiclick` gained `-press #ID` and `-close` for programs that draw their
own buttons, and `-debug`, which runs the program under a debugger thread
and reports its exceptions with the faulting memory, the frame chain and
the code addresses on the stack. `d7draw` prints the last flips, locks and
render target changes, and texture skips by reason.

## Bump mapping in version 1 pixel shaders

Version 1 pixel shaders may read a texture register with the sign scale
(`t0_bx2`) in the `texm3x2`, `texm3x3` and `texdp3` instructions, as normal
maps do; it is applied into a scratch register first. `texm3x3tex` looks up
the third row's vector in a cube map or volume, and `texm3x3spec` and
`texm3x3vspec` look up the reflection of the eye vector (a constant, or the
w of the three rows' coordinates) about the bumped normal. All were refused
before, and the draws using them skipped; each translation was checked
with Mesa's TGSI parser.

On XP SP3 under KVM on the Intel UHD 630 host, 3DMark03 now draws all of
its 18.7 million triangles with no shader failures (4.8 million were skipped
before) and still scores about 1158 3DMarks. `d9pixels` (40 of 40) and
`d8pixels` (23 of 23) are unchanged. Its pictures are not right yet: the
Proxycon and Troll scenes are very dark and the water in Mother Nature is
mostly white; one volume texture is still refused by Direct3D before it
reaches the driver.

## Textures in many render targets: 3DMark 2001 SE

A texture, or a render target texture, was attached to at most 16 colour
contexts at once. 3DMark 2001 SE draws one texture into more than 16 of
its render targets: the 17th attach failed with `ERROR_NOT_ENOUGH_MEMORY`,
the miniport took that for a host error and stopped the GPU, and every
later request failed. The benchmark reported `Present:
D3DERR_DRIVERINTERNALERROR` after Car Chase at low detail. Each texture and
host image now keeps a bit per image slot instead of a list; a draw
attaches its textures to its target on first use, and destroying an image
clears its slot's bits. The display DLL no longer keeps its own list of
targets per texture, which made it upload a texture again for each new
target.

`d7draw` found it: its `D7REFUSED` line now covers fixed-function draw
records too and gives the first refusal (a refused render target texture
says which step failed), and a new `D7FIRST` line names the first failed
request (IOCTL, status, image or texture id) and the first failed host
command (stage and status). Here the first failure was the texture upload
request, at the attach stage, with status 8.

On XP SP3 under KVM on the second Intel UHD 630 host (QEMU 10.0), 3DMark
2001 SE now completes Car Chase, Dragothic and Lobby at low and high
detail (8.6, 4.3, 27.1, 5.7, 16.1 and 10.3 fps, without the fence change
of the previous section), with no failed request. It stops at Nature:
"device does not support bump normal maps", the next fix. `d9pixels` (41
of 41) and `tpixels hal` (18 of 18) pass.

## Bump maps: 3DMark 2001 SE runs its game tests

Direct3D 8 and 9 programs are offered V8U8 and Q8W8V8U8 textures (also as
cube maps and volumes). Their signed texels go to the host as they are, into
`R8G8_SNORM` and `R8G8B8A8_SNORM` textures; the miniport offers them only
where the host's capability set says it samples those formats, as for DXT.
Version 1 pixel shaders translate `texbem` and `texbeml`: stage n's
coordinates move by the bump map's du and dv through stage n's bump matrix,
and `texbeml` then scales the colour by the luminance times the scale plus
the offset. Such a program reads 16 constants: c0..c7, the four stages'
matrices, then their luminance scales and offsets, filled by the display
driver from the texture stage states; its fog constants follow. Version 1.0
shaders, which Direct3D 8.0 programs use, translate like 1.1 (they were
refused, and every 3DMark 2001 SE shader is 1.0).

`d9pixels` case 48 draws through a V8U8 map whose du moves the next stage's
lookup by a quarter of its texture, and checks that the left column shows
the right one. The translations of `texbem`, `texbeml` (with and without
fog) and 3DMark 2001 SE's first shader pass Mesa's TGSI parser.

On XP SP3 under KVM on the second Intel UHD 630 host, with the fence change
above, 3DMark 2001 SE now runs all four game tests: Car Chase 44.3 and 18.9
fps, Dragothic 37.5 and 12.2, Lobby 71.1 and 45.2, Nature 29.0. Its feature
tests run up to Advanced Pixel Shader, which stops the benchmark with
"Could not create texture - D3DERR_INVALIDCALL" before a score; the
environment and DOT3 bump mapping tests report "not supported by hardware"
(the fixed-function stages have no bump operations yet). `d9pixels` (42 of
42) and `tpixels hal` (18 of 18) pass. Many triangles are still skipped
(vertex formats and textures the fixed-function path refuses), so the
pictures are not right yet.

## DXT volume textures

DXT1 to DXT5 were offered as 2D and cube textures only, and the display
driver refused to allocate a compressed volume. They are now offered as volumes too. The
driver allocates them in rows of 4x4 blocks, slice after slice, copies them
block row by block row for the runtime's managed volumes, and decodes them
to 32-bit texels on the way to the host (the host DXT path has no slices). `d9pixels` case 49 reads a 4x4x4 DXT1 volume with one solid
colour per slice through a version 1 shader.

On XP SP3 under KVM on the second Intel UHD 630 host, case 49 passes and
`d9pixels` passes 43 of 43. 3DMark03's "Unable to create volume texture"
remains: the runtime refuses that volume before it reaches the display
driver (no surface was refused), so its format or size is still another
one the device does not offer.

## Colour write mask: 3DMark03's shadow volumes

Direct3D 8 and 9 programs draw stencil shadow volumes with
`D3DRS_COLORWRITEENABLE` at zero: the volumes only count into the stencil
buffer. The display driver ignored that state, so 3DMark03's game tests 2
and 3 showed the volumes as large white and black polygons. The display
driver now passes the channels not written in the blend word (bits 20 to 23),
for fixed-function draws and draws with shaders alike, and the miniport turns
them into the blend state's colour mask; the mask also applies without
blending. Direct3D 8 and 9 are offered `D3DPMISCCAPS_COLORWRITEENABLE`.
`d9pixels` case 50 draws white quads over the clear colour, one with only
red written and one with nothing written, and checks that the first keeps
the clear colour's green and blue and the second all of it.

On XP SP3 under KVM on the second Intel UHD 630 host, case 50 passes and
3DMark03's game tests 2 and 3 no longer show the polygons: the face in
Troll's Lair is shaded by its shadows. Game test 2 is still darker than it
should be, and the water in game test 4 is still white.

## DOT3 in the Direct3D 7 stages

The two-stage combiner of the Direct3D 7 draws now does
`D3DTOP_DOTPRODUCT3`: both arguments go from 0..1 to -1..1, and four times
their dot product over red, green and blue, saturated, goes to every channel.
Direct3D puts that result in the alpha as well, so the display driver gives
the alpha the same operation. Direct3D 7, 8 and 9 programs are offered
`D3DTEXOPCAPS_DOTPRODUCT3`. `d9pixels` case 53 dots a diffuse colour that
encodes a vector along x with a texture factor along x (white) and along y
(black).

On XP SP3 under KVM on the second Intel UHD 630 host, case 53 passes and
`d9pixels` passes 43 of 43. 3DMark 2001 SE's DOT3 Bump Mapping test, which
reported "Not supported by hardware", now runs.

## Speed: scene packets

How a Direct3D frame's time is spent, measured on 3DMark 2001 SE (XP SP3 under
KVM on an Intel UHD 630 host, the host otherwise idle). `d7draw` prints the
display driver's time (`D7TIME`), the miniport's per operation (`D7PROFILE`:
calls, host submits, queueing and fence waits) and the miniport's host
object caches (`D7CACHE`: hits, new entries, entries replaced; `D7SHADERS`:
host shaders made). `D7PROFILE`'s byte counts wrap at 4 GB.

Baseline, a 7.7-minute benchmark run: the caches never replaced an entry
(268 fragment shaders made in all), so host shaders were not remade per
draw. The guest spent 295 s in DrawPrimitives2, 238 s of it in draw requests
to the miniport, and the miniport made 1.95 million host submits. A scene
packet held 64 KB, about ten draws, since a draw carries its vertices as
constants (16 words a transformed vertex). Each submit costs a fence, a
batch in the host's GPU driver and a round trip, with two packets in flight.
On the host, QEMU's virtual processor thread ran at 60% (mostly VM exits)
and its drawing thread at 41% (Mesa 24%, copies in the C library 20%, the
kernel and i915 33%): the two take turns waiting.

Scene packets are now 256 KB (`RB2K_D7_FRAME_WORDS`), as large as Mesa's own
virgl command buffers, each slot in its own common buffer: one 512 KB block
of contiguous memory for both slots was refused at the first scene. 3DMark
2001 SE went from 3696 to 5122 (timed, host alone); submits during draws
from 1.95 million to 648 thousand, and the same fixed-length tests drew 721
million primitives against 515 million.

Only a frame's last packet carries a fence now. QEMU answers a fenced packet
only once the host GPU has finished it, noticed on a 10 ms timer or a
notification; a packet slot needs only that QEMU has read the packet, which
an unfenced packet reports at once. 3DMark 2001 SE went from 5122 to 5446
(timed, host alone), and fence waits during draws from 11.6 s to 0.01 s.
Presents still wait 58 s on their fences in a run: the next lead.
## Texture eviction

The miniport keeps Direct3D textures on the host within a texel budget
(`RB2K_D7_TEXEL_BUDGET`, 64 MB) and 1024 slots. Host textures have no guest
backing; the budget guards the host's memory. Past it, the miniport refuses a
new texture, and the draws that use it used to be skipped.

The display driver now evicts the least recently used texture instead:

- Each texture carries the draw clock (`used`) of the last draw that chose
  it. The clock advances after every queued draw, so all textures of the draw
  being built carry the current value.
- When a new texture is refused for room, the host copy of the texture with
  the oldest stamp is freed (`IOCTL_RB2K_D3D7_TEXTURE_FREE`) and the request
  is sent again, until it fits or nothing is left to free. Textures the
  current draw has chosen and render target textures are never freed.
- Freeing goes through the same request path as everything else, which sends
  the queued draws first, so no queued draw names a freed texture.
- An evicted texture has no host copy (`host` 0), so it is read and uploaded
  again from its surface the next time it is drawn.

The refusal reaches the display driver as the miniport's
`ERROR_NOT_ENOUGH_MEMORY` or as one of its translations by the video port and
GDI; all are accepted. `d7draw` prints `D7EVICT` with the evictions, the new
textures that needed them and the last refusal's status. A scene that needs
more than the budget evicts and uploads again every frame: correct, but slow.

`d9pixels` case 74 draws 24 textures of 1024x1024 (96 MB) in two scenes.
## Lines and points

Line lists, line strips and point lists used to be skipped in every path:
the Direct3D 7 commands (`POINTS`, `LINELIST`, `LINESTRIP`, `INDEXEDLINELIST`,
`INDEXEDLINELIST2`, `INDEXEDLINESTRIP`, `LINELIST_IMM`) and the Direct3D 8
and 9 stream draws. Programs use them for HUDs, maps, wireframes and
particles.

- A draw packet (`RB2K_D3D7_DRAW` and `RB2K_D3D8_DRAW`) says what its
  vertices are in `primitive`: `RB2K_PRIM_TRIANGLES`, `RB2K_PRIM_LINES` (two
  vertices a line) or `RB2K_PRIM_POINTS`. The display driver emits two or one
  vertices a primitive; strips become separate lines.
- The miniport refuses a packet whose vertex count is not a multiple of its
  primitive's, and draws Gallium lines or points. The rasterizer's line width
  and point size are 1.0: Direct3D draws lines one pixel wide and, without
  point sprites, points as one pixel.
- An indexed stream draw reads `2 * n` indices for a line list, `n + 1` for a
  line strip and `n` for a point list.
- Direct3D 7 line caps are the triangle caps, so programs that check them
  find texturing and the depth test for lines.

Point sprites (`D3DRS_POINTSPRITEENABLE`, point sizes above one) are not done:
the maximum point size stays 1.0. `d9pixels` case 73 draws three lines and a
block of nine points.
## Trace tools: d9trace and d8trace

`d9trace.dll` and `d8trace.dll` are logging stand-ins for `d3d9.dll` and
`d3d8.dll`. Copied into a program's folder under the runtime's name, with the
real runtime copied beside them as `d3d9real.dll` or `d3d8real.dll`, they
pass every call on and log to `d9trace.log` (`d8trace.log`): format checks
that fail and those of cube and volume textures, refused textures, devices
made, and the caps the program reads, the whole record as hex words. They pin
themselves, since a program that frees the runtime after probing would
otherwise crash in the unloaded hooks. 3DMark03's refused volume texture and
3DMark05's hardware check were found with them.

`guiclick` drives a program's windows for unattended runs. `-press @Title#N`
presses the button with control ID N only in a window whose title contains
Title (3DMark05's owner-drawn buttons share IDs across windows), and nothing
is pressed in a window a modal dialog has disabled.
## Pixel shader 1.4

A Direct3D 9 runtime offers the programs pixel shader 2.0, which includes
1.4, but the translator refused version 1.4 and the driver skipped those
draws. Direct3D 8 programs were offered 1.3, so 3DMark 2001 SE ran its
Advanced Pixel Shader test in its fallback; they are now offered 1.4. 3DMark03 uses 1.4 for its per-pixel lighting when it is offered: in one
run the driver skipped 6.1 million of its primitives (11,475 shader
failures, all `ffff0104`), and its game tests 2 to 4 came out dark.

Version 1.4 is translated by the version 1 translator:

- Six registers r0 to r5 (`TEMP[0..5]`), six coordinate sets and samplers.
  A t# register is read only by `texld` and `texcrd`.
- `texld rN, src` samples sampler N (the destination's number) at a
  coordinate set, or after `phase` at a register (a dependent read).
  `texcrd rN, t#` copies the coordinates. A t# source may be divided by its z
  or w (`_dz`, `_dw`) and selects xyz or xyw.
- `phase` only separates the two halves: the registers carry over.
- `bem rN.xy` moves its first source by stage N's bump matrix times its
  second, with the constants `texbem` uses.
- `texkill` takes r# or t#, and the `_x2` source and `_x8`/`_d8` result
  modifiers of 1.4 are accepted.

`d9pixels` case 72 copies a coordinate set into a register with `texcrd`,
then after the phase samples a texture at t0 and another through that
register, and adds them.
## L16 textures: 3DMark03's procedural volumes

3DMark03 reported "Unable to create volume texture" and went on without the
texture. Its code (`3DMark03.exe`, the constructor at 0x4f5e10) creates
128x128x128 volume textures with one level in the managed pool: two
`D3DFMT_A8R8G8B8` and two `D3DFMT_L16`, filled procedurally (its "Volume
texture creation %d%%" progress). The driver did not offer L16 at all, so the
L16 creations failed.

L16 is now offered for textures, cube textures and volume textures. Like the
other formats the host does not take as they are, its texels go to the host as
B8G8R8A8: grey from the top eight bits of each one.

`d9pixels` case 63 creates a 128x128x128 L16 volume texture and draws a 2x2
L16 texture, white and half grey (0x8000); on XP SP3 under KVM (UHD 630 host)
both work and `d9pixels` passes 53 of 53.
## Textures up to 2048x2048

The driver offered textures up to 1024x1024 (11 mip levels). 3DMark 2001
SE's Advanced Pixel Shader test creates a 2048x2048 DXT5 texture with 12
levels; the runtime refused it with `D3DERR_INVALIDCALL` and the benchmark
stopped before its score. The limit is now 2048 (12 levels) for Direct3D 7, 8
and 9: in the caps, for video-memory surfaces, for texture registration, and
in the miniport. Uploads were already split into rows, and the texel budget
is counted per texture (a 2048x2048 DXT5 texture with its mipmaps is about
5 MB of 64).

`d9pixels` case 55 draws a 2048x2048 texture, red on its left half and green
on its right. On XP SP3 under KVM on the second Intel UHD 630 host, case 55
passes and `d9pixels` passes 46 of 46, and 3DMark 2001 SE runs all its tests
to a final score for the first time.
## The immediate present interval

3DMark05 treats a device without `D3DPRESENT_INTERVAL_IMMEDIATE` as unable to
run it. A Direct3D 9 runtime fills `D3DCAPS9.PresentationIntervals` itself: it
always sets `D3DPRESENT_INTERVAL_ONE`, sets the immediate interval when the
driver's `Caps2` has 0x00400000 (`D3DCAPS2_PRESENT_INTERVAL_IMMEDIATE` in
`d3dhal.h`, a bit only drivers report), and the intervals two to four when it
has 0x00200000. The driver's own `PresentationIntervals` is ignored. (Read in
the runtime's debug build, where the copy of the driver's caps is built.)

No present of this driver waits for the vertical blank, so it now reports the
immediate interval. `d9pixels` case 69 checks that the device offers it.
## Texture transforms for Direct3D 8 and 9

Fixed-function draws whose stages have `D3DTSS_TEXTURETRANSFORMFLAGS` were
skipped: 3DMark 2001 SE lost 29 million triangles that way. They now go to
the record path with a stage program that applies each stage's texture
matrix per pixel. The matrix columns are in CONST[8+4k] to [11+4k],
coordinates are padded to (u, v, 1, 0) as Direct3D transforms them, and with
`D3DTTFF_PROJECTED` the result is divided by its last element. Direct3D
applies no texture matrix to pretransformed (XYZRHW) vertices; for those only
the projection divides. The driver offers `D3DPTEXTURECAPS_PROJECTED`:
without it the runtime passed such draws as COUNT3 without the flag. The
matrices are kept per stage, also in state blocks. Direct3D 8 and 9 are no
longer offered texturing from system memory, because the driver registers
only video-memory textures.

`d9pixels` case 51 moves u by a half through the matrix on untransformed
vertices; case 52 divides (2u, 2v, 0, 2) by its fourth element on
pretransformed vertices. `d7draw` prints which check skipped a vertex
(D7VERTEX) and texture registration details (D7REG).

On XP SP3 under KVM on the second Intel UHD 630 host, cases 51 and 52 pass and
`d9pixels` passes 47 of 47. In 3DMark 2001 SE the skipped vertices fell from
29 million to 160. Still open there: 97.7 million triangles skipped for
texture handles the driver never registered.

## Fixed-function bump mapping for Direct3D 8 and 9

The two-stage combiner of the Direct3D 7 draws has no bump operations and no
third stage. Direct3D 8 and 9 fixed-function draws that need either now go to
the stage program of the record path, which handles up to four stages. A
`D3DTOP_BUMPENVMAP` stage samples its bump map and moves the next stage's
lookup by du and dv through its bump matrix (00, 01, 10, 11), which takes the
place of its texture matrix in CONST[8+4n]; `BUMPENVMAPLUMINANCE` also scales
the next stage's colour by the luminance times the scale plus the offset
(CONST[9+4n]). The matrix belongs to the bump stage itself, unlike `texbem`
in pixel shaders. ValidateDevice checks Direct3D 8 and 9 stages against what
the stage program does, and the caps offer four blend stages and both bump
operations. `d9pixels` case 54 moves the next stage's lookup by a quarter of
its texture.

On XP SP3 under KVM on the second Intel UHD 630 host, case 54 passes, and
3DMark 2001 SE's Environment Bump Mapping test runs instead of reporting
"Not supported by hardware". Its Advanced Pixel Shader test still stops: it
creates a 2048x2048 DXT5 texture, and the driver offers at most 1024.

## Eight fixed-function stages for Direct3D 9 programs

3DMark05 checks its hardware before it creates a device, and one of the
checks it treats as fatal is "multitexturing with at least 8 textures":
`MaxTextureBlendStages` and `MaxSimultaneousTextures` of at least 8. The
driver offered four, the texture registers of version 1 pixel shaders.

The stage program already emits shader model 2 programs with eight samplers
and eight coordinate sets, and the Direct3D 8 draw packet carries eight
(`RB2K_D8_STAGES`); only the stage check and the caps stopped at four. Once a
runtime has announced Direct3D 9, the driver now offers eight stages and eight
textures; Direct3D 8 programs keep four. Texture and bump matrices have
constants for the first six stages (`RB_STAGE_TRANSFORMS`), so stages 6 and
7 refuse them.

`d9pixels` case 58 draws through all eight stages: stages 0 to 6 add a black
texture each and stage 7 modulates with a texture read through coordinate set
7. On XP SP3 under KVM (UHD 630 host) it passes, and `d9pixels` passes 56 of
56.

## Direct3D 8 and 9 on Windows 2000

Windows 2000 ships with DirectX 7. Microsoft's June 2010 DirectX
redistributable no longer installs there; the February 2010 one does (any
DirectX 9.0c redistributable from 2004 to February 2010 brings the same core
runtime). After `directx_feb2010_redist.exe /Q /T:<folder>` and
`DXSETUP.exe /silent` and a restart, the driver works unchanged.

On Windows 2000 under KVM on the second Intel UHD 630 host, with the driver
installed by `nt5inst`, `d9pixels` passes 44 of 44 and `d8pixels` 23 of 23.
## Vertex shaders with several streams

With vertex shaders on, Direct3D 8 and 9 programs are offered 8 vertex
streams (`MaxStreams`; fixed-function draws still read stream 0, as their
vertex formats only describe one). The display driver keeps each stream's
buffer and stride. A draw gathers, for every vertex, the data of each stream
the shader's declaration names, one after the other, into the record the
vertex program reads; `rb_vs_translate` gets the word at which each stream
starts, and a `STREAM` token moves the declaration there. A shader's program
is made again when the streams' strides, and with them those places,
change. Each stream is range-checked and read from the same first vertex.
This is one of the requirements of the Direct3D 9 driver interface (8
streams for version 2.0 vertex shaders).

`d9pixels` case 33 draws two quads whose positions come from one vertex
buffer (stream 0) and colours from another (stream 1), starting at vertex 6.
On XP SP3 under KVM on the second Intel UHD 630 host, with vertex shaders on,
case 33 passes and `d9pixels` passes 53 of 53. The translations of DirectQ's
shaders with one and two streams pass Mesa's TGSI parser.

## The Direct3D 9 driver interface

3DMark05 asks for a Direct3D 9 driver: the runtime has to see the DirectX 9
driver interface, not only Direct3D 9 capabilities reported through the
Direct3D 8 one. The display DLL offers it when the `RippleD3D9` registry value
is 1 (with `RippleVertexShaders` also 1); it is off by default while it is
being completed.

With the switch on, the driver answers the runtime's interface-version query
with the DirectX 9 version (4), and the runtime then reads the `D3DCAPS9`
answer. The runtime's debug build (`d3d9d.dll` from the June 2010 SDK) shows
what it requires before it offers a hardware device; without any one of these
it falls back silently to "no hardware acceleration":

- `D3DPMISCCAPS_FOGINFVF` set and `D3DPMISCCAPS_LINEPATTERNREP` clear;
- at least 8 vertex streams, `D3DDEVCAPS2_STREAMOFFSET`, and no
  `D3DVTXPCAPS_NO_VSDT_UBYTE4`;
- `D3DPRASTERCAPS_SCISSORTEST` and `D3DCAPS2_DYNAMICTEXTURES`;
- the shader model 2 details (`VS20Caps`, `PS20Caps`) within their ranges.

After a successful `DrawPrimitives2` the driver must leave `dwErrorOffset` at
0: a Direct3D 9 runtime reads it as the size of a reply, and anything else
sends it into a loop of assertions. The driver reports no query types, so the
runtime issues no queries.

What changes in the command stream:

- A vertex shader arrives as two objects, a declaration and a function, set
  separately. The driver pairs each declaration with each function into the
  Direct3D 8 shaders it already runs on the host, and an FVF code (a handle
  with bit 0 clear) stands for stream 0 alone.
- `SETSTREAMSOURCE2` gives each stream a byte offset; the draws add it.
- `SETRENDERTARGET2` and `SETDEPTHSTENCIL` replace `SETRENDERTARGET`
  (one render target).
- `BLT`, `SURFACEBLT` (StretchRect) and `COLORFILL` come as commands. A whole
  surface into another of its size is a host copy; any other pair of
  rectangles is a host blit (virgl's `BLIT`, through the miniport request
  `IOCTL_RB2K_DD_STRETCH`), point-sampled or linear as the runtime asks, so
  the driver reports linear filtering for StretchRect, which 3DMark05
  requires. `COLORFILL` fills whole surfaces; other fills are counted in
  `d7draw`'s `D9DDI` line.
- The scissor test is applied on the host: the miniport intersects the
  rectangle with the viewport and sets it as virgl's scissor, also for
  pretransformed vertices. Setting a render target resets the rectangle to all
  of it, as Direct3D 9 does. `d9pixels` case 60 checks it.
- Vertex shader 2.0 static flow control runs: the runtime keeps `defi` and
  `defb` in the code and passes the integer and boolean constants
  (`SETVERTEXSHADERCONSTI` and `...CONSTB`), which the Direct3D 8 interface
  does not. The driver sends `i0`..`i15` and `b0`..`b15` in `CONST[3..18]`,
  slots the vertex program layout had reserved for them (i# as floats in x,
  y, z: count, start, step; b# as 1 or 0 in w). The translator turns `rep` and
  `loop` into TGSI loops counting down in a temporary (`loop` also steps `aL`,
  which `c[aL + n]` reads), `if b#` and `else` into TGSI `IF`s, and inlines
  `call` and `callnz` (vs_2_0 has no recursion). `if_comp`, `break` and
  predicates are vs_2_x and are still refused. `d9pixels` cases 61 and 62
  check loops and branches. Pixel shader integer and boolean constants are
  counted in `d7draw`, not used (pixel shader 2.0 has no flow control).

Two surface descriptions differ from a Direct3D 8 runtime's. A system-memory
DXT texture is described by its blocks: an 8x8 DXT1 arrives as 16x2 (the
bytes of a block row by the block rows), so the driver takes its size from the
texture it is copied into. A cube map's faces each have a handle, and the
runtime copies one face by that face's handle.

`d9pixels` passes all 55 of its cases on the interface on XP SP3 (UHD 630
host), the same as with the switch off; cases 56 and 57 cover `ColorFill` and
`StretchRect`.
Case 59 stretches the back buffer into a texture of half its size with linear
filtering; with it, `d9pixels` passes 56 of 56 on the interface (it skips the
case when linear StretchRect is not offered, as with the switch off).


## User clip planes

The driver offered no user clip planes (`MaxUserClipPlanes` 0). Among the
checks 3DMark05 makes before it creates a device, a missing plane sends it to
a test of software vertex processing instead of hardware; Direct3D 7 to 9
games use the planes for reflections and portals. Six are offered now.

The planes (`SETCLIPPLANE`) are kept with the context and its state blocks,
and a draw that `D3DRS_CLIPPLANEENABLE` asks to clip carries them in its
packet. The host clips with clip distances:

- A vertex shader's planes are in clip space. Its translated program
  (`RB_VS_CLIP`) writes `dot(plane, oPos)` for each into two `CLIPDIST`
  outputs; the planes follow the shader's constants.
- Object-space draws (fixed-function vertices transformed by the pipeline)
  have their planes in world space. Each of the miniport's four object-space
  vertex shaders (Direct3D 7 draws and pixel shader draws, lit or not) has a
  clip variant that measures the world position once its world transform is
  done; the planes come before the vertices.
- A rasterizer with the planes' bits (`clip_plane_enable`) turns the
  distances on. Pretransformed vertices are not clipped, as in Direct3D.

`d9pixels` cases 64 to 68 clip a quad with one plane through each path: fixed
function, lit, with a pixel shader, lit with a pixel shader, and a vertex
shader.


## Render targets up to 2048x2048

3DMark05's first run stopped on "CreateTexture failed: Driver internal error"
when it made its shadow map, a 2048x2048 render target texture. Offscreen
surfaces, render targets and depth buffers shared the screen's limit
(`RB2K_DD_MAX_WIDTH` by `RB2K_DD_MAX_HEIGHT`, 1680x1200), so DirectDraw
refused the surface. They may now be 2048 on a side (`RB2K_DD_TARGET_SIDE`);
display modes keep their limit.

Readback staging keeps the screen's size: its attach request has to fit one
queue packet. A target larger than the screen is drawn into and sampled, but
locking it fails.

`d9pixels` case 70 clears a 2048x2048 render target texture, with a depth
buffer of its size, and draws it.


## Cube map render targets

3DMark05's run stopped next on "CreateCubeTexture failed: Driver internal
error": it draws reflections into a 512x512 render target cube map, and
render target textures were only flat images.

Each face of a render target cube map is now an image of its own, drawn into
like any target; Direct3D sets a face as the target by the face's surface
handle, which `CreateSurfaceEx` records for each face. Sampling the cube
needs a cube texture, so before a draw samples it, a miniport request
(`IOCTL_RB2K_D7_CUBE_SYNC`) copies the faces drawn since the last time into a
host cube texture, one region copy into each face's layer; it makes the cube
texture the first time. Draws and clears count per image, so an unchanged
face is not copied again.

`d9pixels` case 71 clears each face of a 64x64 render target cube map to its
own colour as a render target and samples the six directions.


## Point sizes and point sprites

The driver reports a maximum point size of 64 (`RB2K_POINT_SIZE_MAX`; it was
1). With a maximum of 1 the Direct3D runtime emulates point sprites itself,
and programs that need them choose software vertex processing: 3DMark05
created its device with `D3DCREATE_SOFTWARE_VERTEXPROCESSING`, so its vertex
shaders ran on the guest processor in the runtime.

- Direct3D 8 and 9 point draws carry `pointSize` and `pointFlags` in their
  packet: the size is `D3DRS_POINTSIZE` within `POINTSIZE_MIN`,
  `POINTSIZE_MAX` and the maximum (compared as IEEE bits, which order
  positive numbers as their values, so the display driver needs no floating
  point); `RB2K_POINT_SPRITE` when `POINTSPRITEENABLE` is set. Direct3D 7
  points stay one pixel.
- A vertex program that writes `oPts` gets a `PSIZE` output, and its point
  draws carry `RB2K_POINT_PER_VERTEX`.
- The miniport binds one point rasterizer (handle 160) for point draws, made
  again when the size or flags change. Points are drawn as quads
  (`point_quad_rasterization`, as Gallium Nine does); sprites replace the
  texture coordinate inputs: `GENERIC[2]` in the fixed-function fragment
  shaders, `GENERIC[2..9]` in the pixel shader and vertex program paths.

Not done: `POINTSCALEENABLE` (sizes by distance) and point sizes in
fixed-function vertices (`D3DFVF_PSIZE`, whose cap is not reported).
`d9pixels` case 75 draws a textured sprite and a plain point of 8 pixels,
case 76 a point sized by a vertex program.


## 32-bit index buffers

The driver reported a maximum vertex index of `0xffff`. On hardware vertex
processing the Direct3D 8 and 9 runtimes then refuse `D3DFMT_INDEX32` index
buffers, and an indexed stream draw with 4-byte indices was skipped. With
software vertex processing the runtime's own caps allow them, so a program
could take a different path depending on which it chose.

The maximum is now `0xffffff`. An indexed stream draw copies its 2- or 4-byte
indices out of the index buffer (`indexScratch`, sized in bytes) and
`prim_vertex` reads them through `indices` or `indices32`. The vertex range
they reach is checked against the vertex buffer with 64-bit arithmetic, as for
16-bit indices, so an index past the buffer skips the draw. `d9pixels`
case 77 draws a quad from vertices 65536 to 65539 of a 65540-vertex buffer.


## Release package

`win2k/package.sh` builds the package on the Linux build host:

    VERSION=0.4.1 THIRDPTY=/path/to/THIRDPTY.TXT win2k/package.sh

It makes `build/win2k-package/moose-ripple-win2k-VERSION.zip` and its
`SHA256SUMS.txt`. The driver files come from `build.py`, with the INF's
`DriverVer` set to the version and the build date, and `ripplegl.dll` is
built from the same checkout's `win9x/mesa`. `README.TXT` is
`win2k/package/README.TXT`; update its lists before a release.
`THIRDPTY.TXT`, the notices for Mesa in `ripplegl.dll`, comes from the
caller and must name the source of the build.

Release 0.4.0 reused the Windows 9x 0.2.0 build of `ripplegl.dll`, which
predates the Windows 2000/XP path in Mesa's winsys: on 2000 and XP its
OpenGL fell back to Mesa's software renderer (`softpipe`), and the install
test ran no OpenGL program. On XP under KVM on the Intel UHD 630 host, with
the same driver build, that DLL reported `renderer=softpipe` and the
current build `renderer=virgl`. The script now refuses a `ripplegl.dll`
that does not import `ExtEscape`, the call that path uses.

The install test is `win2k/tools/pkgtest.cmd`. Install the package on
fresh Windows XP and Windows 2000 guests, restart, copy the package files
and `build/win2k`'s test programs into one folder and run
`pkgtest.cmd [output]` there. It compares the installed files with the
package, runs `glpresent` and fails unless OpenGL's renderer is `virgl`,
and runs the Direct3D checks (`d9pixels` with hardware and software vertex
processing, `d9stencil`, `d8pixels`, `d9full`, `d3d7probe`, `tpixels`). It
ends with `PKGTEST END failed=N`.
