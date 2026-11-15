# gdb script: which of Windows' own error-log lines fire in the guest.
# Breaks on watchdog!WdLogEvent5 (hardware breakpoint), records the caller and the entry's
# parameters, lets the guest run on, and stops after N hits.
import gdb, struct, bisect, os, sys

RE = os.environ.get("WIN7_RE", os.getcwd())  # where the copied modules and their .map files are
N = int(os.environ.get("WDLOG_HITS", "60"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else RE)
from guestmods import *

mods = find_modules()
if not mods:
    print("no module list found")
    gdb.execute("detach")
    gdb.execute("quit")

maps = load_maps(RE, ("dxgkrnl.sys", "dxgmms1.sys", "watchdog.sys"))
for m in ("dxgkrnl.sys", "dxgmms1.sys", "watchdog.sys"):
    print("%s base 0x%x" % (m, mods.get(m, (0, 0))[0]))

def sym(addr):
    for nm, (base, size) in mods.items():
        if base <= addr < base + size:
            rva = addr - base
            tab = maps.get(nm)
            if tab:
                i = bisect.bisect_right(tab, (rva, "\xff")) - 1
                if i >= 0:
                    return "%s!%s+0x%x (rva 0x%x)" % (nm, tab[i][1], rva - tab[i][0], rva)
            return "%s+0x%x" % (nm, rva)
    return "0x%x" % addr

wd_base = mods["watchdog.sys"][0]
target = None
for a, n in maps["watchdog.sys"]:
    if n == "WdLogEvent5":
        target = wd_base + a
if target is None:
    print("no WdLogEvent5 symbol")
    gdb.execute("detach")
    gdb.execute("quit")

hits = []
class Bp(gdb.Breakpoint):
    def stop(self):
        try:
            rsp = reg("rsp")
            ret = rq(rsp)
            rcx = reg("rcx")
            rdx = reg("rdx")
            vals = [rq(rdx + 0x18 + 8 * i) for i in range(5)]
            hits.append((ret, rcx, vals))
        except Exception as ex:
            hits.append((0, 0, [0] * 5))
        return len(hits) >= N

Bp("*0x%x" % target, type=gdb.BP_HARDWARE_BREAKPOINT)
gdb.execute("continue")
for ret, rcx, vals in hits:
    print("%-70s lvl %d  %s" % (sym(ret), rcx, " ".join(sym(v) if v > 0xFFFF800000000000 else "0x%x" % v for v in vals)))
gdb.execute("delete")
gdb.execute("detach")
gdb.execute("quit")
