# DirectDraw on Windows 98

DirectDraw is the base Direct3D builds on (step 5 of [plan.md](plan.md)): games get their
surfaces, page flipping and the screen mode from it. A display driver offers DirectDraw a HAL
(hardware abstraction layer) in two halves: the 16-bit display driver answers DirectDraw's
escapes, and a 32-bit DLL holds the functions DirectDraw calls. Without a HAL DirectDraw uses
its own software emulation (the HEL) on the screen, which is how the driver ran until now.

## Files

| File | What |
|---|---|
| `win9x/display/ddraw16.c` | the escapes; the screen mode, modes list and video memory heap for DirectDraw; the 16-bit DestroyDriver |
| `win9x/display/rdd.h` | the block of memory both halves share, with DirectDraw's DDHALINFO and callback tables in it |
| `win9x/ddraw/rippledd.c`, `.def` | `rippledd.dll`: DriverInit, capabilities, Flip (moves the display start, QEMU's VBE Y offset), an emulated 60 Hz vertical blank, GetDriverInfo |
| `tools/win9x/ddtest.c` | a test program: what the HAL offers, a full-screen flipping chain, pixel and timing checks |

`rippledd.dll` is built with Visual C++ 6 against the Windows Me DDK's DirectDraw headers (not
in this repository); the build checks that `rdd.h`'s layout matches theirs. The display
driver's whole video memory is one tall virtual screen, so back buffers below the visible
part can be shown by moving the display start, down to line 12000, the most QEMU's VBE start
line takes; memory past that (640x480 at 8 bits, say) is a heap for surfaces never shown.

## How DirectDraw finds the HAL (seen in a Windows 98 SE guest)

`DDTRACE=1 win9x/build.sh` builds a driver that logs every escape on QEMU's debug port. When a
program starts DirectDraw, the escapes come in this order:

1. `QUERYESCSUPPORT` for `DCICOMMAND` (3075): the answer must be the HAL version, 0x100.
2. `DCICOMMAND`, sub-command 13 (`DDVERSIONINFO`), twice: the runtime's version. Not answered,
   as Windows 98's own sample driver doesn't.
3. Sub-command 11 (`DDGET32BITDRIVERNAME`): the DLL's name, its entry point (`DriverInit`) and
   a context, the 16:16 address of the shared block. DirectDraw loads the DLL and calls
   `DriverInit` with that context.
4. Sub-command 12 (`DDNEWCALLBACKFNS`): DDRAW16's `SetInfo`.
5. Sub-command 10 (`DDCREATEDRIVEROBJECT`): the driver fills in the mode and calls `SetInfo`.
6. DCI 1.0 commands (version 0x100, command 1): DirectDraw's primary surface. These belong to
   the DIB engine.

After every mode change the driver calls `SetInfo` again with "reset".

## State (2026-11-16)

DirectDraw uses the HAL. In Windows 98 SE on QEMU's virtio-vga, `ddtest` passes at 640x480
(8 and 32 bits), 800x600 (16 bits) and 1024x768 (32 bits): DirectDraw reports 4.5 MB of video
memory (what the screen leaves of the 8 MB) and page flipping, puts the back buffer in video
memory, a flip moves the display start to it and back, the screen shows the frame drawn last,
and 60 waits for the vertical blank take one second. Filling the whole back buffer and flipping
runs at 3,300 (1024x768x32) to 7,600 (640x480x32) frames a second.

One check failed once, on the first run after a restart: the screen showed white instead of
the last frame (the desktop repainting after the mode change, probably). Not seen again.

Not there yet: blits and colour fills in the driver (DirectDraw's own software does them, on
video memory).

## Direct3D 7 (2026-11-16)

`win9x/ddraw/rippled3d.c` adds a Direct3D 7 HAL to the same DLL. Direct3D hands the driver
command buffers (DrawPrimitives2): render states, texture stage states, clears and primitives
of transformed, lit vertices. The DLL plays them on OpenGL through Windows' `opengl32.dll`, so
on our OpenGL driver and the host GPU: each Direct3D context is an OpenGL context drawing into a
framebuffer object the size of the render target, and the picture is read back into the render
target's video memory at the end of a scene, or before the surface is locked or flipped.
Textures are uploaded when first used and again when they were written (through our Lock,
which DirectDraw's own blits use too, a texture blit, a changed contents stamp, or a read-back
of a render target that is also a texture); textures
in system memory without a stamp are taken again once a scene. A context keeps at most 512
OpenGL textures and deletes the one used longest ago. Fog is a texture unit of its own after
Direct3D's stages (a two-texel ramp looked up with the fog factor of each vertex), because the
host's virgl renderer can't translate OpenGL's fog varying.

Offered: the HAL device with hardware rasterization, 16- and 32-bit render targets, 16- and
32-bit z-buffers, textures in six RGB formats (32-bit with and without alpha, 565, 1555, X555 and
4444) up to 2048x2048 in video or system memory, two texture stages with the usual operations,
alpha blending and alpha test, fog (also with specular lighting), flat and Gouraud shading,
every primitive type, viewports smaller than the target (clipped with OpenGL's scissor: the HAL
reports a guard band, so Direct3D leaves that to the driver), render-target changes: each
target surface keeps its own picture on the GPU and each z-buffer its own depth (up to four of
each per context, by surface memory), so the flips of a chain, after which Direct3D names the
next back buffer as the target every frame, cost no copy; a target's memory is loaded only when
the program wrote it or the GPU has no picture of it yet.

Since 2026-11-17 also:
- **Mipmaps.** Every level of a mipmapped texture is uploaded (the levels hang off the top one
  in DirectDraw's attach list), with point or linear filtering between levels, the program's
  level-of-detail bias (`D3DTSS_MIPMAPLODBIAS`) and largest level (`D3DTSS_MAXMIPLEVEL`). A
  write to any level uploads the texture again; a texture blit copies every level the two
  textures have in common, with the rectangle scaled to each. Before, only the top level was
  used, so distant surfaces shimmered.
- **Colour keys.** With `D3DRENDERSTATE_COLORKEYENABLE` on, a texture that has a source colour
  key is uploaded as a 32-bit copy in which texels of the key's colour have alpha 0, and an
  alpha test leaves them out (unless the program tests alpha itself; then its own test does,
  as long as the texture's alpha reaches it).
- **Stencil.** A 32-bit z-buffer format with 8 stencil bits, and the stencil render states
  (every z-buffer already had a stencil part on the GPU).

3DMark 2000 now gets mipmapped textures and runs to the end. Six runs with the new driver on
2026-11-17 made 11161 to 11559 3D marks (11722 to 12189 with the old driver on 2026-11-16, a
different day on the host). The comparison reported first (old 11316 and 11183 against new
11265 and 11326) was the new driver all four times, because the test VM's startup file can't
read the CD that was meant to swap the drivers (see "Changing a driver in the test VM" below).

**Old against new on the same day** (2026-11-18, the swap checked by comparing the installed
files with the CD's before each run; the host's GPU at its default minimum clock after a host
restart and a sound card in the VM, so every number is lower than the ones above): the driver
before 2026-11-17 made 7812 and 7747 3D marks, the one from 2026-11-18 morning 5822 and 5788,
25% less. Since the Re-Volt fix (below) the HAL lets go of its context at the end of every call,
and Mesa flushed the context's commands to the host each time (synchronous, through the VxD),
many times a frame. Mesa patch 0011 lets go without a flush (release behaviour NONE, as
`GL_KHR_context_flush_control` has it): 7784, then 7478, 7461, 7413 and 7412 under `dbgrun`
(which costs a few percent). One run of six quit at the start of the benchmark without a
message and isn't explained; the five after it, under the debugger, ended normally (exit code 0).
gltest and d3dtest run at the same speed with and without the change (1840 and 574 frames/s on
that day's host).

**16-bit targets read back four times faster** (2026-11-18). With `RIPPLE_D3D_PROFILE` set, the
HAL times its read-backs: in 3DMark 2000's game scenes 7.0 ms of each 8.5 ms frame went into
reading the finished picture back from the GPU, and a 16-bit frame at 1024x768 took longer
(5.0 ms) than a 32-bit one twice its size (4.0 ms). Every target's colour buffer on the GPU was
RGBA8, so each read-back of a 565 target converted every pixel. A 565 target gets a 565 colour
buffer now: its read-back is a copy, 1.2 ms at 1024x768, and it reads destination alpha as 1,
as Direct3D does for a target without alpha. 3DMark 2000's game scenes take 4.2 ms a frame,
and its score went from 7784 to **14404 3D marks** the same day on the same host; gltest,
d3dtest (its pixel checks at 16 and 32 bits), pftest and threadtest pass.

Not yet: transform and lighting in the driver (Direct3D does it on the CPU), palettized
textures, state blocks recorded by the driver, colour-key ranges and alpha-blended colour keys.

`tools/win9x/d3dtest.c` checks it the way a game uses it: on Windows 98 SE with DirectX 7 in
Proxmox (virgl on the host's Intel UHD 630) every pixel check passes (clear, Gouraud triangle,
depth test, texture texels, blending), the screen shows the frame, and the scene runs at about
1,040 frames a second at 640x480x32 (each frame read back from the GPU and flipped).

It also checks indexed drawing and texture updates (32-bit, 565, 1555 and 4444 textures:
locked again, blitted from system memory, a texture in system memory rewritten), and since
2026-11-17 mipmaps (a 64x64 texture whose seven levels have seven colours, in video memory and
managed by DirectX: the level a quad's size picks, the bias, one level rewritten), colour keys
(32-bit and 565, key on and off) and stencil (replace, increment, equal). All pass at 640x480
in 32 and 16 bits.

Windows 98 gives a new process the id of one that has just ended; the DLL's data is shared by
all processes, so it tells processes apart by a number of its own kept in each process's
environment (the second run of a program failed before that).

### Full-screen GPU presentation

Set `RIPPLE_D3D_SCANOUT=1` before starting a full-screen Direct3D game to try the GPU display
path. The default is the existing VGA path. With a 16- or 32-bit flip chain, the HAL keeps the
finished colour buffer on the GPU. It reads a page back when DirectDraw needs its pixels, for
example when the game locks a surface to draw into it. Mesa converts the finished frame on
the GPU into a reusable display image. Windowed OpenGL still uses GDI for clipping.

On Windows 98 SE with DirectX 7 and an Intel UHD 630, the same installed driver gave the
following results with the option off and on. All tests ran at 1024×768. Each timed
`d3dtest` loop ran for five seconds.

| Test | VGA | GPU scanout |
|---|---:|---:|
| 3DMark 2000 default benchmark, 16-bit | 14,381 | 21,319 |
| `d3dtest`, 16-bit | 787.4 frames/s | 2,980.2 frames/s |
| `d3dtest`, 32-bit | 243.4 frames/s | 3,913.0 frames/s |
| Guest read-backs per 100 flips in either timed loop | 100 | 0 |

The default 3DMark score improved by 48.2% with the GPU minimum clock at 350 MHz in both
runs. The previous release scored 14,404 under the same settings.

The first GPU trial notified QEMU after every flip and rendered only 240 frames/s at 16 bits.
Updating its VNC console can require a full-frame copy into host memory. The mini-VDD now
publishes the latest image at up to about 60 Hz. The two runs rendered 14,901 and 19,565
frames and each sent 294 display updates. A timer delivered the held frame after drawing
stopped.

Run `d3dtest 1024 768 16 5 present` (or use `32`) to capture the held frame and check CPU
access. Both paths passed the front-buffer read and partial CPU-write checks. The two 32-bit
captures matched at all 786,432 pixels. At 16 bits all 786,432 pixels matched when reduced
to RGB565. The two display paths expand those values to 8-bit channels slightly differently
(maximum difference of 7 red, 3 green and 7 blue on a 0–255 scale).
[Side-by-side host captures](img/direct3d-scanout.webp)

The process-exit test (`d3dtest 1280 720 32 2 present-exit`) left a nonzero CPU page
selected, displayed a GPU frame, and terminated while it still owned that frame. The GDI
desktop and Start menu were visible afterwards. Windowed OpenGL, `rvdtest stall` and
640×480×8 DirectDraw passed after the GPU game exited. Restarting DDHELP and running
DirectDraw again also passed the page-flip and pixel checks.

After the first GPU presentation, the mini-VDD also draws the standard full-screen DOS
colour text console from its VGA character cells and font. An 80×25 prompt accepted input
in full screen and after returning to a window with Alt+Enter. The 720×400 captures
matched the VGA reference in glyph and cursor position at every pixel. Text was grey
170 on the mirror and 168 on VGA. Full-screen DOS graphics
after GPU takeover are unsupported. Use the default VGA path on a fresh boot for those
programs. The GPU presentation results above are from the Intel host. UTM and other
renderers still need testing.

#### Re-Volt and Tomb Raider on the Intel host

Both demos ran at 640×480 in 16-bit colour on the same Windows 98 SE / DirectX 7 driver.
Each run recorded 30 seconds of driver flip counters. The figures below use only complete
100-flip groups inside that interval. They measure how quickly the game submits frames.
The host console updates at up to about 60 Hz.

| Scene | Guest frames/s, VGA → GPU | GPU-to-guest reads per 100 flips, VGA → GPU | Time reading per flip, VGA → GPU |
|---|---:|---:|---:|
| Re-Volt Demo, single race | 564.6 → 1,038.4 | 100 → 0 | 1.331 ms → 0 |
| Tomb Raider: The Last Revelation, title flyby | 30.0 → 30.0 | 100 → 0 | 1.384 ms → 0 |

Re-Volt restarted the same single race in each run. Four seconds after the restart,
the accelerator was held for eight seconds, then released. GPU presentation handled all
31,000 measured flips. VGA read back all 16,800. Render throughput rose 84% in this pair
of runs. Both captures show the track, cars and dashboard at the start line. The game placed
the player fourth in one grid and third in the other, so these images are a visual comparison.
[Re-Volt starting grids](img/revolt-scanout.webp)

Tomb Raider's flyby kept its 30 frames/s pace on both paths. Five corresponding camera
positions, including the title and menu, matched at every one of the 307,200 pixels in
RGB565. Each path recorded 800 measured flips.
[Tomb Raider title flyby](img/tr4-scanout.webp)

Alt+Tab exposed an existing fullscreen restoration failure on both paths. Re-Volt reported
`DDERR_OUTOFMEMORY` when returning to the race. Tomb Raider returned to a black game
window and stopped submitting frames. Alt+F4 exited Tomb Raider with code 0 and restored
the desktop. A separate DirectDraw 4 test reproduced Re-Volt's failure on the default path
after Windows had already restored the game mode and granted exclusive access again.
[DirectDraw restoration report](https://github.com/mizaimao/moose-ripple/issues/82)

### 3DMark 2000 (2026-11-16)

3DMark 2000 1.1 runs its default benchmark to the end on Windows 98 SE in VM 230 (DirectX 7,
256 MB, virgl on the host's Intel UHD 630, guest CPU shown as a 3.6 GHz Pentium III), with the
game scenes, cockpit displays, fog and lighting as they should look: about **12000 3D marks**
at 1024x768, 16-bit colour and textures, 16-bit z, Direct3D's own transform and lighting
(11722 and 12189 with the driver as it is now, see the end of this section). The first
complete run, before the system memory heap below, made 1240. The table is from the build
before the review fixes (11816 3D marks); since then the high polygon count tests are about
20% faster.

| Test | Result (before the review fixes) | Before the heap |
|---|---|---|
| Game 1, helicopter: low, medium, high detail | 188.6, 159.9, 110.9 fps | 42.1, 18.7, 14.5 |
| Game 2, adventure: low, medium, high detail | 207.4, 174.4, 143.4 fps | 10.5, 9.0, 8.5 |
| Fill rate, single and multi-texturing | 881.0, 949.6 Mtexels/s | 858.2, 956.3 |
| High polygon count, 1, 4, 8 lights | 18305, 17674, 16924 Ktriangles/s | 18309, 17711, 17043 |
| Texture rendering speed, 8, 16, 32, 64 MB | 292.4, 273.8, 240.9, 198.4 fps | 59.8, 5.1, 2.1, 1.1 |
| Bump mapping, emboss 3, 2, 1 pass | 261.1, 281.0, 302.5 fps | 24.2, 24.2, 296.1 |
| Bump mapping, environment | not supported (not offered) | |
| CPU speed | 938 CPU 3D marks | 30 |

**More memory for surfaces.** The card's VGA memory is 8 MB (virtio-vga has no setting for
more); at 1024x768 with three buffers and a z-buffer about 3 MB were left for textures, and
DirectX 7's texture manager swapped textures through the same few surfaces many times a frame.
Now the mini-VDD gives the DirectDraw DLL one block of system memory (`RVD_HEAP`: 64 MB with
384 MB of RAM or more, 32 MB from 192 MB, else 16 MB), which the 16-bit driver offers as a second
heap for every surface that is never shown (textures, z-buffers, off-screen surfaces). Textures
and z-buffers go there first and use the card's memory only when it is full; the screen's
flipping chain stays in the card's memory. Direct3D draws on the GPU and reads back, so
nothing in that heap needs the card.

**A deadlock.** Two runs out of six stopped the whole system at 3DMark's start (all threads
waiting, CPU idle). Mesa's DLL hooked windows (SetWindowsHookEx, which needs Windows' 16-bit
lock) in DllMain for every new thread, under the loader's lock, while a thread inside a
DirectDraw call holds the 16-bit lock and may need the loader's: patch 0005 takes that out of
DllMain, and patch 0007 hooks a thread when one of its windows gets an OpenGL framebuffer
(without the hook, a destroyed window's framebuffer stayed and a new window with the same
handle was refused a pixel format). And the HAL no longer reads its process number from the
environment in DirectDraw calls (kernel32's lock): DllMain assigns it at process start.

The text on 3DMark's "Now testing" screens between tests (3DMark 99 and 2000 alike) was
missing until 2026-11-17: after a flip the HAL took Direct3D's back-buffer handle for the
surface on the screen, so the text went into one buffer and another was shown. The handle
lookup follows the flip now, and the screens show the test, settings, status and time left.

**After a review of the whole driver (2026-11-16).** Measured old and new build in turn on the
same host with nothing else running on its GPU: before the fixes 11831 and 11798 3D marks,
after them 11722 and 12189. The high polygon count tests are about 20% faster (22043, 21383,
19763 Ktriangles/s): indexed draws with more than 6144 indices were dropped before, each with a
line on the debug port. Another VM busy on the host's GPU costs a lot: one run beside a
benchmark in another VM made 8196.

### 3DMark 99 Max (2026-11-16)

Its default benchmark runs to the end in the same VM: **21969 3DMarks** and 193848 CPU 3DMarks
at 800x600, 16-bit colour, triple buffering (game 1, the race, about 235 fps).

### What the program draws itself

Games draw into the back buffer with the CPU between scenes (text with GDI, a locked
surface). The read-back at the end of the next scene copied the GPU's whole picture over that.
Now a write lock on the render target marks it, and before the next 3D draw the surface's
memory goes back into the GPU's picture (`glDrawPixels`); a clear of the whole target skips
the copy. d3dtest checks it (a square written between scenes survives the next scene).

Getting there took four fixes, each found with the driver's trace (`RIPPLE_D3D_TRACE`) or the
winsys log: Mesa's thread-data lock (an NT-only static initializer, patch 0004) crashed the
program when a thread ended; textures were re-created whenever DirectDraw kept no contents
stamp, until guest memory ran out and draws lost their index buffers; the texture manager's
thousands of short-lived surfaces each kept an OpenGL texture (the budget above); and OpenGL's
fog varying made the host's shaders fail.

## Games (2026-11-17)

Period demos on Windows 98 SE in the Proxmox VE test VM (DirectX 7, 256 MB, virgl on the host's
Intel UHD 630). Since 2026-11-18 the VM has a Sound Blaster 16 (QEMU `-audiodev none,id=snd0
-device sb16,audiodev=snd0`, sound going nowhere), which Windows 98's Add New Hardware finds
and installs from its own CD. To hear it, or to check that a program makes sound, give QEMU another
back end: `-audiodev wav,id=snd0,path=/tmp/sound.wav` records it (Windows 98 SE's welcome
screen music and its "tada" came out as 30 seconds of 44.1 kHz stereo, 2026-11-18). The demos and the CD image the test machines install
them from are kept with the build tools, not in this repository.

| Game | API | Result |
|---|---|---|
| Quake III Arena demo | OpenGL | works; timedemo about 485 frames/s at 640x480 |
| Unreal Tournament GOTY | OpenGL, Direct3D | works; 26 minutes of play without memory loss ([mesa.md](mesa.md)) |
| Half-Life: Uplink | OpenGL | works once OpenGL is chosen in its Video modes menu (it starts in software); `timerefresh` 1594 frames/s at 320x240 |
| Serious Sam: The First Encounter demo | OpenGL | works (intro, menu) |
| Expendable demo | Direct3D | works (attract mode: particles, explosions, rain) |
| Re-Volt demo | Direct3D | works since a fix the same day (below); menu and race |
| Tomb Raider: The Last Revelation demo | Direct3D | works since 2026-11-18 (title flyby, the demo level with its torch light); needs a sound card: without one it crashes in its own sound code |

On the two other test machines (2026-11-18), with the games copied over from the first VM's
disk: in UTM on an Apple-silicon Mac (x86 emulated, host renderer OpenGL ES through ANGLE)
Re-Volt and Serious Sam draw correctly, and d3dtest passes except for the mipmap level-of-detail
bias, which OpenGL ES doesn't have (ignored there; 455 frames/s). On the Windows host Hog over
ssh (KVM, host renderer llvmpipe, as ssh has no desktop session) d3dtest passes every check
(925 frames/s) and Re-Volt draws correctly.

**Re-Volt drew nothing from its race thread.** It makes its Direct3D device and sends its first
commands in one thread and draws in another. The HAL left its OpenGL context current in the
thread of the last call, and an OpenGL context is current in one thread at a time, so every
call from the drawing thread failed (`wglMakeCurrent`: ERROR_BUSY) and drew nothing; the game
also stopped reading the keyboard. Every call into the HAL now makes the context current and
lets go of it at its end (made current through Mesa itself since 2026-11-18, below). d3dtest:
1043 frames/s against 1044 before.

**Tomb Raider: The Last Revelation lost its OpenGL context to a closed handle.** It releases
its first DirectDraw object and makes another; the DirectDraw DLL closes its handle to the
VxD then, and the VxD ended every GPU context of the process at any close, also the one the
HAL's OpenGL context was using. The new device's buffers were never made (GL answered 0 x 0)
and its framebuffer stayed incomplete. The VxD counts the handles of each process now and
ends its contexts when the last one is closed. The game then got its device and crashed in
its sound code: the VM had no sound card, and the game uses its sound buffers without checking
that they were made.

**Tomb Raider: The Last Revelation then hung after a few hundred frames.** With a sound card it
drew its title screen and stopped, with the whole guest idle. `dbgrun` with `DBGRUN_DUMP` (below)
showed where: its drawing thread inside `wglMakeCurrent`, called by the HAL at the start of a
Direct3D call. Windows' `OPENGL32` checks the window's pixel format there, through GDI and
KERNEL32, which take locks of the whole system; the thread held Windows' 16-bit lock (DirectDraw
holds it during these calls), and a thread of another program waited for that lock. The HAL now
makes its context current through Mesa itself (`RippleMakeCurrent`, Mesa patch 0011): no
drawable (the HAL draws into framebuffer objects), no window, no device context, no OPENGL32.
The context is made with Mesa's `DrvCreateContext` and every OpenGL function comes from Mesa's
`DrvGetProcAddress`; OPENGL32 stays loaded only because GDI's pixel format calls look for it
when the context is made. The game then plays its title flyby and its demo level, and exits
cleanly. d3dtest: 1085 frames/s (1040 before); Re-Volt's race still draws.

## Changing a driver in the test VM

Copying a new `rippledd.dll` or `ripplevd.vxd` over the old one fails while Windows has it
loaded (DirectDraw's helper process keeps the DLL), and the test VM's `AUTOEXEC.BAT` copy from
`D:` works only for a hard disk there: DOS has no CD-ROM driver when it runs. A batch file on
the test CD copies the new files to `C:` and lists them in `C:\WINDOWS\WININIT.INI` under
`[rename]` as `c:\windows\system\rippledd.dll=c:\rbt\rippledd.new` (no space at the end of
the line); Windows replaces them at the next start. Check that the debug port shows the new
build before measuring: a comparison that skipped this (3DMark 2000 above) measured the same
driver twice.

**The output file held after a test run** (2026-11-18): the first batch of test programs after
Windows starts ends with "Sharing violation" on the output file of the first program that used
DirectDraw (d3dtest, its output redirected to a file). The holder is DirectDraw's helper,
`DDHELP.EXE`, which DirectDraw starts for the first program that uses it: that program is
DDHELP's parent, nothing else from the run is still alive, and with DDHELP ended (a scratch
tool calling TerminateProcess) Retry goes on and the batch finishes, every test passed.
DirectDraw starts DDHELP with handle inheritance off, so the file reaches it some other way,
from the DOS prompt's redirection. Later batches in the same session don't see it, as DDHELP
is running already: a test batch starts one DirectDraw program without redirection first.
In earlier runs, answering that prompt froze the guest. On 2026-11-18 Retry and Abort both
returned control. On a separate cold boot, FIND on the first d3dtest output file reproduced
the prompt. Fail returned “Access denied” and the same DOS window ran a new keyboard command.
The original freeze remains unexplained.

## Things learnt the hard way

- **The DLL must be shared.** DirectDraw calls the HAL's functions from whichever program uses
  it, and its helper process (DDHELP) is what loads the DLL. Windows 9x puts a DLL into the
  part of the address space all processes see only if every section is marked shared and the
  base is above 2 GB (`rippledd.def`, `/BASE:0xB00B0000`, as the DDK's own build does).
  Without it the function addresses are only valid in DDHELP and DirectDraw silently runs
  without the HAL ("Invalid 32-bit callback in lpDriverCallbacks").
- **DCI 1.0 commands belong to the DIB engine.** DirectDraw gets the primary surface through
  DCI (`DCICOMMAND` with version 0x100); the escape handler passes those on. "Not ours" is a
  value that can't be a DCI error code (the DCI errors are negative, -1 among them).
- **A Lock function of our own.** Without one, DirectDraw's own lock path for the back buffer
  ended the program (inside DirectDraw, with Windows' 16-bit lock held, no exception visible).
  The HAL's Lock returns the address in the mapped frame buffer.
- **WaitForVerticalBlank with BLOCKBEGIN** waits for the *next* blank to start, also when one is
  on: otherwise a loop of waits returns at once.
- **Finding out why DirectDraw ignores a HAL.** `tools/win9x/ddinfo.c` prints DirectDraw's own
  record of the driver (flags such as "no hardware", the callbacks and heaps it took). The
  DirectX 7 SDK's debug runtime (archive.org, `dx7sdk-7001`) says why, but prints with
  OutputDebugString, and Windows 9x drops those messages while DirectDraw holds the 16-bit
  lock, where the checks run. A copy of its `ddraw.dll` with the message function sending its
  text to QEMU's debug port (port 0xE9, a 25-byte patch in the test VM only) showed every
  message. `tools/win9x/dbgrun.c` (a minimal debugger that keeps OutputDebugString text and
  exceptions) and `tools/win9x/setini.c` (sets `[DirectDraw] debug=9` in WIN.INI) help there.
- **Where a frame's time goes.** `set RIPPLE_D3D_PROFILE=1` before starting a Direct3D
  program: every 100 frames the HAL writes the time per frame and the time of reading the
  picture back from the GPU to QEMU's debug port (`rd3d: profile: ...`).
- **Finding where a program hangs.** `set DBGRUN_DUMP=5` before `dbgrun E9 PROGRAM` logs every
  thread of the program every 5 seconds on QEMU's debug port, which stays readable when the
  guest is stuck: where each thread is, the addresses on its stack inside a loaded module
  (module+offset, for `objdump` on the build host), and the owner of Windows' 16-bit lock.
  Windows 9x reports a thread blocked inside a system DLL at its return address into the
  caller. The lock (KERNEL32 hands out a pointer to it by ordinal 93 only) holds its owner as
  a ring 0 thread handle, which sits 0x50 bytes after the owner's thread information block
  (each thread's line shows it). `GetThreadSelectorEntry`, which finds that block, itself
  waits for the 16-bit lock, so dbgrun asks once per thread, early.
