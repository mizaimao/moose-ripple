# 0001: Build on Linux with clang, cctools-port and the 10.6 SDK

Date: 2026-11-10. Status: in use.

## Decision

Kexts and userspace code build on dev.lan (Debian 13, x86_64), everything under the repo's
git-ignored `.tools/` there:
- **Compiler:** the system clang 19.1.7, `-target {i386,x86_64}-apple-darwin10`.
- **Linker and Mach-O tools:** cctools-port branch `973.0.1-ld64-609` (commit 75b174c5),
  built into `.tools/cctools`. The newest branch (ld64-957.1) needs libBlocksRuntime, which
  dev.lan doesn't have; 609 needs nothing extra.
- **SDK:** `MacOSX10.6.sdk` from Xcode 3.2.6, copied from VM 210's `/Developer/SDKs` into
  `.tools/sdk`. Kernel headers come from its `Kernel.framework`, the kmod glue from its
  `usr/lib/libkmod.a`, userspace headers and libSystem stubs from the rest of it.

Apple's SDK and Xcode are Apple-licensed: they stay in `.tools/` and in the guest, never in git.
Xcode 3.2.6 is also installed in VM 210 (`/Developer`) as the reference toolchain.

## Evidence

- 10.6.8's own kexts are universal: x86_64 `MH_KEXT_BUNDLE`, i386 `MH_OBJECT` (`file` on
  AppleAPIC, AppleSMBusPCI, AppleFileSystemDriver, IOPlatformPluginFamily in the guest).
- Kernel compiler flags follow xnu-1504.15.3's `makedefs/MakeInc.def` (`CFLAGS_GEN`,
  `CFLAGS_X86_64`) and `libkern/kmod/Makefile.kmod` (`-Os -mkernel`).
- x86_64: `ld -kext` output loads and unloads as is.
- i386: `ld -r` output first failed in kxld with "Invalid segment type in MH_OBJECT kext:
  36" (LC_VERSION_MIN_MACOSX). Linking with `-no_data_in_code_info` and stripping the
  version command with `vtool -remove-build-version macos` leaves only LC_SEGMENT and
  LC_SYMTAB, and that loads and unloads.
- `ld -r` does pull `__start`/`__stop` out of the SDK's `libkmod.a` for i386.

## Media (from the owner's Macintosh Repository downloads, 2026-11-10)

- `xcode_3.2.6_and_ios_sdk_4.3.dmg`, 4443150993 bytes, SHA-256
  `47f6fecdce63bb00640a9f99c2a65778c1356630286d8003d597ffb83dfcdf3b` (Xcode 3.2.6, build 10M2518)
- `kernel_debug_kit_10-6-8_10k549.dmg`, 93429770 bytes, SHA-256
  `8dc391def99bc69700bdc1a5e91d4e88451f60c16241ce1940954fb22e018f0a`

## History

Before the SDK arrived (same day), kexts built against the open-source xnu-1504.15.3 headers
(tarball SHA-256 `05cc3e9617581f8f9bfa434901fb902761c89bbcf770d822e84a31dd625c2b58`, include
order `EXTERNAL_HEADERS bsd osfmk libkern iokit osfmk/libsa`) with xnu's own
`libkern/kmod/c_start.c` and `c_stop.c`, and userspace linked against the guest's own
libSystem. Both worked on both kernels. The copies stay in `.tools/` on dev.lan as a
fallback but the build no longer uses them.

## Open

- C++ IOKit kexts (`-fapple-kext`, `libkmodc++.a`, OSObject vtables) are untested; M2 needs them.
- No compiler runtime (`libcc_kext.a`) in the link yet: 64-bit division in i386 code will need it.
