#!/usr/bin/env python3
"""List what a Windows program or DLL imports that Windows 98 SE does not export.

    python -I check-imports.py SYSDIR FILE...

SYSDIR holds Windows 98 SE's own system DLLs (kernel32.dll, user32.dll, msvcrt.dll, ...), taken
from its setup cabinets. For each FILE the script prints the imported DLLs it has no copy of,
and the functions the system's copy lacks. An empty report means the loader will accept the
file; it says nothing about functions that exist in Windows 98 only as stubs (most of the
Unicode "W" functions).

Exit status: 0 when nothing is missing, 1 otherwise.
"""
import os
import struct
import sys


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError(path + ": not an executable")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(path + ": not a PE file")
        nsect, = struct.unpack_from("<H", d, pe + 6)
        optsize, = struct.unpack_from("<H", d, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from("<H", d, opt)
        self.pe32plus = magic == 0x20B
        self.subsystem_version = struct.unpack_from("<HH", d, opt + 48)
        self.os_version = struct.unpack_from("<HH", d, opt + 40)
        ddir = opt + (112 if self.pe32plus else 96)
        self.dirs = [struct.unpack_from("<II", d, ddir + 8 * i) for i in range(16)]
        self.sections = []
        s = opt + optsize
        for i in range(nsect):
            vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", d, s + 40 * i + 8)
            self.sections.append((vaddr, max(vsize, rsize), raw))

    def off(self, rva):
        for vaddr, size, raw in self.sections:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + raw
        return rva                      # headers

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin-1")

    def exports(self):
        rva, size = self.dirs[0]
        names, ordinals = set(), set()
        if not rva:
            return names, ordinals
        o = self.off(rva)
        base, nfunc, nname, afunc, aname, aord = struct.unpack_from("<IIIIII", self.d, o + 16)
        for i in range(nfunc):
            if struct.unpack_from("<I", self.d, self.off(afunc) + 4 * i)[0]:
                ordinals.add(base + i)
        for i in range(nname):
            names.add(self.cstr(struct.unpack_from("<I", self.d, self.off(aname) + 4 * i)[0]))
        return names, ordinals

    def imports(self):
        """{dll name (lower case): [function name or ordinal number, ...]}"""
        out = {}
        for rva in (self.dirs[1][0], ):
            if not rva:
                continue
            o = self.off(rva)
            while True:
                ilt, _, _, name, iat = struct.unpack_from("<IIIII", self.d, o)
                if not name:
                    break
                funcs = out.setdefault(self.cstr(name).lower(), [])
                t = self.off(ilt or iat)
                width, fmt, flag = (8, "<Q", 1 << 63) if self.pe32plus else (4, "<I", 1 << 31)
                while True:
                    v, = struct.unpack_from(fmt, self.d, t)
                    if not v:
                        break
                    funcs.append(v & 0xFFFF if v & flag else self.cstr(v + 2))
                    t += width
                o += 20
        return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sysdir = sys.argv[1]
    have = {f.lower(): os.path.join(sysdir, f) for f in os.listdir(sysdir)}
    cache = {}
    bad = 0
    for path in sys.argv[2:]:
        pe = PE(path)
        local = {f.lower() for f in os.listdir(os.path.dirname(os.path.abspath(path)))}
        lines = []
        if pe.subsystem_version > (4, 10) or pe.os_version > (4, 10):
            lines.append("  header asks for Windows %d.%d (system) / %d.%d (subsystem): 98 is 4.10"
                         % (pe.os_version + pe.subsystem_version))
        for dll, funcs in sorted(pe.imports().items()):
            if dll not in have:
                if dll not in local:
                    lines.append("  %s: not a Windows 98 SE library (%d functions)" % (dll, len(funcs)))
                continue
            if dll not in cache:
                cache[dll] = PE(have[dll]).exports()
            names, ordinals = cache[dll]
            missing = [f for f in funcs if (f not in ordinals if isinstance(f, int) else f not in names)]
            for f in sorted(missing, key=str):
                lines.append("  %s: %s" % (dll, f))
        print("%s: %s" % (path, "nothing missing" if not lines else "%d missing" % len(lines)))
        for line in lines:
            print(line)
        bad += bool(lines)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
