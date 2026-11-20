"""Check Ripple's three display binaries against files extracted from a Vista image."""

import argparse
from functools import lru_cache
from pathlib import Path
import re
import subprocess


def readobj(option, binary):
    return subprocess.check_output(["llvm-readobj", option, binary], text=True)


@lru_cache(maxsize=None)
def exports(binary):
    result = {}
    for block in re.findall(r"^Export \{\n(.*?)^\}", readobj("--coff-exports", binary), re.M | re.S):
        fields = dict(line.strip().split(": ", 1) for line in block.splitlines() if ": " in line.strip())
        forward = fields.get("ForwardedTo")
        result["#" + fields["Ordinal"]] = forward
        if "Name" in fields:
            result[fields["Name"]] = forward
    return result


def resolve(directory, library, symbol, visited=()):
    name = library.lower()
    if "." not in name:
        name += ".dll"
    binary = directory / name
    if not binary.is_file():
        binary = directory / "drivers" / name
    if not binary.is_file():
        raise ValueError(f"missing reference file {name} in {directory}")
    table = exports(binary)
    if symbol not in table:
        raise ValueError(f"{name} does not export {symbol}")
    target = table[symbol]
    if target:
        if (name, symbol) in visited:
            raise ValueError(f"forwarder cycle at {name}!{symbol}")
        provider, forwarded_name = target.rsplit(".", 1)
        resolve(directory, provider, forwarded_name, (*visited, (name, symbol)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("windows", type=Path, help="Windows directory from the mounted/extracted image")
    parser.add_argument("mesa_source", type=Path, help="patched Mesa source used for the build")
    args = parser.parse_args()
    bridge = args.mesa_source / "src/gallium/auxiliary/gdikmt/gdikmt_d3dkmt.c"
    kmt = re.findall(r'\[KMT_\w+\]\s*=\s*"(D3DKMT\w+)"', bridge.read_text())
    if not kmt:
        parser.error("could not find the required GDI entry points in the Mesa source")
    failed = False
    for file, system, machine in [
        ("viogpu3d.sys", "System32", "AMD64"),
        ("viogpu_d3d10.dll", "System32", "AMD64"),
        ("viogpu_d3d10.x86.dll", "SysWOW64", "I386"),
    ]:
        binary = args.package / file
        headers = readobj("--file-headers", binary)
        if not all(field in headers.splitlines() for field in ("  MajorSubsystemVersion: 6", "  MinorSubsystemVersion: 0")) or f"IMAGE_FILE_MACHINE_{machine} " not in headers:
            raise ValueError(f"{file}: unexpected architecture or subsystem version")
        imports = []
        for block in re.findall(r"^(?:Delay)?Import \{\n(.*?)^\}", readobj("--coff-imports", binary), re.M | re.S):
            lib = re.search(r"^  Name: (\S+)", block, re.M)[1]
            for symbol, ordinal in re.findall(r"^  Symbol: (.*?) \((\d+)\)", block, re.M):
                imports.append((lib, symbol or "#" + ordinal))
        if not imports:
            raise ValueError(f"{file}: no imports found")
        required = list(imports)
        if file.endswith(".dll"):
            if "OpenAdapter" not in exports(binary):
                raise ValueError(f"{file}: missing Direct3D 9 OpenAdapter export")
            required.extend(("gdi32.dll", entry) for entry in kmt)
        missing = []
        for lib, symbol in required:
            try:
                resolve(args.windows / system, lib, symbol)
            except ValueError as error:
                missing.append(f"{lib}!{symbol}: {error}")
        print(f"{file}: {len(imports)} imports, {len(required) - len(imports)} GDI lookups, {len(missing)} unresolved")
        for item in missing:
            print("  " + item)
        failed |= bool(missing)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
