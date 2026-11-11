# Disassemble functions of a thin x86_64 Mach-O with llvm-objdump and annotate what it can't:
# calls through dyld stubs get the imported symbol's name, and RIP-relative references to C
# strings and CFStrings get the string. For reading Apple binaries on dev.lan (never committed).
#   .venv/bin/python -I tools/re/dis.py BINARY SYMBOL [SYMBOL...]
#   .venv/bin/python -I tools/re/dis.py BINARY --refs STRING   functions that reference STRING
# (M5 R1.)
import re
import struct
import subprocess
import sys


def load(path):
    d = open(path, "rb").read()
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<IiiIIIII", d, 0)
    if magic != 0xFEEDFACF:
        sys.exit("not a thin 64-bit Mach-O (use llvm-lipo -thin x86_64 first)")
    off = 32
    secs, symtab, dysymtab = [], None, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, off)
        if cmd == 0x19:                                   # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", d, off + 64)[0]
            for i in range(nsects):
                s = off + 72 + 80 * i
                sect = d[s:s + 16].rstrip(b"\0").decode()
                seg = d[s + 16:s + 32].rstrip(b"\0").decode()
                addr, sz, fo = struct.unpack_from("<QQI", d, s + 32)
                flags, r1, r2 = struct.unpack_from("<III", d, s + 64)
                if flags & 0xFF != 0x8:                   # only S_SYMBOL_STUBS need r1/r2
                    r1 = r2 = 0
                secs.append((seg, sect, addr, sz, fo, r1, r2))
        elif cmd == 0x2:                                  # LC_SYMTAB
            symtab = struct.unpack_from("<IIII", d, off + 8)
        elif cmd == 0xB:                                  # LC_DYSYMTAB
            dysymtab = struct.unpack_from("<18I", d, off + 8)
        off += size
    return d, secs, symtab, dysymtab


def stub_names(d, secs, symtab, dysymtab):
    symoff, nsyms, stroff, strsize = symtab
    indoff, nind = dysymtab[12], dysymtab[13]
    names = {}
    for seg, sect, addr, sz, fo, r1, r2 in secs:
        if r2:
            for i in range(sz // r2):
                idx = struct.unpack_from("<I", d, indoff + 4 * (r1 + i))[0]
                if idx & 0xC0000000:
                    continue
                n_strx = struct.unpack_from("<I", d, symoff + 16 * idx)[0]
                end = d.index(b"\0", stroff + n_strx)
                names[addr + i * r2] = d[stroff + n_strx:end].decode()
    return names


def file_off(secs, va):
    for seg, sect, addr, sz, fo, r1, r2 in secs:
        if addr <= va < addr + sz and fo:
            return fo + va - addr, sect
    return None, None


def cstr(d, fo):
    end = d.index(b"\0", fo)
    return d[fo:end].decode("latin-1")


def describe(d, secs, va):
    fo, sect = file_off(secs, va)
    if fo is None:
        return None
    if sect == "__cstring":
        return '"%s"' % cstr(d, fo)
    if sect == "__cfstring":
        ptr, length = struct.unpack_from("<QQ", d, fo + 16)
        pfo, _ = file_off(secs, ptr)
        return '@"%s"' % (d[pfo:pfo + length].decode("latin-1") if pfo is not None else "?")
    return None


def disassemble(path, symbols):
    d, secs, symtab, dysymtab = load(path)
    stubs = stub_names(d, secs, symtab, dysymtab)
    for sym in symbols:
        out = subprocess.run(["llvm-objdump-19", "-d", "--no-show-raw-insn",
                              "--disassemble-symbols=" + sym, path],
                             capture_output=True, text=True).stdout
        for line in out.splitlines()[4:]:
            m = re.search(r"call[q]?\s+0x([0-9a-f]+)", line)
            if m and int(m.group(1), 16) in stubs:
                line = re.sub(r"<[^>]*>", "", line).rstrip() + "  -> " + stubs[int(m.group(1), 16)]
            m = re.search(r"## 0x([0-9a-f]+)", line)
            if m:
                s = describe(d, secs, int(m.group(1), 16))
                if s:
                    line += "  " + s
            print(line)


def refs(path, text):
    """Functions whose code takes the address of a C string or CFString containing TEXT."""
    d, secs, symtab, dysymtab = load(path)
    targets = set()
    for seg, sect, addr, sz, fo, r1, r2 in secs:
        if sect == "__cstring":
            for m in re.finditer(re.escape(text.encode()), d[fo:fo + sz]):
                start = d.rfind(b"\0", fo, fo + m.start()) + 1
                targets.add(addr + start - fo)
    for seg, sect, addr, sz, fo, r1, r2 in secs:
        if sect == "__cfstring":
            for i in range(0, sz, 32):
                ptr = struct.unpack_from("<Q", d, fo + i + 16)[0]
                if ptr in targets:
                    targets.add(addr + i)
    out = subprocess.run(["llvm-objdump-19", "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True).stdout
    func = None
    hits = {}
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.*)>:", line)
        if m:
            func = m.group(1)
            continue
        m = re.search(r"## 0x([0-9a-f]+)", line)
        if m and int(m.group(1), 16) in targets:
            hits.setdefault(func, describe(d, secs, int(m.group(1), 16)))
    for f, s in sorted(hits.items(), key=lambda x: str(x[0])):
        print(f, s)


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[2] == "--refs":
        refs(sys.argv[1], sys.argv[3])
    elif len(sys.argv) >= 3:
        disassemble(sys.argv[1], sys.argv[2:])
    else:
        sys.exit("usage: dis.py BINARY SYMBOL... | dis.py BINARY --refs STRING")
