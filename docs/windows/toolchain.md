# Windows toolchain: building for Windows 7 off Windows

Can a WDDM 1.1 kernel miniport and a user-mode display driver for Windows 7 (x64 and x86) be
built on the Linux build host, and with which headers? Short answer: yes for the kernel side,
checked with a "hello" kernel driver and a "hello" DLL ([tools/win7-hello](../../tools/win7-hello/))
built with clang and lld-link against WDK 7.1's headers and libraries, test-signed with
osslsigncode, and loaded in the Windows 7 x64 test VM ([test-vms.md](test-vms.md)). 2026-11-14.

## What a Windows 7 display driver is made of

- Kernel-mode miniport (`.sys`): WDDM 1.1 (`DXGKDDI_INTERFACE_VERSION_WIN7`, 0x2005). It
  registers with the DirectX kernel through `DxgkInitialize`, which comes from the static
  library `displib.lib`, and imports from `ntoskrnl.exe` and `hal.dll`. Headers:
  `dispmprt.h`, `d3dkmddi.h`, `d3dkmdt.h`, `d3dukmdt.h`.
- User-mode display driver (`.dll`): Direct3D 9 (`d3dumddi.h`) and/or Direct3D 10
  (`d3d10umddi.h`). Windows 7's Desktop Window Manager needs one of them for Aero.
- An INF and a catalog (`.cat`) to install them through Plug and Play.

## Header sets and their licences

| Set | Has the WDDM 1.1 DDI headers | Targets Windows 7 | Licence | Where from |
|---|---|---|---|---|
| WDK 7.1.0 (7600.16385.1) | yes: all of the above, plus `displib.lib`, `ntoskrnl.lib`, `hal.lib`, `BufferOverflowK.lib`, and user-mode import libraries, for `win7` x86 and amd64 | yes, it is the Windows 7 kit | Microsoft's WDK licence: use to build, no redistribution | `GRMWDK_EN_7600_1.ISO` from Microsoft's download centre (still online) |
| WDK 10.0.19041 (Windows 10 2004) | yes | yes, the last kit that does | Microsoft | Windows installer or the EWDK ISO (about 10 GB); not on NuGet |
| WDK 10.0.26100 as NuGet (`Microsoft.Windows.WDK.x64`/`.x86`, 111 MB and 24 MB) with the SDK NuGet (`Microsoft.Windows.SDK.CPP` 10.0.19041.5, 101 MB plus 51 MB for x64) | yes; the Windows 7 interface versions are still defined | not supported since 10.0.22621; may work with care | Microsoft | nuget.org; vmlab-win3d builds a Windows 10 WDDM driver this way |
| MinGW-w64 headers | no: `wdm.h`, `ntddk.h`, `video.h`, XDDM's `d3dhal.h` and the user-mode Direct3D headers, but no `dispmprt.h`, `d3dkmddi.h`, `d3dumddi.h` or `d3d10umddi.h`, and no `displib` | n/a | public domain | with llvm-mingw or a MinGW-w64 package |

WDK 7.1.0 is the set used here: it is the period kit, it has everything the kernel side needs
(`displib.lib` included), and it is small. Microsoft's headers and libraries are downloaded
into `.tools/` at build time and never committed (the owner's rule, 2026-11-14).

## What was downloaded, and from where (dev.lan, `.tools/`)

| What | From | Size | sha256 |
|---|---|---|---|
| `.tools/wdk71/GRMWDK_EN_7600_1.ISO` | `https://download.microsoft.com/download/4/A/2/4A25C7D5-EFBE-4182-B6A9-AE6850409A78/GRMWDK_EN_7600_1.ISO` | 649,877,504 bytes | `5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d` |
| `.tools/osslsigncode/osslsigncode-2.10.tar.gz` (built into `.tools/osslsigncode/build/`) | `https://github.com/mtrojnar/osslsigncode/archive/refs/tags/2.10.tar.gz` | | `2a864e6127ee2350fb648070fa0d459c534ac6400ca0048886aeab7afb250f65` |

From the ISO only three packages are unpacked, `WDK/headers.msi`, `WDK/libs_x64fre.msi` and
`WDK/libs_x86fre.msi`, with Wine's `msiexec /a` (an administrative install only copies files),
into `.tools/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538/` (467 MB). Wine (10.0) was
already on the build host; its prefix is `.tools/wine`.

## The build

`tools/win7-hello/build.sh` (runs on dev.lan) uses what the build host already has: clang 19
in `cl` mode (`--driver-mode=cl`, `--target=x86_64-pc-windows-msvc` or `i686-pc-windows-msvc`)
and `lld-link`. Points that needed care:

- Case. The kit's headers include each other in any case (`<specstrings.h>` for
  `SpecStrings.h`, `WINDOWS.H`). `mkoverlay.py` writes a clang VFS overlay marked
  case-insensitive over the kit's include directories (`-vfsoverlay`).
- Old headers, new compiler. `-fms-compatibility-version=15.00` (MSVC 2008, which the kit
  was made for) and a handful of `-Wno-*` for what MSVC accepted (token pasting in the SAL
  macros, `#pragma pack` warnings).
- x86 interlocked helpers. `ntddk.h` defines `_InterlockedAnd64` and friends inline for
  x86, which clang has as builtins; `NONTOSPINTERLOCK` skips those definitions.
- Kernel link. `lld-link /driver /subsystem:native,6.01 /release /nodefaultlib` with
  `ntoskrnl.lib`, `hal.lib` and `BufferOverflowK.lib` (the security cookie, which the x86
  kernel library's `memset` needs); entry `DriverEntry` (x86: `DriverEntry@8`).

Result: `hello.sys` and `hellodll.dll` for x64 and x86, PE images for Windows 6.01 (native
subsystem for the driver), with a checksum.

Not tried yet: a WDDM miniport against `displib.lib` (that is the kernel-driver work in W1),
and the user-mode driver. Mesa's user-mode parts are normally built with MinGW-w64; for those
llvm-mingw (about 180 MB, Apache 2.0 with the LLVM exception, MinGW-w64 runtime in the public
domain) is the likely compiler, and it is not downloaded yet.

## Signing and loading on Windows 7

- x64 needs a signature for every kernel driver. A self-made test certificate is enough in
  test mode: `bcdedit /set testsigning on` and a restart. Outside test mode the VM refuses the
  test-signed driver: `StartService FAILED 577`, "Windows cannot verify the digital
  signature". (Per boot, F8's "Disable Driver Signature Enforcement" also works.)
- SHA-256 or SHA-1. Windows 7 SP1 checks SHA-256 signatures only with KB3033929 or
  KB4474419. The test VM has KB4474419, so `sign.sh` signs with SHA-256; a machine without
  either update needs a SHA-1 signature.
- x86 loads unsigned kernel drivers (with a warning when installed through an INF).
- Off Windows: `tools/win7-hello/sign.sh` makes the test certificate once with OpenSSL
  (`.tools/win7-testcert/`, never committed) and signs with osslsigncode (GPL 3; used as a
  tool, nothing of it copied). A driver package installed through an INF also needs a signed
  catalog; a tool to make `.cat` files off Windows is still to be chosen (LINBIT's
  generate-cat-file is GPL 2, also usable as a tool only). The hello driver needs none: it is
  loaded as a plain service (`sc create rbhello type= kernel binPath= C:\rb\hello.sys`,
  `sc start rbhello`).

## Checked in the test VM (2026-11-14)

VM 220, Windows 7 SP1 x64 in test mode: the x64 `hello.sys`, signed by `sign.sh`, starts and
stops as a kernel service (`sc start`: RUNNING, `sc stop`: STOPPED), and the x64
`hellodll.dll` runs (`rundll32 hellodll.dll,HelloRun` shows its message box). The files went
into the VM over HTTP from the build host (`bitsadmin /transfer`). The VM's snapshot
`testsigning-on` keeps that state. The x86 files built the same way are untested: there is no
32-bit Windows 7 VM yet.
