#!/bin/sh
# Cross-build Mesa 24.3.4 for Windows 98 on the Linux build host (docs/win9x/mesa.md).
#   win9x/mesa/build.sh [--clean]
#   DRIVERS=softpipe win9x/mesa/build.sh     software rendering only (default: virgl,softpipe)
# Its own source tree and build directory under TOOLS (default .tools, never committed):
#   mesa/src-24.3.4-win9x     the release tarball, patches/ applied, overlay/ copied over it
#   mesa/build-win9x          the build;  mesa/out-win9x  the DLLs
# Toolchain: llvm-mingw, msvcrt variant (clang, lld, compiler-rt, libc++, mingw-w64), the same
# one the Windows 7 build uses. Three things make its output load on Windows 98:
#   - everything from the toolchain linked statically, no native thread-local storage
#     (-femulated-tls: Windows before Vista gives a DLL loaded at run time none);
#   - win9x/compat/k32shim.c, the few kernel32 functions newer than Windows 98 that the
#     toolchain's libraries call;
#   - version 4.0 in the file headers.
# tools/win9x/check-imports.py then compares every DLL's imports with Windows 98 SE's own
# libraries (W98SYS, taken from its setup cabinets) and the build fails if one is missing.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
T=${TOOLS:-$root/.tools}
MESA_VERSION=24.3.4
MESA_SHA256=e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025
TARBALL=$T/src/mesa-dl/mesa-$MESA_VERSION.tar.xz
DRIVERS=${DRIVERS:-virgl,softpipe}
SRC=$T/mesa/src-$MESA_VERSION-win9x
BUILD=$T/mesa/build-win9x
OUT=${OUT:-$T/mesa/out-win9x}
LLVM_MINGW=${LLVM_MINGW:-$(ls -d "$T"/llvm-mingw/llvm-mingw-*-msvcrt-*/ 2>/dev/null | head -1)}
LLVM_MINGW=${LLVM_MINGW%/}
W98SYS=${W98SYS:-$T/win9x/w98dlls/sys}
PYTHON=${PYTHON:-$root/.venv/bin/python}
[ -x "$PYTHON" ] || PYTHON=$(dirname "$T")/.venv/bin/python
export PATH="$LLVM_MINGW/bin:$(dirname "$PYTHON"):$T/gnu/bin:$PATH"
TRIPLE=i686-w64-mingw32

[ -x "$LLVM_MINGW/bin/$TRIPLE-clang" ] || { echo "build.sh: no llvm-mingw under $T/llvm-mingw" >&2; exit 1; }
if [ ! -f "$TARBALL" ]; then
    mkdir -p "$(dirname "$TARBALL")"
    curl -sfLo "$TARBALL" "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz"
fi
echo "$MESA_SHA256  $TARBALL" | sha256sum -c --quiet

# Source tree: unpacked again when the patches change, or with --clean. The overlay (our own
# files) and the VxD's interface header are copied over it on every run, changed files only,
# so that an edit there costs an incremental build.
stamp=$( (echo "$MESA_SHA256"; cat "$here"/patches/*.patch 2>/dev/null) | sha256sum | cut -c1-16)
if [ "${1:-}" = --clean ] || [ "$(cat "$SRC/.ripple-win9x-stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$SRC" "$BUILD"
    mkdir -p "$SRC"
    tar xJf "$TARBALL" -C "$SRC" --strip-components=1
    for p in "$here"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 --quiet < "$p"
    done
    echo "$stamp" > "$SRC/.ripple-win9x-stamp"
fi
(cd "$here/overlay" && find . -type f) | while read -r f; do
    cmp -s "$here/overlay/$f" "$SRC/$f" || { mkdir -p "$(dirname "$SRC/$f")"; cp "$here/overlay/$f" "$SRC/$f"; }
done
cmp -s "$here/../minivdd/rvd.h" "$SRC/src/gallium/winsys/virgl/rvd/rvd.h" ||
    cp "$here/../minivdd/rvd.h" "$SRC/src/gallium/winsys/virgl/rvd/rvd.h"
# A different set of drivers needs a fresh build directory.
[ "$(cat "$BUILD/.ripple-drivers" 2>/dev/null)" = "$DRIVERS" ] || rm -rf "$BUILD"

# Pentium II instructions only: the guest CPU model may be an old one, and Windows 98 predates
# SSE2. No frame-pointer omission games either: plain, debuggable code.
CPU_FLAGS="'-march=pentium2', '-mtune=generic', '-femulated-tls'"
mkdir -p "$BUILD.compat"
"$TRIPLE-clang" -O2 -march=pentium2 -femulated-tls -Wall -Werror \
    -c "$here/../compat/k32shim.c" -o "$BUILD.compat/k32shim.new.o"
# ninja doesn't know the DLLs depend on this object: when it changes, they are linked again.
if ! cmp -s "$BUILD.compat/k32shim.new.o" "$BUILD.compat/k32shim.o"; then
    mv "$BUILD.compat/k32shim.new.o" "$BUILD.compat/k32shim.o"
    [ ! -d "$BUILD" ] || find "$BUILD" -name '*.dll' -delete
fi
LINK_FLAGS="'-static', '-Wl,--major-os-version=4,--minor-os-version=0', '-Wl,--major-subsystem-version=4,--minor-subsystem-version=0', '$BUILD.compat/k32shim.o'"

cross=$T/mesa/cross-win9x.ini
cat > "$cross" <<EOC
[binaries]
c = '$LLVM_MINGW/bin/${TRIPLE}-clang'
cpp = '$LLVM_MINGW/bin/${TRIPLE}-clang++'
ar = '$LLVM_MINGW/bin/${TRIPLE}-ar'
strip = '$LLVM_MINGW/bin/${TRIPLE}-strip'
windres = '$LLVM_MINGW/bin/${TRIPLE}-windres'
pkg-config = 'false'

[built-in options]
c_args = [$CPU_FLAGS, '-Wno-error=incompatible-pointer-types', '-Wno-error=missing-prototypes']
cpp_args = [$CPU_FLAGS]
c_link_args = [$LINK_FLAGS]
cpp_link_args = [$LINK_FLAGS]

[host_machine]
system = 'windows'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOC

if [ ! -f "$BUILD/build.ninja" ]; then
    meson setup "$BUILD" "$SRC" --cross-file "$cross" \
        --buildtype=release -Db_ndebug=true -Dprefix=/ \
        -Dplatforms=windows -Dmin-windows-version=7 -Dsse2=false \
        -Dgallium-drivers="$DRIVERS" -Dvulkan-drivers= \
        -Dopengl=true -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled \
        -Dgbm=disabled -Dllvm=disabled -Dzstd=disabled -Dzlib=disabled -Dexpat=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
        -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dgallium-va=disabled \
        -Dvideo-codecs= -Dshared-glapi=enabled
    echo "$DRIVERS" > "$BUILD/.ripple-drivers"
fi
ninja -C "$BUILD"
rm -rf "$OUT" && mkdir -p "$OUT"
find "$BUILD" -name '*.dll' -exec cp {} "$OUT/" \;
"$TRIPLE-strip" "$OUT"/*.dll
ls -l "$OUT"
"$PYTHON" -I "$root/tools/win9x/check-imports.py" "$W98SYS" "$OUT"/*.dll
