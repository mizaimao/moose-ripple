#!/bin/sh
# Cross-build the application-local WineD3D runtime for Windows 2000 and XP.
# Uses the project's x86 MSVCRT llvm-mingw toolchain on a 64-bit Linux host.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
archive=${1:?usage: build.sh WINE-1.7.52.tar.bz2 OUTPUT_DIRECTORY}
out=${2:?missing output directory}
tools=${TOOLS:-$repo/.tools}
mingw=${LLVM_MINGW:-}
if [ -z "$mingw" ]; then
    set -- "$tools"/llvm-mingw/llvm-mingw-*-msvcrt-*/
    [ "$#" -eq 1 ] || { echo "Set LLVM_MINGW to the MSVCRT toolchain directory." >&2; exit 1; }
    mingw=$1
fi
test -x "$mingw/bin/i686-w64-mingw32-clang"
export PATH="$mingw/bin:$PATH"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}
jobs=${JOBS:-6}
digest=0773b32a0c358323db4c383ceb1e9e28d5d4ed4ea37570af2bcb41fecf0d554b
if ! printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status; then
    echo "Expected the Wine 1.7.52 source tarball (SHA-256 $digest)." >&2
    exit 1
fi
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d "$out/.wined3d.XXXXXX")
trap 'rm -rf "$work"' EXIT
tar -xjf "$archive" -C "$work"
src=$work/wine-1.7.52
patch -s -d "$src" -p1 < "$here/clear-srgb.patch"
mkdir "$work/host" "$work/target" "$work/files"
cd "$work/host"
../wine-1.7.52/configure --enable-win64 --without-x --without-freetype --disable-tests
make -j"$jobs" __tooldeps__
cd "$work/target"
# This release's GL headers rely on the pre-GCC 10 common-symbol convention.
CFLAGS='-O2 -fcommon -DWINE_NOWINSOCK -DUSE_WIN32_OPENGL' \
    ../wine-1.7.52/configure --host=i686-w64-mingw32 --with-wine-tools="$work/host" \
    --without-x --without-freetype --disable-tests --disable-kernel32
flags='-Wl,--major-os-version,5,--minor-os-version,0,--major-subsystem-version,5,--minor-subsystem-version,0,--no-insert-timestamp'
# Wine's libwine.dll link uses CC directly. The graphics DLLs use winegcc and LDFLAGS.
make -j"$jobs" CC="i686-w64-mingw32-clang -Wno-unused-command-line-argument $flags" \
    LDFLAGS="$flags" dlls/wined3d dlls/d3d8 dlls/d3d9
for file in libs/wine/libwine.dll dlls/wined3d/wined3d.dll dlls/d3d8/d3d8.dll dlls/d3d9/d3d9.dll; do
    llvm-strip -o "$work/files/${file##*/}" "$file"
done
cp "$src/COPYING.LIB" "$work/files/"
for api in d3d8 d3d9; do
    mkdir -p "$out/$api" "$work/$api"
    for file in "$api.dll" libwine.dll wined3d.dll COPYING.LIB; do
        cp "$work/files/$file" "$work/$api/$file"
        mv -f "$work/$api/$file" "$out/$api/$file"
    done
done
