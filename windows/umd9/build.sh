#!/bin/sh
# Build the Direct3D 9 part of the Windows 7 driver (viogpu_d3d9.dll) on Linux with llvm-mingw.
#   windows/umd9/build.sh [OUT]        default OUT: build/win7-umd9 (in MAIN)
# LOG=C:/path/file: a build that writes every question Windows asks to that file in the guest.
# MAIN: the checkout whose .tools has the toolchain (default: this one). The WDK 7.1 DDI headers
# are the ones windows/mesa/build-win7.sh prepares in its Mesa source tree (include/winddk), so
# that build has to have run once.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
MAIN=${MAIN:-$root}
T=$MAIN/.tools
LLVM_MINGW=${LLVM_MINGW:-$T/llvm-mingw/llvm-mingw-20261104-msvcrt-ubuntu-22.04-x86_64}
DDK=${DDK:-$T/mesa/src-24.3.4-win-virgl/include/winddk}
OUT=${1:-$MAIN/build/win7-umd9}
ARCH=${ARCH:-x64}
case $ARCH in
    x64) TRIPLE=x86_64-w64-mingw32 ;;
    x86) TRIPLE=i686-w64-mingw32 ;;
    *) echo "build.sh: ARCH is x64 or x86" >&2; exit 1 ;;
esac
[ -f "$DDK/d3dumddi.h" ] || { echo "build.sh: no DDI headers at $DDK (run windows/mesa/build-win7.sh first)" >&2; exit 1; }
mkdir -p "$OUT/$ARCH"
DEFS=
[ -z "${LOG:-}" ] || DEFS="-DRIPPLE_D3D9_LOG=\"$LOG\""
"$LLVM_MINGW/bin/$TRIPLE-clang++" -O2 -std=c++17 -Wall -Wno-unknown-pragmas -Wno-unused-function \
    -shared -static -I"$DDK" $DEFS \
    -o "$OUT/$ARCH/viogpu_d3d9.dll" "$here/ripple_d3d9.cpp" "$here/umd9.def"
ls -l "$OUT/$ARCH/viogpu_d3d9.dll"
