# Windows test VMs

The Windows 7 test VM on the Proxmox host, and what Windows 7 does on virtio-gpu without a
driver of ours: the baseline every later step is measured against. 2026-11-14.

## What the host has

Read only; none of it was changed.

- The owner's VMs 100 to 105: Windows 7, Vista, XP, 2000, ME and 98, each set up for the
  GeForce 9600 GT passed through (`hostpci0`, `x-vga=1`) and a USB receiver. They are the
  real-hardware reference and are never changed: tests run on clones under new VM IDs.
  VM 100 (Windows 7 Ultimate x64 SP1) is on `pc-q35-2.10`, SeaBIOS, 4 cores, 8 GB, one
  256 GB SATA disk (26.5 GB used), no snapshots.
- Install media on the host: Windows 7 SP1 x64 (an update rollup from 2020), Vista Ultimate
  SP2 x64, XP SP3 x86 and XP x64, Windows 10.
- virtio-win driver discs 0.1.285, 0.1.215 and 0.1.173. Only 0.1.285 has `viogpudo`, the
  virtio-gpu display-only driver, and only for Windows 8, 8.1, 10, 11 and Server 2012 and
  later: no `w7` folder. 0.1.215 and 0.1.173 have no virtio-gpu driver at all; their `qxl`
  folder (XDDM, Server 2008 R2) is for QEMU's QXL card, not virtio-gpu. So Windows 7 has no
  driver for virtio-gpu from virtio-win.
- QEMU `pve-qemu-kvm` 9.2.0-5, virglrenderer 0.10.4. The host has 15 GB of RAM: one 4 GB test
  VM fits next to VM 210 (8 GB).

## VM 220: `ripple-win7`

A full clone of VM 100's disk (`qm clone 100 220 --full`), with the 9600 GT, the USB receiver
and the CD removed, a new network card address, and the virgl display:

| Setting | Value |
|---|---|
| Machine | `pc-q35-2.10`, SeaBIOS, 4 cores (Penryn), 4 GB |
| Disk | SATA, `local:220/vm-220-disk-0.qcow2` |
| Display | `vga: virtio-gl`: QEMU's `virtio-vga-gl` at PCI 00:01.0, ID `1AF4:1050`, subsystem `1100:1AF4`, revision 1, class 0300 |
| Network | e1000 on the host bridge |
| Snapshots | `clone-fresh` (as cloned, before the first boot), `testsigning-on` (test mode on, the hello driver in `C:\rb`) |

The 9600 GT reference stays VM 100. Never run it while a virgl VM is drawing.

## Baseline: Windows 7 on virtio-gpu without our driver

Windows 7 Ultimate x64 SP1 (6.1.7601), DirectX 11.

- Display driver: Windows binds its own *Standard VGA Graphics Adapter* to the virtio-vga
  function (`PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01`) and reports it as working. That is
  the XDDM VGA driver, which draws through the VGA BIOS's VESA modes. dxdiag: DirectDraw,
  Direct3D and AGP acceleration "Not Available", DDI version and driver model "unknown".
  On the first boot Windows also reported that installing a driver failed, and lists two other
  devices without drivers (unrelated to the display).
- Resolutions: 800x600 at the first boot; Screen Resolution offers 800x600 up to 1920x1080
  (1024x768, 1280x720, 1280x800, 1280x1024, 1440x900, 1600x900, 1600x1200, 1680x1050 and
  others), all from the VGA BIOS.
- Aero: not available. Personalization still lists the Aero themes, but the desktop draws
  as Windows 7 Basic: no glass, no translucent taskbar, no Desktop Window Manager. Aero needs a
  WDDM driver.
- Windows Experience Index: cannot be computed. Refreshing it ends with "The Windows
  Experience Index for your system could not be computed ... an error being reported from the
  operating system, driver, or other component". The same disk's scores on record, from the
  9600 GT in VM 100 (2025-07-30), are the reference: processor 7.7, memory 7.9, graphics
  7.8, gaming graphics 7.8, disk 7.9 (base score 7.7).
- Driver signatures: KB4474419 (SHA-2 code signing support) and KB4490628 are installed,
  so the VM checks SHA-256 signatures. A test-signed kernel driver fails to start outside
  test mode (`StartService FAILED 577`, "Windows cannot verify the digital signature").

Screenshots (dxdiag, Device Manager, Personalization, the Experience Index error, the
resolution list) are kept out of the repository in `docs/re/img/win7-baseline/`, since they
show the owner's account and desktop.

## Load test, step by step

How the kernel driver load tests on VM 220 were run (docs/windows/kmd-port-notes.md, "Load
tests"). Three machines: the Linux build host, the Proxmox host that runs VM 220, and the
workstation the `scripts/` helpers run from (they reach the Proxmox host over ssh, see
`scripts/lib.sh`). Below, `REPO` is the repository's checkout on the build host and `BUILD_IP`
the build host's address on the LAN the VM is on.

### 1. The package folder on the build host

The VM fetches everything from one folder over HTTP, `/tmp/rb-pkgw/srv/pkg/` on the build host:

| File | Where it comes from |
|---|---|
| `viogpu3d.inf` | `windows/package/viogpu3d.inf`, with Windows line ends: `sed 's/$/\r/' windows/package/viogpu3d.inf > /tmp/rb-pkgw/srv/pkg/viogpu3d.inf` |
| `viogpu3d.sys` | the kernel driver build (verbose: `REPO/build/win7-kmd-verbose/amd64/viogpu3d.sys`), test-signed (below) |
| `viogpu_d3d10.dll` | `REPO/.tools/mesa/out-win7-x64-virgl/viogpu_d3d10.dll` (`VARIANT=virgl windows/mesa/build-win7.sh`) |
| `devcon.exe` | WDK 7.1: `REPO/.tools/wdk71/kit-tools/WinDDK/7600.16385.win7_wdk.100208-1538/tools/devcon/amd64/devcon.exe` |

The HTTP server, started once on the build host from `/tmp/rb-pkgw/srv` (it serves that folder,
so the files are at `http://BUILD_IP:8765/pkg/NAME`):

    cd /tmp/rb-pkgw/srv && REPO/.venv/bin/python -I -m http.server 8765 --bind 0.0.0.0

Signing: `tools/win7-hello/sign.sh FILE` signs in place with the test certificate in
`REPO/.tools/win7-testcert/` (never committed). By hand, into the package folder (osslsigncode
refuses to overwrite its output, so the old file goes first):

    cd REPO; P=/tmp/rb-pkgw/srv/pkg
    rm -f $P/viogpu3d.sys
    .tools/osslsigncode/build/osslsigncode sign -certs .tools/win7-testcert/cert.pem \
        -key .tools/win7-testcert/key.pem -h sha256 -n "moose-ripple test" \
        -in build/win7-kmd-verbose/amd64/viogpu3d.sys -out $P/viogpu3d.sys

### 2. Fresh VM and the COM1 capture (Proxmox host)

Snapshot `pre-kmd` holds the running guest (memory included), logged on at the desktop, test
mode on, `C:\rb\pkg` present, COM1 on the socket `/var/run/qemu-server/220.serial0`, automatic
restart after a bug check off (the blue screen stays). For run N:

    pkill -x socat
    qm stop 220 && qm rollback 220 pre-kmd && qm start 220
    sleep 2
    (nohup socat -u UNIX-CONNECT:/var/run/qemu-server/220.serial0 \
        OPEN:/tmp/rb-220-com1-runN.log,creat,append >/dev/null 2>&1 &)

The socket exists only while the VM runs, so socat starts again after every `qm start`. It
stays connected across the guest's own reboots. Use `pkill -x socat`: `pkill -f` with a
pattern also matches the ssh command that runs it and kills that.

### 3. In the guest (from the workstation)

The screen is 800x600 until our driver takes it; the click positions are absolute QMP
coordinates (0 to 32767) for that size. After the rollback the first key is sometimes lost:
take a screenshot and press again.

    export VMID=220
    scripts/qmp-input.sh "key meta_l" "sleep 2"          # Start menu (check with screen.sh)
    scripts/type.sh -n 'powershell -command "Start-Process cmd -Verb RunAs"'
    scripts/screen.sh /tmp/w7.png                        # the UAC prompt takes a few seconds
    scripts/qmp-input.sh "click 20012 16465"             # UAC: Yes
    scripts/type.sh -n 'cd /d C:\rb\pkg & for %f in (viogpu3d.inf viogpu3d.sys viogpu_d3d10.dll devcon.exe) do @bitsadmin /transfer rN%f /download /priority foreground http://BUILD_IP:8765/pkg/%f C:\rb\pkg\%f | findstr /i "complete error"'
    # wait for four "Transfer complete." lines (bitsadmin takes 10 to 30 s; keystrokes typed
    # meanwhile are lost)
    scripts/type.sh -n 'devcon update viogpu3d.inf "PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01"'
    scripts/qmp-input.sh "click 13943 15700"             # "Install this driver software anyway"
    # wait for "Drivers installed successfully."
    scripts/type.sh -n 'shutdown /r /t 0'

`scripts/type.sh -n` types the line and presses Enter. To check that the guest has the new
driver file: `certutil -hashfile viogpu3d.sys SHA256` against `sha256sum` on the build host.

After the reboot: `wc -l /tmp/rb-220-com1-runN.log` on the Proxmox host shows the driver
starting within about 40 s. With a desktop, the same elevated cmd gives `devcon status
"PCI\VEN_1AF4&DEV_1050*"` (problem code), `devcon resources ...`, `sc query VgaSave`, and
`wevtutil qe System /c:8 /rd:true /f:text` for the event log. When the driver has the screen
and nothing is drawn, QEMU shows "Display output is not active." and only the COM1 log tells
what happens.

### Files out of the guest

Windows 7's PowerShell 2.0 can PUT a file to a one-off receiver on the build host (any small
HTTP server that writes the request body to a file, run from `REPO/.venv`):

    powershell -command "$w=New-Object Net.WebClient; $r=$w.UploadData('http://BUILD_IP:8766/NAME','PUT',[IO.File]::ReadAllBytes('C:\path\NAME'))"

and `certutil -hashfile C:\path\NAME SHA256` in the guest checks the copy.

## Clean-install check of a release package (2026-11-26)

How release 0.4.0 of the Vista and Windows 7 driver was checked on systems that had never had
the driver, a test certificate or test mode. On the Proxmox host, in a folder of its own
(its `/tmp` is emptied at a restart):

- `tools/win7-debug/rig/cleanvm.sh vista|win7 prep|boot|reset|kill` starts a throwaway
  guest as a QEMU process of its own, with the clock and CPU options Proxmox gives its Windows
  VMs, `virtio-vga-gl`, a USB tablet for clicks, COM1 in a file, QMP and VNC on sockets and no
  network. Vista runs in an overlay over VM 101's disk, Windows 7 in an overlay over a copy
  of VM 220's snapshot `clone-fresh`; neither VM is touched. It refuses to start while
  another guest runs. A guest started this way does not show in `qm list`.
- `rel.iso` next to it is the guest's CD: the unpacked release archive plus a `tests` folder
  with `tools/win7-debug/guest/relcheck.cmd`, `tools/vista/pixeltests.cmd` and the six
  pixel-test programs (`genisoimage -J -r -o rel.iso cd`).
- `rig/guestctl.pl OS key|type|line|run|click|shot ...` presses keys, types, clicks and saves
  the screen. Once the driver draws through the host GPU QEMU has no picture to save; read
  the VNC socket then (`tools/vncgrab/vncgrab.py`, which also takes a server without a
  password).

The check itself follows `docs/windows/install.md` by hand, a screenshot after each step:
an administrator Command Prompt from the Start menu (type `cmd`, Ctrl+Shift+Enter, Alt+C on
Vista or Alt+Y on Windows 7 for the permission question), `D:\tests\relcheck.cmd before`,
`bcdedit /set testsigning on`, restart, the install through Device Manager (Alt+N for Next,
Alt+I for "Install this driver software anyway"), restart, `relcheck.cmd after` and
`pixeltests.cmd`, the uninstall, restart, `relcheck.cmd removed`. Wait for a script's end
marker on COM1 before typing the next line: keys typed while a full-screen test has the
keyboard are lost. One more guest installs with test mode left off, to see the driver refused
(code 52) and the desktop stay on VGA.

## Reference pictures on the GeForce 9600 GT

**One boot of the card per power-on of the host, and a restart of the host after every card
test.** Starting a VM with the card has frozen the host four times (2026-11-10, 2026-11-14 and
twice on 2026-11-26), a hard reset each time: once with a virgl VM running beside it, once
seconds after a virgl VM had been stopped, and twice at a second card boot with no restart of
the host in between. The first card boot after a restart has always worked, also when virgl
guests had run and been stopped ten minutes before. Run nothing else on the host meanwhile,
keep its kernel log streaming to another machine, and turn off the automatic restart after a
crash in the guest, so that a crashing guest stops instead of resetting the card again.

For the side-by-side pictures of a milestone (first used for Aero, 2026-11-15). VM 220's disk
still has NVIDIA's driver from the machine it was cloned from, so the reference is VM 220
itself, started from its snapshot with the card in place of the virtio display. The card and a
virgl VM never run at the same time, and the host has hung once when a card VM started shortly
after a virgl VM stopped: stop everything, restart the host first, and keep its kernel log
streaming to another machine while the card VM starts.

    qm stop 220; reboot                         # the Proxmox host, with no VM running
    qm rollback 220 pre-kmd
    qm set 220 --hostpci0 0000:01:00,pcie=1,x-vga=1,romfile=9600gt-tpu.bin --delete vga
    qm start 220

`qm set` changes only the current settings; the snapshots keep theirs, so `qm rollback 220
pre-kmd` afterwards brings the virtio display back. QEMU has no picture of this guest (no
`screendump`, no VNC); keys and the pointer still go in through QMP. Pictures are taken inside
Windows with `tools/win7-debug/shot.ps1`, which sends them to the receiver described above:
Win+R, `cmd`, then in `C:\rb\pkg` (fetch `shot.ps1` and `setres.ps1` there with bitsadmin):

    powershell -ExecutionPolicy Bypass -File setres.ps1 1024 768
    start /min powershell -WindowStyle Hidden -ExecutionPolicy Bypass -File shot.ps1 http://BUILD_IP:8766 desktop 12

Several of these with growing delays, then `exit`, and the scenes are set up from outside in
the gaps (Win+E for a window, the pointer on a taskbar button for a thumbnail, Ctrl+Win+Tab
for Flip 3D, the pointer on the right end of the taskbar for desktop peek). Shut Windows down
from the Run box (`shutdown /s /t 0`), roll back, and run a load test to return to our driver.

## 3DMark06 against the GeForce 9600 GT, like for like (2026-11-26)

How far our driver on the host's Intel GPU is from a real card of the period, with everything
else the same: one Windows 7 disk, the CPU model, four cores and 4 GB that Proxmox gives VM
100, and 3DMark06's default run (1280x1024, no anti-aliasing), once on the virtio GPU and once
on the card.

| 3DMark06 1.2.1, default run | 3DMarks | SM2.0 score | HDR/SM3.0 score | CPU score |
| --- | --- | --- | --- | --- |
| release 0.4.0 on the host's Intel UHD 630 (virgl) | 10881 | 3480 | 4554 | 8261 |
| GeForce 9600 GT, NVIDIA's driver 341.92 | 9655 | 4165 | 2892 | 8317 |

The SM2.0 score is 120 times the mean frame rate of the first two graphics tests, the HDR/SM3.0
score 100 times that of the last two: 29.0 against 34.7 frames a second in the first pair
(84% of the card), 45.5 against 28.9 in the second (157%). The CPU scores agree, as they must.
One run each. During the virgl run the host GPU's render engine was 85 to 95% busy in the
first test, about 50% in the second (Firefly Forest), and 75 to 90% in the last two.

The same prepared disk and guest shape on a second host with the same GPU model but QEMU 10.0
and virglrenderer 1.1 (the first has QEMU 9.2 and virglrenderer 0.10.4) scored 10728 3DMarks
(SM2.0 3618, HDR/SM3.0 4267, CPU 8604): a baseline of that host's own, not to be mixed with
the table.

The run is unattended, because the card's picture cannot be seen from the host:

- `tools/win7-debug/rig/refvm.sh` starts the guest as a private QEMU, with the virtio GPU or
  with the card as VM 100 has it, on a throwaway overlay over one prepared disk. No VM
  configuration changes. `busylog.sh` beside it logs the GPU's busy share and QEMU's threads.
- The prepared disk: test mode, the driver, 3DMark06, and `tools/win7-bench/setup.cmd` run once
  elevated. That makes a second disk the volume R: (results, crash dumps), turns off the
  automatic restart after a crash, and adds a logon task that runs `auto.cmd` from the CD's
  `3dm` folder. `auto.cmd` does nothing unless `go.txt` there names a run; it then runs
  `3dmfull.ps1` once for that name (start, press Run, a mark at each change of the full-screen
  size, the score window's text, a 50-minute cap; on Vista `3dmfull.cmd`, the same in a batch
  file, because Vista's PowerShell 1.0 does not run the script), writes everything to the first serial port
  and to R:, and shuts Windows down. The helpers it uses are the `bench-*.c` programs in the
  same folder (built with llvm-mingw like the other test programs).
- 3DMark06's free edition shows no score without the vendor's web site. With `key.txt` (a
  registration code, one line) beside the scripts, `bench-reg.exe` enters it through the
  program's own dialog; it prints window titles only. Keep that file off any shared medium
  and delete it with the CD image afterwards.
- A disk prepared on another machine type has to settle first: Windows sees the virtio GPU as
  a new device there and gives it the Standard VGA driver, because a test-signed driver loses
  the automatic ranking against a signed one. Boot it once with the virtio GPU, install our
  driver again through Device Manager (Have Disk, from a copy of the driver store folder),
  restart, shut down, and take the overlays from that.

The order that worked: the virgl run first, then ten minutes with no guest on the host, then
the card's one boot (kernel log streaming to another machine), then a restart of the host.

### Quiet runs and disturbed runs

Of four runs of this guest on the virtio GPU on this host, two took 40% longer than the other
two, with the same disk, settings and commands:

| run | the two SM2.0 tests | the CPU tests | the two HDR tests |
| --- | --- | --- | --- |
| quiet (the virgl run of the table, a later one, and the card run) | 183 to 193 s | 41 to 44 s | 165 to 167 s |
| disturbed (two runs) | 268 and 272 s | 105 and 124 s | 220 and 226 s |

The graphics tests last a fixed time, so their parts differ by the loading between them (45
seconds without GPU work in a disturbed run, none visible in a quiet one); the CPU tests draw a
fixed number of frames and took two and a half times as long. During loading and the CPU tests
of a disturbed run the guest's processors left guest mode 58,000 to 136,000 times a second for
a device register (the kernel's `kvm_exit` event, reason EPT_MISCONFIG; 200 to 9,000 in a
quiet run) and were 30 to 50% busy instead of all four at full load. Which device was not
recorded; the disk controller is the likely one, with something else in the guest reading the
disk at the same time. `auto.cmd` now ends with every guest process's processor time and disk
reads, to name it at the next such run.

Take a score only from a quiet run: the lengths of the parts are in the marks. The two runs of
the table are of the quiet kind, and their parts agree within ten seconds.

### What QEMU's main thread does during a run

virglrenderer draws on QEMU's main thread. `tools/win7-debug/rig/maintrace.sh` traces one
thread with the kernel's own events (scheduler switches and system calls, nothing to install)
and says how much of the time it was on a processor and which system calls took the rest;
`kvmexits.sh` counts the guest's exits by reason; `prof.sh` runs both, over and over, for as
long as a guest lives.

| | on a processor | waiting in `ppoll` | in `DRM_IOCTL_SYNCOBJ_WAIT` |
| --- | --- | --- | --- |
| desktop, nothing drawing | 2% | 98% (woken about 2,000 times a second) | 1% |
| first SM2.0 test (GPU 85 to 95% busy), two runs | 27% and 59% | 27% and 17% | 47% in the run where it was split out (13,450 waits in ten seconds, 0.35 ms each) |
| first HDR test (GPU 80 to 90% busy) | 31% | 15% | 56% |
| loading between two tests (no GPU work), two samples | 12% and 25% | 83% and 92% | 6% and none |

In the tests where the GPU is nearly always busy the thread spends half of its time waiting
for the GPU, as it should: the GPU is the limit there. Firefly Forest, the test where the GPU
is only half busy, was missed twice by samples taken at fixed times into the run (they fell
into loading and into the CPU tests), which is why `prof.sh` now samples all the time.
(A `top -H -b -n1` taken every few seconds had shown this thread at 100% in every test, the
CPU tests too. That first sample of `top` is no measure.)

## Crash dumps

When a Windows program crashes inside our user-mode driver (or inside Direct3D because of
it), the Application event log names the module and the offset
(`wevtutil qe Application /c:2 /rd:true /f:text /q:"*[System[(EventID=1000)]]"`), which is
enough to name the function: the DLLs built here keep their symbol table. For the callers,
have Windows write a dump. In the guest, elevated:

    reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting" /v DontShowUI /t REG_DWORD /d 1 /f
    reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpFolder /t REG_EXPAND_SZ /d C:\rb\dumps /f
    reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpType /t REG_DWORD /d 1 /f

The first keeps the "has stopped working" dialog from waiting for a click. After the crash,
send the newest file of `C:\rb\dumps` to the build host ("Files out of the guest") and, next to
a copy of exactly the DLL the guest had:

    PY tools/win7-debug/dumpstack.py crash.dmp viogpu_d3d10.dll

It prints the exception, the registers and every word of the crashed thread's stack that
points into the DLL, named, innermost first. Stale words are in the list too; the real chain
is usually plain to see.

## Scripts in the guest

Typing long command lines into the guest through QMP is slow and breaks easily. The small
batch files in `tools/win7-debug/guest/` make one short line enough: copy them into the
package folder on the build host next to the driver files, and the load test fetches them
into `C:\rb\pkg` and sets two variables for them in the guest, `RB_GET` (the package folder's
URL) and `RB_PUT` (the receiver of "Files out of the guest").

- `rb NAME [ARGS]` fetches `NAME.cmd` from the package folder and runs it. A test is then a
  batch file written on the build host and one line typed in the guest.
- `get FILE...` and `put FILE...` fetch files and send results back.
- `inst` replaces the user-mode driver in place (the compositor is stopped around it).
- `prep` makes a crashing program leave a dump and no dialog ("Crash dumps").
- `wei` runs Windows' full assessment with the driver's log and sends back the results.
- `dx` runs only the assessment's Direct3D tests, one after the other, into `dx.txt` (about
  80 seconds; the full assessment also measures the processor and disk again after a driver
  change).
- `kinst` replaces the kernel driver in place and restarts the display device, without a
  reboot (in an elevated prompt). Direct3D programs that are open lose their device.
- `chk` runs the 32-bit and 64-bit checks and dxdiag after a fresh install and sends the
  results back.
- `dxone 10 Batch 1280 1024` runs one assessment test alone (12 s); `prof` the same with the
  module addresses for a profile (`tools/win7-debug/README.md`).
- `wmp` plays the sample video for a few seconds with the Direct3D 9 trace at level 3 and
  sends the trace back (`umd-wmp.log`).

In `cmd`, `for %f in (..) do A & B` repeats B as well; put the loop in parentheses.

## On Hog (2026-11-15)

Hog is a Windows PC whose WSL2 distro `moose-ripple` runs QEMU 8.2 with KVM, and
virglrenderer 1.0 on Mesa's Direct3D 12 driver over the PC's NVIDIA GeForce RTX 4090. The test
VM there, `/root/win7`, is the rig's VM 220 at its `pre-kmd` snapshot, exported with
`qemu-img convert -l snapshot.name=pre-kmd -c` (14 GB) and copied through the build host's
package server. Its `start.sh` lays the machine out like VM 220 (Proxmox's `pve-q35.cfg`, the
same machine type, CPU, SMBIOS UUID and network card address), so Windows finds the same
devices and stays activated; the network is QEMU's user network, which reaches the build host.

`tools/win7-debug/hog.sh` drives it from the workstation: `start` (keep it running in the
background: WSL stops the distro when no session is left), `stop`, `type`, `key`, `click`,
`mon`, and `shot`, which works only until our driver owns the display (QEMU then has "no
surface" to dump): after that, pictures come from inside Windows (`shot.ps1`). The distro has
no socat; the helper uses netcat. The package is installed by hand as `loadtest.sh` does it:
an elevated prompt, the files fetched with bitsadmin, `devcon update`, "Install this driver
software anyway", a restart.

## In UTM (2026-11-15, first try)

The same disk as on Hog, in a UTM VM `ripple-win7` on macserver (x86-64 emulated on Apple
silicon, 4 processors, 4 GB, SeaBIOS, the network card's address and SMBIOS UUID of VM 220, a
PS/2 keyboard for typed input, QMP at `rb-qmp-w7.sock` in UTM's group container).
`tools/win7-debug/utm.sh` drives it like `hog.sh`. Windows boots (slowly: emulated) and the
package installs, but with our driver the screen is violet with garbage and in-guest pictures
come out black. UTM's host renderer is virglrenderer over ANGLE on Metal, and it reports GLSL
1.30 only (`[VREND CAPS] FINAL VALUES: glsl_level=130, max_samples=1` in the VM's debug log):
no geometry shaders, nothing Direct3D 10 needs, while the compositor still gets a Direct3D 10
device from us. Being worked on: the driver has to offer less on such a host.

The driver now refuses Direct3D 10 devices on such a host (Mesa patch 0028; virgl still reports
geometry shaders there, so the check is on the GLSL level). The compositor then does without and
the desktop comes up plainly (Windows Basic, 1280x1024 by UTM's resizing display). Aero on such a
host would need the compositor's feature level 9 path, which fails for now (the probe gets "no
interface" for levels 9.1-9.3).

A trap found on the way: programs started from `C:\rb\pkg` load the user-mode driver from that
folder (Windows looks in the program's folder first), not the one installed in System32. Run
test programs from another folder when the package folder holds a different build.

Second try (2026-11-16), with the kernel driver of "Speed, third round"
(`docs/windows/kmd-port-notes.md`): the desktop comes up in under two minutes from start,
Windows Basic as before. Chess Titans stopped at "Loading": the host renderer refused its
textures (`Unknown format is 106` in the VM's debug log, then the context dropped). Format 106 is
DXT1; UTM's renderer offers the guest no DXT formats (ANGLE has them, under names that
virglrenderer does not look for: `mesa-win7.md`, "UTM's host could take DXT textures"). The
Direct3D 9 driver now asks the host for the DXT formats when a device is made, and where they
are missing keeps DXT textures as R8G8B8A8 and decompresses them on upload (the program still
sees DXT), since 2026-11-25 with a decoder of its own (`mesa-win7.md`, "A game in UTM: where
the time goes"). Chess Titans then draws right, marble and reflections. Still open there: a
DXT texture the program locks directly in video memory (rare; textures normally arrive by the
runtime's upload) is mapped as R8G8B8A8, not converted.

Third try (2026-11-17): Aero. The compositor runs on the driver's Direct3D 9 part there, after
six changes (`mesa-win7.md`, "Aero on a host without Direct3D 10"): glass frames, taskbar and
shadows, and Chess Titans in a glass frame. The emulated machine is slow to start programs
(Chess Titans needs more than 12 seconds), and a window opened while the compositor restarts
can lose the keyboard: click it before typing.

Seeing the guest: QEMU's screendump stays black once the driver draws through the host GPU, so
pictures come from `C:\rb\pkg\shot.ps1` in the guest (as on Hog).

### Keeping Aero active on cold boots

Replaced on 2026-11-24 by the arguments in [Guest clock on an emulated
processor](#guest-clock-on-an-emulated-processor): with `hpet=on` the Windows
tick clock runs slow under load, and with those arguments the counter fault
described here did not come back. This section stays as the record of that
fault and of the check for it.

On the x86-64 Q35 test VM on Apple silicon, add `-machine hpet=on` to the
VM's additional QEMU arguments while it is stopped, then start it. UTM
supplied `hpet=off` in its generated machine arguments. With HPET off,
one cold boot used a 3,579,545 Hz Windows performance counter that jumped
backwards at the 24-bit wrap. The jump was about 4.687 seconds even on
a single CPU. Desktop Window Manager logged Application event 9020 with
`0x8898009b` and exited. Restarting the desktop composition service on
that boot hit the same error.

With HPET enabled, Windows detected ACPI device `PNP0103` and used a
100,000,000 Hz counter on three cold boots. On each boot the counter stayed
monotonic on one processor, with normal scheduling and while rotating between
all four processors. On the final boot DWM stayed running through the
timer-resolution sweep and Explorer opened the **New** submenu five times
in the same process. Run `timerbench.exe counter` to repeat the clock check.
The toolbar repaint can still appear with Aero running. [Paired resize captures](mesa-win7.md#white-strip-beside-the-search-box)

### Guest clock on an emulated processor

With `-machine hpet=on` the Windows tick clock of these emulated VMs ran slow
whenever a program asked for a fine timer, as games, 3DMark06 and video
players do (1 ms). Windows then takes its ticks from the HPET, and QEMU's HPET
skips every periodic tick it cannot deliver on time. In a 3DMark06 run on the
Vista guest the tick clock moved 58% to 66% of real time. Whatever a program
times by that clock ran that much slower, and 3DMark06, which counts its
frames against tick time, reported scores about 1.7 times too high.

Since 2026-11-24 the Windows 7 and the Vista VM have these additional QEMU
arguments instead of `-machine hpet=on`:

    -machine hpet=off
    -rtc driftfix=slew

Without an HPET, Windows ticks from the RTC, and with `driftfix=slew` QEMU
delivers the RTC ticks it was late with afterwards. Both arguments are
needed: UTM's own default, `hpet=off` without the RTC option, loses ticks as
the HPET does (on the Vista guest the tick clock ran at 63% of real time with
a 1 ms timer and at 33% with 0.5 ms).

On the Vista guest (driver 7.14.1.20, a disposable session each, 1280x1024,
default suite):

| | `hpet=on` | `hpet=off`, `driftfix=slew` |
| --- | ---: | ---: |
| Tick clock against real time during 3DMark06 | 58% to 66% | 98% to 101% |
| 3DMark06 | 2,117 | 1,182 |
| Return to Proxycon (FPS) | 7.33 | 4.41 |
| Firefly Forest | 4.03 | 2.37 |
| Red Valley CPU 1 | 0.195 | 0.117 |
| Red Valley CPU 2 | 0.209 | 0.131 |
| Canyon Flight | 15.93 | 7.52 |
| Deep Freeze | 8.89 | 5.19 |
| From the Run click to the score | about 42 min | 37 min |

The right column is the real speed; the left one is the same speed divided by
a slow clock. The run stays long because the two processor tests draw one
frame in about eight seconds on the emulated processor (the Intel test
machine draws 2.5 to 3.5 a second).

The new arguments cost little or nothing. `timerbench.exe` without an
argument compares the tick clock with the performance counter at each timer
rate, and `timerbench.exe cost` runs the same arithmetic on all four
processors at each rate and then counts reads of the counter:

| | `hpet=on` | `hpet=off`, `driftfix=slew` |
| --- | ---: | ---: |
| Windows 7 tick clock, 1 ms timer | 72% | 100% |
| Windows 7 tick clock, 0.5 ms timer | 37% | 100% |
| Vista tick clock, 1 ms timer | 65% | 99% to 100% |
| Vista tick clock, 0.5 ms timer | 34% | 91% to 94% |
| Windows 7 work a second, 1 ms timer (billions) | 1.48 | 1.41 to 1.51 |
| Vista work a second, 1 ms timer (billions) | 1.28 | 1.28 |
| Windows 7 counter reads a second, one thread (millions) | 1.3 | 1.6 to 2.2 |
| Vista counter reads a second, one thread (millions) | 0.6 to 1.1 | 1.3 to 1.4 |

At a 0.5 ms timer, where every one of 2,048 ticks a second now arrives, the
work a second was 3% to 12% lower than at the default rate on Windows 7 and
4% lower than with `hpet=on` on Vista. On Vista the 0.5 ms rate stayed in
force after the test released it, with either set of arguments; with
`hpet=on` the tick clock then ran at a third of real time in the two test
runs that followed.

`hpet=on` had been added for the counter fault of the section above, so that
check was repeated with the new arguments. `timerbench.exe counter` found a
3,579,545 Hz counter that never stepped backwards, pinned to one processor,
with normal scheduling and rotating between all four, on Vista and on three
Windows 7 cold boots. On those three the compositor was running and no new
event 9020 was logged. If the fault returns, that check and the event log
show it.

A guest that keeps real time also notices that nobody touches it. A 3DMark06
run takes over half an hour here, and the test scripts send no keyboard or
mouse input while it runs. The Vista guest has a screen saver with a wait of
ten minutes. With the clock right it started while 3DMark06 was on the
desktop between its two processor tests, 22 minutes after the Run click in
three runs of three. 3DMark06 lost the screen, showed a message box and ended
the run without a score, which from outside looked like a benchmark that had
hung. The slow clock had hidden this. Before a long unattended run, in the
guest (the two `powercfg` lines elevated):

    keep-awake.exe
    powercfg -change -monitor-timeout-ac 0
    powercfg -change -standby-timeout-ac 0

`tools/vista/keep-awake.c` switches the screen saver off for the logon
session and leaves the saved setting alone. Scheduled jobs come due as well:
a Windows Defender scan started in the middle of one run.

UTM's **Use TSO** switch does not apply to these VMs. It works only with the
hypervisor, which an x86 guest on Apple silicon cannot use. The VMs' debug
log is not a cost either: about 28 lines a second during a 3DMark06 run.

### Windows' Meltdown mitigation on an emulated processor

On a host that emulates the processor (UTM on Apple silicon), switch Windows' Meltdown
mitigation off in the test guest. With it on, Windows 7 keeps separate page tables for user and
kernel mode (kernel page-table isolation, "KVA shadow") and writes the CR3 register on the way
into and out of every system call and interrupt. QEMU's emulator empties its address
translation cache and its jump cache on every CR3 write, so the guest's processors then spend
more time refilling those than running Windows' code. Windows turns the isolation on here
because the VM's CPU model, `Penryn` with vendor `GenuineIntel`, counts as affected.

Microsoft's switch (KB4072699), in an elevated prompt, then a restart:

    reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverride /t REG_DWORD /d 3 /f
    reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverrideMask /t REG_DWORD /d 3 /f

`rb kvasoff` does both and restarts, `rb kvason` removes the two values again, and `rb kvas`
reports the state: bit 0 of the first value is set while the isolation is on (`0x2b73` on,
`0x2b70` off in this guest). The same two values also switch off the Spectre variant 2
mitigation.

Measured on 2026-11-18 on the M1 Max with the Explorer gestures of `explorerrestore.exe N
motion` (1280x1024, Aero, windows drawn while dragging; same driver, one boot each): a
three-second corner resize went from 17 to 18 distinct window sizes to 39 to 45, and the
title-bar drag stayed at 155 to 170 positions. How it was found: `tools/win7-debug/README.md`,
"What the host does meanwhile".

This is for test guests on an emulated processor. A guest that runs on the host's own
processor (VM 220, Hog) keeps Windows' default: there the switch of page tables is cheap.
