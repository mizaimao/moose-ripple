#!/bin/sh
# Build the Windows 9x display driver on the Linux build host (docs/win9x/plan.md, Toolchain).
#   win9x/build.sh          build/win9x/ripple9x.drv, ripplevd.vxd, rippledd.dll, ripple9x.sym,
#                           ripple9x.inf
#   TRACE=1 win9x/build.sh  the same, with a letter per driver call written to QEMU's debug port
# Tools, all under .tools/win9x (set up once, never committed):
#   Microsoft's 16-bit C compiler and linker (Server 2003 DDK) and MASM 6.11d (98 DDK), run
#   under Wine; the 98 DDK's import libraries (libw, dibeng); Open Watcom's wrc for resources.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
T=${WIN9X_TOOLS:-$root/.tools/win9x}
OUT=${OUT:-$root/build/win9x}
export WINEPREFIX=$T/wine WINEDEBUG=-all
export WATCOM=$T/openwatcom
W() { winepath -w "$1"; }
for f in d16/bin/cl16.exe d16/bin/link16.exe ddk98/ddk/bin/win98/ml.exe ddk98/ddk/lib/win98/dibeng.lib \
         ddk98/ddk/bin/link.exe ddk98/ddk/inc/win98/vmm.inc vc6/VC98/Bin/CL.EXE \
         openwatcom/binl64/wrc; do
    [ -e "$T/$f" ] || { echo "build.sh: $T/$f is missing (see docs/win9x/plan.md)" >&2; exit 1; }
done

CDEF= ADEF= DEVDEF= DEVLINK=
[ -z "${TRACE:-}" ] || { CDEF=/DTRACE; ADEF=-DTRACE; }
[ -z "${DDTRACE:-}" ] || CDEF="$CDEF /DDDTRACE"      # DirectDraw's escapes on the debug port
# Development checks (on unless DEVCHECKS=0, which a release build sets): rippledd.dll counts what
# it skips and the pages it flips to, and exports the counts (RippleSkips and RippleFrames in
# ddraw/rippled3d.c).
[ "${DEVCHECKS:-1}" = 0 ] || { DEVDEF=/DRIPPLE_DEV_CHECKS; DEVLINK="/EXPORT:RippleSkips=_RippleSkips@8 /EXPORT:RippleFrames=_RippleFrames@0"; }
B=$OUT/obj-display
rm -rf "$B" && mkdir -p "$B"
cp "$here"/display/*.c "$here"/display/*.h "$here"/display/*.asm "$here"/display/*.def "$here"/display/*.rc "$B/"
cd "$B"

# C: small model, SS != DS (w), 386 code, no stack probes, byte-packed structures, no library
# names in the objects. Warnings are errors: a call into the C run-time would not link.
WINEPATH="$(W "$T/d16/bin")" wine cl16.exe /nologo /c /W3 /WX /G3 /Gs /Zp1 /Zl /ASw /Os $CDEF enable.c ddraw16.c
WINEPATH="$(W "$T/ddk98/ddk/bin/win98")" wine ml.exe -nologo -c -Cp $ADEF asm.asm

# link16 answers its prompts from this file: objects, output, map, libraries, definitions.
printf 'enable.obj +\r\nddraw16.obj +\r\nasm.obj\r\nripple9x.drv\r\nripple9x.map\r\nlibw.lib dibeng.lib\r\nripple9x.def\r\n' > link.rsp
LIB="$(W "$T/ddk98/ddk/lib/win98")" WINEPATH="$(W "$T/d16/bin")" \
    wine link16.exe /NOE /NOD /LI /MAP /AL:16 @link.rsp > link.log 2>&1 || { cat link.log; exit 1; }
grep -i -E "error|warning|unresolved" link.log && { cat link.log; exit 1; }

"$T/openwatcom/binl64/wrc" -q -r -bt=windows -fo=ripple9x.res ripple9x.rc
"$T/openwatcom/binl64/wrc" -q -bt=windows -31 ripple9x.res ripple9x.drv
WINEPATH="$(W "$T/ddk98/ddk/bin/win98")" wine mapsym.exe -n -o ripple9x.sym ripple9x.map >/dev/null 2>&1 || true

cp ripple9x.drv "$OUT/"

# ---- The mini-VDD: a VxD, assembled with MASM and linked with the 98 DDK's linker (/VXD).
V=$OUT/obj-minivdd
rm -rf "$V" && mkdir -p "$V"
cp "$here"/minivdd/* "$V/"
cd "$V"
# Windows 95 rejects the 98 DDK's default 4.10 VxD version. The mini-VDD uses the 4.00 interface.
WINEPATH="$(W "$T/ddk98/ddk/bin/win98")" wine ml.exe -nologo -coff -DBLD_COFF -DMASM6 -DMINIVDD=1 -DWIN40COMPAT=1 -W2 -c -Cx \
    "-I$(W "$T/ddk98/ddk/inc/win98")" ripplevd.asm
# The C part: Visual C++ 6, 32-bit flat, no run-time library, sections named as the VxD wants.
WINEPATH="$(W "$T/vc6/VC98/Bin");$(W "$T/vc6/Common/MSDev98/Bin")" \
    wine cl.exe /nologo /c /W3 /WX /O1 /Oy- /Zl /Gs $CDEF vgpu.c
WINEPATH="$(W "$T/ddk98/ddk/bin")" wine link.exe /NOLOGO /VXD /NOD /IGNORE:4078 /IGNORE:4039 \
    /OUT:ripplevd.vxd /MAP:ripplevd.map /DEF:ripplevd.def ripplevd.obj vgpu.obj > link.log 2>&1 || { cat link.log; exit 1; }
cp ripplevd.vxd "$OUT/"
cd "$B"
[ ! -f ripple9x.sym ] || cp ripple9x.sym "$OUT/"
sed 's/$/\r/' "$here/inf/ripple9x.inf" > "$OUT/ripple9x.inf"
ls -l "$OUT"/ripple9x.* "$OUT"/ripplevd.vxd

# ---- The DirectDraw HAL's 32-bit DLL: Visual C++ 6 against the Windows Me DDK's DirectDraw
# and Direct3D headers (ddraw.h, ddrawi.h, d3dhal.h; not in this repository), no C run-time,
# based above 2 GB. Direct3D draws through OpenGL (opengl32.dll, loaded when first needed).
D=$OUT/obj-ddraw
rm -rf "$D" && mkdir -p "$D"
cp "$here"/ddraw/* "$here"/display/rdd.h "$here"/minivdd/rvd.h "$here"/../win2k/d3d8ddi.h \
   "$here"/../win2k/d3d8_ps.c "$here"/../win2k/d3d8_ps.h \
   "$here"/../win2k/d3d9_decl.c "$here"/../win2k/d3d9_decl.h "$D/"
cd "$D"
(
    export INCLUDE="$(W "$T/ddkme/ddk/inc/win_me");$(W "$T/vc6/VC98/Include")" LIB="$(W "$T/vc6/VC98/Lib")"
    export WINEPATH="$(W "$T/vc6/VC98/Bin");$(W "$T/vc6/Common/MSDev98/Bin")"
    wine cl.exe /nologo /c /W3 /WX /O1 /Oy- /Zl /Gs /GF /DRB_PS2_SAMPLERS=16 $DEVDEF rippledd.c rippled3d.c tgsi_arb.c tnl_arb.c vs_arb.c d3d8_ps.c d3d9_decl.c
    wine link.exe /NOLOGO /DLL /NODEFAULTLIB /ENTRY:DllMain /BASE:0xB00B0000 /DEF:rippledd.def $DEVLINK \
        /OUT:rippledd.dll /MAP:rippledd.map rippledd.obj rippled3d.obj tgsi_arb.obj tnl_arb.obj vs_arb.obj d3d8_ps.obj d3d9_decl.obj \
        kernel32.lib user32.lib gdi32.lib advapi32.lib > link.log 2>&1 || { cat link.log; exit 1; }
)
cp rippledd.dll "$OUT/"
ls -l "$OUT"/rippledd.dll
