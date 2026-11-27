"""Cross-compile the NT5 x86 driver and its diagnostic programs with LLVM."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


source = Path(__file__).resolve().parent
repo = source.parent
kit = Path(os.environ.get("WIN9X_TOOLS", repo / ".tools/win9x")).resolve()
ddk = kit / "ddk2k/ddk"
vc_headers = kit / "vc6/VC98/Include"
ddk_headers = ddk / "inc/ddk"
video_headers = ddk / "src/video/inc"
public_headers = ddk / "inc"
# Direct3D 8 and 9 application headers for the test programs: the Windows 7
# WDK's copies of Microsoft's DirectX headers.
wdk = Path(os.environ.get("WDK_ROOT", repo / ".tools/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538")).resolve()
dest = Path(os.environ.get("OUT", repo / "build/win2k")).resolve()
stamp = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
if not 0 <= stamp <= 0xffffffff:
    raise SystemExit("SOURCE_DATE_EPOCH is outside the PE timestamp range")
if "LLVM_MINGW" in os.environ:
    toolchain = Path(os.environ["LLVM_MINGW"]).resolve()
else:
    installs = sorted(p for p in (repo / ".tools/llvm-mingw").glob("llvm-mingw-*-msvcrt-*") if p.is_dir())
    if len(installs) != 1:
        raise SystemExit("Set LLVM_MINGW to the msvcrt LLVM toolchain directory")
    toolchain = installs[0]
bin_dir = toolchain / "bin"


def header_overlay(work):
    # DDK includes use Windows filename casing. Clang also needs an explicit
    # byte operand size in one x86 inline assembler instruction in ntddk.h.
    roots = []
    for directory in (ddk_headers, public_headers, video_headers, vc_headers):
        files = []
        for header in sorted(directory.iterdir()):
            if not header.is_file():
                continue
            mapped = header
            if header == ddk_headers / "ntddk.h":
                data = header.read_bytes()
                instruction = b"movzx eax, _PCR KPCR.Number"
                if data.count(instruction) == 1:
                    mapped = work / "ntddk-clang.h"
                    mapped.write_bytes(data.replace(instruction, b"movzx eax, byte ptr _PCR KPCR.Number"))
            files.append({"type": "file", "name": header.name, "external-contents": str(mapped)})
        roots.append({"type": "directory", "name": str(directory), "contents": files})
    path = work / "ddk.json"
    path.write_text(json.dumps({"version": 0, "case-sensitive": False, "roots": roots}))
    return path


def build_kernel(work, overlay, target, modules, includes, definitions, libraries, link_flags):
    compiler = [bin_dir / "clang", "-target", "i686-pc-windows-msvc", "-std=gnu89", "-O2",
                "-ffreestanding", "-fno-stack-protector", "-fms-extensions", "-fms-compatibility",
                "-fms-compatibility-version=12", "-mno-sse", "-mno-sse2", "-mno-mmx",
                "-Xclang", "-fdefault-calling-conv=stdcall", "-ivfsoverlay", overlay,
                "-Wno-ignored-pragmas", "-Wno-pragma-pack", "-Wno-microsoft-anon-tag",
                "-Wno-ignored-attributes", "-D_X86_=1", "-D_WIN32_WINNT=0x0500"]
    common = [source / "compat", repo / "third_party/virtio", repo / "third_party/virgl", repo / "win9x/minivdd"]
    compiler += definitions + ["-I" + str(path) for path in common + includes]
    objects = []
    for module in modules.split():
        obj = work / (module + ".obj")
        subprocess.run(compiler + ["-c", source / (module + ".c"), "-o", obj], check=True)
        objects.append(obj)
    subprocess.run([bin_dir / "lld-link", "/machine:x86", "/subsystem:native,5.00", "/osversion:5.0",
                    "/nodefaultlib", "/safeseh:no", f"/timestamp:{stamp}", "/release",
                    "/libpath:" + str(ddk / "libfre/i386"), "/out:" + str(work / target)]
                   + ([f"/map:{work / target}.map"] if os.environ.get("RIPPLE_LINK_MAP") else [])
                   + link_flags + objects + libraries, check=True)


def stamp_program(path):
    # The MinGW linker accepts --no-insert-timestamp. Preserve the caller's
    # SOURCE_DATE_EPOCH in both timestamps and the PE checksum when supplied.
    if not stamp:
        return
    data = bytearray(path.read_bytes())
    pe, = struct.unpack_from("<I", data, 60)
    optional = pe + 24
    struct.pack_into("<I", data, pe + 8, stamp)
    export, = struct.unpack_from("<I", data, optional + 96)
    if export:
        count, = struct.unpack_from("<H", data, pe + 6)
        size, = struct.unpack_from("<H", data, pe + 20)
        for n in range(count):
            rva, length, offset = struct.unpack_from("<III", data, optional + size + 40 * n + 12)
            if rva <= export and export + 8 <= rva + length:
                struct.pack_into("<I", data, offset + export - rva + 4, stamp)
                break
        else:
            raise ValueError(f"{path.name}: export directory outside file-backed sections")
    checksum = optional + 64
    if struct.unpack_from("<I", data, checksum)[0]:
        struct.pack_into("<I", data, checksum, 0)
        total = sum(word for word, in struct.iter_unpack("<H", data + bytes(len(data) % 2)))
        while total >> 16:
            total = (total & 0xffff) + (total >> 16)
        struct.pack_into("<I", data, checksum, total + len(data))
    path.write_bytes(data)


def build_programs(work):
    exports = work / "coinstaller.def"
    exports.write_text("EXPORTS\nRippleInstall=RippleInstall@16\n")
    targets = [
        ("nt5inst.exe", ["install"], ["-lsetupapi"]),
        ("rbntmode.dll", ["setupmode"], ["-shared", exports, "-lsetupapi", "-lole32", "-ladvapi32", "-luser32"]),
        ("w2kprobe.exe", ["tools/w2kprobe", "tools/ddraw_probe", "tools/window_probe", "tools/flip_probe"],
         ["-lsetupapi", "-lcfgmgr32", "-luser32", "-lgdi32"]),
        ("ddbench.exe", ["tools/ddraw_bench"], ["-mwindows", "-lgdi32", "-luser32"]),
        ("flipcycle.exe", ["tools/flipcycle"], ["-mwindows", "-lgdi32", "-luser32"]),
        ("memtest.exe", ["tools/memtest"], ["-mwindows", "-lgdi32", "-luser32"]),
        ("regmode.exe", ["tools/regmode"], ["-mwindows", "-luser32"]),
        ("rvdstat.exe", ["tools/rvdstat"], ["-lgdi32", "-luser32"]),
        ("d3d7probe.exe", ["tools/d3d7probe"], ["-lddraw", "-ldxguid", "-lgdi32", "-luser32"]),
        ("ipixels.exe", ["tools/d3d7probe"], ["-DRB_D3D7_INDEXED_PROBE", "-lddraw", "-ldxguid", "-lgdi32", "-luser32"]),
        ("z16pixels.exe", ["tools/z16pixels"], ["-lddraw", "-ldxguid", "-lgdi32", "-luser32"]),
        ("tpixels.exe", ["tools/tpixels"], ["-lddraw", "-ldxguid", "-lgdi32", "-luser32"]),
        ("tlight.exe", ["tools/tlight"], ["-lddraw", "-ldxguid", "-luser32"]),
        ("tfog.exe", ["tools/tfog"], ["-lddraw", "-ldxguid", "-luser32"]),
        ("tstage.exe", ["tools/tstage"], ["-lddraw", "-ldxguid", "-luser32"]),
        ("tmip.exe", ["tools/tmip"], ["-lddraw", "-ldxguid", "-luser32"]),
        ("d7draw.exe", ["tools/d7draw"], ["-lgdi32"]),
        ("rbskips.exe", ["tools/rbskips"], ["-lgdi32", "-luser32"]),
        ("zstore.exe", ["tools/zstore"], ["-lddraw", "-ldxguid", "-lgdi32", "-luser32"]),
        ("glpresent.exe", ["tools/glpresent"], ["-lopengl32", "-lgdi32", "-luser32"]),
        ("cursorqa.exe", ["tools/cursorqa"], ["-lgdi32", "-luser32"]),
        ("rundiag.exe", ["tools/rundiag"], ["-luser32"]),
        ("guiclick.exe", ["tools/guiclick"], ["-luser32"]),
        ("scap.exe", ["tools/scap"], ["-lgdi32", "-luser32"]),
        ("d9asm.exe", ["tools/d9asm"], []),
    ]
    # Only Microsoft's headers declare the Direct3D 8 interfaces here.
    d3d = work / "d3d-include"
    d3d.mkdir()
    for name in ("d3d8.h", "d3d8types.h", "d3d8caps.h", "d3d9.h", "d3d9types.h", "d3d9caps.h"):
        (d3d / name).write_bytes((wdk / "inc/api" / name).read_bytes())
    targets.append(("d8pixels.exe", ["tools/d8pixels"], ["-I", d3d, "-luser32", "-lgdi32"]))
    targets.append(("d9pixels.exe", ["tools/d9pixels"], ["-I", d3d, "-luser32", "-lgdi32"]))
    targets.append(("d9texload.exe", ["tools/d9texload"], ["-I", d3d, "-luser32"]))
    targets.append(("d9stencil.exe", ["tools/d9stencil"], ["-I", d3d, "-luser32"]))
    targets.append(("d9plain.exe", ["tools/d9plain"], ["-I", d3d, "-luser32"]))
    targets.append(("d9full.exe", ["tools/d9full"], ["-I", d3d, "-luser32"]))
    # Logging stand-ins for d3d9.dll and d3d8.dll; the exports they do not handle go to d3d9real.dll
    # and d3d8real.dll.
    trace = work / "d9trace.def"
    trace.write_text("LIBRARY d3d9\nEXPORTS\nDirect3DCreate9=Direct3DCreate9@4\n" + "".join(
        f"{name}=d3d9real.{name}\n" for name in (
            "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame",
            "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion", "DebugSetLevel", "DebugSetMute",
            "Direct3DShaderValidatorCreate9", "PSGPError", "PSGPSampleTexture")))
    targets.append(("d9trace.dll", ["tools/d9trace"], ["-shared", trace, "-I", d3d, "-luser32"]))
    trace8 = work / "d8trace.def"
    trace8.write_text("LIBRARY d3d8\nEXPORTS\nDirect3DCreate8=Direct3DCreate8@4\n" + "".join(
        f"{name}=d3d8real.{name}\n" for name in ("DebugSetMute", "ValidatePixelShader", "ValidateVertexShader")))
    targets.append(("d8trace.dll", ["tools/d8trace"], ["-shared", trace8, "-I", d3d, "-luser32"]))
    args = [bin_dir / "i686-w64-mingw32-clang", "-O2", "-D_WIN32_WINNT=0x0500",
            "-I", source, "-I", repo / "win9x/minivdd",
            "-Wl,--major-subsystem-version,5,--minor-subsystem-version,0,--major-os-version,5,--minor-os-version,0,--no-insert-timestamp"]
    for target, modules, flags in targets:
        output = work / target
        subprocess.run(args + [source / (module + ".c") for module in modules] + flags + ["-o", output], check=True)
        stamp_program(output)
    return [target for target, _, _ in targets]


def main():
    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".obj.", dir=dest) as directory:
        work = Path(directory)
        overlay = header_overlay(work)
        build_kernel(work, overlay, "rb2kmp.sys",
                     "rb2kmp gpu_transport gpu_pixel gpu_triangle gpu_present gpu_surface gpu_rvd gpu_memory gpu_view gpu_cursor gpu_primary",
                     [ddk_headers, video_headers, public_headers, vc_headers], ["-Di386=1"],
                     ["videoprt.lib", "ntoskrnl.lib", "hal.lib"], ["/driver", "/entry:DriverEntry@8"])
        build_kernel(work, overlay, "rb2kdisp.dll", "display display_ddraw display_palette display_rvd display_gdi display_cursor display_d3d7 d3d8_ps d3d9_decl",
                     [public_headers, video_headers, vc_headers],
                     ["-DUNICODE", "-D_UNICODE", "-DWINVER=0x0500", "-DRB_PS2_SAMPLERS=16",
                      # The skip counts query of the test tools (rbskips.h); DEVCHECKS=0 leaves it out, as release packages do.
                      "-DRB2K_DEVCHECKS=" + ("0" if os.environ.get("DEVCHECKS") == "0" else "1")],
                     ["win32k.lib", "libcntpr.lib"], ["/dll", "/entry:DrvEnableDriver@12",
                                                      "/export:DrvEnableDriver=_DrvEnableDriver@12"])
        programs = build_programs(work)
        (work / "rb2k.inf").write_bytes((source / "rb2k.inf").read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        for target in ["rb2kmp.sys", "rb2kdisp.dll", "rb2k.inf", *programs]:
            (work / target).replace(dest / target)
        if os.environ.get("RIPPLE_LINK_MAP"):
            # Link maps of the two drivers, to place guest samples (RIPPLE_LINK_MAP=1).
            for target in ["rb2kmp.sys.map", "rb2kdisp.dll.map"]:
                (work / target).replace(dest / target)


if __name__ == "__main__":
    main()
