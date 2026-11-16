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

The faulting address minus the module base on the bluescreen, plus the image base in the
module's `.map`, names the function there. `kmem.py` knows Windows 7 x64 kernel memory dumps
only (4-level page tables, the summary bitmap layout of that version).
