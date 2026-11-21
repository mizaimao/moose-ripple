#!/bin/sh
# Cross-build Mesa 24.3.4 for Windows 7 x64 on Linux (docs/windows/mesa-win7.md): software
# rendering only (softpipe), Mesa's OpenGL (opengl32.dll + libgallium_wgl.dll) and the
# Direct3D 10 driver (libgallium_d3d10.dll).
#   windows/mesa/build-win7.sh [--clean]            x64 (ARCH=x86 for 32-bit Windows;
#                                                   VARIANT=virgl for the virtio-gpu driver)
# Its own source tree and build directory, apart from the Mac side's:
#   .tools/mesa/src-24.3.4-win[-virgl]   the release tarball, the patches, WDK 7.1 DDI headers
#   .tools/mesa/build-win7-ARCH[-virgl]  the build; .tools/mesa/out-win7-ARCH[-virgl] the DLLs
# Patches: patches/ for both variants, for virgl also patches-virgl/, all applied in file-name
# order; then, for virgl, the files in overlay-virgl/ (our kernel bridge) copied over the tree.
# Toolchain: llvm-mingw (msvcrt variant, as Windows 7 has no UCRT) in .tools/llvm-mingw. Its
# clang 23 makes incompatible pointer types an error, which Mesa 24.3's SPIR-V code trips on
# MinGW (setjmp buffer): kept a warning.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
MAIN=${MAIN:-$root}
T=$MAIN/.tools
TARGET=${RIPPLE_TARGET:-win7}
case $TARGET in
    win7) SRC_PLATFORM=win ;;
    vista) SRC_PLATFORM=vista ;;
    *) echo "Unsupported RIPPLE_TARGET: $TARGET" >&2; exit 1 ;;
esac
MESA_VERSION=24.3.4
MESA_SHA256=e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025
TARBALL=$T/src/mesa-dl/mesa-$MESA_VERSION.tar.xz
ARCH=${ARCH:-x64}
case $ARCH in
    x64) TRIPLE=x86_64-w64-mingw32; CPU_FAMILY=x86_64; CPU=x86_64 ;;
    x86) TRIPLE=i686-w64-mingw32;   CPU_FAMILY=x86;    CPU=i686 ;;
    *) echo "build-win7.sh: ARCH is x64 or x86" >&2; exit 1 ;;
esac
# VARIANT=sw: software rendering (softpipe), Mesa's own DLL names. VARIANT=virgl: the virgl
# driver over virtio-gpu through the WDDM kernel driver (patches 0001-0005, from the Mesa side of
# virtio-win pull request 943), as viogpu_wgl.dll and viogpu_d3d10.dll.
VARIANT=${VARIANT:-sw}
case $VARIANT in
    sw)    DRIVERS=softpipe; NAMES=; SUFFIX= ;;
    virgl) DRIVERS=virgl; NAMES="-Dgallium-wgl-dll-name=viogpu_wgl -Dgallium-d3d10-dll-name=viogpu_d3d10"; SUFFIX=-virgl ;;
    *) echo "build-win7.sh: VARIANT is sw or virgl" >&2; exit 1 ;;
esac
SRC=$T/mesa/src-$MESA_VERSION-$SRC_PLATFORM$SUFFIX
PATCHDIRS="$here/patches"
OVERLAY=
[ $VARIANT = sw ] || { PATCHDIRS="$PATCHDIRS $here/patches-virgl"; OVERLAY=$here/overlay-virgl; }
if [ "$TARGET" = vista ]; then
    [ "$VARIANT" = virgl ] || { echo "Vista supports VARIANT=virgl" >&2; exit 1; }
    PATCHDIRS="$PATCHDIRS $here/patches-vista"
fi
BUILD=$T/mesa/build-$TARGET-$ARCH$SUFFIX
OUT=$T/mesa/out-$TARGET-$ARCH$SUFFIX
LLVM_MINGW=${LLVM_MINGW:-$T/llvm-mingw/llvm-mingw-20261104-msvcrt-ubuntu-22.04-x86_64}
KIT=${KIT:-$T/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538}
export PATH="$LLVM_MINGW/bin:$MAIN/.venv/bin:$T/gnu/bin:$PATH"

[ -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" ] || { echo "build-win7.sh: no llvm-mingw at $LLVM_MINGW" >&2; exit 1; }
[ -d "$KIT/inc/api" ] || { echo "build-win7.sh: no WDK 7.1 at $KIT" >&2; exit 1; }
if [ ! -f "$TARBALL" ]; then
    mkdir -p "$(dirname "$TARBALL")"
    curl -sfLo "$TARBALL" "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz"
fi
echo "$MESA_SHA256  $TARBALL" | sha256sum -c --quiet

# Source tree: unpacked again when the patches change or with --clean.
stamp=$( (echo "$MESA_SHA256"; for d in $PATCHDIRS; do cat "$d"/*.patch 2>/dev/null; done
        [ -z "$OVERLAY" ] || (cd "$OVERLAY" && find . -type f | LC_ALL=C sort | xargs cat)) | sha256sum | cut -c1-16)
if [ "${1:-}" = --clean ] || [ "$(cat "$SRC/.ripple-win-stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$SRC" "$BUILD"
    mkdir -p "$SRC"
    tar xJf "$TARBALL" -C "$SRC" --strip-components=1
    for p in $(for d in $PATCHDIRS; do ls "$d"/*.patch 2>/dev/null; done | awk -F/ '{print $NF "\t" $0}' | sort | cut -f2); do
        patch -d "$SRC" -p1 --quiet < "$p"
    done
    # virgl: our own files (the kernel bridge, windows/mesa/overlay-virgl) copied over the tree.
    [ -z "$OVERLAY" ] || cp -R "$OVERLAY"/. "$SRC"/
    # The DDI headers Mesa's d3d10umd wants in include/winddk (its README.txt), from WDK 7.1:
    # copied into the build's own tree, never into the repository.
    # Not dxgiformat.h and dxgitype.h: MinGW has newer ones, and WDK 7.1's would hide them from
    # code that needs DXGI 1.2 and later (the WGL target's DXGI framebuffer).
    for f in api/d3d10TokenizedProgramFormat.hpp api/d3d10umddi.h api/d3d11TokenizedProgramFormat.hpp \
             api/d3dkmddi.h ddk/d3dkmdt.h api/d3dkmthk.h ddk/d3dukmdt.h api/d3dumddi.h api/dxgiddi.h \
             api/dxmini.h ddk/wmidata.h; do
        base=$(basename "$f")
        cp "$KIT/inc/$f" "$SRC/include/winddk/$(echo "$base" | tr A-Z a-z)"
    done
    # Two fixes to the local copy of d3d10umddi.h for MinGW: it pastes "MAKE_##TYPE##(", which
    # only MSVC accepts; and it defines its D3D10 constants as global const variables, which in
    # C files are duplicate symbols (with MSVC, Mesa includes d3d10_1.h first to hide them).
    sed -i -e "s/MAKE_##TYPE##(/MAKE_##TYPE(/" -e "s/^const \(UINT\|INT\) /static const \1 /" \
        "$SRC/include/winddk/d3d10umddi.h"
    # The D3D10 header declares a pointer to the newer allocation record even
    # with the Vista DDI. Only the WDDM 1.0 member of that union is used.
    if [ "$TARGET" = vista ]; then
        sed -i '/^typedef struct D3D10DDIARG_OPENRESOURCE/i typedef struct _D3DDDI_OPENALLOCATIONINFO2 D3DDDI_OPENALLOCATIONINFO2;' "$SRC/include/winddk/d3d10umddi.h"
    fi
    # d3dkmthk.h uses SAL annotations that MinGW lacks. Mesa's winddk_compat.h defines them
    # empty, but only the Direct3D 10 code includes it first; gdikmt and the virgl winsys include
    # d3dkmthk.h directly, so the local copy includes it itself.
    sed -i '1i #include "winddk_compat.h"' "$SRC/include/winddk/d3dkmthk.h"
    echo "$stamp" > "$SRC/.ripple-win-stamp"
fi

cross=$T/mesa/cross-$TARGET-$ARCH.ini
cat > "$cross" <<EOC
[binaries]
c = '$LLVM_MINGW/bin/${TRIPLE}-clang'
cpp = '$LLVM_MINGW/bin/${TRIPLE}-clang++'
ar = '$LLVM_MINGW/bin/${TRIPLE}-ar'
strip = '$LLVM_MINGW/bin/${TRIPLE}-strip'
windres = '$LLVM_MINGW/bin/${TRIPLE}-windres'
pkg-config = 'false'

[host_machine]
system = 'windows'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'
EOC
if [ "$TARGET" = vista ]; then
    cat >> "$cross" <<'EOC'

[built-in options]
cpp_args = ['-DRIPPLE_VISTA', '-DD3D_UMD_INTERFACE_VERSION=0x000C', '-DDXGKDDI_INTERFACE_VERSION=0x1053']
EOC
fi

c_args="'-Wno-error=incompatible-pointer-types','-Wno-error=missing-prototypes'"
if [ "$TARGET" = vista ]; then
    c_args="$c_args,'-DRIPPLE_VISTA','-DD3D_UMD_INTERFACE_VERSION=0x000C','-DDXGKDDI_INTERFACE_VERSION=0x1053'"
fi
if [ ! -f "$BUILD/build.ninja" ]; then
    meson setup "$BUILD" "$SRC" --cross-file "$cross" \
        --buildtype=release -Db_ndebug=true -Dprefix=/ \
        -Dplatforms=windows -Dmin-windows-version=7 \
        -Dgallium-drivers=$DRIVERS $NAMES -Dvulkan-drivers= -Dgallium-d3d10umd=true \
        -Dopengl=true -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled \
        -Dgbm=disabled -Dllvm=disabled -Dzstd=disabled -Dzlib=disabled -Dexpat=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
        -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dgallium-va=disabled \
        -Dvideo-codecs= -Dshared-glapi=enabled \
        "-Dc_args=[$c_args]"
fi
if [ "$TARGET" = vista ]; then
    dll=src/gallium/targets/d3d10umd/viogpu_d3d10.dll
    ninja -C "$BUILD" "$dll"
    mkdir -p "$OUT"
    cp "$BUILD/$dll" "$OUT/"
    exit
fi
ninja -C "$BUILD"
rm -rf "$OUT" && mkdir -p "$OUT"
find "$BUILD" -name '*.dll' -exec cp {} "$OUT/" \;
ls -l "$OUT"
