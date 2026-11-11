# Stage the OpenCore boot disk's files on dev.lan, in the repo's .venv (standard library only):
# fetch the releases in sources.txt (SHA-256 checked every time), validate config.plist with
# OpenCore's own ocvalidate, collect OpenCore, the drivers/kexts/tools/ACPI tables the config
# names and OpenDuet's loader, and write them as a tar stream to stdout for mkimage.sh.
#   .venv/bin/python -I stage.py WORKDIR VARIANT DUET > staged.tar
#   WORKDIR holds config.plist and sources.txt; downloads go to WORKDIR/dl, unpacked to WORKDIR/x.
#   VARIANT: RELEASE or DEBUG.  DUET: bootX64-blockio, bootX64 or none.
# Messages go to stderr. Called by build.sh.
import hashlib
import io
import os
import plistlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile


def log(*a):
    print(*a, file=sys.stderr)


def fetch(work):
    dl = os.path.join(work, "dl")
    os.makedirs(dl, exist_ok=True)
    unpacked = {}
    for line in open(os.path.join(work, "sources.txt")):
        if not line.strip() or line.startswith("#"):
            continue
        name, sha, url = line.split()
        path = os.path.join(dl, name)
        if not os.path.exists(path):
            log("downloading", url)
            with urllib.request.urlopen(url) as r, open(path + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
            os.rename(path + ".part", path)
        got = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if got != sha:
            sys.exit("checksum mismatch for %s: %s" % (name, got))
        dest = os.path.join(work, "x", name[:-4])
        shutil.rmtree(dest, ignore_errors=True)
        zipfile.ZipFile(path).extractall(dest)
        unpacked[name] = dest
    return unpacked


def main():
    work, variant, duet = sys.argv[1], sys.argv[2], sys.argv[3]
    unpacked = fetch(work)
    oc = next(d for n, d in unpacked.items() if n.startswith("OpenCore-") and n.endswith(variant + ".zip"))
    rel = next(d for n, d in unpacked.items() if n.startswith("OpenCore-") and n.endswith("RELEASE.zip"))
    config = os.path.join(work, "config.plist")

    ocv = os.path.join(rel, "Utilities", "ocvalidate", "ocvalidate.linux")
    os.chmod(ocv, 0o755)
    res = subprocess.run([ocv, config], capture_output=True, text=True)
    log(res.stdout.strip())
    if res.returncode != 0 or "No issues found" not in res.stdout:
        sys.exit("ocvalidate failed")

    p = plistlib.load(open(config, "rb"))
    files = {  # path in the tar -> source file
        "esp/EFI/BOOT/BOOTx64.efi": os.path.join(oc, "X64/EFI/BOOT/BOOTx64.efi"),
        "esp/EFI/OC/OpenCore.efi": os.path.join(oc, "X64/EFI/OC/OpenCore.efi"),
        "esp/EFI/OC/config.plist": config,
    }
    dirs = ["esp/EFI/OC/Drivers", "esp/EFI/OC/Kexts", "esp/EFI/OC/ACPI", "esp/EFI/OC/Tools"]
    for d in p["UEFI"]["Drivers"]:
        if d.get("Enabled"):
            files["esp/EFI/OC/Drivers/" + d["Path"]] = os.path.join(oc, "X64/EFI/OC/Drivers", d["Path"])
    for t in p["Misc"]["Tools"]:
        if t.get("Enabled"):
            files["esp/EFI/OC/Tools/" + t["Path"]] = os.path.join(oc, "X64/EFI/OC/Tools", t["Path"])
    for a in p["ACPI"]["Add"]:
        if a.get("Enabled"):
            files["esp/EFI/OC/ACPI/" + a["Path"]] = os.path.join(os.path.dirname(os.path.abspath(__file__)), "acpi", a["Path"])
    kexts = {}
    for k in p["Kernel"]["Add"]:
        if k.get("Enabled"):
            name = k["BundlePath"].split("/")[0]
            src = next((os.path.join(r, name) for d in unpacked.values() if d.endswith("RELEASE")
                        for r, ds, fs in os.walk(d) if name in ds), None)
            if not src:
                sys.exit("kext not in sources: " + name)
            kexts["esp/EFI/OC/Kexts/" + name] = src
    legacy = os.path.join(oc, "Utilities", "LegacyBoot")
    if duet != "none":
        files["esp/boot"] = os.path.join(legacy, duet)
        files["legacy/boot0"] = os.path.join(legacy, "boot0")
        files["legacy/boot1f32"] = os.path.join(legacy, "boot1f32")

    out = tarfile.open(fileobj=sys.stdout.buffer, mode="w|", format=tarfile.USTAR_FORMAT)
    for d in dirs:
        ti = tarfile.TarInfo(d)
        ti.type, ti.mode = tarfile.DIRTYPE, 0o755
        out.addfile(ti)
    for arc, src in sorted(files.items()):
        out.add(src, arcname=arc)
        log("staged", arc)
    for arc, src in sorted(kexts.items()):
        out.add(src, arcname=arc)
        log("staged", arc)
    # Hide OpenCore's own disk from its menu (otherwise it is listed, as the default on a fresh
    # VM, and chain-loads itself). ScanPolicy did this before but also hid the installers'
    # Apple-partition-map volumes. OpenCore docs, Boot Algorithm: .contentVisibility.
    hide = b"Disabled"
    ti = tarfile.TarInfo("esp/EFI/BOOT/.contentVisibility")
    ti.size, ti.mode = len(hide), 0o644
    out.addfile(ti, io.BytesIO(hide))
    log("staged", ti.name)
    out.close()


if __name__ == "__main__":
    main()
