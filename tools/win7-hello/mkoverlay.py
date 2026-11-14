"""A clang VFS overlay that makes the given include directories case-insensitive, as Windows
kit headers #include each other in any case. Usage: mkoverlay.py OUT.yaml DIR [DIR ...]
(JSON is valid YAML; clang reads it with -ivfsoverlay OUT.yaml.) MIT licence."""
import json, os, sys

def entry(path):
    if os.path.isdir(path):
        return {"type": "directory", "name": os.path.basename(path),
                "contents": [entry(os.path.join(path, n)) for n in sorted(os.listdir(path))]}
    return {"type": "file", "name": os.path.basename(path), "external-contents": path}

roots = []
for d in sys.argv[2:]:
    d = os.path.abspath(d)
    roots.append({"type": "directory", "name": d,
                  "contents": [entry(os.path.join(d, n)) for n in sorted(os.listdir(d))]})
json.dump({"version": 0, "case-sensitive": "false", "roots": roots}, open(sys.argv[1], "w"))
