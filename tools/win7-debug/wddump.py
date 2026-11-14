# gdb script: dump the guest's watchdog log (Windows' own in-memory error log of the display
# kernel), newest entries last, callers and pointer parameters named from the public symbols.
import gdb, struct, bisect, os, sys
RE = os.environ.get("WIN7_RE", os.getcwd())  # where the copied modules and their .map files are
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else RE)
from guestmods import *
LAST = int(os.environ.get("WDLOG_LAST", "120"))
mods = find_modules()
if not mods:
    print("no module list found")
else:
    maps = {}
    for m in ("dxgkrnl.sys", "dxgmms1.sys", "watchdog.sys"):
        out = []
        try:
            for line in open(os.path.join(RE, m + ".map")):
                a, n = line.rstrip("\n").split(" ", 1)
                out.append((int(a, 16), n))
        except IOError:
            pass
        out.sort()
        maps[m] = out
        print("%s base 0x%x" % (m, mods.get(m, (0, 0))[0]))
    for k in sorted(mods):
        if "viogpu" in k or k in ("cdd.dll", "win32k.sys", "vga.sys", "videoprt.sys"):
            print("%s base 0x%x size 0x%x" % (k, mods[k][0], mods[k][1]))
    def sym(addr):
        for nm, (base, size) in mods.items():
            if base <= addr < base + size:
                rva = addr - base
                tab = maps.get(nm)
                if tab:
                    i = bisect.bisect_right(tab, (rva, "\xff")) - 1
                    if i >= 0:
                        return "%s!%s+0x%x" % (nm[:-4], tab[i][1][:60], rva - tab[i][0])
                return "%s+0x%x" % (nm, rva)
        return "0x%x" % addr
    wd = mods["watchdog.sys"][0]
    root = wd + 0x16120
    entries = []
    for lvl in range(8):
        buf = rq(root + 24 * lvl)
        size = rd(root + 24 * lvl + 8)
        idx = rd(root + 24 * lvl + 12)
        if not buf or not size:
            continue
        print("level %d: %d entries, %d written" % (lvl, size, idx))
        try:
            raw = mem(buf, size * 0x40)
        except gdb.MemoryError:
            print("  unreadable")
            continue
        for i in range(size):
            e = raw[i * 0x40:(i + 1) * 0x40]
            order, = struct.unpack_from("<I", e, 0)
            thread, caller, p0, p1, p2, p3, p4 = struct.unpack_from("<7Q", e, 8)
            if caller:
                entries.append((order, lvl, caller, (p0, p1, p2, p3, p4)))
    entries.sort()
    for order, lvl, caller, ps in entries[-LAST:]:
        print("%6d L%d %-58s %s" % (order, lvl, sym(caller), " ".join(sym(p) if p > 0xFFFF800000000000 else "0x%x" % p for p in ps)))
gdb.execute("detach")
gdb.execute("quit")
