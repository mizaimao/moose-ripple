# Vista SP2 x64 display driver probe

The kernel driver and both Direct3D 9 DLLs build from the Windows 7 driver at
`63739fe`. On Vista SP2 x64 with test signing enabled, version 7.14.1.2
starts the adapter, sets an 800×600 desktop and opens a hardware Direct3D 9
device for both x64 and x86 applications. The offscreen color checks currently
return black, and the guest reports Aero composition off.

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

## First boot

Use a snapshot of a disposable Vista SP2 x64 guest with the virtio-gpu 3D
device. After installing the signed package and rebooting normally, record the
Device Manager status and `sc query viogpu3d`. Correlate those with the guest
serial driver log. The first display-specific uncertainty is the primary
surface holding the desktop. Ripple exposes a GPU aperture the CPU cannot
address directly. The Windows 7 driver used a memory capability bit for that
surface that the Vista headers reserve. The Vista build leaves it clear.
Record the first mode set and primary allocation before investigating Aero.

Once Ripple is driving the display, run the existing x64 `d3d9probe.exe` and
`d3d9test.exe` in a writable directory. Confirm the adapter ID, successful
HAL device creation, the red triangle on green (`picture read back: right`),
a successful present, and GPU commands from Ripple for that draw. The triangle
test's exit code alone doesn't check its readback. For Aero, enable a glass
theme and check actual composition and a correctly rendered glass window.
