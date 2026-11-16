"""Who called what, from a Windows minidump: the crashed thread's stack, searched for return
addresses into one module and named from that module's own symbol table.

    dumpstack.py CRASH.dmp DRIVER.dll [MORE.dll ...]
    dumpstack.py --threads HANG.dmp DRIVER.dll [MORE.dll ...]

For a user-mode driver that crashes inside a Windows program (docs/windows/test-vms.md,
"Crash dumps"): Windows writes the dump when asked to (the LocalDumps registry key), and the
DLLs built here with llvm-mingw keep their symbol table, which `llvm-nm` prints. No debugger is
needed. Every stack word that points into a named module is listed, so the list holds stale
words too: read it as the candidates for the call chain, innermost first.
With --threads, for a dump of a program that hangs or waits (tools/win7-hello/dumpproc.c
writes one): every thread, where it is and the modules its stack leads through.
Needs llvm-nm on the PATH.
"""
import bisect
import os
import struct
import subprocess
import sys


def streams(d):
    sig, _ver, count, rva = struct.unpack_from("<4sIII", d, 0)
    if sig != b"MDMP":
        sys.exit("not a minidump")
    out = {}
    for i in range(count):
        kind, size, at = struct.unpack_from("<III", d, rva + 12 * i)
        out[kind] = (at, size)
    return out


def modules(d, at):
    (n,) = struct.unpack_from("<I", d, at)
    out = []
    for i in range(n):
        base, size, _chk, _ts, name_rva = struct.unpack_from("<QIIII", d, at + 4 + 108 * i)
        (length,) = struct.unpack_from("<I", d, name_rva)
        name = d[name_rva + 4:name_rva + 4 + length].decode("utf-16-le")
        out.append((base, size, name.rsplit("\\", 1)[-1]))
    return out


def symbols(path):
    """Sorted (address relative to the image's start, name) from the DLL's symbol table."""
    text = subprocess.run(["llvm-nm", "-n", path], capture_output=True, text=True).stdout
    with open(path, "rb") as f:
        pe = f.read(4096)
    (e_lfanew,) = struct.unpack_from("<I", pe, 0x3C)
    (magic,) = struct.unpack_from("<H", pe, e_lfanew + 24)
    if magic == 0x20B:
        (image_base,) = struct.unpack_from("<Q", pe, e_lfanew + 24 + 24)
    else:  # a 32-bit DLL
        (image_base,) = struct.unpack_from("<I", pe, e_lfanew + 24 + 28)
    out = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT":
            out.append((int(parts[0], 16) - image_base, parts[2]))
    return sorted(out)


X64_REGS = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13",
            "r14", "r15"]
X86_REGS = ["edi", "esi", "ebx", "edx", "ecx", "eax", "ebp", "eip", "cs", "eflags", "esp"]


def is_x86(d, s):
    """A dump of a 32-bit program (processor architecture 0 in the system information)."""
    return 7 in s and struct.unpack_from("<H", d, s[7][0])[0] == 0


def registers(d, ctx, x86):
    """(stack pointer, instruction pointer, {name: value}) from a thread context."""
    if x86:
        vals = struct.unpack_from("<11I", d, ctx + 0x9C)
        regs = dict(zip(X86_REGS, vals))
        return regs["esp"], regs["eip"], regs
    vals = struct.unpack_from("<16Q", d, ctx + 0x78)
    regs = dict(zip(X64_REGS, vals))
    return regs["rsp"], struct.unpack_from("<Q", d, ctx + 0xF8)[0], regs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    d = open(args[0], "rb").read()
    named = {os.path.basename(p).lower(): symbols(p) for p in args[1:]}
    s = streams(d)
    mods = modules(d, s[4][0])
    x86 = is_x86(d, s)
    word, step = ("<I", 4) if x86 else ("<Q", 8)

    def where(addr):
        for base, size, name in mods:
            if base <= addr < base + size:
                off = addr - base
                table = named.get(name.lower())
                if table:
                    i = bisect.bisect_right(table, (off, "\xff")) - 1
                    if i >= 0:
                        return "%s!%s+0x%x" % (name, table[i][1], off - table[i][0]), True
                return "%s+0x%x" % (name, off), False
        return None, False

    if "--threads" in sys.argv:
        at = s[3][0]
        (n,) = struct.unpack_from("<I", d, at)
        for i in range(n):
            tid, _susp, _pc, _pr, _teb, start, size, rva, _cs, ctx = struct.unpack_from(
                "<IIIIQQIIII", d, at + 4 + 48 * i)
            rsp, rip, _regs = registers(d, ctx, x86)
            print("thread %d at %s" % (tid, where(rip)[0] or hex(rip)))
            stack = d[rva:rva + size]
            last, shown = None, 0
            for off in range(max(0, rsp - start) & ~(step - 1), len(stack) - step + 1, step):
                (value,) = struct.unpack_from(word, stack, off)
                name, known = where(value)
                if not name:
                    continue
                module = name.split("!")[0].split("+")[0]
                if known or module != last:
                    print("  [sp+0x%05x] %s" % (start + off - rsp, name))
                    shown += 1
                last = module
                if shown >= 40:
                    break
        return

    # The exception: which thread, which code, and the registers at that moment
    at = s[6][0]
    thread_id, _pad, code, _flags, _rec, address = struct.unpack_from("<IIIIQQ", d, at)
    _ctx_size, ctx = struct.unpack_from("<II", d, at + 8 + 152)
    rsp, rip, regs = registers(d, ctx, x86)
    print("exception 0x%08x at %s, thread %d" % (code, where(address)[0] or hex(address), thread_id))
    print("  " + "  ".join("%s=%x" % (r, v) for r, v in regs.items()))
    print("  at %s" % (where(rip)[0] or hex(rip)))

    # That thread's stack, from the thread list
    at = s[3][0]
    (n,) = struct.unpack_from("<I", d, at)
    for i in range(n):
        tid, _susp, _pc, _pr, _teb, start, size, rva = struct.unpack_from("<IIIIQQII", d, at + 4 + 48 * i)
        if tid != thread_id:
            continue
        stack = d[rva:rva + size]
        skip = max(0, rsp - start)
        shown = 0
        for off in range(skip & ~(step - 1), len(stack) - step + 1, step):
            (value,) = struct.unpack_from(word, stack, off)
            name, known = where(value)
            if name and (known or "--all" in sys.argv):
                print("  [sp+0x%05x] %s" % (start + off - rsp, name))
                shown += 1
                if shown >= 200 and "--all" not in sys.argv:
                    print("  ... (more; --all lists everything)")
                    break


main()
