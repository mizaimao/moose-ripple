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

Since 2026-11-26 full-screen Direct3D games use the GPU display path by default on a host with
virgl. Set `RIPPLE_D3D_SCANOUT=0` before starting a game to use the VGA path instead (every
frame read back into the card's memory). Without virgl (Mesa's software renderer) the display
stays on VGA. Until then the GPU path was opt-in with `RIPPLE_D3D_SCANOUT=1`, which the
measurements below use. With a 16- or 32-bit flip chain, the HAL keeps the
finished colour buffer on the GPU. It reads a page back when DirectDraw needs its pixels, for
example when the game locks a surface to draw into it. Mesa converts the finished frame on
the GPU into a reusable display image. Full-screen OpenGL windows use the same path since
2026-11-26 ([mesa.md](mesa.md#full-screen-opengl-from-the-gpu-2026-11-26)), and OpenGL in a
smaller window is copied on the host into the desktop image
([mesa.md](mesa.md#opengl-in-a-window-from-the-gpu-2026-11-26)).

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
170 on the mirror and 168 on VGA.

Standard BIOS mode 13h (320×200, 256 colours) also works after GPU presentation.
On the Intel host, `DOS13.COM /K` displayed a frame, changed four palette entries
without touching video memory, then moved a white block without changing the palette.
All three 640×400 host captures matched fresh VGA at every pixel. At the native
resolution, the palette change affected 53,572 pixels and the block move affected
640. DOS read its palette entry back correctly and returned to the text prompt.
[Fresh VGA and GPU captures](img/dos-mode13.webp)

DOS writes its 8-bit indices in guest video memory. With virgl, the mini-VDD keeps
a 320×200 index texture and a 256-entry colour table on the host. A GPU shader
looks up each colour and doubles each pixel into a 640×400 display image. The first
upload sends 64,000 index bytes and a 1,024-byte table. Later samples send changed
indices or table entries. Holding a frame requires no upload or draw.

| Change in the DOS probe | Indexed GPU uploads on Intel | 2D fallback uploads |
|---|---:|---:|
| Change four palette entries, keep the image indices | 16 bytes | 1,024,000 bytes |
| Move a 20×16 white block | 6,264 bytes | 100,224 bytes |

The byte counts cover image and palette data sent by the guest. On the block move,
the old and new positions fit inside a 108×58 rectangle of 8-bit indices. The
fallback updates the corresponding 216×116 rectangle of 32-bit pixels. It also
handles hosts that cannot set up the indexed renderer. Both paths reproduced the
three test frames exactly. The five-second 1024×768×32 Direct3D scene reached
3,940.6 guest flips/s before the DOS change and 3,939.0 with the initial 50 ms
indexed driver. Every measured group of 100 flips in that run recorded zero GPU readbacks.
After DOS returned to Windows, DirectDraw surface restoration, ordinary DirectDraw
and Direct3D passed with GPU presentation both off and on.

To repeat the check, build the probe with `sh tools/win9x/build-tools.sh dos13`,
run a GPU-presented Direct3D scene, and start the probe from a full-screen DOS prompt.
Press Space to advance each frame. To test a palette write across Alt+Enter, run
`DOS13.COM /S`. Switch to a window at the first frame and press Space there. Switch
back to full screen and press Space to finish the colour. Repeat with `/S0` and
`/S2` to switch before red or after green. Windows can reset the hardware write
index during this transition. The driver restores the interrupted position before
the program writes its next component. For planar DOS graphics, custom strides or
display starts, use the default VGA path on a fresh boot.

On Apple silicon with UTM 5.0.8, the 50 ms and 14 ms drivers both passed
`DOS13.COM /K` and all three split palette writes after GPU presentation.
In each run the guest verified the unchanged indices after the palette update,
the block move and palette entry 3 (63, 32, 0). It returned to 80×25 text and
then to Windows.

A diagnostic driver read back the 640×400 GPU image selected for display after
the scanout and flush commands succeeded. All 17 readbacks across the four runs
matched the expected RGB values at every one of the 256,000 pixels, including
frames during full-screen reentry. The RGB hashes for the initial frame, the
palette change and the block move were `c3feaf35`, `51d0e925` and `f541a925`.
The palette change uploaded 16 palette bytes and the block move uploaded 6,264
index bytes. The UTM viewer itself was outside this pixel comparison.

#### Scrolling in Tyrian 2000 on the Intel host

Tyrian 2000's first recorded demo runs in standard chained mode 13h. With the
50 ms refresh delay, a projectile could occupy the same spot in three consecutive
host captures as the game continued to scroll. The driver now requests its next
refresh after 14 ms while the current DOS session has a valid indexed image
selected for display. The index texture and palette stay on the GPU.

| Display path | Image changes/s, two 8 s samples | Median gap | 95th-percentile gap |
|---|---:|---:|---:|
| Native VGA | 33.34 / 33.33 | 30 ms | 30 ms |
| Indexed GPU, 50 ms delay | 13.98 / 13.85 | 60 ms | 90 ms |
| Indexed GPU, 14 ms delay | 33.33 / 33.32 | 30 ms | 30 ms |

The guest ran `FILE0001 NOSOUND NOJOY` and entered its first attract demo
without input. Each run sampled the same 192×144 region of the 640×400 host
display. A local VNC client requested 120 samples/s and received about 33.3/s.
Every reply during gameplay contained a changed image on native VGA and with
the 14 ms delay. This measures the frames delivered to that client. To align the
three sequences below, the first crop in each row was matched byte for byte
before compression. Each subsequent column is the next reply, about 30 ms later.
[Six consecutive captures from each display path](img/tyrian-pacing.webp)

The five-second 1024×768×32 Direct3D takeover scene passed on the same two
driver builds at 3,921.6 and 3,942.0 guest flips/s, respectively. Every measured
group of 100 flips on each build recorded zero GPU readbacks. After Tyrian
exited, the 14 ms build matched native VGA at every pixel in the three DOS
probe frames, including the Alt+Enter switch during a palette write. The
DirectDraw restoration, DirectDraw and Direct3D tests then passed with GPU
presentation both off and on.

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

Alt+Tab exposed a fullscreen restoration failure in the driver from #80 on both paths. Re-Volt reported
`DDERR_OUTOFMEMORY` when returning to the race. Tomb Raider returned to a black game
window and stopped submitting frames. Alt+F4 exited Tomb Raider with code 0 and restored
the desktop. A separate DirectDraw 4 test reproduced Re-Volt's failure on the default path
after Windows had already restored the game mode and granted exclusive access again.
[DirectDraw restoration report](https://github.com/mizaimao/moose-ripple/issues/82)

#### Restoring a game after Alt+Tab

DirectDraw 7 calls the HAL's `CreateSurface` entry when restoring a lost flip chain on a
driver that offers `CreateSurfaceEx`. Ripple previously supplied only `CanCreateSurface`
for ordinary surface creation. A 32-bit `CreateSurface` entry now returns
`DDHAL_DRIVER_NOTHANDLED` so DirectDraw can use its existing allocator for the primary
surface and its attached back buffer.

Run `ddtest restore` from a desktop mode other than 640×480×16. It enters that game mode,
flips once, lets a second process take focus, and checks that both surfaces become lost.
After regaining exclusive access, it restores the primary and flips again. The same test
returned `0x8007000e` on the previous driver. With the fix, both restore and the next flip
returned `DD_OK`. The ordinary `ddtest` and `d3dtest` checks also passed with GPU scanout
off and on.

Tomb Raider resumed its flyby at 30 frames/s with either setting. Re-Volt resumed its
race, accepted the restart and accelerator controls, and returned to the 1280×720 desktop
on exit. After the focus switch, a 30-second race sample gave 428.1 frames/s on a fresh
VGA boot and 737.7 with GPU scanout. All 22,000 measured GPU flips avoided a read-back.
The default 3DMark 2000 benchmark scored 21,442 with the fix and GPU scanout enabled,
compared with 21,319 and 21,362 in the earlier GPU runs.
[Both games after returning from the desktop](img/alt-tab-restore.webp)

#### VGA flips after GPU presentation

After the first GPU presentation in a Windows session, the host display stays on virtio.
A game using VGA can still change the display-start register, but the VxD also has to
show that page on the host. Previously it scanned the page on every flip and uploaded
every changed frame. Re-Volt reached only 69.7 frames/s in that case.

The HAL now identifies an ordinary DirectDraw flip separately from a request to return
the screen to GDI. Every flip selects its VGA page immediately. The VxD samples the
latest page at most once per 17 ms during a run of flips from the same process.
Its timer continues sampling when the game stops flipping, so a direct write to the
visible page can still reach the host. A new owner and a return to GDI take effect
immediately. Direct GPU presentation continues to keep eligible frames on the host.

On the Intel UHD 630, each run first used GPU presentation at 640×480, then turned it
off and ran the same five-second `d3dtest` scene at two resolutions. The rates measure
completed guest flips. The paced mirror sampled the latest VGA page and uploaded it
when the pixels changed.

| 16-bit mode | Synchronous mirror, guest flips/s | Paced mirror, guest flips/s | Paced stream flips | Mirror samples | Host uploads |
|---|---:|---:|---:|---:|---:|
| 640×480 | 80.2 | 497.6 | 2,489 | 297 | 296 |
| 1024×768 | 58.8 | 102.6 | 514 | 293 | 287 |

Each stream count includes one setup flip before the timed loop. At 1024×768, the
held reference frame matched the synchronous mirror at all 786,432 pixels, both in
the host's captured RGB values and in the guest's RGB565 format.

To exercise the last-frame case, run a GPU-presented Direct3D scene, then run
`ddtest mirror` with GPU scanout off in the same boot. The test issues three quick
flips between a red and a green page, then holds for seven seconds. On both drivers
all 307,200 captured pixels were green. The test then wrote directly to the front
buffer and held again without a flip. On both drivers precisely the upper-left
320×240 pixels turned blue, and the other 230,400 stayed green.
[Captured VGA pages after the final flip and front-buffer write](img/mixed-vga-mirror.webp)

With GPU presentation off after takeover, Re-Volt survived a switch to the desktop and
back on both drivers. In the same restarted, 30-second race its throughput rose from
73.0 to 139.6 guest flips/s. Every measured flip still read the application image back
to VGA. The improvement came from sampling fewer VGA pages for the host. In the earlier
fresh-boot VGA run the game reached 428.1 flips/s. Direct GPU presentation reached
737.7 flips/s after the focus switch and avoided the per-frame read-back.

The game exited to the 1280×720 desktop. The 80×25 DOS prompt displayed text typed in
full screen and accepted a new command after Alt+Enter returned it to a window.
On both fresh VGA and the mirror, the window omitted the output typed in full screen.
DirectDraw surface restoration, ordinary DirectDraw and the Direct3D rendering tests
all passed with GPU scanout both off and on. The final ordinary driver scored 21,705
in the default 3DMark 2000 benchmark with GPU presentation, compared with 21,442
before this change on the same Intel rig. At 1024×768×32 the GPU-presented scene
reached 3,942.6 guest frames/s. Its held image and the VGA mirror's held image each
matched the earlier VGA reference at all 786,432 RGB pixels.
Windowed OpenGL, recovery from a timed-out GPU request and 8-bit DirectDraw also
passed after the 32-bit scene.

A separate timing build measured the remaining cost of mirroring a 1024×768×16 page.
Over 5,057 ms it spent 438 ms converting pixels, 3,019 ms in the transfer call
including its fence wait, and 916 ms updating the display. The 288 uploads averaged
10.48 ms in the transfer call. At 640×480 that call averaged 9.88 ms. The longest
1024×768 refresh was 16 ms at the guest clock's observed 1 ms resolution.
[Follow-up on allowing flips during a fenced upload](https://github.com/mizaimao/moose-ripple/issues/85)

#### Letting a VGA game render during an upload

When the CPU mirror is already on screen, the mini-VDD can now submit one fenced
upload and return to the game. DirectDraw selects its next VGA page while the host
reads the previous sample. The mini-VDD holds that sample's backing until it receives
the matching completion. It then flushes the image if the same process and mode
still own the display. A timer handles the last upload when the flips stop.

The first frame from a new owner still goes to the display synchronously. A mode
change can allocate another image if one is still visible and the host still owns
the backing for a different, earlier upload. This lets Windows construct the new
image before switching to it and release the old backing when it is safe.

On Windows 98 SE with an Intel UHD 630 at its 350 MHz minimum clock, each pass ran
a GPU-presented scene, then two five-second VGA scenes. The older and newer drivers
used the same HAL and Mesa DLLs. Rates are completed guest flips per second.

| VGA mode after GPU takeover | Waited upload, two runs | One upload in flight, two runs |
|---|---:|---:|
| 640×480×16 | 482.0, 504.8 | 1,423.2, 1,450.8 |
| 1024×768×16 | 89.4, 97.8 | 534.0, 538.4 |

Both 1024×768 candidate streams submitted 293 asynchronous uploads. They flushed
292 and 293 of them respectively before ownership changed. A guest flip still
reads the application image back to VGA in this mode.

In the same Re-Volt race, the guest went from **124.4 to 383.2 flips/s**. Both
drivers read back once per flip. The average time spent in read-back fell from
6.93 to 1.98 ms per flip. The guest counters cover 3,600 and 11,400 flips
respectively, with the viewer recorder idle.

Two separately restarted drives measured what a VNC viewer actually received.
In an eight-second capture of the same moving 288×224 area, the older driver
delivered 258 changed frames (32.1/s) and the newer one delivered 260 (32.5/s).
The 95th-percentile gap between changed frames was 36.8 and 36.9 ms respectively.

The held 1024×768×16 scene matched in all 786,432 captured pixels. After the last
flip in `ddtest mirror`, the host showed a solid green page, then showed the blue
320×240 front-buffer write without another flip. All other pixels stayed green.
[Paired 1024×768 host captures](img/vga-upload-comparison.webp)

The `rvdtest async` regression holds a real upload in the driver, switches from
1024×768 to 640×480, then expires a second held upload and switches back. The first
build failed the return to 1024×768. The revised driver completed the return, ran
independent 3D commands during the timeout, received a fresh upload's matching reply
and restored the original desktop. The test forces the deadline in software.

At 1024×768×32 the direct GPU scene reached 3,957.2 guest frames/s with zero
timed-loop read-backs. The VGA fallback reached 174.2 frames/s with one read-back
per flip and displayed all 292 asynchronous uploads. Both held images matched the
earlier VGA reference in all 786,432 RGB pixels.

Re-Volt resumed its race after switching to the desktop and back. The full-screen
DOS prompt echoed typed input and accepted a new command on return to a window.
DirectDraw surface restoration, ordinary DirectDraw and Direct3D passed with GPU
presentation both off and on. Windowed OpenGL, 8-bit DirectDraw and 3D request
timeout recovery also passed.

The default 3DMark 2000 benchmark with GPU presentation scored 20,843 immediately
after the fault and mode-switch tests. A fresh-boot comparison scored 21,122 on
the previous driver and 21,437 on the new driver. Both sent about 81.1% of timed
flips directly to the GPU, recorded 233 read-backs in complete timing blocks and
sent zero CPU mirror uploads. This benchmark exercised direct GPU presentation.

#### Apple silicon UTM (2026-11-19)

Windows 98 SE ran the same four driver binaries as the Intel test in a disposable
UTM 5.0.8 guest with `virtio-vga-gl`. The guest compared each installed binary
with the test disc after restarting. The default VGA path, opt-in GPU presentation
and VGA fallback after GPU takeover ran on the same installed build.

After a fresh boot with the Mac running only the Windows 98 VM, six five-second
1024×768 scenes produced the following guest render rates. In both depths the
direct path presented all 100 flips in each complete timing block on the GPU
with zero read-backs. The two VGA paths performed one read-back per timed flip.

| Scene depth | Fresh VGA (flips/s) | Direct GPU (flips/s) | VGA after GPU takeover (flips/s) |
|---|---:|---:|---:|
| 16-bit | 180.8 | 1,983.0 | 103.8 |
| 32-bit | 216.8 | 1,932.0 | 100.0 |

In Re-Volt, the older installed driver reported `DDERR_OUTOFMEMORY` when restoring
the primary display after Alt+Tab. With the new driver the game returned to
640×480×16 and resumed rendering when its window regained focus. On the GPU run
a second Alt+Tab left the desktop active. The foreground helper returned focus to
Re-Volt, and the driver resumed GPU presentation. After returning to the game on
the VGA fallback, both guest VGA pages contained the live race and advancing HUD
clocks. On separate restarted drives the guest completed **65.8 flips/s** with GPU
presentation (1,800 flips, zero read-backs), and **41.5 flips/s** with the VGA
fallback (800 flips, 800 read-backs). These rates use complete 100-flip blocks.
[Re-Volt before and after the display restore fix](img/utm-revolt-restore.webp)

The DirectDraw test held a green front buffer, wrote a blue top-left quarter and
read the new pixels without a further flip. All 307,200 pixels in the sampled
guest VGA page were green before the write. Afterwards the top-left 76,800 were
blue and the remaining 230,400 stayed green. The asynchronous upload test recovered
from a forced deadline, received the next upload's reply, and restored the desktop.
The DOS text buffer contained the command typed in full screen and its echoed output.
After returning to windowed DOS, another typed command wrote its marker to serial.
In both color depths the two mipmap LOD-bias checks failed in all three display
paths. The guest's other pixel checks passed.

QEMU's virtual framebuffer capture showed the VGA reference before takeover.
After takeover it returned solid black for the GPU scene, the VGA fallback, the
desktop and DOS, even while the guest continued rendering. The eight-second Re-Volt
captures in both modes also contained only black frames. Those captures cannot
measure the rate of distinct frames in UTM's viewer. A separate eight-second
GPU race with QEMU's SPICE update trace enabled produced no new VM log records.
Full-screen GPU presentation remains opt-in pending a check of the UTM window.
[QEMU captures before and after GPU takeover](img/utm-qmp-scanout.webp)

#### Frames reaching the UTM window (2026-11-26)

UTM's own log (`Data/debug.log` in the VM's bundle, with debug logging on) has one
`gl draw` line for every picture QEMU hands its window, with a millisecond time stamp.
Counting them over the timed part of a test gives the rate of pictures the viewer received,
apart from the guest's own flip rate. Same UTM 5.0.8 guest and boot, the driver with GPU
presentation as the default; the VGA runs set `RIPPLE_D3D_SCANOUT=0`, and came first, before
anything had used the GPU path:

| Test | Path | Guest flips/s | Read-backs per flip | Pictures to the window/s | Gap median / 95th percentile |
|---|---|---:|---:|---:|---:|
| `d3dtest` 1024×768×16, 5 s | VGA | 190.0 | 1 | 30.6 | 33 / 34 ms |
| | GPU (default) | 1,993.0 | 0 | 58.8 | 17 / 18 ms |
| Re-Volt race, 8 s of throttle | VGA | about 33 | 1 | 28.3 | 33 / 42 ms |
| | GPU (default) | about 50 | 0 | 47.4 | 20 / 30 ms |

On the VGA path QEMU samples the card's memory on its own display timer, about every 30 ms.
On the GPU path the mini-VDD sends each new frame, at most one every 16.7 ms, so in Re-Volt
nearly every frame the game made reached the window. During the 10-second hold after the
scene, with an unchanged picture, no picture was sent. A live look at the UTM window on
2026-11-19 had found the same coloured test pattern on both paths, the desktop between
tests, and the front-buffer update without a flip.

After the first GPU frame of a session the desktop stays on the mini-VDD's copy of the card's
memory (see below). That copy has a cost when nothing moves: on an idle desktop QEMU used 10%
of a host core before the GPU scene and 31% after it, the mini-VDD comparing the whole
1280×720 screen every 32 ms. The cheaper copy described under "The desktop after GPU
presentation" brought that to 20%.

#### The desktop after GPU presentation

QEMU can't go back to showing the VGA memory once a virtio image was on screen, so after the
first GPU frame the mini-VDD shows the Windows desktop itself: every 32 ms it compares the
card's memory with its own 32-bit copy, and sends what changed to the host
(`desktop_refresh` in `win9x/minivdd/vgpu.c`). Until 2026-11-26 it compared every pixel each
time, uploaded the whole screen when anything had changed, and counted the next 32 ms from the
end of that work. In UTM a window being moved reached the viewer 17.8 times a second (median
gap 50 ms): not Windows 98's slow timer (the guest counted 30,017 ms over 30.06 real seconds
during the test), but 32 ms plus the comparison and the full-screen upload.

Now the comparison has a loop for each colour depth, an unchanged desktop is compared one
row in four (a different set each time, so every row is looked at within four samples, and a
difference anywhere makes it a full comparison), only the changed rows go to the host, and
the next sample is due 32 ms after the start of this one. Changes shorter than four rows can
show up to about 100 ms late; the pointer, a text caret and a line of text are taller.

Same UTM guest and procedure before and after (1280×720×32 desktop, a GPU-presented
`d3dtest` scene first, then `w98move`: a 320×240 window moved back and forth 40 pixels as fast
as it goes for 30 seconds):

| | Before | After |
|---|---:|---:|
| Pictures to the UTM window while the window moves, per second | 17.8 | 26.6 |
| Gap between them, median / 95th percentile | 50 / 94 ms | 35 / 69 ms |
| Window moves the program made in 30 s | 51,285 | 60,571 |
| QEMU's host CPU with the desktop idle (10% before any GPU frame) | 31% | 20% |

The picture, on the Intel host (VM 230, the same driver files, captures from QEMU's VNC server
because its screendump has no picture once a virtio image is shown): the 1280×720×32 desktop
shown through the copy after a GPU-presented program matched the desktop captured before any
GPU frame at all 921,600 pixels (the tray clock left out), and again after `w98move` had moved
its window 635,018 times and closed it. The default Direct3D scene took the GPU path with no
variable set and passed every pixel check, and its held frame looked as before.
[Desktop before and after the GPU frame, and the scene](img/desktop-copy-rig.webp)

### Flip chains larger than the card's memory (2026-11-26)

The card's VGA memory is 8 MB, and through 0.6.0 a flip chain had to fit in it: the screen's
page and every back buffer. In 32-bit colour that ends at 1152x864 with one back buffer
(1280x1024 takes 5 MB a page) and leaves no room for a second back buffer at 1024x768. Past it
DirectDraw quietly made the chain in its own system memory and emulated the flips, and Direct3D
refused to draw into such a chain: CreateDevice failed with `D3DERR_SURFACENOTINVIDMEM`
(887602eb). No Direct3D program started at 1280x960, 1280x1024 or 1600x1200 in 32 bits.

The system memory heap (see "More memory for surfaces" below) now takes a chain's back buffers
too, in DirectDraw's second pass over the heaps, after the card's memory has refused them
(`ddsCapsAlt` in `ddraw16.c`). A chain that fits the card stays there, as before. The card can't
show a page that lies in that heap, so Flip shows it another way:

- from the GPU, when Direct3D drew it. Full-screen GPU presentation (above) goes by surface,
  wherever its memory lies, and this is the usual case;
- otherwise through the mini-VDD: `RVD_DESKTOP` with `RVD_DESKTOP_HEAP` names a page counted
  from the heap's start. The mini-VDD mirrors it into its display image, the same way it shows
  the desktop after a GPU frame, follows later writes to it (a Lock of the shown page), and
  takes the display from VGA for it if it had not yet.

The DLL asks the mini-VDD which request flags it knows and only then lets the 16-bit driver
open the heap to back buffers, so an older mini-VDD keeps the old behaviour.

Checked on the Intel host (VM 230, Windows 98 SE, DirectX 9.0c, 256 MB, untimed) at
1280x1024 in 32 bits, 0.6.0 against this change:

| | 0.6.0 | Now |
|---|---|---|
| `ddtest 1280 1024 32`: the back buffer | system memory, flips emulated (925 a second) | video memory, the HAL's flips (1066 a second) |
| `d3dtest 1280 1024 32` | CreateDevice fails (887602eb) | every pixel check passes, 2570 frames a second, shown from the GPU |
| `ddtest mirror 1280 1024 32` (pages shown by the mini-VDD) | | the flipped page, then a write to the shown page, are on the screen (VNC captures) |
| `d3dtest 1280 1024 32` with `RIPPLE_D3D_SCANOUT=0` | CreateDevice fails | passes; each frame is read back into the heap page and shown by the mini-VDD (VNC capture) |
| 3DMark 2001 SE, Car Chase | does not start | runs with the right picture (VNC captures) |

At 1600x1200 in 32 bits (7.7 MB a page) `ddtest` and `d3dtest` pass the same way: 646 flips a
second through the mini-VDD, 1924 frames a second from the GPU. At 1024x768 in 32 bits, where a
chain with one back buffer fits the card, nothing changed: the display start moves between
lines 0 and 768 as before, `ddtest` and `d3dtest` pass, `d8pixels` 23 of 23, `d9pixels` 48 of 49
(the known vertex declaration).

**Two back buffers.** 3DMark 2001 SE's "Triple" frame buffer at 1024x768 in 32 bits needs 9 MB:
the second back buffer now comes from the heap, and the benchmark runs with the right picture
(loading screens and Car Chase, VNC captures). It first ran at half speed. With three pages
the runtime makes the page that was neither front nor back at the last flip the render target
every frame, the HAL took that for a target outside the chain and read every page back:
100 read-backs per 100 flips, 3.7 of a frame's 6.5 ms (`RIPPLE_D3D_PROFILE=1`, Car Chase low
detail, another guest busy on the host). A page of the screen's chain no longer ends the chain:
0 read-backs per 100 flips and about 3.5 ms a frame on the same scene. The same code
serves 16-bit chains with two back buffers, which always fitted the card.

Limits: the heap is 32 MB on a guest with 192 to 383 MB of memory and 16 MB below that, and
textures and z-buffers share it. At 1600x1200 in 32 bits a back buffer and a z-buffer take
15 MB of it. A frame shown by the mini-VDD is copied once more on its way (5 MB at
1280x1024), which only matters to programs that draw with DirectDraw alone at such sizes.

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
flipping chain stays in the card's memory, unless it is too large for it (see "Flip chains
larger than the card's memory" above). Direct3D draws on the GPU and reads back, so
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

## Direct3D 8 through its own interface (stage 1, 2026-11-26; on by default since 0.6.0)

The Direct3D 8 runtime drives a driver that only has the Direct3D 7 interface by translating
(it runs the vertex processing and sends Direct3D 7 commands), which is how Direct3D 8 programs
have run on this driver so far. Shaders need the Direct3D 8 interface itself: the same
GetDriverInfo2 requests and DrawPrimitives2 commands as on Windows 2000 (`win2k/d3d8ddi.h` is
shared). Stage 1 answers GetDriverInfo2 (D3DCAPS8 without shaders or hardware vertex
processing, the format list, the runtime's version) and draws from streams: SETVERTEXSHADER
with a vertex format, SETSTREAMSOURCE and its user-memory form, SETINDICES, DRAWPRIMITIVE,
DRAWPRIMITIVE2, DRAWINDEXEDPRIMITIVE and its second form, CLIPPEDTRIANGLEFAN and BUFFERBLT;
shader and patch commands are stepped over. At first it was offered only when the registry
value `HKLM\Software\moose-ripple`, `D3D8` was `"1"` (a `.reg` file on the test CD set it).

**On by default since 0.6.0** (2026-11-26): without a `D3D8` value the driver behaves as with
`"2"`. The values under that key are read when DirectDraw loads the HAL, so a change needs a
restart:

| Value | Meaning |
|---|---|
| `D3D8` = `"0"` | no Direct3D 8 interface: the runtime drives the HAL through Direct3D 7 and processes vertices itself (the behaviour before 0.6.0) |
| `D3D8` = `"1"` | the Direct3D 8 interface; the runtime processes vertices on the CPU |
| `D3D8` = `"2"` or no value | and vertex processing on the GPU (fixed function, vertex shaders), with vertex and index buffers in the driver's memory |
| `D3D8Buffers` = `"0"` | with `"2"`: buffers stay in the runtime's system memory (the path before 2026-11-26 13:00) |

Learnt on Windows 98 SE with DirectX 8.1 (UTM):

- **The runtime asks once per boot.** The first program that uses Direct3D 8 decides, for every
  program until the next restart, whether the driver has the Direct3D 8 interface; that is why
  the switch is in the registry and not in a program's environment.
- **Its surface records for buffers live only during CreateSurfaceEx.** The runtime's
  `DDRAWI_DDRAWSURFACE_LCL` for a vertex or index buffer is freed when the call returns and its
  memory is soon reused: the HAL read a "global" pointer of `00810800` from it at the first draw,
  faulted while DirectDraw held Windows' 16-bit lock, and Windows stopped answering (no crash
  dialog can show then). The HAL now copies each buffer's address and size into its own table
  when the runtime names it (`buffer_set`), and never looks at that record again.
- **A draw can start at any byte.** The runtime packs vertices of different formats into one
  buffer and gives DRAWPRIMITIVE2 a byte offset that need not be a whole vertex.
- **Texture records are just as short-lived.** For textures named in a session with a
  Direct3D 8 context, the HAL keeps its own copy of each level's records (`shadow_make`: the
  LCL, its MORE, the GBL with the GBL_MORE pointer just before it, and attach lists linking the
  levels) and the handle table points at the copy; the memory stays the runtime's. Writes
  still reach the texture by its memory address (Lock and texture blits mark it). Cube maps
  aren't offered yet and get no copy.

`d8pixels sw` (software vertex processing), Windows 98 SE with DirectX 8.1 in UTM: all 8 cases
pass through the new interface (clear, user memory, vertex buffer, strip, indexed, base vertex,
managed texture, untransformed vertices), in the first program of the boot and in a second one.
Through the Direct3D 7 translation (switch off) all 8 pass too, as before. `d3dtest`
(Direct3D 7) in the same boot with the switch on fails the same two checks as with the released
driver in UTM (mipmap level with LOD bias +1: level 3's colour where level 4's is expected), so
that one is the UTM host's and not this work's.

Seen twice and not explained, with the switch off (the released driver too): in two boots, a
second run of `d8pixels` loaded no driver code, made no OpenGL context and still passed its
pixel checks, so something else drew it. In a later boot it did not happen: three runs of a
small frame-rate test (`d8fps`, 169 to 185 frames a second in UTM) and then two runs of
`d8pixels` each reached the HAL. Not understood yet; watch for it.

### Pixel shaders 1.1 to 1.3 (stage 2, 2026-11-26)

With the Direct3D 8 interface on, the device reports pixel shader version 1.3. The HAL does not
write its own translator: the Windows 2000 driver's (`win2k/d3d8_ps.c`, built unchanged with
Visual C++ 6) turns a shader into Gallium TGSI text, and `win9x/ddraw/tgsi_arb.c` turns that
text, line by line, into an OpenGL ARB fragment program, which Mesa compiles for the host.
TGSI names become ARB ones (TEMP[n] is Tn, CONST[n] program.env[n], the translator's
immediates program.local, SAMP[n] texture unit n, the colours and coordinate sets
fragment.color and fragment.texcoord); what ARB lacks is spelled out (`|x|` through ABS, SSG
through two SLT, KILL_IF is KIL, scalar instructions take one component). Shaders are compiled
when the runtime creates them; constants c0..c7 go to program.env before a draw; with a shader
set, each stage's texture is bound on its unit and the stages' operations don't apply. The HAL
keeps eight texture stages: Direct3D 8 is offered four (the runtime sends stage states only
for the blend stages a device reports; Direct3D 9 gets eight, below), Direct3D 7 still two,
and fixed-function fog moved to the fifth OpenGL texture unit.

Fog after the shader: the HAL already gives each vertex Direct3D's fog factor f (from the
specular alpha for vertex fog, from the depth for table fog) as the coordinate 0.25 + 0.5 f of
the unit that does fixed-function fog. A shader gets a second program when a draw with fog
first needs it: the translator's linear fog from a vertex depth, reading that coordinate, with
start 0.75 and end 0.25, which gives back f.

`d8pixels sw`, Windows 98 SE with DirectX 8.1 in UTM: 23 of 23 pass (the 8 fixed-function
cases and 15 pixel shader cases: mov, texture, constants, modifiers, co-issue, cnd, cmp, dp3,
texcoord, texkill, two textures, fog from the specular alpha, table fog, lit, lit with fog).
A shader adding four textures (t0..t3, `d8tex4`) reads back the expected sum. `d3dtest`
(Direct3D 7) in the same boot fails only the two mipmap LOD-bias checks it fails in UTM with
the released driver. Not yet: the bump and matrix texture instructions of version 1 beyond
what d8pixels covers, vertex shaders.

### Direct3D 9 and pixel shader 2.0 (2026-11-26)

The Direct3D 9 runtime drives this HAL through the same Direct3D 8 interface, as on Windows
2000: it asks for the Direct3D 9 capabilities, gets no answer, and uses the Direct3D 8 ones. It
announces itself twice, as version 0x902 and then, before asking for those capabilities, as
0x802, so the HAL keeps the highest version it has been told. For a runtime of version 9 the
capabilities say pixel shader 2.0 with eight samplers and eight blend stages (the runtime sends
stage states only for those), and constants c0..c31 go to program.env. Shader model 2 reads up
to eight texture coordinate sets, which would collide with the fixed-function fog unit, so under
a shader the fog values go in as OpenGL's fog coordinate (`fragment.fogcoord`) instead.
Fixed-function stages stay at four for Direct3D 8 and 9 (before the fog unit) and two for
Direct3D 7.

DirectX 9.0c from December 2006 is the last that installs on Windows 98 (download store:
`runtimes/dx9c-dec2006`; extract with `/Q /T:dir /C`, then `DXSETUP.EXE /silent`, then
restart). `d9pixels sw` on Windows 98 SE in UTM: 40 of 40 pass, the 24 fixed-function, version 1
and render-to-texture cases and all 10 shader model 2 cases (texture, arithmetic, texkill, pow,
rcp, rsq, nrm, dp2add, eight coordinate sets, blend). Untimed.

DXT textures (with the Direct3D 8 interface on): DirectDraw is given the FourCC codes DXT1 to DXT5
(the 16-bit driver points HALINFO at them, the DLL says how many), CreateSurface asks DirectDraw
for a block of the bytes a DXT surface's 4x4 blocks take (`DDHAL_PLEASEALLOC_BLOCKSIZE`; there
is no pitch to size it by), Lock offsets count in blocks, texture blits copy rows of blocks, and
the blocks go to the host as they are (`glCompressedTexImage2D`, DXT2 and DXT4 as DXT3 and DXT5).
On a host without S3TC (UTM) Mesa decodes them (see mesa.md). `d9pixels sw`: 43 of 43, the
three DXT cases (DXT1, DXT3 and DXT5 alpha) included.

Cube maps (with the Direct3D 8 interface on), sampled by pixel shaders as on Windows 2000: the
HAL's copy of a cube map's records holds all six faces with their levels, texture blits copy
every face, and a version 1 shader gets a program per set of stages holding cube maps (shader
model 2 declares its samplers). Two things the Windows 98 runtimes need besides D3DCAPS8: the
Direct3D 7-level texture caps must list cube maps, and DirectDraw must be told through
`GUID_DDMoreSurfaceCaps` that video memory takes them (`DDSCAPS2_CUBEMAP`); without either the
runtime keeps a managed cube map in system memory only and draws nothing from it. `d9pixels
sw`: 45 of 45, both cube cases (shader model 2 and version 1) included.

Volume textures likewise: each level a surface with its slices one after another, uploaded to
OpenGL's 3D target, copied by VOLUMEBLT box by box, and version 1 shader variants per set of
volume stages. CreateSurface asks DirectDraw for a block of the bytes a level's slices take
(rows tight), and once DirectDraw has allocated it, CreateSurfaceEx puts the slice pitch into the
field that held the block height: the Direct3D 9 runtime fills a managed volume through Lock and
reads its slice pitch there (`lSlicePitch` in DirectX 8 headers, `dwBlockSizeY` in the Me DDK).
With 1 left there, its slices landed a byte apart; changing it inside Lock froze Windows. Also
needed: the volume bit in the Direct3D 7-level texture caps and `DDSCAPS2_VOLUME` in
`GUID_DDMoreSurfaceCaps`. `d9pixels sw`: 47 of 47, both volume cases included; `d3dtest`
(Direct3D 7) unchanged.

DXT volumes: sized in rows of 4x4 blocks, slice after slice, and decoded by the HAL to 32-bit
texels at upload (OpenGL has no compressed 3D textures of this kind). `d9pixels` case
`ps1_volume_dxt1` passes on the rig.

Bump maps: V8U8 and Q8W8V8U8 textures, which the runtime describes as `DDPF_BUMPDUDV` with 16 and
32 bits, go to OpenGL as `RG8_SNORM` (blue swizzled to 1: Direct3D reads V8U8 as du, dv, 1, 1)
and `RGBA8_SNORM`. A version 1 shader with `texbem` or `texbeml` reads each stage's bump matrix
and luminance scale and offset from c8..c15 (the Windows 2000 translator's layout), loaded from
the stage states before each draw. 3DMark 2001 SE needs Q8W8V8U8 for Nature ("device does not
support bump normal maps" with V8U8 alone). `d9pixels`: 49 of 49 on the rig, `ps_texbem`
included.

### Vertex processing on the GPU (2026-11-26, `D3D8` = `"2"`)

With the registry value at `"2"` the Direct3D 8 and 9 devices report hardware transform and
lighting (`D3DDEVCAPS_HWTRANSFORMANDLIGHT`, eight lights, Direct3D 7's vertex processing caps;
no vertex shaders yet, so a device that wants them still gets the runtime's). The runtime then
sends untransformed vertices, with the world, view and projection matrices (`SETTRANSFORM`,
`MULTIPLYTRANSFORM`), the lights (`CREATELIGHT`, `SETLIGHT`), the material and the depth range,
and the HAL draws them through an ARB vertex program made from that state (`tnl_arb.c`): the
full transform, with OpenGL's y flip, z range and Direct3D's pixel centres folded into its matrix;
Direct3D 7 lighting (directional, point and spot lights, attenuation, range, material sources,
local viewer) with `LIT` for the specular term; vertex fog from view depth or range, table fog
from the depth buffer's z; texture coordinates from the vertex or made in view space (normal,
position, reflection vector), through the texture matrices, projected or not. Programs are made
per state key and kept (16 a context); their state sits in `program.env`, reloaded only where it
changed. While such programs draw, OpenGL's viewport and depth range are Direct3D's; transformed
vertices get the whole target and 0..1 back. A check on the build host compiles 266 key
variants with Mesa's parser. In UTM (DirectX 9.0c): `d9pixels` 42 of 42 and `d8pixels` 23 of 23
with `vertex_processing=hardware`, `d8pixels`' lit case included; d9pixels' vertex shader and
luminance cases report "not offered" there.

Fixed-function stages also sample cube maps and volumes now: the unit enables OpenGL's cube map
or 3D target for such a texture (OpenGL prefers them to 2D); before, the unit sampled whatever 2D
texture it held.

Vertex shaders, also with `"2"`: vs_1_1 for Direct3D 8 and vs_2_0 without flow control when the
runtime has announced version 9 (`vs_arb.c`). ARB vertex programs were modelled on Direct3D 8's
vertex shaders, so most instructions keep their names. The Direct3D 8 declaration that comes with
each shader (one stream: registers, types and skips) becomes generic attribute arrays: floats as
floats, D3DCOLOR as normalised bytes swapped from BGRA in the program, UBYTE4 and shorts as
integers. c0..c255 live in `program.env`, with the shader's `def` constants loaded over them;
`program.env` is shared with the fixed-function programs, so the HAL tracks whose state it holds.
a0 is kept as whole numbers in a temporary and loaded with ARL before each relative read (vs_2_0's
a0 has four components). ARB lacks |x|, `_sat`, `lrp`, `nrm`, `sgn` and `mova`'s rounding to the
nearest, so those are spelt out; `sincos` and flow control are refused. oPos becomes OpenGL's clip
position the way the fixed-function programs make it; oFog (or table fog from z / w) goes to the
fog unit or the fog coordinate. Test shaders using every vs_1_1 instruction and the vs_2_0
additions compile with Mesa on the build host.

Also: the runtime version that picks the shader caps is kept per process. In the DLL's shared
data, a Direct3D 8 program run after a Direct3D 9 one in the same boot was offered shader model 2.

On the rig (VM 230, DirectX 9.0c, `"2"`, untimed): `d9pixels` 48 of 49 with
`vertex_processing=hardware`, its five vs_2_0 cases on the GPU included; `vs1_inputs` fails
because the runtime refuses that case's declaration (UBYTE4 and SHORT2 inputs) when vertices are
processed in hardware, before the HAL sees it (open). `d8pixels` 23 of 23, offered vs_1_1 and
ps_1_3 after a Direct3D 9 program in the same boot. 3DMark 2001 SE picks "D3D Hardware T&L"
itself and runs to its score: **12066 3DMarks** at 1024x768x32. Dragothic 139.8 and 35.9 fps,
Lobby 298.3 and 69.3, Nature 119.5, High Polygon Count 52.9 and 49.1 MTriangles/s (40.0 and 20.3
with the runtime's vertex processing), Vertex Shader 172.1 fps, Pixel Shader 673.7, Environment
Bump Mapping 735.3, DOT3 673.9, Advanced Pixel Shader 99.9, Point Sprites 4.2 MSprites/s, fill
rate 10066.3 and 14673.8 MTexels/s (the Car Chase lines did not scroll into view in the
results dialog). Pictures checked through VNC; Car Chase's ground was dark until lights got
slots by Direct3D's index (3DMark's indices go far beyond 32), and its vertex shaders are vs_1_0.

**Timed baseline** (2026-11-26 09:40 EST, rig VM 230 alone, Windows 98 SE, DirectX 9.0c,
`D3D8` = `"2"`, the PR #209 build with the FOURCC fix, GPU minimum clock 1200 MHz with the
performance governor, no trace, one run, default settings 1024x768x32 "D3D Hardware T&L"):
**12134 3DMarks**. Car Chase 221.5 / 48.7 fps, Dragothic 140.0 / 35.6, Lobby 299.7 / 69.8,
Nature 122.1, fill rate 9986.6 / 14558.6 MTexels/s, High Polygon Count 53.6 / 49.0 MTriangles/s,
EMBM 734.3, DOT3 681.1, Vertex Shader 170.0, Pixel Shader 671.1, Advanced Pixel Shader 98.3,
Point Sprites 4.2 MSprites/s. The number later 9x speed work has to beat. Note Dragothic high
detail: 82.5 fps with the runtime's vertex processing, 35.6 with the GPU's.

**First speed changes** (same rig, VM and settings, one timed run each, each on top of the one
before; 2026-11-26 10:19 to 11:26 EST):

| Change | 3DMarks | Car Chase high | Dragothic high | Lobby high | Vertex Shader | Adv. Pixel Shader |
|---|---|---|---|---|---|---|
| Baseline | 12134 | 48.7 | 35.6 | 69.8 | 170.0 | 98.3 |
| 1: fixed-function state above c0..c95 | 12220 | 49.5 | 36.0 | 70.2 | 183.7 | 99.7 |
| A: a texture target keeps the flip chain | 12469 | 59.9 | 35.8 | 73.4 | 183.0 | 198.1 |
| B: textures rendered into stay on the GPU | **14716** | 75.5 | 54.9 | 150.3 | 183.9 | 605.9 |

The other tests stayed within a few percent. A per-draw trace of Dragothic found the cause: at
high detail it renders into ten 128x128 textures a frame, and each switch away from the flip
chain read both 3 MB chain pages back into memory (fixed by A), while each rendered texture was
read back and uploaded again (fixed by B: `glCopyTexSubImage2D` on the GPU, memory only when the
program locks the surface). A context now keeps 16 colour buffers so a frame's targets fit.
Pictures checked through VNC after B: Car Chase, Dragothic and Lobby at high detail and the
Advanced Pixel Shader lake (its reflections are such textures) look as before. Dragothic high is
still below the 82.5 fps of the runtime's own vertex processing.

**Vertex and index buffers in the driver's memory** (2026-11-26, timed 13:13 EST, same setup):
**30226 3DMarks** (14716 before). Car Chase 297.9 / 95.3 fps, Dragothic 534.4 / 271.6, Lobby
403.0 / 180.6, Nature 346.1, fill rate 10136.4 / 14293.2 MTexels/s, High Polygon Count 556.8 /
334.0 MTriangles/s (53.2 / 48.4 before), EMBM 938.0, DOT3 666.2, Vertex Shader 421.8, Pixel
Shader 661.2, Advanced Pixel Shader 749.7, Point Sprites 4.2 MSprites/s.

How it was found, without a profiler on the host: during Dragothic high detail the guest's
processor ran guest code 91% of the time while QEMU's main thread (virglrenderer and the host's
OpenGL) was busy only 47% (`/proc/<pid>/task/*/stat`), so the guest was the limit. Samples of the
guest's instruction pointer (`info registers` through `qm monitor`) put 27% in one `rep movsd` of
`msvcrt.dll`'s memcpy; stack samples (`stop`, `x /64wx $esp`, `cont`) and the code around the
return addresses in `ripplegl.dll` named the caller: Mesa's `u_vbuf_upload_buffers`, copying the
client-side vertex arrays of every draw, 170 KB to 1 MB a call. The draw trace showed why: the
runtime kept every vertex and index buffer in its own system memory (caps `DDSCAPS_SYSTEMMEMORY`),
so the HAL could not know when one changed and handed each draw's whole vertex range to OpenGL
again, about 4 MB a frame, mostly from buffers the program fills once.

Now, with hardware vertex processing (`D3D8` = `"2"`), the device reports driver-allocated
buffers (`D3DDEVCAPS_HWVERTEXBUFFER`, `D3DDEVCAPS_HWINDEXBUFFER`, and `DDSCAPS_EXECUTEBUFFER` in
DirectDraw's surface caps), and DirectDraw creates them through the driver's execute-buffer
callbacks (`DDHALINFO.lpDDExeBufCallbacks`). The driver allocates them in a heap in the shared
arena (DirectDraw may free one from another process), marks their surface records and counts
the Locks that may write. A draw from a buffer marked static (`DDSCAPS2_HINTSTATIC`) uses its
OpenGL buffer object, uploaded again only after a write; an indexed draw reads its indices from
the index buffer's object as they are. In the Dragothic run that is 12.7 MB uploaded in all, where
the old path copied about 1.9 GB. `D3D8Buffers` = `"0"` (same key) turns it off.

Traps: the callback table pointer in the HAL description is 16:16 like the other tables (a flat
address made DirectDraw allocate execute buffers in video memory itself, and reading before such
a buffer for a header faulted). Dynamic buffers stay on the old path: the runtime appends
DrawPrimitiveUP vertices to its own dynamic buffers after a single Lock, so their later writes are
not seen (d9pixels lost four cases until only static buffers went to buffer objects). Checked on
the rig: d9pixels 48/49 (the one failure is the known `vs1_inputs` declaration the runtime
refuses with hardware vertex processing), d8pixels 23/23, Dragothic at both details through VNC.

A kept list of the lights that are on (instead of scanning the 128 light slots each draw) was
measured on top of this and dropped: 30341 3DMarks, every test within 2% (eight lights 328.6
against 334.0 MTriangles/s).

**What limits Car Chase and Lobby now** (0.6.0, rig, untimed, 2026-11-26 17:40 EST). Both are
still limited by the guest's processor, not the host. During 20 s of each high-detail test,
QEMU's thread for the guest CPU was busy all the time (Car Chase 97% guest code, Lobby 96%), while
its main thread, where virglrenderer and the host's OpenGL run, was busy 23% and 19%. About 220
samples of the guest's instruction pointer each show no single hot spot:

| Where the guest's time goes | Car Chase high | Lobby high |
|---|---|---|
| 3DMark's own code (its EXE and engine) | ~22% | ~25% |
| Mesa in the guest (`ripplegl.dll`) | ~14% | ~10% |
| KERNEL32 and MSVCRT | ~17% | ~10% |
| Windows' ring 0 (VMM and VxDs) | ~8% | ~9% |
| the HAL (`rippledd.dll`) | ~5% | ~10% |

The HAL's share is almost all in `index_list`, which copies a draw's indices and rebases them to
their smallest one. Draws whose vertices are in a buffer object skip it. It is still used for:
- the runtime's dynamic buffers;
- DrawPrimitiveUP data;
- pre-transformed vertices.

Keeping dynamic buffers on the GPU too (renaming on `DDLOCK_DISCARDCONTENTS` instead of
uploading again) would remove most of it. The d9pixels cases that broke when dynamic buffers were
first given buffer objects (`ps_lit_fog`, `vs2_*`) are the test for that.

Advanced Pixel Shader's white boat and jetty: a trace with each draw's state (new trace lines)
showed nine draws a frame with no texture on the stage their shader samples, and about one
refused CanCreateSurface a frame. The refused format was FOURCC 63: for mipmapped Q8W8V8U8
textures the runtime names the format by its D3DFORMAT number instead of `DDPF_BUMPDUDV`. Such
surfaces have no bit count, so CreateSurface sizes them itself, and Lock and texture blits take
the texel size from the format. Since then the boat and jetty are shaded dark grey in the dusk
scene and the water shows its ripples; whether their flat look is right needs a reference
picture.

### What the current pixel tests found (2026-11-26, evening)

`d8pixels` and `d9pixels` had grown many cases on the Windows 2000/XP side since the copies used
for 0.6.0 were built. Rebuilt for Windows 98 (below) and run on the Intel host's VM 230 with
0.6.0 plus the flip-chain change, Direct3D 9 passed 57 cases and failed 10, and Direct3D 8
failed one of 27. What was wrong, in the order it was fixed:

| Case | What the program saw | Cause | Now |
|---|---|---|---|
| `luminance` | L8 and A8L8 not offered | no such formats | L8, A8L8 and A8 as OpenGL's luminance and alpha formats, which sample the way Direct3D does |
| `ps_unbound_stage` (Direct3D 8) | white from a stage without a texture | the fixed-function stand-in (white) was bound under pixel shaders too | opaque black under a pixel shader |
| `colorwrite` | all channels written | `D3DRS_COLORWRITEENABLE` ignored | `glColorMask` for draws; a clear and the upload of a CPU-written page write every channel |
| `points`, `lines_points`, `program_point` | no points at all | OpenGL's mode for points is 0, and the draw path took 0 for an unknown primitive type | point lists drawn; `D3DRS_POINTSIZE` between its minimum and maximum, point sprites, a shader's `oPts`, sizes scaled by distance; `MaxPointSize` 64 |
| `texture2048`, `rt2048` | white, and a refused render target | DirectDraw refuses a video memory surface wider than the display mode unless the driver has `DDCAPS2_WIDESURFACES`; a managed texture then gets no video memory copy and is drawn as if no texture were set | the capability is set (textures live in the linear system heap, which has no width). The same hit a 1024 wide texture at 800x600 |
| `stages8` | white | the caps said eight simultaneous fixed-function textures with a Direct3D 9 runtime; four are drawn | eight blend stages (pixel shader 2.0 needs their sampler states) and four simultaneous textures, as cards of the time reported |
| `cube_rt` | the last face's colour on +X, the others black | a cube map's faces and a texture's lower mipmap levels have surface handles of their own; only the top handle was known, so `SETRENDERTARGET` for another face found nothing | the copies of the records are entered under those handles; such a target is read back and its texture uploaded again when used |
| `vs2_streams`, `ff_streams` | one vertex stream offered | only stream 0 was read | streams 1 to 15 with hardware vertex processing: a vertex shader's inputs, and the fixed-function attributes of a declaration that comes without shader code, each from the vertex buffer of its stream at the same vertex; `MaxStreams` 16 |
| `index32` | 16-bit indices only | index buffers of 32-bit indices were refused | drawn; turned into 16-bit indices where a draw's range fits them |
| `ff_streams`, only in a full run | nothing drawn | the runtime gives a freed vertex buffer's handle to the next one, and the heap gives it the freed block: same handle, same address, and after one fill the same count of writes, so the old buffer's contents stayed on the GPU | a buffer announced again under a known handle is uploaded again |

With those: Direct3D 9 69 pass and 2 fail, Direct3D 8 28 of 28. The totals grew because cases
that were skipped as not offered run now, and because the tests gained cases on the way:
`static_indexed`, `scaled_point` and `ff_streams` in `d9pixels`, `ff_streams` in `d8pixels`.
Still failing: the caps lack the immediate presentation interval (the runtime
drops it from what the driver reports; not understood yet), and the `vs1_inputs` declaration.
Not offered, and next: user clip planes, the scissor test, L16, bump environment mapping in the
Direct3D 9 caps, vertex shader flow control, linear StretchRect. Not done for points: sizes
carried in the vertex. Not read from a declaration without code: blend weights and indices,
the point size and the tweening inputs.

The last row is a fault no single case shows: `ff_streams` passes when run alone (`d9pixels ps=80`) and
failed after the cases before it had made and freed their buffers. A case that fails only in
the full run points at state kept from an earlier case.

**The pixel tests alone are not enough.** One of these commits put braces around the indexed
branch of each draw path and kept deciding "indexed" by the index pointer. For indices in a GL
buffer object that pointer is a byte offset, 0 for a draw from the buffer's start; such draws
had only been made because the unbraced `glDrawElements` ran after every draw. Both pixel tests
still passed, while 3DMark 2001 SE lost most of its static geometry (Car Chase black but for
its HUD) and reported a score half as high again: 45745. The captures of that run showed it at
once. "Indexed" goes by the index count now, `d9pixels` has a case for such a draw
(`static_indexed`), and a benchmark number without a look at its frames counts for nothing.

**Building and running the tests for Windows 98.** `d8pixels.c` and `d9pixels.c` (in
`win2k/tools`) build with Visual C++ 6 and the DirectX 8 and 9 headers, `d3d9.dll` loaded at run
time: `cl /nologo /W3 /O2 /MT d9pixels.c /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib`. Give
every run its own output file. DirectDraw's helper process keeps the first redirected output
file of a boot open (see "The output file held after a test run" below); a later run into the
same name fails without a word, and reading the file shows the old run's results.

### 3DMark 2001 SE (2026-11-26, rig VM 230)

Windows 98 SE with DirectX 9.0c, the Direct3D 8 interface on, 1024x768 32-bit, the runtime's
software vertex processing; one untimed run per test, pictures checked through VNC. Car Chase
192.2 and 55.7 fps (low and high detail), Dragothic 223.0 and 82.5, Lobby 320.4 and 71.3, High
Polygon Count 40.0 and 20.3 MTriangles/s (one and eight lights), Vertex Shader 165.2 fps. With
the bump map formats: Nature 145.6 fps, Environment Bump Mapping 558.4 fps, DOT3 Bump Mapping
691.7, Pixel Shader 660.6, Advanced Pixel Shader 66.8, Point Sprites 8.6 MSprites/s; the fill-rate
tests run too. All pictures look right except Advanced Pixel Shader: its boat and jetty are plain
white and the water has no ripples (no shader was refused and no draw skipped; next to look at).
Once, the multi-texturing fill-rate test made 3DMark quit without a message, on a second
benchmark run in the same process after the game tests; two later fill-rate runs, one of them the
second in its process, finished. Not explained.


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
