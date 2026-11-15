#!/usr/bin/env python3
"""Disassemble one function of a PE image with names from its PDB's public symbols.
  resym.py url IMAGE              print the symbol server URL of the image's PDB
  resym.py map IMAGE PDB          write IMAGE.map (address name), from publics and imports
  resym.py dis IMAGE NAME [N]     disassemble NAME (to the next symbol, or N bytes), calls named
"""
import struct, subprocess, sys, re, os, bisect

def pe(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    nsec, = struct.unpack_from("<H", d, e + 6)
    optsz, = struct.unpack_from("<H", d, e + 20)
    opt = e + 24
    magic, = struct.unpack_from("<H", d, opt)
    plus = magic == 0x20B
    base = struct.unpack_from("<Q" if plus else "<I", d, opt + 24 if plus else opt + 28)[0]
    ddir = opt + (112 if plus else 96)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        name = d[so + 40 * i: so + 40 * i + 8].rstrip(b"\0").decode()
        vsize, va, rsize, raw = struct.unpack_from("<IIII", d, so + 40 * i + 8)
        secs.append((name, va, vsize, raw, rsize))
    return d, base, ddir, secs, plus

def rva2off(secs, rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + rva - va
    raise ValueError(hex(rva))

def url(path):
    d, base, ddir, secs, plus = pe(path)
    dbg_rva, dbg_size = struct.unpack_from("<II", d, ddir + 8 * 6)
    off = rva2off(secs, dbg_rva)
    for i in range(dbg_size // 28):
        typ, size, rva, ptr = struct.unpack_from("<IIII", d, off + 28 * i + 12)
        if typ == 2 and d[ptr:ptr + 4] == b"RSDS":
            g = d[ptr + 4: ptr + 20]
            age, = struct.unpack_from("<I", d, ptr + 20)
            name = d[ptr + 24: d.index(b"\0", ptr + 24)].decode()
            a, b, c = struct.unpack_from("<IHH", g, 0)
            guid = "%08X%04X%04X%s" % (a, b, c, g[8:].hex().upper())
            print("https://msdl.microsoft.com/download/symbols/%s/%s%X/%s" % (name, guid, age, name))
            return
    print("no RSDS record")

def omap(pdb):
    """For a module whose code was rearranged after linking (most of Windows' big ones): the
    section starts the symbols refer to, and the table from those addresses to the final ones.
    None for a module that was left as linked."""
    txt = subprocess.run(["llvm-pdbutil", "dump", "--streams", pdb], capture_output=True, text=True).stdout
    idx = {}
    for line in txt.splitlines():
        m = re.search(r"Stream\s+(\d+) .*\[(Section Header Original Data|Omap From Source Data)\]", line)
        if m:
            idx[m.group(2)] = m.group(1)
    if len(idx) != 2:
        return None
    raw = {}
    for name, n in idx.items():
        tmp = pdb + ".stream" + n
        subprocess.run(["llvm-pdbutil", "export", "--stream=" + n, "--out=" + tmp, pdb], capture_output=True)
        raw[name] = open(tmp, "rb").read()
        os.remove(tmp)
    sh = raw["Section Header Original Data"]
    orig = [struct.unpack_from("<I", sh, 40 * i + 12)[0] for i in range(len(sh) // 40)]
    om = raw["Omap From Source Data"]
    table = [struct.unpack_from("<II", om, 8 * i) for i in range(len(om) // 8)]
    return orig, [t[0] for t in table], [t[1] for t in table]

def mkmap(path, pdb):
    d, base, ddir, secs, plus = pe(path)
    out = {}
    moved = omap(pdb)
    txt = subprocess.run(["llvm-pdbutil", "dump", "--publics", pdb], capture_output=True, text=True).stdout
    # lines: "  123 | S_PUB32 [size = 36] `Name`" then "      flags = function, addr = 0001:00012345"
    name = None
    for line in txt.splitlines():
        m = re.search(r"S_PUB32 \[size = \d+\] `(.*)`", line)
        if m:
            name = m.group(1)
            continue
        m = re.search(r"addr = (\d+):(\d+)", line)
        if m and name:
            seg, off = int(m.group(1)), int(m.group(2))
            if moved:
                orig, src, dst = moved
                if 1 <= seg <= len(orig):
                    a = orig[seg - 1] + off
                    i = bisect.bisect_right(src, a) - 1
                    if i >= 0 and dst[i]:
                        out[base + dst[i] + (a - src[i])] = name
            elif 1 <= seg <= len(secs):
                out[base + secs[seg - 1][1] + off] = name
            name = None
    imp = subprocess.run(["llvm-readobj", "--coff-imports", path], capture_output=True, text=True).stdout
    # Import { Name: x.dll ... ImportAddressTableRVA: 0x.. Symbol: Foo (N) ... }
    lib = None; iat = None; idx = 0
    for line in imp.splitlines():
        line = line.strip()
        if line.startswith("Name:"):
            lib = line.split(":", 1)[1].strip(); idx = 0
        elif line.startswith("ImportAddressTableRVA:"):
            iat = int(line.split(":", 1)[1].strip(), 16)
        elif line.startswith("Symbol:") and iat is not None:
            sym = line.split(":", 1)[1].strip().split(" ")[0]
            out[base + iat + idx * (8 if plus else 4)] = "__imp_" + sym
            idx += 1
    with open(path + ".map", "w") as f:
        for a in sorted(out):
            f.write("%x %s\n" % (a, out[a]))
    print(len(out), "symbols")

def dis(path, name, nbytes=None):
    syms = {}
    for line in open(path + ".map"):
        a, n = line.rstrip("\n").split(" ", 1)
        syms[int(a, 16)] = n
    addrs = sorted(syms)
    start = [a for a in addrs if syms[a] == name or syms[a].startswith(name + "@") or ("?" + name + "@") in syms[a]]
    if not start:
        cand = [syms[a] for a in addrs if name.lower() in syms[a].lower()]
        print("not found; similar:", cand[:40]); return
    start = start[0]
    if nbytes:
        stop = start + int(nbytes, 0)
    else:
        stop = addrs[addrs.index(start) + 1]
    txt = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel",
                          "--start-address=0x%x" % start, "--stop-address=0x%x" % stop, path],
                         capture_output=True, text=True).stdout
    for line in txt.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
        if not m:
            continue
        a = int(m.group(1), 16); ins = m.group(2)
        def rep(mm):
            v = int(mm.group(0), 16)
            return syms.get(v, mm.group(0))
        t = re.search(r"# 0x([0-9a-f]+)", ins)
        note = ""
        if t and int(t.group(1), 16) in syms:
            note = "   ; " + syms[int(t.group(1), 16)]
        ins2 = re.sub(r"0x[0-9a-f]{9,}", rep, ins)
        print("%x:  %s%s" % (a, ins2, note))

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "url": url(sys.argv[2])
    elif cmd == "map": mkmap(sys.argv[2], sys.argv[3])
    elif cmd == "dis": dis(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
