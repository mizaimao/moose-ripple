#!/bin/sh
# Cross-build Mesa 24.3.4 for Windows 98 on the Linux build host (docs/win9x/mesa.md).
#   win9x/mesa/build.sh [--clean]
#   DRIVERS=softpipe win9x/mesa/build.sh     software rendering only (default: virgl,softpipe)
# Its own source tree and build directory under TOOLS (default .tools, never committed):
#   mesa/src-24.3.4-win9x     the release tarball, patches/ applied, overlay/ copied over it
#   mesa/build-win9x          the build;  mesa/out-win9x  the DLLs (ripplegl.dll, opengl32.dll)
# Toolchain: llvm-mingw, msvcrt variant (clang, lld, compiler-rt, libc++, mingw-w64), the same
# one the Windows 7 build uses. The 9x runtime needs:
#   - everything from the toolchain linked statically, no native thread-local storage
#     (-femulated-tls: Windows before Vista gives a DLL loaded at run time none);
#   - win9x/compat/k32shim.c, the few kernel32 functions newer than Windows 98 that the
#     toolchain's libraries call;
#   - version 4.0 in the file headers;
#   - the MinGW formatter and its decimal conversion helpers built without SSE.
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
    # to a temporary name first: an interrupted download must not look like the tarball
    curl -sfLo "$TARBALL.part" "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
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
# One DLL, ripplegl.dll: Mesa's OpenGL with the dispatch layer (glapi) inside, the file Windows'
# opengl32.dll loads as the installable client driver. Mesa's own opengl32.dll is built too:
# next to a program it takes the place of Windows', for tests.
NAMES="-Dgallium-wgl-dll-name=ripplegl -Dshared-glapi=disabled"
[ "$(cat "$BUILD/.ripple-drivers" 2>/dev/null)" = "$DRIVERS $NAMES" ] || rm -rf "$BUILD"

# Mesa and the compatibility objects target Pentium II.
CPU_FLAGS="'-march=pentium2', '-mtune=generic', '-femulated-tls'"
# This is llvm-mingw 20261104's exact mingw-w64 revision. Its precompiled
# printf uses XMM registers on ordinary formatting paths that Windows 95 runs.
MINGW_REV=ee9bd87644156c41072a11ad957d1e11831a72df
MINGW_SHA256=6a346a2e3eafa0e5ec8e30df89b16a05add8b50cb4cece8c47593799ca19e43b
MINGW_ARCHIVE=$T/src/mesa-dl/mingw-w64-$MINGW_REV.tar.gz
CRT=$T/mesa/mingw-crt-$MINGW_REV
if [ ! -d "$CRT" ]; then
    temp=$(mktemp -d "$T/mesa/mingw-crt.XXXXXX")
    trap 'rm -rf "$temp"' 0
    archive=$MINGW_ARCHIVE
    if [ ! -f "$archive" ]; then
        archive=$temp/archive
        curl -fsSL --retry 2 -o "$archive" "https://codeload.github.com/mingw-w64/mingw-w64/tar.gz/$MINGW_REV"
    fi
    echo "$MINGW_SHA256  $archive" | sha256sum -c --quiet
    [ "$archive" = "$MINGW_ARCHIVE" ] || mv "$archive" "$MINGW_ARCHIVE"
    mkdir "$temp/crt"
    prefix=mingw-w64-$MINGW_REV/mingw-w64-crt
    tar xzf "$MINGW_ARCHIVE" -C "$temp/crt" --strip-components=2 "$prefix/stdio" "$prefix/gdtoa"
    mv -T "$temp/crt" "$CRT"
    rmdir "$temp"
    trap - 0
fi
# Use a fresh object directory when upgrading an existing Windows 98 build.
COMPAT=$BUILD.runtime
FORMATTERS="mingw_pformat gdtoa dmisc misc gmisc"
mkdir -p "$COMPAT"
make -C "$COMPAT" -f "$here/runtime.mk" CC="$LLVM_MINGW/bin/$TRIPLE-clang" CRT="$CRT" FORMATTERS="$FORMATTERS"
# Meson links these external objects explicitly. Refresh previously linked DLLs
# when the objects change, including after an interrupted build.
if [ -d "$BUILD" ]; then
    find "$BUILD" -name '*.dll' ! -newer "$COMPAT/runtime.stamp" -delete
fi
LINK_FLAGS="'-static', '-Wl,--major-os-version=4,--minor-os-version=0', '-Wl,--major-subsystem-version=4,--minor-subsystem-version=0'"
for object in k32shim $FORMATTERS; do
    LINK_FLAGS="$LINK_FLAGS, '$COMPAT/$object.o'"
done

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
        -Dvideo-codecs= $NAMES
    echo "$DRIVERS $NAMES" > "$BUILD/.ripple-drivers"
elif [ "$(cat "$BUILD/.ripple-runtime-link" 2>/dev/null)" != "$LINK_FLAGS" ]; then
    # Meson caches machine-file link arguments at setup. Update an existing
    # configuration without rebuilding Mesa's C and C++ objects.
    meson configure "$BUILD" "-Dc_link_args=[$LINK_FLAGS]" "-Dcpp_link_args=[$LINK_FLAGS]"
fi
printf '%s\n' "$LINK_FLAGS" > "$BUILD/.ripple-runtime-link"
ninja -C "$BUILD"
rm -rf "$OUT" && mkdir -p "$OUT"
find "$BUILD" -name '*.dll' -exec cp {} "$OUT/" \;
"$TRIPLE-strip" "$OUT"/*.dll
ls -l "$OUT"
"$PYTHON" -I "$root/tools/win9x/check-imports.py" "$W98SYS" "$OUT"/*.dll
