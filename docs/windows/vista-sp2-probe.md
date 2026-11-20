# Vista SP2 x64 display driver probe

The kernel driver and both Direct3D 9 DLLs build from the Windows 7 driver at
`63739fe`. On Vista SP2 x64 with test signing enabled, version 7.14.1.6
starts the adapter, runs the desktop at 1280×768 and draws the Direct3D 9
triangle test correctly for both x64 and x86 applications. The guest reports
Aero composition on. Earlier builds read every GPU result back as black;
[Why the GPU's data stayed invisible](#why-the-gpus-data-stayed-invisible)
has the cause and the fix.

## What changed

- The kernel driver targets NT 6.0 SP2 and links with the Vista display library
  from WDK 7.1. It registers the Vista SP1 graphics interface (0x1053, also
  used for SP2). Vista's callback table ends before the Windows 7 kernel GDI
  rendering and display-path capability callbacks.
- Direct3D 9 reports the Vista interface (0x000c). Its function table has
  Vista's size, including when tracing is enabled. The Windows 7 video and
  shared-resource callbacks following the end of that table are omitted.
- Both Windows 7 graphics DLLs imported `TryAcquireSRWLockExclusive`, which
  this Vista image lacks. The reference was in an unused function in LLVM's
  static C++ runtime. Discarding unused sections removes it. The x86 unwind
  library also used `K32EnumProcessModules`. The x86 Vista build calls
  `PSAPI!EnumProcessModules` for that operation.
- The Vista INF registers the Direct3D 9 driver for native x64 and 32-bit
  applications. The DLL keeps the existing filename `viogpu_d3d10.dll`.

## Verified on disk

Reference image: Vista Ultimate SP2 x64, updated media, kernel file version
`6.0.6002.24170 (vistasp2_ldr.170721-0600)`. The SHA-256 of the ISO was
`3e90124acee5a806b0f5018abf2c8a963cf348bce99267b4c5404763ecabd35c`.
The audit used System32 for
native x64 and SysWOW64 for x86, following forwarded exports in both cases.
It also checked the PE architecture, subsystem version 6.0 and both DLLs'
Direct3D 9 `OpenAdapter` export.

| Binary | Imports | Additional GDI functions looked up at runtime | Unresolved |
| --- | ---: | ---: | ---: |
| viogpu3d.sys (x64) | 69 | 0 | 0 |
| viogpu_d3d10.dll (x64) | 127 | 16 | 0 |
| viogpu_d3d10.x86.dll (x86) | 132 | 16 | 0 |

The default Windows 7 kernel driver and x64 and x86 Mesa targets also rebuilt
in a separate build directory.

### UTM test guest

An independent copy of an existing Vista installation booted in UTM with a
native x64 kernel, file version `6.0.6003.20555`. The guest's SetupAPI log
recorded `PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01` and selected the
built-in `display.inf` VGA driver. Windows accepted a normal shutdown, and a
powered-off snapshot preserves this state. A command launched from the guest
desktop returned its output on COM1.

The same three Ripple binaries were checked again against the native and
WOW64 files in that guest. All 328 static imports resolved, including forwarded
exports, as did the 16 GDI lookups in each DLL.

### First signed run on Vista

Vista enabled test signing and restarted. WDK 7.1 SignTool signed the kernel
binary in the guest. Inf2Cat created the Vista x64 catalog, SignTool signed it,
and Windows verified all four catalog members. DevCon installed 7.14.1.1 on
the virtio display device and reported that the driver was running. After the
next boot the kernel driver again enumerated the GPU's capabilities and the
monitor's modes. The shared primary and shadow surface requests repeated at
800×600 and 640×480. Each attempt ended with device teardown.

Both the x64 and x86 probes obtained a Direct3D 9 object. On both,
`Direct3DCreate9Ex` returned `0x8876086a` and both hardware-device creation
attempts returned `0x8876086c`. Neither reached the triangle. The desktop
composition query succeeded and reported composition off.

### CPU-visible desktop surface

Version 7.14.1.2 places the shared desktop primary in a second memory segment
backed by contiguous guest RAM. Other allocations keep using the virtio
aperture. Vista reserved 64 MiB on the test VM and requested 1,920,000 bytes
for its 800×600 desktop. It set this mode during installation and again after
reboot. The x64 and x86 probes identified Ripple, created a Direct3D 9Ex
object and a hardware device, and submitted a clear, triangle and present.
Both pixel checks returned black at the red and green sample positions.

A separate offscreen test preserved `123456` in a CPU-only surface. On a
lockable render target, a clear to `2060a0` and a fill to `cb4080` both read
back as zero, through a direct lock and through a copy to system memory.
The host trace recorded virtio GPU context submissions during the run.
The x64 triangle also read back black with transfer optimizations disabled
(`VIRGL_DEBUG=xfer`), with synchronous submissions (`VIRGL_DEBUG=sync`) and
with coherent resources disabled (`VIRGL_DEBUG=nocoherent`). A pitched
texture upload returned 2,048 mismatches across 2,048 pixels. The event query
signaled for each of five occlusion tests, but all five visibility queries
timed out. The revised kernel binary imports 71 functions.

The UTM display remained blank while desktop commands still returned results
on the guest serial port. In an independent test using the Windows graphics
kernel interface directly, Ripple signaled completion for the upload and
download of a private texture. After clearing the guest copy between the two
transfers, zero of 1,024 bytes matched the uploaded pattern. A separate Mesa
buffer copy returned zero of 4,096 matching bytes. The diagnostic then exited
with an access violation before reporting its two optional color-format tests.

In the saved offscreen color trace, QEMU attached both 64×64 test textures
(resource IDs 0xc and 0x9) and their guest backing before the first command
submission for that test context. The trace records the attach requests but
does not include the bytes in those pages. In the separate texture round-trip
run, the serial log contained no GPU command error between the probe's begin
and end markers. Both transfers signaled completion and the first byte read
back as 0x00. The expected byte was 0x1e.

The display thread also treated address zero as an absent desktop. Zero is a
valid offset in Vista's dedicated desktop segment. A follow-up build accepts
this offset when Windows identifies that segment and supplies a primary
allocation. The change compiled for Vista and Windows 7; its effect on the
visible desktop has not been verified.

For recovery, Vista's VGA boot option alone kept Ripple selected at 640×480.
Rebinding the PCI adapter to Microsoft's built-in `display.inf` selected
`vgapnp.sys`. After reboot, Vista reported Standard VGA Graphics Adapter
at 640×480 in 32-bit color, Explorer running and the Ripple service stopped.
The test-signed package remains available for the next driver run.

### Tagged texture run

On November 20, Vista read version 7.14.1.3 from its removable DVD drive.
After installation and reboot, the active display was Ripple at 1280×768
in 32-bit color, Explorer was running, and the installed kernel binary
matched the signed package byte for byte. The 32-bit and 64-bit triangle
tests both read black at the two sample points. Desktop composition was off.
The guest reported its display mode over the serial port. Visual inspection
of this build is still pending.

For the 16×16 test texture (resource 9), the kernel logged
`mapped=1 match=32/32` when it attached the backing pages. QEMU recorded
creation, context attachment, backing attachment, upload, download, and
then teardown of that resource. Both transfers signaled completion. The
application recovered 0 of 1,024 bytes, starting with 0x00 where it had
written 0x1e. The independent buffer copy recovered 0 of 4,096 bytes.
The next check is to sample the same backing after download completion
and compare it with the application's read before it releases the texture.

### Why the GPU's data stayed invisible

Two Vista rules, each found with the direct kernel-interface test in
`pipecheck.c`, explain the black pixels. Windows 7 needs neither.

1. A CPU pointer returned by a lock does not show data the GPU writes after
   that lock. The test wrote its pattern, uploaded it, overwrote the guest
   copy, downloaded and read through the same lock: 0 of 1,024 bytes
   matched. Unlocking and locking again before each step: 1,024 of 1,024.
2. A fresh lock shows the GPU's data only when the download's submission
   marks the allocation as written (`WriteOperation` in the allocation
   list). With the mark: 1,024 of 1,024. Without it, relocking as in rule 1:
   0 of 1,024, every byte still the sentinel.

Mesa's GDI winsys broke both rules. It kept one lock for an allocation's
whole life, and its downloads never set the write mark. Two Vista-only Mesa
patches follow the rules: `0101` takes a lock per CPU access and releases it
at unmap (the staging pool, which holds a lock for its lifetime, is turned
off), and `0102` marks a download's allocation as written. With both,
Mesa's GPU buffer copy reads back 4,096 of 4,096 bytes; before them it read
0 of 4,096, also 500 ms later, so the cause was not timing.

### 7.14.1.5 run

On November 20 the guest downloaded package 7.14.1.5 (the `169f188` kernel
driver and Mesa with both patches) from the UTM host over HTTP, verified
each file's SHA-1, signed it in the guest and installed it. After the
reboot, Ripple was the active display at 1280×768 in 32-bit color, Explorer
was running, and the installed kernel driver and both Direct3D DLLs matched
the package byte for byte. Both the x64 and x86 triangle tests reported
`picture read back: right`, with red (0 0 255 in blue green red order)
inside the triangle. Composition stayed off. The visible UTM window has not
been checked yet.

### Aero composition, 7.14.1.6

Composition was requested in the guest's settings (`Composition` 1, Aero
theme), and the Desktop Window Manager started on Ripple, but it exited at
once with `0x8007000e`. A full `winsat formal` on 7.14.1.5 rated every
component 5.9, Vista's maximum (Direct3D compositor test 1,799 frames/s),
so the assessment was not the reason.

The driver trace (`RIPPLE_UMD_TRACE=1` and `RIPPLE_UMD_LOG` set for the
user, then the compositor service restarted) showed the first refusal: the
compositor's full-screen pair of 1280×768 primary surfaces. Windows' own
allocation call returned `E_INVALIDARG` for it; the driver passed that on as
out of memory. The kernel driver had put only the desktop's own primary in
its desktop segment, and offered every other allocation the aperture segment
alone. Vista refuses a primary that can only live there, as it refused the
desktop's primary before 7.14.1.2.

In 7.14.1.6 the user-mode driver tags each primary it allocates with the
same marker the kernel driver gives the desktop's primary
(`VIOGPU_VISTA_PRIMARY` in `viogpum.h`, and Mesa's copy of that header via
patch `0103`). The kernel driver places tagged allocations in the desktop
segment. After installing it and restarting, Vista reported composition on,
`dwm.exe` kept running, and no new compositor error was logged. Both
triangle tests still read back right with composition on. The kernel log
shows about 130 escapes rejected as `buffer too small 0, should be at least
20` from a caller not yet identified. The UTM window has not been looked at.

## Rebuild and signing handoff

From the Vista worktree on the Linux builder, set `MAIN` to the checkout
holding the existing WDK, llvm-mingw, Mesa tarball and Python environment.
The WDK's Vista x64 import libraries come from `vistalibs_x64fre.msi`.
If they were extracted separately, set `WDK_TARGET_LIBS` to their
`lib/wlh` directory. The base kit supplies `BufferOverflowK.lib`.

```sh
export RIPPLE_TARGET=vista
windows/kmd/build.sh build/vista-kmd
for arch in x64 x86; do
    ARCH=$arch VARIANT=virgl windows/mesa/build-win7.sh || exit 1
done
pkg=build/ripple-vista-sp2-x64
mkdir "$pkg" || exit 1
cp windows/package/vista/viogpu3d.inf build/vista-kmd/amd64/viogpu3d.sys "$pkg/"
cp "$MAIN/.tools/mesa/out-vista-x64-virgl/viogpu_d3d10.dll" "$pkg/"
cp "$MAIN/.tools/mesa/out-vista-x86-virgl/viogpu_d3d10.dll" "$pkg/viogpu_d3d10.x86.dll"
"$MAIN/.venv/bin/python" tools/vista/audit-imports.py "$pkg" "$VISTA_WINDOWS" \
    "$MAIN/.tools/mesa/src-24.3.4-vista-virgl"
```

`VISTA_WINDOWS` is the Windows directory extracted from the reference image.
The archive contains the INF and three unsigned binaries. The Linux builder's
Wine prefix lacks the .NET runtime required by WDK 7.1 Inf2Cat. On the Windows
signing machine, finish any embedded signing first, then run
`Inf2Cat /driver:"C:\path\to\ripple-vista-sp2-x64" /os:Vista_X64`
and sign the resulting `viogpu3d.cat`.
[Microsoft's catalog instructions](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/creating-a-catalog-file-for-a-pnp-driver-package)

## Next Vista run

The local worktree is `tmp-vista-sp2` on branch `vista-sp2-probe`. The
following paths are relative to that worktree. The private lab artifacts are
available in the existing checkout.

| Artifact | What it contains |
| --- | --- |
| `build/ripple-vista-7.14.1.6/` | Unsigned package installed in the 7.14.1.6 run: kernel driver and both Mesa DLLs with patches `0101` to `0103` (composition on). |
| `build/ripple-vista-7.14.1.5/` | The 7.14.1.5 package: the `169f188` kernel driver and both Mesa DLLs with patches `0101` and `0102`. |
| `tmp-utm-validation/fix5/` and `deliver-fix.rb` | The same package with its guest install script, and the host script that delivers it over HTTP, installs it, reboots and runs the checks. |
| `tmp-utm-validation/pipecheck-mark.c` | The write-mark experiment: the direct download with and without `WriteOperation`. |
| `build/ripple-vista-7.14.1.2-test-signed.zip` | Guest-tested package that held 800×600 and reproduced the black pixels. |
| `build/lab-tagged-3.iso` | Guest-tested 7.14.1.3 CD, SHA-256 `906e869ffb80d1c973899e8d13c5959dae8dc4973bb4a38716cc0fad9b17ab13`. It is still the guest's D: drive and supplies the signing tools. |

1. Use the dedicated `ripple-vista` VM. The powered-off snapshot
   `clean-before-ripple` preserves the original VGA installation. The recovery
   boot entry is the default. The Ripple test entry is recorded in
   `C:\RippleVista\ripple-boot-entry.txt`, and the install scripts boot it
   once with `bcdedit /bootsequence`.
2. To try a new build, put the package and an install script in a folder
   like `fix5/` on the UTM host and run
   `ruby deliver-fix.rb <folder> <guest address> <number>`. No CD change is
   needed. The run ends with the display check and both triangle tests.
3. Check the visible desktop in the UTM window, with Aero on. Reading the
   UTM window (any capture of the host's screen) needs the owner's approval
   each time.
4. Then 3DMark06 and F.E.A.R. from the lab CD. Full-screen Direct3D 9 games
   allocate primaries as the compositor does, so they depend on 7.14.1.6.

### Recover the lab desktop

On this guest, the VGA boot option still let Ripple own the display.
The successful recovery used the following elevated command on the lab CD.
DevCon returned 1 to request a reboot.

```bat
D:\bin\devcon.exe update "%windir%\inf\display.inf" "PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01"
```

After reboot, `D:\basecheck.cmd` reported Standard VGA Graphics Adapter at
640×480 in 32-bit color, Explorer running and `viogpu3d` stopped. The check
reads the guest's active display and service state over COM1. The recovered
UTM window still needs a visual check.
