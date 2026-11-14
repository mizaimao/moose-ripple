# Shared helpers: find the guest's loaded kernel modules through the debug stub.
import gdb, struct, re
inf = gdb.selected_inferior()
def mem(a, n):
    return bytes(inf.read_memory(a, n))
def rq(a):
    return struct.unpack("<Q", mem(a, 8))[0]
def rd(a):
    return struct.unpack("<I", mem(a, 4))[0]
def rw(a):
    return struct.unpack("<H", mem(a, 2))[0]
def reg(name):
    return int(gdb.parse_and_eval("$" + name)) & 0xFFFFFFFFFFFFFFFF

def nt_base():
    txt = gdb.execute("monitor info registers", to_string=True)
    m = re.search(r"IDT=\s*([0-9a-f]+)", txt)
    idt = int(m.group(1), 16)
    e = mem(idt, 16)
    lo, sel, ist, typ, mid, hi = struct.unpack("<HHBBHI", e[:12])
    handler = lo | (mid << 16) | (hi << 32)
    a = handler & ~0xFFF
    for i in range(0x1000):
        try:
            if mem(a, 2) == b"MZ":
                pe = rd(a + 0x3C)
                if pe < 0x400 and mem(a + pe, 4) == b"PE\0\0":
                    return a
        except gdb.MemoryError:
            pass
        a -= 0x1000
    return None

def find_modules():
    base = nt_base()
    if base is None:
        return None
    pe = rd(base + 0x3C)
    size = rd(base + pe + 24 + 56)
    # the debugger data block: its header has the tag "KDBG" at offset 0x10
    head = None
    a = base
    while a < base + size and head is None:
        try:
            page = mem(a, 0x1000)
        except gdb.MemoryError:
            a += 0x1000
            continue
        i = page.find(b"KDBG")
        while i >= 0:
            blk = a + i - 0x10
            try:
                kb = rq(blk + 0x18)
                if kb == base:
                    head = rq(blk + 0x48)
                    break
            except gdb.MemoryError:
                pass
            i = page.find(b"KDBG", i + 1)
        a += 0x1000
    if head is None:
        return None
    mods = {}
    e = rq(head)
    guard = 0
    while e != head and guard < 400:
        guard += 1
        b = rq(e + 0x30)
        sz = rd(e + 0x40)
        ln = rw(e + 0x58)
        buf = rq(e + 0x60)
        try:
            nm = mem(buf, ln).decode("utf-16-le", "replace").lower() if ln and buf else "?"
        except gdb.MemoryError:
            nm = "?%x" % b
        mods[nm] = (b, sz)
        e = rq(e)
    return mods
