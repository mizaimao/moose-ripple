# tools/win7-debug: looking into Windows 7's display kernel

Windows 7 says nothing when it refuses a display driver: the device shows Code 43 or the
screen stays off, and no log has a line about it. These scripts are how the reasons were
found (`docs/windows/kmd-port-notes.md`, "Load tests"). They run on the Linux build machine
with Python 3 (the repository's `.venv`), LLVM's `llvm-objdump`, `llvm-pdbutil` and
`llvm-readobj`, and gdb.

Nothing of Microsoft's is in this folder or anywhere in the repository. The modules are copied
out of the test VM into a scratch folder, and their symbol files come from Microsoft's public
symbol server for that exact build.

## Reading the code

In the scratch folder, with `dxgkrnl.sys`, `dxgmms1.sys` and `watchdog.sys` from the guest's
`System32\drivers`, and `PY` standing for the repository's `.venv/bin/python -I`:

    $PY resym.py url dxgkrnl.sys                 # where its symbol file is; download it
    $PY resym.py map dxgkrnl.sys dxgkrnl.pdb     # writes dxgkrnl.sys.map (address, name)
    $PY resym.py dis dxgkrnl.sys DpiFdoStartAdapter > start.txt
    $PY calls.py dxgkrnl.sys start.txt           # the listing with call targets named

## Reading Windows' own error log from a running guest

The display kernel reports every internal failure to `watchdog.sys`, which keeps the reports
in memory only when asked to. In the guest, before the boot to look at (N = 1 to 7; do not set
level 0, those are the fatal ones):

    reg add "HKLM\SYSTEM\CurrentControlSet\Control\Watchdog\Logging" /v N-LevelSizeLg2 /t REG_DWORD /d 10 /f

Then open QEMU's gdb stub on the VM, bound to the host's loopback (through the monitor:
`gdbserver tcp:127.0.0.1:12345`), and from the scratch folder with the `.map` files:

    WIN7_RE=$PWD gdb -q -batch -ex "set architecture i386:x86-64" \
        -ex "target remote | ssh HOST socat - TCP:127.0.0.1:12345" -x wddump.py

It prints the log oldest first: the address that reported (module, function, offset) and up to
five values, usually a status code and the objects involved. The line number Windows gives
each report is the constant stored just before the call at that address in the listing.
Close the stub afterwards (`gdbserver none`).

`stuck.py`, run the same way, is for a guest that has stopped responding: for every processor
it prints where it is and the code addresses on its kernel stack, named by module and
function. A stop error early in the boot never reaches the screen of a virtio display and
looks like a hang (all processors busy, no disk activity); this shows the stop and what led to
it. Close the stub afterwards here too.

`wdlog.py` does the same live, with a hardware breakpoint on the reporting function, for a
guest without the registry values. `guestmods.py` finds the guest's loaded modules for both
(the kernel's base from the interrupt table, the module list from the kernel's debugger data
block); it knows Windows 7 x64 only.

## Reading a bluescreen

Windows writes `C:\Windows\MEMORY.DMP` (a kernel memory dump) and a small dump in
`C:\Windows\Minidump`. For a crash in a system thread (stop `0x7E`) or a page fault, the
exception record and the registers at the fault sit below the stack the small dump keeps, so
take `MEMORY.DMP` (copy it to `C:\rb\pkg` in an elevated prompt and send it with `put.cmd`):

    $PY kmem.py MEMORY.DMP crash                 # the exception and the registers at the fault
    $PY kmem.py MEMORY.DMP q fffffa8004da2100 24 # kernel memory, 64-bit words

    $PY kstack.py MEMORY.DMP fffff880031c1ad8 viogpu3d.sys=viogpu3d.map
                                                 # the code addresses on the kernel stack below and
                                                 # around an address, named by module (and by
                                                 # function where a .map is given): what ran last
                                                 # on that thread, also after the stack unwound

`l9req.py d3d10level9.dll ADDR1 ADDR2 ADDR3` prints what Windows 7's Direct3D 10 Level 9 layer
requires of a Direct3D 9 driver before it makes a device at feature level 9.1, 9.2 and 9.3
(capability bits, minimum values, formats with their operations), read from the tables
`MinCapsLevel1` to `3` in a copy of the DLL; the three addresses are those symbols' (from
`resym.py map` with the DLL's public symbols). The compositor uses that layer on a host that
cannot carry Direct3D 10 (`docs/windows/mesa-win7.md`).

The faulting address minus the module base on the bluescreen, plus the image base in the
module's `.map`, names the function there. `kmem.py` knows Windows 7 x64 kernel memory dumps
only (4-level page tables, the summary bitmap layout of that version).

## Where a guest spends its time

`rig/` holds scripts for the VM host (copy them to its `/tmp`, run as root):

- `threads.sh VMID SECONDS`: CPU use of each QEMU thread. `CPU_n/KVM` are the guest's processors,
  `kvm` is QEMU's main thread, where virglrenderer issues the host's GL calls.
- `gpubusy.sh SECONDS`: how much of the time the host's Intel GPU was awake, and its clock.
- `rcsbusy.sh SECONDS`: the share of time the render engine was executing requests (the i915
  driver's own counter), which says whether the GPU is the limit; `gpubusy.sh` only says awake.
- `i915trace.sh SECONDS`: the Intel GPU driver's requests and waits, counted (tracing is
  switched off again at the end).
- `ripsample.sh VMID COUNT`: COUNT samples of every guest processor's instruction pointer,
  privilege level and page table base, through QEMU's monitor (about 80 a second). The most
  frequent address is the idle loop.
- `modbase.sh VMID [ADDRESS]`: where the guest kernel (or the module below ADDRESS) is loaded,
  for naming kernel samples with the module's `.map`; the HAL sits just below the kernel.
- `cleanvm.sh vista|win7 prep|boot|reset|kill` and `guestctl.pl`: a throwaway guest on a disk
  that never had the driver, and its keys, clicks and screenshots, for the clean-install check
  of a release package (`docs/windows/test-vms.md`).
- `maintrace.sh TID SECONDS`, `kvmexits.sh SECONDS` and `prof.sh NAME`: what one thread (QEMU's main
  thread, where virglrenderer draws) does, from the kernel's trace events: its share of the time on a
  processor and its system calls by kind; why the guest's processors leave guest mode; and both, over
  and over, while a guest runs.
- `ripstream.sh NAME SECONDS`, `sm2sample.sh NAME` and `attribute.pl`: where a guest's processors are
  during a benchmark. The first samples every processor's instruction pointer about 70 times a
  second through QEMU's monitor (64-bit and 32-bit code), the second takes a minute of samples in
  each of 3DMark06's first two graphics tests, and the third gives the samples to kernel modules,
  to the modules of the benchmark's process and, with a symbol list, to functions. The module
  lists come from `tools/vista/modlist.c` in the guest (`tools/win7-bench/profile`).
  `portsample.sh NAME SECONDS` records RDX with each sample: at a port read or write that is
  the port number, which names the emulated device a processor is waiting for.
- `refvm.sh new|virgl|card|kill NAME` and `busylog.sh NAME`: the same Windows 7 disk, CPU model
  and memory with the virtio GPU or with a passed-through graphics card, for a like-for-like
  comparison, and a log of the GPU's busy share and QEMU's threads during a run. The guest's
  side is `tools/win7-bench` (`docs/windows/test-vms.md`, "3DMark06 against the GeForce 9600 GT").

In the guest, `guest/timerres.ps1 [SECONDS]` samples Windows' timer resolution and says how
much of the time it ran fine (under 1 ms): the kernel driver takes the half-millisecond timer
only for a stream of work, and an idle desktop should show 0%.

The UI probes build on Linux with `sh tools/win7-hello/build-ui-probes.sh` (set
`LLVM_MINGW` for another llvm-mingw prefix and `OUT` for another output directory).
Run the executables outside the driver package directory so they use the installed driver:

- `d3d9upload.exe` and `d3d9upload32.exe`: guard an unrelated texture's borrowed pixels during
  resource creation, then render/read back explicit and managed uploads. Exit 0 means pass.
- `shellmenu.exe 5`: open the desktop context menu and New submenu five times, checking that
  the submenu appears and Explorer stays alive. Uses English menu mnemonics, shows the
  desktop, and takes over the pointer/keyboard; leave input alone until it finishes.
- `dwmtiming.exe move 12 > move.csv`: repeat window operations for 12 QPC seconds. Modes are
  `idle`, `move`, `repaint`, `resize`, and `animate`. CSV includes tick time, timer resolution,
  requested operations and DWM timing results. Queries run on the window's UI thread:
  querying first from a worker returned `DWM_E_COMPOSITIONDISABLED` in the UTM guest.
  Check HRESULTs; the global query succeeds, while the GDI window query returns `E_PENDING`.
  Operation counts and submitted frame identifiers are not counts of displayed frames.
- `timerbench.exe counter` checks whether QPC moves backwards over twelve tick-clock seconds
  each on one logical processor, with normal scheduling, and while rotating between processors.
  It prints the counter frequency, number of regressions and largest backwards step. A backwards
  step gives exit code 1. This checks the clock error that can make Aero exit on a UTM cold boot.
- `timerbench.exe > timerbench.txt`: compare Windows' tick clock with QPC at several temporary
  timer resolutions. It releases each request and finishes with the ordinary timer. Run
  without other active programs requesting fine timers; the actual resolution is recorded.

With the HTTP guest helpers, `rb uploadcheck`, `rb uploadcheck32`, `rb menuchk` and
`rb utmtiming` fetch the corresponding executables and upload their results. `rb utmstate`
records driver hashes, compositor/Explorer processes, animation settings and recent crashes.

To measure individual minimize and restore requests, run these commands in an
elevated Windows 7 command prompt in a scratch directory with `dwmtiming.exe`.
The second provider records when the probe calls and returns from `ShowWindow`.

```bat
>providers.txt echo {8c9dd1ad-e6e5-4b07-b455-684a9d879900} 0xffffffffffffffff 5
>>providers.txt echo {6c38bba1-1768-48c6-9c07-40dadba4b38f} 0xffffffffffffffff 5
logman start RippleTransitions -ets -o transitions.etl -pf providers.txt
dwmtiming.exe transitions 23 > transitions.csv
logman stop RippleTransitions -ets
tracerpt transitions.etl -of XML -o transitions.xml
```

Each transition starts at a `minimize-start` or `restore-start` marker.
Compare it with the following `SCHEDULE_RENDER` and
`MILEVENT_MEDIA_UCE_PRESENTEVENT` start/stop events. Check that the global
timing query succeeds throughout the CSV and the compositor PID stays constant.
The probe waits at least 1.5 seconds between requests. Host delivery timing
requires a separately synchronized host display log.

For a real Explorer window, run `explorerrestore.exe` in place of the
`dwmtiming.exe` command inside the same trace session. The probe opens its
own empty temporary folder in an 800×540 Explorer window. It sends 30
minimize and restore commands, records the request markers, and verifies
the window state and Aero composition each round. It closes the window and
removes the empty folder at the end.

On the analysis machine, run `python3 tools/win7-debug/restore-timing.py transitions.xml`
on English `tracerpt` output. It reports the initial wait and the longest
subsequent gap for each run as a median, 95th percentile and maximum. Within
1.15 seconds of each restore request it counts guest presents preceded by a
render with a dirty rectangle at least 32 pixels wide and high. This removes
small updates such as the text caret. The resulting timestamps measure guest
repaints. Measuring delivery to UTM also requires the host display log.

To measure the delay from a taskbar click, use `explorerrestore.exe 50 taskbar`
in the same trace session. Leave the mouse and keyboard idle. The probe finds
the dedicated folder's taskbar button by its accessible name, clicks it, and
checks that the same window is restored and in the foreground. Use a desktop
with enough space between the restored window and the taskbar. The probe
hides its console during this test and restores the original pointer position
on exit.

To measure moving and resizing, enable **Show window contents while dragging**
and run `explorerrestore.exe 6 motion > motion.txt` inside the same ETW
session. Leave the pointer idle. Each round moves the temporary folder by
its title bar, then resizes it from the lower-right corner. The pointer
travels 216 pixels across and 108 down over three seconds each time.
The probe records the pointer command time and the observed Explorer window
bounds. It verifies that the window changed during the gesture and reached
the expected bounds before closing it.

Run `python3 tools/win7-debug/motion-timing.py motion.txt transitions.xml`
on the analysis machine. It reports pointer updates, changes in window
bounds and compositor presents separately. It trims 250 ms at the start
and 100 ms at the end of each gesture and computes gaps within each gesture.
For host display draws, add `--utm-log debug.log` and two
`--sync MARKER HOST_LOCAL_TIME` arguments. Each names a unique ETW marker
and its independently measured timestamp on the host clock, in ISO format
using the log's local time. The anchors must bracket the gestures. The
probe emits `motion-run-start` and `motion-run-stop`. A synchronization
tool can also write markers to the same ETW provider. A compositor present or host draw
can repeat the same window geometry.

For a comparison with a plain Win32 window, run `motioncontrol.exe 6 > control.txt`
in a fresh trace. It uses the same pointer path, hit tests and bounds checks.

To reproduce the white strip beside Explorer's search box, run
`explorerrestore.exe 5 search > search.txt` in the same kind of ETW session,
with Aero and full-window dragging enabled. This version grows the window by
216×108 pixels in half a second. The `search` rows contain the round, time in
milliseconds, observed right window edge and two widths in pixels. The first
width is the longest near-white run shared by two rows of glass immediately
above the search edit. The second checks the edit's top, middle and bottom
rows for a solid block across all three. Compare each width during movement
with its settled samples before and after the gesture.
Use a background that makes the strip distinguishable from the glass.
The exit status verifies the gesture and observer. The widths report the
visual outcome.

To time the search field's repaint on the same drag, use
`explorerrestore.exe 10 searchphase > searchphase.txt`. The probe tracks
when the search child reaches its final right edge and samples a 26-pixel-wide
patch around the search icon at its final location. It reads the window's
backing bitmap through its window DC and the composed desktop through the
screen DC. Use `searchpaint` in a separate run to omit the slower desktop
readback. These modes leave the existing `search` strip-width measurement
available.

For minimize and restore, run `explorerrestore.exe 10 animationpixels >
animationpixels.txt`. It samples two columns through the dedicated folder
and its taskbar button during each transition. Use a bottom taskbar with
the button inside the folder's horizontal span. Measure uninstrumented
timing separately with `taskbar` and the UTM draw log. Pixel readback
slows the guest down. On the uninstrumented taskbar trace, run
`restore-timing.py TRACE.xml` and `restore-timing.py --minimize TRACE.xml`
to get the first guest motion present and the longest gap for each direction.

Run `python3 tools/win7-debug/explorer-pixels.py FILE` on each sample file.
The resize bounds run from the beginning of the last mismatching read to
the end of the first settled read, relative to the observed child move.
A settled patch has at most 2% of pixels differing from its final state by
more than eight levels in any RGB channel. The animation count includes
samples that differ from both endpoints and from the preceding sample by
at least 2%. It measures observed changes and can miss frames between reads.

For taskbar clicks the analyzer reports when the probe first observed the
window leave its minimized state and when DWM first presented movement away
from the button. It looks for a repaint crossing at least 32×32 pixels in the
space between the taskbar and the restored window, up to 200 pixels from the
taskbar edge. It excludes 16 pixels beside the edge and accepts only renders
that started after the click. Keep the desktop otherwise quiet because DWM's
dirty rectangles describe the composited desktop. The host draw timestamp
measures delivery to UTM's display client.

Run `vblank.exe` separately to measure the driver's refresh signals. It waits
for 180 consecutive vertical blanks on the display adapter, times them with
the performance counter, and prints the observed rate, median and p95 interval.

Run `d3d9upload.exe pitch` and `d3d9upload32.exe pitch` outside the driver
package directory to verify subrectangle uploads. Each compares every pixel
after two updates from a wider, caller-owned bitmap.

Use the same executables with `bitmap` to check reuse of larger upload
buffers. The test queues full and partial updates to two 640×640 textures,
then samples each texture and compares all 1,024 output pixels, including
the border around the partial update.

`d3d9flipex.exe` and `d3d9flipex32.exe` (`guest/flipex.cmd` runs both) check that
what a program draws reaches the screen the way Windows' video renderer presents:
a Direct3D 9Ex flip-model window, or the copy model with the argument `copy`.
Each step draws one colour and then reads the screen where the window is: a
clear, a system-memory surface copied to the back buffer, a video-memory surface
filled under an ordinary lock and under a discarding one, a frame sent through
Windows' software video processor, and a frame sent through the driver's own
video processor to the middle of the window, with the background colour checked
beside it, frames in the two video formats, interlaced frames of which one
field has to show alone, and a still interlaced picture whose edge has to
stay on its row for either field. Run them outside the driver package directory too: beside the package
a 64-bit program loads the package's copy of the driver, not the installed one.

`d3d9msaa.exe [SAMPLES [full]]` and `d3d9msaa32.exe` (`guest/msaa.cmd` runs both and
sends the driver log) check multisampling: the sample counts offered, then a
triangle drawn with the most samples offered (or SAMPLES; 1 is the control) into
a render target, a window and, with "full", the whole screen, each read back and
counted for grey edge pixels.

`vidplay.exe ds|mf FILE` (`guest/vidplay.cmd` runs both, in both process sizes)
plays a video in a window of its own through DirectShow or through Media
Foundation, both ending in the Enhanced Video Renderer, and reads the screen at
64 points: how many are not black, and whether the picture moves. Give the
video's own width and height after the seconds to have it shown row for row
for a screen capture. For interlaced video the trace at level 3 says which
field each video output shows.

`guest/hdclips.cmd TAG` runs each video playback test of Windows' own assessment
alone, with the command line the full assessment uses, and sends what each
printed and its result file (frames rendered against frames expected).
`guest/weirun.cmd TAG` runs the full assessment and sends the scores, the limits
applied and the video figures from its result file.

`guest/pace.cmd TAG` records what paces the compositor while a window is dragged
and resized, and `pace.py TAG-trace.xml` summarizes it (frames presented per
second, gaps between presents, where the compositor waits). `guest/quiet.cmd`
waits until Windows' own background work after a boot has finished; measure
after it.

At trace level 3 a lock or unlock of a surface in memory also says how many of
256 points spread over it are not black, which tells an empty frame from a dark
one, and what address Windows passed with the lock.

For user-mode samples, run the program with `RIPPLE_UMD_LOG` set: the user-mode driver writes the
load addresses of the program's main modules when it is loaded (`guest/prof.cmd` does that for
one assessment test, `guest/dxone.cmd` runs one test alone). Our driver's functions are named
from its own symbol table (`llvm-nm`), Windows' from the public symbol server (`resym.py`).

## What the host does meanwhile (UTM)

When the guest is slow and its own traces show only Windows at work, look at the emulator.
`tools/win7-debug/utm-hostprof.sh SECONDS OUTDIR [INTERVAL_MS]` samples the call stacks of the
VM's QEMU process on the Mac with macOS's `sample`, in one-second reports taken back to back,
and brings them home with the display draws that UTM's debug log recorded over the same time.
Start it about 12 seconds before the workload, so that the first reports show the idle guest.
`sample` stops the process for every sample: at the default 1 ms the Explorer resize ran at a
little over half its speed, at 10 ms within a tenth of it, with the same proportions in both.
It runs as the logged-in user; if macOS refuses to let it look at the process, it says so and
stops.

`hostprof.py OUTDIR` (on the analysis machine) finds the gestures of `explorerrestore.exe N
motion` in the draw log and prints, for the drags, the resizes and the idle time: how much of
their samples each thread ran, and what the threads that run guest code were doing. The
categories are translated guest code, the emulator's helpers called from it (memory slow
path, TLB flush, lookup of translated code, device access), the emulator's own loop, and
waits for QEMU's global lock. Three lists per phase give the raw function names behind them:
the executing function, the first emulator function called from guest code, and who asked
for the global lock. A fourth lists device work that waits while it holds that lock, which
stops every guest processor that needs it.

Two findings of 2026-11-18 came from it. Windows' Meltdown mitigation made the guest write
CR3 on every system call (`docs/windows/test-vms.md`, "Windows' Meltdown mitigation on an
emulated processor"). And the ordered uploads of the compositor's vertex buffer made the
host wait for the GPU with the global lock held (`docs/windows/mesa-win7.md`, "Vertex buffer
appends on a host that waits for the GPU").

## Timing the Welcome screen

Copy `guest/welcome-trace.ps1` and the built `desktopactive.exe` to the same
local directory on the Windows 7 guest. From that directory, in an elevated
prompt under the account that will log on, run the arm command.

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File welcome-trace.ps1 arm
```

Reboot and log on to the local console. After the desktop settles, minimize
and restore an Explorer window three times as a trace control. From the
same directory and account in an elevated prompt, stop the trace.

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File welcome-trace.ps1 stop
```

The script stops the recording, removes its boot trace configuration and
logon task, and leaves `welcome.etl` beside the script. Move the ETL before
arming another run. The logon task runs at standard privilege and polls
`UOI_IO` on its own Default desktop. In the ETL, find the last
`watch-default-inactive` and first `watch-default-active` from the same
probe process. The switch falls between those samples. A run that starts
with the desktop already active cannot locate the switch. [Microsoft’s description of UOI_IO](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getuserobjectinformationw)

In the same trace, find the first Dwm-Core `MILEVENT_MEDIA_UCE_PRESENTEVENT`
start and stop on the compositor thread. Compare its duration with the
next few presents. Windows 7's classic DxgKrnl `QueuePacket` and `Flip`
events place graphics work inside that interval. Match queue starts and
stops by context and submission sequence. Dwm-Udwm event 5002 records
ordinary minimize (type 2) and restore (type 3) animation starts. Check
those controls and the trace's lost-event and lost-buffer counts before
interpreting missing events at logon. [Windows 7 graphics event layouts](https://github.com/GameTechDev/PresentMon/blob/main/PresentData/ETW/Microsoft_Windows_DxgKrnl_Win7.h)

### What the screen shows after the switch, and Windows' fade

Three small programs answer what a trace cannot. Each of the first two is
started by a task at logon; the guest scripts arm it and restart, and after
the next logon fetch the result (elevated prompt):

- `rb logonshots arm`, then `rb logonshots fetch`: `logonshots.exe` reads the
  screen 48 times from the moment the user's desktop becomes the input desktop
  and writes one sheet of reduced frames (`logonshots.bmp`, rows of six) with
  each frame's time, whether it differs from the one before, and whether the
  shell window and the taskbar exist (`logonshots.txt`). Reading the screen
  takes 80 to 150 ms a frame in UTM and makes the compositor work; take times
  from an undisturbed trace.
- `rb gammawatch arm`, then `rb gammawatch fetch`: `gammawatch.exe` polls the
  display's gamma ramp every few milliseconds for 20 s and records every
  change of its top value and the moment of the desktop switch. Windows 7
  fades out and in around the switch with the ramp, half a second each way.
- `rb fadeprobe`: `fadeprobe.exe` tells whether the installed driver takes a
  gamma ramp (it sets half brightness for 0.6 s and reads it back) and times a
  switch to an empty desktop and back. `SwitchDesktopWithFade` refuses an
  ordinary program; the plain switch is the control.
- `rb gammahold MS`: half brightness for MS milliseconds (the same program), for a look at
  the host's display meanwhile: `VMID=220 scripts/screen-vnc.sh -n 10 -i 1 OUT.png` on the
  Intel host. A capture taken inside the guest never shows a gamma ramp.
