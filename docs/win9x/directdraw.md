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
Textures are uploaded when first used and again when DirectDraw says their contents changed.

Offered: the HAL device with hardware rasterization, 16- and 32-bit render targets, 16- and
32-bit z-buffers, textures in five RGB formats up to 2048x2048 in video or system memory, two
texture stages with the usual operations, alpha blending and alpha test, fog, flat and Gouraud
shading, every primitive type. Not yet: transform and lighting in the driver (Direct3D does it
on the CPU), mipmap filtering (level 0 only), colour keys, stencil, palettized textures, state
blocks recorded by the driver.

`tools/win9x/d3dtest.c` checks it the way a game uses it: on Windows 98 SE with DirectX 7 in
Proxmox (virgl on the host's Intel UHD 630) every pixel check passes (clear, Gouraud triangle,
depth test, texture texels, blending), the screen shows the frame, and the scene runs at about
1,040 frames a second at 640x480x32 (each frame read back from the GPU and flipped).

Windows 98 gives a new process the id of one that has just ended; the DLL's data is shared by
all processes, so it tells processes apart by a number of its own kept in each process's
environment (the second run of a program failed before that).

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
