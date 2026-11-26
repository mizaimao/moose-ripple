# The Windows 9x display driver (2D stage)

`win9x/display/` is a display minidriver for Windows 95/98/ME: a 16-bit driver (`ripple9x.drv`)
that sets a mode on QEMU's card and lets Microsoft's DIB engine (part of Windows) draw. It is
step 2 of [plan.md](plan.md). For the 2D desktop QEMU's `virtio-vga` (and its standard VGA,
`-vga std`) is a VGA card with the Bochs/QEMU mode-setting registers (I/O ports 01CEh/01CFh)
and a linear frame buffer in its first PCI memory range, and that is all this page needs. 3D
goes through the mini-VDD's virtio-gpu side ([vxd-interface.md](vxd-interface.md)), the OpenGL
driver ([mesa.md](mesa.md)) and `rippledd.dll` ([directdraw.md](directdraw.md)).

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
enough for a desktop) and a hardware pointer. DirectDraw and Direct3D: [directdraw.md](directdraw.md).

## Restart and Shut Down ended in a blue screen (found and fixed 2026-11-16)

Restarting or shutting down Windows with this driver stopped at a text screen that looked like
noise: a blue-screen message ("A fatal exception 06 has occurred at C000:00008008"), unreadable
because the text font in video memory was gone, waiting for a key. Seen in QEMU 10.0 on the
build host and in Proxmox's QEMU on the GPU host. Windows' own VGA driver on QEMU's Cirrus card
restarts cleanly in the same VM.

**The fix (mini-VDD):** when the display driver is disabling, and before every later video BIOS
mode set in Windows' VM, the mini-VDD maps the card's VGA window (A0000h-BFFFFh) into that VM
(`_PhysIntoV86`). The BIOS's writes then go to the card without page faults, so the VDD's
oversized fill (step 2 below) never runs. Restart and Shut Down from the Start menu work (build
host, 2026-11-16).

What happened, found with QEMU's monitor and gdb stub:

1. At shutdown the driver's Disable calls the VDD (VDD_DRIVER_UNREGISTER). The VDD switches the
   system VM to text mode 3 with the video BIOS (INT 10h). Any later mode switch through the BIOS
   (the driver's own, or Windows' after Disable) does the same as below.
2. The BIOS clears the 32 KB text buffer at B800:0000. The first write page-faults into the VDD,
   whose page initialiser for text mode blank-fills (0720h) 16 pages, 64 KB, from B8000h through
   its ring-0 alias of physical memory from A0000h. The last 32 KB of that are C0000h-C7FFFh:
   the video BIOS's own code.
3. That write sticks because under KVM QEMU does not enforce the chipset's write protection of
   the BIOS shadow (its memory map shows the range read-only, but the KVM memory slot is
   writable). On real hardware it would be dropped.
4. The BIOS returns into the blanks (`20 07` is `and [bx],al`), runs through them to C8000h and
   meets font data at C8008h: invalid opcode, exception 6, in the system VM: blue screen.

Ruled out on the way: the order of the calls in Disable (now the DDK sample's), leaving out the
unregister call or the driver's own BIOS call, registering with EDX=-1 (no VGA in a window),
and putting the VGA memory-map registers back to text values first. Why the VDD grows plane 0
to 64 KB for our driver and not for the Cirrus one is still not known; the fix makes it moot.
The mini-VDD logs the VDD's calls and Windows' exit on QEMU's debug port. Note: `rundll32
shell32.dll,SHExitWindowsEx 2` does not restart (rundll32 passes a window handle as the flags);
it shuts down.

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

One folder holds the driver: `ripple9x.inf`, `ripple9x.drv`, `ripplevd.vxd`, `rippledd.dll` (the
DirectDraw and Direct3D driver; these four `build.sh` makes, in `build/win9x/`) and `ripplegl.dll`
(the OpenGL driver, from `win9x/mesa/build.sh`).

- **A new install:** start the VM with QEMU's `virtio-vga` (or `virtio-vga-gl` for 3D).
  Windows finds the new PCI display adapter at start-up; give the wizard that folder. It
  copies `ripple9x.drv`, `ripplevd.vxd`, `rippledd.dll` and `ripplegl.dll`, registers the mini-VDD
  and the OpenGL driver, and asks for a restart.
  Checked 2026-11-15 on a Windows 98 SE install that had run on a Cirrus card, before
  `rippledd.dll` was in the INF: after the restart the desktop runs on our driver and OpenGL
  programs find `ripplegl.dll` (on a host without 3D, Mesa's software renderer).
- **Replacing another display driver:** Display Properties > Settings > Advanced > Adapter >
  Change > Have Disk, and the same folder.
- **UTM (macOS, x86 emulated):** display `virtio-vga-gl`, machine `pc` (i440fx), one CPU, and
  the CPU without its local APIC (`-cpu pentium3,apic=off` as a QEMU argument, or remove the
  `apic` flag in the CPU settings). With the APIC, UTM's emulated APIC keeps the PIC's timer
  interrupt from the CPU after Windows 98 restarts or shuts down (the APIC's LINT0 stays
  masked), and the BIOS waits for timer ticks forever; Windows 98 has no use for the APIC. Checked
  2026-11-16 in UTM 5.0.8: start-up in about 15 seconds, Restart, OpenGL, Direct3D and
  DirectDraw tests. Don't give the VM a QEMU `fat:` folder as a disk: UTM's sandbox refuses its
  writes, and Windows hangs at start-up.
- **QEMU in WSL2 on a Windows host:** checked 2026-11-16 with QEMU 8.2 and KVM, virgl on an
  NVIDIA RTX 4090 through Mesa's d3d12 driver: Restart, OpenGL, Direct3D and DirectDraw tests.
- **Updating ours:** the same way, with the new folder. Windows keeps a copy of the INF from the
  first install (in `C:\WINDOWS\INF\OTHER`) and uses that copy, not the new one, when the card
  turns up in another PCI slot (another VM setup). A copy from before 0.1.0.0 has no mini-VDD
  in it: the desktop then works without 3D, and DOS sessions and Restart misbehave.
- If Device Manager still lists the old card (with a warning sign), remove that entry: it
  can hold on to the VGA ports, and our adapter then reports "cannot find any free
  Input/Output Range" (Code 12).

Then pick a monitor type that allows the sizes you want (Advanced > Monitor): with
"(Unknown Monitor)" Display Properties offers 640x480 only (programs can still set any mode
the driver has).

### One package for Windows 95 OSR2.5, Windows 98 SE and Windows Me

Use the [Windows 9x 0.2.0 release](https://github.com/mizaimao/moose-ripple/releases/tag/win9x-v0.2.0)
for Windows 95 OSR2.5, Windows 98 Second Edition and Windows Me. The ISO and
ZIP contain one INF and the same four driver files for all three systems.
Select the **moose-ripple virtio-vga** model. The installer reports version
`0.2.0.0`, including when updating the earlier Windows 98 SE 0.1.0 package.

On a clean install, check Device Manager for the PCI bus before choosing **Have Disk**.
If the only display adapter is the generic VGA device and the PCI bus is missing, use
**Add New Hardware → System devices → PCI bus** and let Windows detect its devices.
Restart when prompted. Select the PCI display adapter with vendor ID `1AF4` and
device ID `1050`, install the driver from the CD root, then restart and select
16-bit or 32-bit colour in Display Properties.

Learnt in the 0.6.0 clean install (2026-11-26):
- **Fresh Windows 98 SE disk on the Intel host.** Windows took a few restarts to add the PCI
  bus's devices, and asked for its own CD for some of them; `C:\WIN98` had the files.
- **Windows protection error.** One boot stopped with a Windows protection error, the fault of
  Windows 98 on fast processors. Patching the disk with patcher9x fixed it; the rig's VM 230 has
  the patcher9x boot floppy for this.
- **Adapter conflict.** If Windows reports that the display adapter conflicts with another
  device, Device Manager still lists the adapter from before the VM's display changed (Cirrus
  Logic on that disk). Remove it and restart.
- **DirectX 7 or later.** With the DirectX 6.1 that Windows 98 SE ships, DirectDraw gets no video
  memory or flips from the HAL (ddtest in software, also with `D3D8` = `"0"`), while OpenGL works.
  Install DirectX 9.0c (December 2006) on 98 SE and Me, DirectX 7 or 8.1 on 95.

On Windows 95, the bundled IE4 installation supplies `MSVCRT.DLL` for OpenGL.
Microsoft's DirectX 7 runtime also supplies that library and the Direct3D 7 and
DirectInput interfaces used by the game tests. Install it for those games and restart.

On OSR2.5, Quake III completed all 1,346 frames of `demo001` with the default
driver settings. The full Direct3D 7 test passed, including texture mipmap
selection and restoring pixels written by the CPU. Re-Volt loaded its Toys in
the Hood 1 track, continued rendering and exited cleanly when closed. These
tests used virgl with llvmpipe, the host software renderer. With
`RIPPLE_D3D_SCANOUT=1`, all 1,300 measured Direct3D frame flips stayed on
the host with zero frame readbacks to the guest. The displayed colours also
passed the pixel checks.

Windows 98 SE and Windows Me passed OpenGL and Direct3D 7 with the same driver
files on Intel UHD 630. Windows 98 SE passed DirectDraw surface restoration,
page flipping and Direct3D with host presentation both enabled and disabled.
Windows Me also passed surface restoration and completed two full Quake III
timedemos.

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
  PCI devices, the virtio-vga among them, which then matched our INF by its PCI ID. (Without
  that, installing the adapter by hand from the Add New Hardware list leaves it "not
  configured properly".) The test disk keeps a snapshot from before any of this, so a new
  install can be tried again.
- Shutting down often hangs at the last screen (with any display driver). The test script resets
  the VM instead; `AutoScan=0` in `C:\MSDOS.SYS` keeps ScanDisk from running at every start.

`tools/win9x/vm.sh` runs the VM headless under plain QEMU on the build host: start, stop,
screenshots, keys, typed text, a folder shared as drive D:, COM1 and the debug port in files.
A line in the guest's `AUTOEXEC.BAT` copies `D:\RIPPLE9X.DRV` over the installed driver at every
start, so a new build is tested by restarting the VM.
The pointer actions (`vm.sh mouse`, `click`) need a PS/2 mouse driver in the guest, which this
test disk doesn't have yet; so far everything is driven with keys.
