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
part can be shown by moving the display start.

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
which DirectDraw's own blits use too, a texture blit, or a changed contents stamp); textures
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

Not yet: transform and lighting in the driver (Direct3D does it on the CPU), mipmap filtering
(level 0 only), colour keys, stencil, palettized textures, state blocks recorded by the driver.

`tools/win9x/d3dtest.c` checks it the way a game uses it: on Windows 98 SE with DirectX 7 in
Proxmox (virgl on the host's Intel UHD 630) every pixel check passes (clear, Gouraud triangle,
depth test, texture texels, blending), the screen shows the frame, and the scene runs at about
1,040 frames a second at 640x480x32 (each frame read back from the GPU and flipped).

It also checks indexed drawing and texture updates (32-bit, 565, 1555 and 4444 textures:
locked again, blitted from system memory, a texture in system memory rewritten).

Windows 98 gives a new process the id of one that has just ended; the DLL's data is shared by
all processes, so it tells processes apart by a number of its own kept in each process's
environment (the second run of a program failed before that).

### 3DMark 2000 (2026-11-16)

3DMark 2000 1.1 runs its default benchmark to the end on Windows 98 SE in VM 230 (DirectX 7,
256 MB, virgl on the host's Intel UHD 630, guest CPU shown as a 3.6 GHz Pentium III), with the
game scenes, cockpit displays, fog and lighting as they should look: **11816 3D marks** at
1024x768, 16-bit colour and textures, 16-bit z, Direct3D's own transform and lighting. The
first complete run, before the system memory heap below, made 1240.

| Test | Result | Before the heap |
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

Open: the text on 3DMark's "Now testing" screens between tests doesn't show (3DMark 99 and
2000 alike). It is not lost to the read-back below: a trace shows the write lock that is
probably the text on a surface that is shown next, untouched by Direct3D in between.

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
