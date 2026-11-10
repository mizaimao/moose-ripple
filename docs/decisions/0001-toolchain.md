# 0001: Build kexts on Linux with clang, cctools-port and the xnu-1504.15.3 headers

Date: 2026-11-10. Status: in use for M0; revisit when the owner's Xcode 3.2.6 / 10.6 SDK arrive.

## Decision

Kexts build on dev.lan (Debian 13, x86_64) with:
- **Compiler:** the system clang 19.1.7, `-target {i386,x86_64}-apple-darwin10`.
- **Linker and Mach-O tools:** cctools-port branch `973.0.1-ld64-609` (commit 75b174c5),
  built into `.tools/cctools` (git-ignored). The newest branch (ld64-957.1) needs
  libBlocksRuntime, which dev.lan doesn't have; 609 needs nothing extra.
- **Kernel headers:** Apple's open-source `xnu-1504.15.3` (the 10.6.8 kernel, matching
  `uname -v` in the guest), tarball SHA-256
  `05cc3e9617581f8f9bfa434901fb902761c89bbcf770d822e84a31dd625c2b58`, used straight from
  the source tree. Include order matters: `EXTERNAL_HEADERS bsd osfmk libkern iokit
  osfmk/libsa` (bsd before osfmk so `machine/` and `i386/` resolve to the public BSD
  headers; libsa last because it has its own `i386/types.h`, but it holds `string.h`).
- **kmod glue:** `_start`/`_stop` from xnu's `libkern/kmod/c_start.c` and `c_stop.c`, and a
  hand-written `info.c` per kext (`kmod_info`, `_realmain`, `_antimain`).

## Why

The 10.6 SDK and Xcode 3.2.6 aren't here yet, and the guest has no headers
(`Kernel.framework` holds only `Resources`). Everything above is open source, so nothing
Apple-licensed is needed to build, and no Apple binary goes in the repo.

## Evidence

- 10.6.8's own kexts are universal: x86_64 `MH_KEXT_BUNDLE`, i386 `MH_OBJECT` (`file` on
  AppleAPIC, AppleSMBusPCI, AppleFileSystemDriver, IOPlatformPluginFamily in the guest).
- Compiler flags follow xnu's `makedefs/MakeInc.def` (`CFLAGS_GEN`, `CFLAGS_X86_64`) and
  `libkern/kmod/Makefile.kmod` (`-Os -mkernel`).
- x86_64: `ld -kext` output loads and unloads as is.
- i386: `ld -r` output first failed in kxld with "Invalid segment type in MH_OBJECT kext:
  36" (LC_VERSION_MIN_MACOSX). Linking with `-no_data_in_code_info` and stripping the
  version command with `vtool -remove-build-version macos` leaves only LC_SEGMENT and
  LC_SYMTAB, and that loads and unloads.

## Open

- C++ IOKit kexts (`-fapple-kext`, `libkmodc++`, OSObject vtables) are untested; M2 needs them.
- No compiler runtime (`libcc_kext.a`): 64-bit division in i386 code will need care.
- Userspace has no SDK headers yet: sources declare the few libSystem functions they use.

## Userspace stopgap (until the 10.6 SDK arrives)

Dylibs link with `-syslibroot .tools/guest-root` against the guest's own libSystem, copied
from VM 210 on 2026-11-10 (the owner's OK). These are Apple binaries: they stay in `.tools/` on
dev.lan and are never committed.
- `/usr/lib/libSystem.B.dylib` (i386, x86_64, ppc7400; current version 125.2.11), SHA-256
  `c5f4b97cbc9340795608ae0f78b399ecdadee64c63ec38b69ba15233f87b597c`
- `/usr/lib/system/libmathCommon.A.dylib`, SHA-256
  `288d8f7e86217faf16ce44eb44b1ec0064ce8db90214b6825c8c42900c905cd2`
