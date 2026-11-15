# The Windows 9x display driver (2D stage)

`win9x/display/` is a display minidriver for Windows 95/98/ME: a 16-bit driver (`ripple9x.drv`)
that sets a mode on QEMU's card and lets Microsoft's DIB engine (part of Windows) draw. It is
step 2 of [plan.md](plan.md). No 3D yet, and nothing virtio: QEMU's `virtio-vga` (and its
standard VGA, `-vga std`) starts out as a VGA card with the Bochs/QEMU mode-setting registers
(I/O ports 01CEh/01CFh) and a linear frame buffer in its first PCI memory range, and that is all
this stage uses.

## What works (Windows 98 SE in QEMU 10.0, `-device virtio-vga`, 2026-11-15)

- The desktop at 8, 16, 24 and 32 bits a pixel, 640x480 up to what the card's 8 MB hold
  (1600x1200 at 32 bits), including sizes Windows 98 never had, such as 1280x720.
- 256 colours with Windows' palette manager (the Welcome window's gradient is right).
- Changing resolution and colour depth from Display Properties without a restart.
- A full-screen DOS session and back (Alt+Enter): the VDD (Windows' virtual display device)
  tells the driver, which sets its mode again and has Windows repaint.
- The pointer is the DIB engine's software pointer (colour and animated pointers included).

- A DOS session in a window, and installing a monitor, leave the screen alone: the mini-VDD
  (`win9x/minivdd/`, below) keeps the video BIOS off the card's mode-setting ports.

Not there yet: hardware drawing (everything is the DIB engine in software, which QEMU makes fast
enough for a desktop), DirectDraw and a hardware pointer.

## Files

| File | What |
|---|---|
| `display/enable.c` | Enable, ReEnable, Disable, modes, palette, ValidateMode, screen switches |
| `display/asm.asm` | library entry, the forwarders to the DIB engine, and what 16-bit C can't do: 32-bit port I/O, the VDD's register calls, DPMI, interrupt hooks |
| `display/r9x.h` | the driver interface's structures and constants, written out from the documentation |
| `display/ripple9x.def`, `.rc` | exports by ordinal; the resources Windows reads from a display driver (screen metrics, system colours, stock fonts) |
| `minivdd/ripplevd.asm`, `.def` | the mini-VDD: a VxD (32-bit kernel module) the VDD loads, which owns ports 01CEh and 01CFh |
| `minivdd/vgpu.c` | the virtio-gpu side, in C inside the same VxD (Visual C++ 6, no run-time library): finds the card, brings up the virtio transport and exchanges commands with the host |
| `inf/ripple9x.inf` | setup information: matches `PCI\VEN_1AF4&DEV_1050` and `PCI\VEN_1234&DEV_1111` |
| `build.sh` | builds on the Linux build host: `win9x/build.sh`, output in `build/win9x/` |

`TRACE=1 win9x/build.sh` builds a driver that writes one letter per driver call to QEMU's debug
port (0E9h): with `-debugcon file:debug.log` the log shows the last calls before a crash. The
driver always writes a few lines there (`r9x: ...`); nothing listens on a real PC.

## Install

Display Properties > Settings > Advanced > Adapter > Change > Have Disk, and point it at the
folder with `ripple9x.inf`, `ripple9x.drv`, `ripplevd.vxd` (the three files `build.sh` makes)
and `ripplegl.dll` (the OpenGL driver, from `win9x/mesa/build.sh`). Or let the Add New Hardware wizard find the PCI
display adapter and give it the same folder. Then pick a monitor type that allows the sizes you
want (Advanced > Monitor): with "(Unknown Monitor)" Windows offers 640x480 only.

## Things learnt the hard way

- **A selector for the frame buffer:** `AllocSelector(0)` gives an empty descriptor that can't
  be used for anything. The driver copies its own data segment's descriptor (`AllocSelector(DS)`)
  and then sets base and limit; the base comes from DPMI function 0800h (map physical memory).
- **The DIB engine answers in EAX.** `CreateDIBPDevice` returns the screen's size in bytes in
  EAX, not in DX:AX: a 1024x768x32 screen is 300000h bytes, a zero low word, which 16-bit C
  takes for failure. The call goes through a small assembly wrapper.
- **`ValidateMode` must answer in all of EAX.** Display Properties calls it for every mode in the
  registry, and reads more than AX: with a C function (which leaves the upper half of EAX as it
  was) only 640x480 was offered. The export is an assembly wrapper that zero-extends the answer.
- **`RefreshRate` in the INF.** Without `HKR,DEFAULT,RefreshRate,,-1` the VDD reports the
  monitor information as "disabled by the user".
- **Screen switches:** the driver hooks int 2Fh for functions 4001h (the screen goes to someone
  else: stop drawing) and 4002h (it is back: set the mode, repaint through USER's entry 275).
  The VDD also calls the function registered with `VDD_DRIVER_REGISTER` to have the mode set.
- **The video BIOS turns our mode off behind our back.** QEMU's VGA BIOS writes the Bochs
  mode-setting ports on every mode set. Windows' VDD traps the standard VGA ports of DOS
  sessions but knows nothing of ports 01CEh/01CFh, so a DOS session *in a window* (its BIOS sets
  text mode when it starts) switches the real card to text mode, and so does a BIOS call Windows
  makes when a monitor is installed. The mini-VDD takes the two ports over: the VM that has the
  screen reaches the card (the display driver in protected mode, or a full-screen DOS session),
  everything else writes to a private copy of the registers. While the VDD itself switches the
  screen to or from a DOS session (it asks the BIOS for the mode) nothing is held back. The
  display driver tells the mini-VDD when its mode is set (`VDD_REGISTER_DISPLAY_DRIVER_INFO`).
- **The card only decodes 16-bit accesses** to those ports, so the trap handles word I/O itself
  instead of letting VMM split it into bytes.

## Test VM notes

The test disk is a copy of a Windows 98 SE install made for another card. Two things in it had
nothing to do with our driver and cost time:

- Its "Plug and Play BIOS" device had failed, so Windows saw no PCI devices at all and the
  display adapter was a leftover entry. Changing that device's driver to "PCI bus" (Device
  Manager > Update Driver > show all hardware > Standard system devices) made Windows find the
  PCI devices, the virtio-vga among them, which then matched our INF by its PCI ID.
- Shutting down often hangs at the last screen (with any display driver). The test script resets
  the VM instead; `AutoScan=0` in `C:\MSDOS.SYS` keeps ScanDisk from running at every start.

`tools/win9x/vm.sh` runs the VM headless under plain QEMU on the build host: start, stop,
screenshots, keys, typed text, a folder shared as drive D:, COM1 and the debug port in files.
A line in the guest's `AUTOEXEC.BAT` copies `D:\RIPPLE9X.DRV` over the installed driver at every
start, so a new build is tested by restarting the VM.
The pointer actions (`vm.sh mouse`, `click`) need a PS/2 mouse driver in the guest, which this
test disk doesn't have yet; so far everything is driven with keys.
