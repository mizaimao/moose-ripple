# gdb script: what each processor of a guest that has stopped responding is doing. For every
# processor: where it is (module, function, offset) and the code addresses that lie on its
# kernel stack, named the same way: what it was running, innermost first, with whatever older
# calls left behind in between. Used like wddump.py, through QEMU's debug stub, from the scratch
# folder that holds the guest's modules and their .map files (README.md):
#
#     WIN7_RE=$PWD gdb -q -batch -ex "set architecture i386:x86-64" \
#         -ex "target remote | ssh HOST socat - TCP:127.0.0.1:12345" -x stuck.py
#
# STUCK_WORDS (default 512) is how many 64-bit words of each stack are read. The first words are
# printed as they are too: a stop (bugcheck) keeps its code and four values there.
# MIT licence (LICENSE at the repository root).
import gdb, struct, bisect, os, sys
RE = os.environ.get("WIN7_RE", os.getcwd())
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else RE)
from guestmods import *
WORDS = int(os.environ.get("STUCK_WORDS", "512"))

mods = find_modules() or {}
have = [m for m in sorted(mods) if os.path.exists(os.path.join(RE, m)) and
        os.path.exists(os.path.join(RE, m + ".map"))]
maps = load_maps(RE, have)
if not mods:
    print("no module list found: addresses stay unnamed")
for k in sorted(mods):
    if k in have or "viogpu" in k or k in ("ntoskrnl.exe", "hal.dll", "dxgkrnl.sys", "dxgmms1.sys"):
        print("%s base 0x%x size 0x%x%s" % (k, mods[k][0], mods[k][1], "" if k in have else " (no .map)"))

def sym(addr):
    for nm, (base, size) in mods.items():
        if base <= addr < base + size:
            rva = addr - base
            tab = maps.get(nm)
            if tab:
                i = bisect.bisect_right(tab, (rva, "\xff")) - 1
                if i >= 0:
                    return "%s!%s+0x%x" % (nm.rsplit(".", 1)[0], tab[i][1][:70], rva - tab[i][0])
            return "%s+0x%x" % (nm, rva)
    return None

def stack(rsp):
    out = b""
    a = rsp
    while len(out) < WORDS * 8:
        n = min(0x1000 - (a & 0xFFF), WORDS * 8 - len(out))
        try:
            out += mem(a, n)
        except gdb.MemoryError:
            break
        a += n
    return out

for t in sorted(gdb.selected_inferior().threads(), key=lambda t: t.num):
    t.switch()
    rip, rsp = reg("rip"), reg("rsp")
    print("\nprocessor %d: at %s, stack 0x%x" % (t.num - 1, sym(rip) or "0x%x" % rip, rsp))
    raw = stack(rsp)
    words = struct.unpack_from("<%dQ" % (len(raw) // 8), raw)
    print("  first words: " + " ".join("%x" % w for w in words[:10]))
    shown = 0
    for i, w in enumerate(words):
        if w < 0xFFFF800000000000:
            continue
        name = sym(w)
        if name:
            print("  +0x%03x %s" % (i * 8, name))
            shown += 1
            if shown >= 48:
                break
gdb.execute("detach")
gdb.execute("quit")
