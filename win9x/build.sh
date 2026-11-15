#!/bin/sh
# Build the Windows 9x display driver on the Linux build host (docs/win9x/plan.md, Toolchain).
#   win9x/build.sh          build/win9x/ripple9x.drv, ripplevd.vxd, ripple9x.sym, ripple9x.inf
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
         ddk98/ddk/bin/link.exe ddk98/ddk/inc/win98/vmm.inc \
         openwatcom/binl64/wrc; do
    [ -e "$T/$f" ] || { echo "build.sh: $T/$f is missing (see docs/win9x/plan.md)" >&2; exit 1; }
done

CDEF= ADEF=
[ -z "${TRACE:-}" ] || { CDEF=/DTRACE; ADEF=-DTRACE; }
B=$OUT/obj-display
rm -rf "$B" && mkdir -p "$B"
cp "$here"/display/*.c "$here"/display/*.h "$here"/display/*.asm "$here"/display/*.def "$here"/display/*.rc "$B/"
cd "$B"

# C: small model, SS != DS (w), 386 code, no stack probes, byte-packed structures, no library
# names in the objects. Warnings are errors: a call into the C run-time would not link.
WINEPATH="$(W "$T/d16/bin")" wine cl16.exe /nologo /c /W3 /WX /G3 /Gs /Zp1 /Zl /ASw /Os $CDEF enable.c
WINEPATH="$(W "$T/ddk98/ddk/bin/win98")" wine ml.exe -nologo -c -Cp $ADEF asm.asm

# link16 answers its prompts from this file: objects, output, map, libraries, definitions.
printf 'enable.obj +\r\nasm.obj\r\nripple9x.drv\r\nripple9x.map\r\nlibw.lib dibeng.lib\r\nripple9x.def\r\n' > link.rsp
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
WINEPATH="$(W "$T/ddk98/ddk/bin/win98")" wine ml.exe -nologo -coff -DBLD_COFF -DMASM6 -DMINIVDD=1 -W2 -c -Cx \
    "-I$(W "$T/ddk98/ddk/inc/win98")" ripplevd.asm
WINEPATH="$(W "$T/ddk98/ddk/bin")" wine link.exe /NOLOGO /VXD /NOD /IGNORE:4078 /IGNORE:4039 \
    /OUT:ripplevd.vxd /MAP:ripplevd.map /DEF:ripplevd.def ripplevd.obj > link.log 2>&1 || { cat link.log; exit 1; }
cp ripplevd.vxd "$OUT/"
cd "$B"
[ ! -f ripple9x.sym ] || cp ripple9x.sym "$OUT/"
sed 's/$/\r/' "$here/inf/ripple9x.inf" > "$OUT/ripple9x.inf"
ls -l "$OUT"/ripple9x.* "$OUT"/ripplevd.vxd
