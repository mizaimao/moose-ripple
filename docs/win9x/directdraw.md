# DirectDraw on Windows 98 (work in progress)

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

## State (2026-11-15)

Steps 1 to 5 go through and `SetInfo` accepts the description, but DirectDraw doesn't use the
HAL yet: `ddtest` reports no video memory and no flipping from the driver, and the program runs
on the emulation as before (nothing is worse than without the HAL). The driver answered the DCI
1.0 commands of step 6 with "failed" instead of passing them to the DIB engine, which may be
why; that change is made and not yet tested.
