#!/bin/sh
# Build the Windows 7 kernel driver on Linux: clang in cl mode and lld-link against WDK 7.1's
# headers and libraries (docs/windows/toolchain.md; same flags as tools/win7-hello/build.sh).
#   windows/kmd/build.sh [-k] [OUT]     default OUT: $MAIN/build/win7-kmd
#     -k   keep going after a file fails to compile, to see every error (no link then)
# MAIN: the checkout that holds .tools and .venv (default: this tree's root).
# KIT:  the unpacked WDK 7.1 (default $MAIN/.tools/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538).
# ARCHS: "amd64 i386" (default amd64 until the driver builds).
set -eu
keep=0
[ "${1:-}" = -k ] && { keep=1; shift; }
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
MAIN=${MAIN:-$root}
KIT=${KIT:-$MAIN/.tools/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538}
OUT=${1:-$MAIN/build/win7-kmd}
ARCHS=${ARCHS:-amd64}
# DEBUG=1 (the default while the driver is being brought up): the driver's messages go to the
# first serial port (COM1), which the test VM writes to a file on the host. DEBUG=0: no messages.
DEBUG=${DEBUG:-1}
# LEVEL: how much the driver says, 1 (fatal only) to 5 (every call); 3, warnings, by default.
LEVEL=${LEVEL:-3}
[ -d "$KIT/inc/ddk" ] || { echo "build.sh: no WDK 7.1 at $KIT (see docs/windows/toolchain.md)" >&2; exit 1; }
mkdir -p "$OUT"
ov=$OUT/kit-overlay.yaml
"$MAIN/.venv/bin/python" -I "$root/tools/win7-hello/mkoverlay.py" "$ov" "$KIT/inc/ddk" "$KIT/inc/api" "$KIT/inc/crt" \
    "$here/VirtIO" "$here/common" "$here/shared" "$here/viogpu3d"
INC="-vfsoverlay $ov /I$here/common /I$here/shared /I$here/VirtIO /I$here/viogpu3d /I$KIT/inc/ddk /I$KIT/inc/api /I$KIT/inc/crt"
# Windows 7 (NT 6.1) as the target; MSVC 2008's version, which these headers were made for.
COMMON="/nologo /c /O2 /W3 /Zp8 /GS- /Gy /kernel /Gz -fms-compatibility-version=15.00 \
        -Wno-microsoft-anon-tag -Wno-pragma-pack -Wno-ignored-pragma-intrinsic -Wno-ignored-attributes \
        -Wno-nonportable-include-path -Wno-invalid-token-paste -Wno-unknown-pragmas \
        /FI$here/compat/sal2.h /FI$here/compat/win7.h /D_KERNEL_MODE /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000 /DVIOGPU_3D=1"
# (The driver's trace.h turns its own messages on; COM_DEBUG sends them to the serial port
# instead of the kernel debugger. DBG itself is not defined: the kit's assertions do not
# compile as C++ with clang.)
[ "$DEBUG" = 1 ] && COMMON="$COMMON /DCOM_DEBUG /DRIPPLE_DEBUG_LEVEL=$LEVEL"
# TRACE_PRESENT=1: a line for every copy to the screen (surfaces and rectangles), at any LEVEL
[ -z "${TRACE_PRESENT:-}" ] || COMMON="$COMMON /DRIPPLE_TRACE_PRESENT"
# TEST_TDR=1: a test build that can hold a command buffer back on request (VIOGPU_TEST_STALL),
# to bring about a GPU timeout and check the recovery
[ -z "${TEST_TDR:-}" ] || COMMON="$COMMON /DRIPPLE_TEST_TDR"
# TRACE_TIMING=1: where command buffers' time goes, written to the driver's log
[ -z "${TRACE_TIMING:-}" ] || COMMON="$COMMON /DRIPPLE_TRACE_TIMING"
CXX="/GR- /EHs-c- /std:c++17"
C_SRC="compat/intrinsics.c VirtIO/VirtIOPCICommon.c VirtIO/VirtIOPCILegacy.c VirtIO/VirtIOPCIModern.c VirtIO/VirtIORing.c VirtIO/VirtIORing-Packed.c"
CXX_SRC="common/edid.cpp common/viogpu_idr.cpp common/ripple_pci.cpp common/viogpu_queue.cpp common/baseobj.cpp
         common/bitops.cpp viogpu3d/driver.cpp viogpu3d/viogpu_command.cpp viogpu3d/viogpu_device.cpp
         viogpu3d/viogpu_allocation.cpp viogpu3d/viogpu_adapter.cpp viogpu3d/viogpu_vidpn.cpp"
status=0
for arch in $ARCHS; do
    case $arch in
        amd64) T=x86_64-pc-windows-msvc; M=x64; D="/D_AMD64_ /DAMD64 /D_WIN64"; E=DriverEntry ;;
        i386)  T=i686-pc-windows-msvc;   M=x86; D="/D_X86_ /Di386 /DSTD_CALL /DNONTOSPINTERLOCK"; E=DriverEntry@8 ;;
    esac
    o=$OUT/$arch; mkdir -p "$o"
    objs=
    for f in $C_SRC $CXX_SRC; do
        obj=$o/$(basename "${f%.*}").obj
        case $f in *.cpp) x=$CXX ;; *) x=-Wno-error=incompatible-function-pointer-types ;; esac
        if clang --driver-mode=cl --target=$T $COMMON $x $INC $D "$here/$f" "/Fo$obj" 2> "$obj.log"; then
            objs="$objs $obj"
        else
            status=1
            echo "== $f: $(grep -c ' error: ' "$obj.log") errors (first: $(grep -m1 ' error: ' "$obj.log" | sed "s#$here/##" | cut -c1-200))"
            [ $keep = 1 ] || { sed "s#$here/##" "$obj.log" | grep -B2 -A6 -m3 ' error: '; exit 1; }
        fi
    done
    [ $status = 0 ] || continue
    lld-link /nologo /machine:$M /driver /subsystem:native,6.01 /release /nodefaultlib \
        /entry:$E /out:"$o/viogpu3d.sys" $objs "$KIT/lib/win7/$arch/displib.lib" \
        "$KIT/lib/win7/$arch/ntoskrnl.lib" "$KIT/lib/win7/$arch/hal.lib" "$KIT/lib/win7/$arch/wmilib.lib" \
        "$KIT/lib/win7/$arch/BufferOverflowK.lib"
    ls -l "$o/viogpu3d.sys"
done
exit $status
