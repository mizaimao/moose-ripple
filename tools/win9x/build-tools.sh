#!/bin/sh
# Build the Win32 test programs for Windows 95/98 on the Linux build host, with Visual C++ 6
# under Wine (docs/win9x/plan.md, Toolchain). Output: build/win9x/tools/*.exe
#   tools/win9x/build-tools.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
T=${WIN9X_TOOLS:-$root/.tools/win9x}
OUT=${OUT:-$root/build/win9x}/tools
export WINEPREFIX=$T/wine WINEDEBUG=-all
W() { winepath -w "$1"; }
[ -e "$T/vc6/VC98/Bin/CL.EXE" ] || { echo "build-tools.sh: no Visual C++ 6 in $T/vc6" >&2; exit 1; }
mkdir -p "$OUT" && cd "$OUT"
export WINEPATH="$(W "$T/vc6/VC98/Bin");$(W "$T/vc6/Common/MSDev98/Bin")"
export INCLUDE="$(W "$T/vc6/VC98/Include")" LIB="$(W "$T/vc6/VC98/Lib")"
for src in "$here"/rvdtest.c "$here"/gltest.c "$here"/threadtest.c "$here"/pftest.c "$here"/modeset.c "$here"/ddtest.c "$here"/dbgrun.c "$here"/setini.c; do
    cp "$src" "$here/../../win9x/minivdd/rvd.h" .
    # /MT: the C run-time linked in, so the program needs nothing but Windows itself
    wine cl.exe /nologo /W3 /O2 /MT "$(basename "$src")" /link /SUBSYSTEM:CONSOLE gdi32.lib user32.lib opengl32.lib ddraw.lib
done
ls -l "$OUT"/*.exe

# ddinfo reads DirectDraw's own records: the Windows Me DDK's DirectDraw headers (ddraw.h,
# ddrawi.h; not in this repository) come first.
cp "$here"/ddinfo.c .
INCLUDE="$(W "$T/ddkme/ddk/inc/win_me");$INCLUDE" \
    wine cl.exe /nologo /W3 /O2 /MT ddinfo.c /link /SUBSYSTEM:CONSOLE ddraw.lib
ls -l "$OUT"/ddinfo.exe

# d3dtest uses Direct3D 7: the DirectX 7 SDK's headers and libraries come first.
cp "$here"/d3dtest.c .
INCLUDE="$(W "$T/dx7sdk/include");$INCLUDE" LIB="$(W "$T/dx7sdk/lib");$LIB" \
    wine cl.exe /nologo /W3 /O2 /MT d3dtest.c /link /SUBSYSTEM:CONSOLE ddraw.lib dxguid.lib user32.lib
ls -l "$OUT"/d3dtest.exe
