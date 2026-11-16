#!/usr/bin/env python3
# Kernel memory out of a Windows 7 x64 kernel crash dump (C:\Windows\MEMORY.DMP, the "kernel
# memory dump" kind: PAGEDU64 with a page bitmap), for a bluescreen whose exception and
# registers a small dump does not hold (they lie below the saved stack):
#   kmem.py MEMORY.DMP crash            the exception record and registers the bugcheck points to
#   kmem.py MEMORY.DMP q ADDR [COUNT]   64-bit words at a kernel address (hex)
# Name the faulting address with the module's symbol map (docs/windows/test-vms.md, "Crash dumps").
# Standard library only. MIT licence (LICENSE at the repository root).
import struct, sys
path = sys.argv[1]
f = open(path, "rb")
hdr = f.read(0x2000)
assert hdr[:8] == b"PAGEDU64"
cr3 = struct.unpack_from("<Q", hdr, 0x10)[0]
params = struct.unpack_from("<4Q", hdr, 0x40)
s = f.read(0x28)
assert s[:8] == b"SDMPDUMP", s[:8]
# Windows 7: header size, bitmap size in bits and page count as 32-bit words, then an RTL_BITMAP
# (size, buffer pointer) and the bitmap itself at 0x2028
header_size, bitmap_bits, pages = struct.unpack_from("<3I", s, 0xC)
f.seek(0x2028)
bitmap = f.read((bitmap_bits + 7) // 8)
# file position of every page present: count the set bits before it
index = {}
n = 0
for byte_i, b in enumerate(bitmap):
    if b:
        for bit in range(8):
            if b >> bit & 1:
                index[byte_i * 8 + bit] = n
                n += 1

def phys(pa, size):
    out = b""
    while size:
        pfn, off = pa >> 12, pa & 0xFFF
        k = min(size, 0x1000 - off)
        if pfn not in index:
            return None
        f.seek(header_size + index[pfn] * 0x1000 + off)
        out += f.read(k)
        pa += k
        size -= k
    return out

def pq(pa):
    d = phys(pa, 8)
    return None if d is None else struct.unpack("<Q", d)[0]

def v2p(va):
    e = pq((cr3 & ~0xFFF) + ((va >> 39) & 0x1FF) * 8)
    if not e or not e & 1: return None
    e = pq((e & 0xFFFFFFFFFF000) + ((va >> 30) & 0x1FF) * 8)
    if not e or not e & 1: return None
    if e & 0x80: return (e & 0xFFFFFC0000000) + (va & 0x3FFFFFFF)
    e = pq((e & 0xFFFFFFFFFF000) + ((va >> 21) & 0x1FF) * 8)
    if not e or not e & 1: return None
    if e & 0x80: return (e & 0xFFFFFFFE00000) + (va & 0x1FFFFF)
    e = pq((e & 0xFFFFFFFFFF000) + ((va >> 12) & 0x1FF) * 8)
    if not e or not e & 1: return None
    return (e & 0xFFFFFFFFFF000) + (va & 0xFFF)

def virt(va, size):
    out = b""
    while size:
        k = min(size, 0x1000 - (va & 0xFFF))
        pa = v2p(va)
        if pa is None: return None
        d = phys(pa, k)
        if d is None: return None
        out += d
        va += k
        size -= k
    return out

def q(va, count=1):
    d = virt(va, 8 * count)
    return None if d is None else struct.unpack("<%dQ" % count, d)

if sys.argv[2] == "crash":
    exr = virt(params[2], 0x98)
    ctx = virt(params[3], 0x100)
    if exr:
        c, fl, _, addr, n = struct.unpack_from("<IIQQI", exr, 0)
        info = struct.unpack_from("<%dQ" % min(n, 15), exr, 0x20)
        print("exception %08x at %x, info %s" % (c, addr, " ".join("%x" % i for i in info)))
    if ctx:
        regs = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15 rip".split()
        vals = struct.unpack_from("<17Q", ctx, 0x78)
        for i in range(0, 17, 6):
            print("  ".join("%s=%016x" % (r, v) for r, v in list(zip(regs, vals))[i:i + 6]))
elif sys.argv[2] == "q":
    va = int(sys.argv[3], 16)
    cnt = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    for i in range(cnt):
        w = q(va + 8 * i)
        print("%x: %s" % (va + 8 * i, "????" if w is None else "%016x" % w[0]))
