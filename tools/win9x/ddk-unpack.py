"""Lay out an old Windows DDK (98, ME, 2000) from its setup cabinets, without running its setup.

The DDKs of that time ship their files in CAB archives under flat names such as
SRC_DISPLAY_MINI_FRAMEBUF_CHIPBLT.ASM; each CAB's INF says where every file goes:

    [DestinationDirs]   FRAMEBUF_Files5= 49000,"src\\display\\mini\\framebuf"
    [FRAMEBUF_Files5]   CHIPBLT.ASM,SRC_DISPLAY_MINI_FRAMEBUF_CHIPBLT.ASM

Usage (on the build host, in the repo's .venv):
    .venv/bin/python -I tools/win9x/ddk-unpack.py RAW FLAT OUT
RAW holds the setup files (the INFs), FLAT the CABs' contents extracted into one folder
(7z x -oFLAT RAW/*.CAB), OUT receives the tree. Prints what it couldn't place.
"""
import os
import re
import shutil
import sys

raw, flat, out = sys.argv[1:4]
flat_files = {f.lower(): f for f in os.listdir(flat)}
placed, missing = 0, []


def sections(path):
    """INF sections as {name_lower: [lines]}; comments and blank lines dropped."""
    secs, cur = {}, None
    with open(path, encoding="latin-1") as f:
        for line in f:
            line = line.split(";", 1)[0].strip()
            if not line:
                continue
            m = re.match(r"^\[(.+)\]$", line)
            if m:
                cur = secs.setdefault(m.group(1).strip().lower(), [])
            elif cur is not None:
                cur.append(line)
    return secs


for inf in sorted(os.listdir(raw)):
    if not inf.lower().endswith(".inf"):
        continue
    secs = sections(os.path.join(raw, inf))
    for line in secs.get("destinationdirs", []):
        if "=" not in line:
            continue
        name, value = (s.strip() for s in line.split("=", 1))
        parts = [p.strip().strip('"') for p in value.split(",")]
        sub = parts[1] if len(parts) > 1 else ""
        dest = os.path.join(out, *[p for p in sub.replace("\\", "/").split("/") if p]).lower()
        for entry in secs.get(name.lower(), []):
            fields = [p.strip() for p in entry.split(",")]
            target = fields[0]
            source = fields[1] if len(fields) > 1 and fields[1] else target
            src = flat_files.get(source.lower())
            if not src:
                missing.append(f"{inf}: {source}")
                continue
            os.makedirs(dest, exist_ok=True)
            dst = os.path.join(dest, target.lower())
            if os.path.exists(dst):          # a file listed twice; the CABs mark some read-only
                os.chmod(dst, 0o644)
            shutil.copyfile(os.path.join(flat, src), dst)
            os.chmod(dst, 0o644)
            placed += 1

print(f"placed {placed} files under {out}")
for m in missing:
    print("not found:", m)
