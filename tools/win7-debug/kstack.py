# kstack.py MEMORY.DMP ADDR [MAP...]: the loaded modules of a Windows 7 x64 kernel dump, and the
# kernel stack around ADDR read as a list of code addresses, live or stale (what ran last on that
# thread), each named by its module and, for modules with a linker .map given as NAME=FILE, by
# the function.
import struct, sys, re, bisect
src = open(sys.argv[0].replace("kstack.py", "kmem.py")).read().split('if sys.argv[2] == "crash":')[0]
argv = sys.argv
sys.argv = [argv[0], argv[1], "none"]
exec(src)
sys.argv = argv
addr = int(argv[2], 16)
maps = {}
for a in argv[3:]:
    name, path = a.split("=")
    syms, base = [], None
    for line in open(path, errors="replace"):
        m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", line)
        if m: base = int(m.group(1), 16)
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
        if m: syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    maps[name.lower()] = (base, syms)

head = struct.unpack_from("<Q", hdr, 0x20)[0]      # PsLoadedModuleList
mods = []
e = q(head)[0]
while e and e != head and len(mods) < 400:
    base, = q(e + 0x30); size = struct.unpack("<I", virt(e + 0x40, 4))[0]
    ln, = struct.unpack("<H", virt(e + 0x58, 2)); buf, = q(e + 0x60)
    nm = virt(buf, ln)
    mods.append((base, size, nm.decode("utf-16le", "replace") if nm else "?"))
    e = q(e)[0]
mods.sort()
def name(a):
    i = bisect.bisect_right([m[0] for m in mods], a) - 1
    if i < 0 or a >= mods[i][0] + mods[i][1]: return None
    base, size, nm = mods[i]
    s = "%s+%x" % (nm, a - base)
    if nm.lower() in maps:
        pbase, syms = maps[nm.lower()]
        t = a - base + pbase
        j = bisect.bisect_right([x[0] for x in syms], t) - 1
        if j >= 0: s += "  %s+%x" % (syms[j][1], t - syms[j][0])
    return s
print("modules of interest:", ", ".join("%s at %x" % (m[2], m[0]) for m in mods if m[2].lower() in ("viogpu3d.sys", "dxgkrnl.sys", "dxgmms1.sys", "ntoskrnl.exe", "hal.dll", "win32k.sys")))
lo = (addr & ~0xFFF) - 0x5000
for va in range(lo, (addr & ~0xFFF) + 0x1000, 8):
    w = q(va)
    if not w: continue
    n = name(w[0])
    if n and not n.lower().startswith(("ntoskrnl.exe+0 ",)):
        print("%x%s  %x  %s" % (va, " *" if va >= addr else "  ", w[0], n))
