# Vista SP2 x64 display driver probe

The kernel driver and both Direct3D 9 DLLs build from the Windows 7 driver at
`63739fe`. Their imports resolve against the Vista SP2 x64 installation image
used for the probe. Starting the adapter, drawing a frame, and composing the
desktop with Aero still need a normal-boot test.

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
