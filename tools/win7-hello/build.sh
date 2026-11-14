#!/bin/sh
# Build the hello kernel driver and DLL for Windows 7 x64 and x86 on Linux, with clang in
# cl mode and lld-link against WDK 7.1's headers and libraries (docs/windows/toolchain.md).
#   tools/win7-hello/build.sh [OUT]        default OUT: build/win7-hello
# KIT: the unpacked WDK 7.1 (default .tools/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
KIT=${KIT:-$root/.tools/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538}
OUT=${1:-$root/build/win7-hello}
[ -d "$KIT/inc/ddk" ] || { echo "build.sh: no WDK 7.1 at $KIT (see docs/windows/toolchain.md)" >&2; exit 1; }
mkdir -p "$OUT"
ov=$OUT/kit-overlay.yaml
"$root/.venv/bin/python" -I "$here/mkoverlay.py" "$ov" "$KIT/inc/ddk" "$KIT/inc/api" "$KIT/inc/crt"
INC="-vfsoverlay $ov /I$KIT/inc/ddk /I$KIT/inc/api /I$KIT/inc/crt"
# Windows 7 (NT 6.1) as the target; MSVC 2008's version, which these headers were made for.
COMMON="/nologo /c /O2 /W3 /Zp8 /GS- /Gy -fms-compatibility-version=15.00 -Wno-microsoft-anon-tag \
        -Wno-pragma-pack -Wno-ignored-pragma-intrinsic -Wno-ignored-attributes \
        -Wno-nonportable-include-path -Wno-invalid-token-paste \
        /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000"
for arch in amd64 i386; do
    case $arch in
        amd64) T=x86_64-pc-windows-msvc; M=x64; D="/D_AMD64_ /DAMD64 /D_WIN64"; E=DriverEntry; DE=DllMain ;;
        i386)  T=i686-pc-windows-msvc;   M=x86; D="/D_X86_ /Di386 /DSTD_CALL /DNONTOSPINTERLOCK"; E=DriverEntry@8; DE=DllMain@12 ;;
    esac
    o=$OUT/$arch; mkdir -p "$o"
    clang --driver-mode=cl --target=$T $COMMON $INC $D /kernel /Gz /D_KERNEL_MODE \
        "$here/hello.c" "/Fo$o/hello.obj"
    lld-link /nologo /machine:$M /driver /subsystem:native,6.01 /release /nodefaultlib \
        /entry:$E /out:"$o/hello.sys" "$o/hello.obj" \
        "$KIT/lib/win7/$arch/ntoskrnl.lib" "$KIT/lib/win7/$arch/hal.lib" "$KIT/lib/win7/$arch/BufferOverflowK.lib"
    clang --driver-mode=cl --target=$T $COMMON $INC $D "$here/hellodll.c" "/Fo$o/hellodll.obj"
    lld-link /nologo /machine:$M /dll /subsystem:windows,6.01 /release /nodefaultlib \
        /entry:$DE /out:"$o/hellodll.dll" "$o/hellodll.obj" \
        "$KIT/lib/win7/$arch/kernel32.lib" "$KIT/lib/win7/$arch/user32.lib"
    echo "built $o/hello.sys $o/hellodll.dll"
done
