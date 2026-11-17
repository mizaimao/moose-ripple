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

In the guest, `guest/timerres.ps1 [SECONDS]` samples Windows' timer resolution and says how
much of the time it ran fine (under 1 ms): the kernel driver takes the half-millisecond timer
only for a stream of work, and an idle desktop should show 0%.

For user-mode samples, run the program with `RIPPLE_UMD_LOG` set: the user-mode driver writes the
load addresses of the program's main modules when it is loaded (`guest/prof.cmd` does that for
one assessment test, `guest/dxone.cmd` runs one test alone). Our driver's functions are named
from its own symbol table (`llvm-nm`), Windows' from the public symbol server (`resym.py`).
