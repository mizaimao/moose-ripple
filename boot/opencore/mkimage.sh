#!/bin/bash
# Build the OpenCore boot disk image: GPT, one FAT32 EFI system partition holding OpenCore,
# plus OpenDuet (OpenCore's UEFI layer for BIOS machines) so the same image also boots under
# SeaBIOS. OC_DUET picks the OpenDuet build: bootX64-blockio (default; disk access through the
# BIOS), bootX64 (OpenDuet's own drivers), or none. See docs/decisions/0007-opencore.md.
# Runs as root on a Linux host with dosfstools, gdisk, curl and python3 (the retro rig).
#   mkimage.sh CONFIG.plist OUT.img [CACHE-DIR]
# Downloads the releases in sources.txt into CACHE-DIR (default /tmp/opencore-build/dl) and
# checks their SHA-256 every time. OC_VARIANT=DEBUG uses OpenCore's debug build.
set -euo pipefail

config=${1:?usage: mkimage.sh CONFIG.plist OUT.img [CACHE-DIR]}
out=${2:?usage: mkimage.sh CONFIG.plist OUT.img [CACHE-DIR]}
cache=${3:-/tmp/opencore-build/dl}
variant=${OC_VARIANT:-RELEASE}
duet=${OC_DUET:-bootX64-blockio}
here=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "mkimage.sh: run as root (loop device)" >&2; exit 1; }

work=$(mktemp -d /tmp/opencore-img.XXXXXX)
loop=
cleanup() {
    mountpoint -q "$work/mnt" 2>/dev/null && umount "$work/mnt"
    [ -n "$loop" ] && losetup -d "$loop" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

# 1. Fetch and verify. Each archive is unpacked into its own empty folder.
mkdir -p "$cache" "$work/x"
grep -v '^#' "$here/sources.txt" | while read -r file sum url; do
    [ -n "$file" ] || continue
    [ -f "$cache/$file" ] || curl -sSLf -o "$cache/$file" "$url"
    echo "$sum  $cache/$file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
    mkdir "$work/x/${file%.zip}"
    (cd / && python3 -I -m zipfile -e "$cache/$file" "$work/x/${file%.zip}")
done
oc=$(ls -d "$work"/x/OpenCore-*-"$variant")
legacy=$oc/Utilities/LegacyBoot

# 2. ESP contents: OpenCore and the drivers, kexts and ACPI tables the config names.
esp=$work/esp
mkdir -p "$esp/EFI/BOOT" "$esp/EFI/OC/Drivers" "$esp/EFI/OC/Kexts" "$esp/EFI/OC/ACPI" "$esp/EFI/OC/Tools"
cp "$oc/X64/EFI/BOOT/BOOTx64.efi" "$esp/EFI/BOOT/"
cp "$oc/X64/EFI/OC/OpenCore.efi" "$esp/EFI/OC/"
cp "$config" "$esp/EFI/OC/config.plist"
[ "$duet" = none ] || cp "$legacy/$duet" "$esp/boot"
python3 -I - "$config" > "$work/needs" <<'EOF'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], "rb"))
for d in p["UEFI"]["Drivers"]:
    if d.get("Enabled"):
        print("driver", d["Path"])
for k in p["Kernel"]["Add"]:
    if k.get("Enabled"):
        print("kext", k["BundlePath"].split("/")[0])
for a in p["ACPI"]["Add"]:
    if a.get("Enabled"):
        print("acpi", a["Path"])
EOF
while read -r kind name; do
    case "$kind" in
        driver) cp "$oc/X64/EFI/OC/Drivers/$name" "$esp/EFI/OC/Drivers/" ;;
        kext)   src=$(find "$work/x" -path "*-RELEASE/*" -type d -name "$name" -prune | head -1)
                [ -n "$src" ] || { echo "kext not in sources: $name" >&2; exit 1; }
                cp -R "$src" "$esp/EFI/OC/Kexts/" ;;
        acpi)   cp "$here/acpi/$name" "$esp/EFI/OC/ACPI/" ;;
    esac
done < "$work/needs"

# 3. Disk: 200 MiB, GPT, partition 1 = EFI system partition from sector 2048 to the end.
rm -f "$out"
truncate -s 200M "$out"
sgdisk -o -n 1:2048:0 -t 1:EF00 -c 1:EFI "$out" >/dev/null
start=$(sgdisk -i 1 "$out" | awk '/^First sector/ {print $3}')
end=$(sgdisk -i 1 "$out" | awk '/^Last sector/ {print $3}')
loop=$(losetup --show -f -o $((start * 512)) --sizelimit $(((end - start + 1) * 512)) "$out")
mkfs.vfat -F 32 -n OPENCORE "$loop" >/dev/null

# 4. OpenDuet boot sectors, as OpenCore's Utilities/LegacyBoot scripts do: boot1f32 into the
# partition's boot record (keeping the FAT32 parameter block, bytes 3-89), boot0 into the
# MBR's code area (first 446 bytes; boot0 finds the EFI partition through the GPT).
if [ "$duet" != none ]; then
    dd if="$loop" of="$work/origbs" count=1 status=none
    cp "$legacy/boot1f32" "$work/newbs"
    dd if="$work/origbs" of="$work/newbs" skip=3 seek=3 bs=1 count=87 conv=notrunc status=none
    dd if=/dev/urandom of="$work/newbs" skip=496 seek=496 bs=1 count=14 conv=notrunc status=none
    dd if="$work/newbs" of="$loop" conv=notrunc status=none
fi

mkdir "$work/mnt"
mount -t vfat "$loop" "$work/mnt"
cp -R "$esp/." "$work/mnt/"
sync
umount "$work/mnt"
losetup -d "$loop"; loop=
[ "$duet" = none ] || dd if="$legacy/boot0" of="$out" bs=1 count=446 conv=notrunc status=none

echo "$out: OpenCore $(basename "$oc"), $(sha256sum "$out" | cut -c1-16)…"
(cd "$esp" && find . -type f | sort)
