#!/bin/sh
# Build native x86 tests for both versions of Direct3D using llvm-mingw.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: build-probes.sh OUTPUT_DIRECTORY}
cc=${CC:-i686-w64-mingw32-clang}
cxx=${CXX:-i686-w64-mingw32-clang++}
mkdir -p "$out"
for api in 8 9; do
    "$cxx" -std=c++11 -Os -Wall -Wextra -Werror -D_WIN32_WINNT=0x0500 -DRIPPLE_D3D="$api" \
        -fno-exceptions -fno-rtti -nostdlib -Wl,--subsystem,console:5.0,--no-insert-timestamp \
        "$here/texture.cpp" -o "$out/tex$api.exe" -luser32 -lkernel32 -lmsvcrt
done
"$cc" -Os -Wall -Wextra -Werror -D_WIN32_WINNT=0x0500 -D__USE_MINGW_ANSI_STDIO=0 \
    -Wl,--subsystem,console:5.0,--no-insert-timestamp \
    "$here/../../tools/win7-hello/d3d9const.c" -o "$out/dx9shade.exe" -ld3d9
