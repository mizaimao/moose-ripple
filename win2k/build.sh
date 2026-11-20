#!/bin/sh
# Build the Windows 2000 x86 framebuffer driver, installer, and GDI mode test
# with Visual C++ 6.0 and the Windows 2000 DDK under Wine.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
kit=$(cd "${WIN9X_TOOLS:-$repo/.tools/win9x}" && pwd)
out=${OUT:-$repo/build/win2k}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d "$out/.obj.XXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"

export WINEPREFIX="$kit/wine" WINEDEBUG=-all
ddk="$kit/ddk2k/ddk"
vc="$kit/vc6/VC98"
w() { winepath -w "$1"; }
export WINEPATH="$(w "$vc/Bin");$(w "$kit/vc6/Common/MSDev98/Bin")"
cl() { wine "$vc/Bin/CL.EXE" /nologo "$@"; }
link() { wine "$vc/Bin/LINK.EXE" /nologo "$@"; }

cp "$here/rb2kmp.c" "$here"/gpu_*.c "$here"/*.h "$here/display.c" "$here/display_ddraw.c" "$here/display_rvd.c" "$here/install.c" \
    "$here/tools/w2kprobe.c" "$here/tools/ddraw_probe.c" "$here/tools/window_probe.c" "$here/tools/flip_probe.c" "$here/tools/ddraw_bench.c" "$here/tools/rvdstat.c" .
export INCLUDE="$(w "$here/compat");$(w "$repo/third_party/virtio");$(w "$repo/third_party/virgl");$(w "$repo/win9x/minivdd");$(w "$ddk/inc/ddk");$(w "$ddk/src/video/inc");$(w "$ddk/inc");$(w "$vc/Include")"
export LIB="$(w "$ddk/libfre/i386")"
cl /c /W3 /WX /O1 /Gz /Zl /GX- /D_X86_=1 /Di386=1 /D_WIN32_WINNT=0x0500 /DWINVER=0x0500 \
    rb2kmp.c gpu_transport.c gpu_pixel.c gpu_surface.c gpu_view.c gpu_rvd.c gpu_memory.c
link /driver /subsystem:native,5.00 /osversion:5.0 /version:0.1 /machine:ix86 /nodefaultlib \
    /entry:DriverEntry@8 /out:rb2kmp.sys rb2kmp.obj gpu_transport.obj gpu_pixel.obj gpu_surface.obj gpu_view.obj gpu_rvd.obj gpu_memory.obj videoprt.lib ntoskrnl.lib

export INCLUDE="$(w "$here/compat");$(w "$repo/third_party/virgl");$(w "$repo/win9x/minivdd");$(w "$ddk/inc");$(w "$ddk/src/video/inc");$(w "$vc/Include")"
cl /c /W3 /O1 /Oi /Zl /Gz /D_X86_=1 /DWINVER=0x0500 /D_WIN32_WINNT=0x0500 /DUNICODE /D_UNICODE \
    display.c display_ddraw.c display_rvd.c
link /dll /release /nodefaultlib /machine:i386 /subsystem:native,5.00 /osversion:5.0 /version:5.00 \
    /entry:DrvEnableDriver@12 /export:DrvEnableDriver=_DrvEnableDriver@12 /out:rb2kdisp.dll \
    display.obj display_ddraw.obj display_rvd.obj win32k.lib libcntpr.lib

export INCLUDE="$(w "$vc/Include")"
export LIB="$(w "$vc/Lib");$(w "$ddk/libfre/i386")"
cl /W3 /O1 /MD install.c /Fent5inst.exe /link /subsystem:console,5.00 setupapi.lib

export INCLUDE="$(w "$ddk/inc");$(w "$vc/Include")"
export LIB="$(w "$ddk/libfre/i386");$(w "$vc/Lib")"
cl /O1 /W3 /MT w2kprobe.c ddraw_probe.c window_probe.c flip_probe.c /Few2kprobe.exe /link /subsystem:console,5.00 /version:1.0 \
    /machine:i386 setupapi.lib cfgmgr32.lib user32.lib gdi32.lib kernel32.lib
cl /O1 /W3 /MT ddraw_bench.c /Feddbench.exe /link /subsystem:windows,5.00 /version:1.0 \
    /machine:i386 user32.lib gdi32.lib kernel32.lib
export INCLUDE="$(w "$repo/win9x/minivdd");$(w "$ddk/inc");$(w "$vc/Include")"
cl /O1 /W3 /MT rvdstat.c /Fervdstat.exe /link /subsystem:console,5.00 /version:1.0 \
    /machine:i386 user32.lib gdi32.lib kernel32.lib

# VC6 stamps the COFF header and exported DLLs with the current time. Use a
# repeatable timestamp and maintain the image checksums required for drivers.
"${PYTHON:-$repo/.venv/bin/python}" - rb2kmp.sys rb2kdisp.dll nt5inst.exe w2kprobe.exe ddbench.exe rvdstat.exe <<'PY'
import os
from pathlib import Path
from struct import iter_unpack, pack_into, unpack_from
import sys

stamp = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
for filename in sys.argv[1:]:
    path = Path(filename)
    data = bytearray(path.read_bytes())
    pe, = unpack_from("<I", data, 60)
    optional = pe + 24
    assert data[pe:pe + 4] == b"PE\0\0" and unpack_from("<H", data, optional)[0] == 0x10b
    pack_into("<I", data, pe + 8, stamp)
    export, = unpack_from("<I", data, optional + 96)
    if export:
        count, = unpack_from("<H", data, pe + 6)
        size, = unpack_from("<H", data, pe + 20)
        for n in range(count):
            section = optional + size + 40 * n
            rva, length, offset = unpack_from("<III", data, section + 12)
            if rva <= export and export + 8 <= rva + length:
                pack_into("<I", data, offset + export - rva + 4, stamp)
                break
        else:
            raise ValueError(f"{path}: export directory outside file-backed sections")
    checksum = optional + 64
    if unpack_from("<I", data, checksum)[0]:
        pack_into("<I", data, checksum, 0)
        total = sum(word for word, in iter_unpack("<H", data + bytes(len(data) % 2)))
        while total >> 16:
            total = (total & 0xffff) + (total >> 16)
        pack_into("<I", data, checksum, total + len(data))
    path.write_bytes(data)
PY
cp rb2kmp.sys rb2kdisp.dll nt5inst.exe w2kprobe.exe ddbench.exe rvdstat.exe "$here/rb2k.inf" "$out/"
