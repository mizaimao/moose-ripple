import re, sys
syms = {}
for line in open(sys.argv[1] + ".map"):
    a, n = line.rstrip("\n").split(" ", 1)
    syms[int(a, 16)] = n
for line in open(sys.argv[2]):
    line = line.rstrip("\n")
    m = re.search(r"(call|jmp|j[a-z]+)\s+0x([0-9a-f]+) <[^>]*>", line)
    if m:
        t = int(m.group(2), 16)
        if t in syms:
            line = line[:m.start()] + m.group(1) + " " + syms[t]
        else:
            line = line[:m.start()] + m.group(1) + " 0x%x" % t
    m = re.search(r"lea\s+\w+, \[rip [+-] 0x[0-9a-f]+\]\s+# 0x([0-9a-f]+)", line)
    print(line)
