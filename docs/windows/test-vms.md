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
