#!/bin/bash
# Build the OpenCore boot disk image from the files stage.py staged (a tar stream on stdin):
# GPT, one FAT32 EFI system partition holding OpenCore, plus OpenDuet's boot sectors when they
# were staged, so the same image boots under OVMF and under SeaBIOS. Shell tools only (no
# Python on the rig): truncate, sgdisk, losetup, mkfs.vfat, dd, mount. Runs as root on the rig.
#   ... stage.py ... | mkimage.sh OUT.img
# See build.sh and docs/decisions/0007-opencore.md.
set -euo pipefail

out=${1:?usage: mkimage.sh OUT.img < staged.tar}
[ "$(id -u)" = 0 ] || { echo "mkimage.sh: run as root (loop device)" >&2; exit 1; }

work=$(mktemp -d /tmp/opencore-img.XXXXXX)
loop=
cleanup() {
    mountpoint -q "$work/mnt" 2>/dev/null && umount "$work/mnt"
    [ -n "$loop" ] && losetup -d "$loop" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

tar -C "$work" -xf -
[ -f "$work/esp/EFI/OC/OpenCore.efi" ] || { echo "mkimage.sh: nothing staged on stdin" >&2; exit 1; }

# Disk: 200 MiB, GPT, partition 1 = EFI system partition from sector 2048 to the end.
rm -f "$out"
truncate -s 200M "$out"
sgdisk -o -n 1:2048:0 -t 1:EF00 -c 1:EFI "$out" >/dev/null
start=$(sgdisk -i 1 "$out" | awk '/^First sector/ {print $3}')
end=$(sgdisk -i 1 "$out" | awk '/^Last sector/ {print $3}')
loop=$(losetup --show -f -o $((start * 512)) --sizelimit $(((end - start + 1) * 512)) "$out")
mkfs.vfat -F 32 -n OPENCORE "$loop" >/dev/null

# OpenDuet boot sectors, as OpenCore's Utilities/LegacyBoot scripts do: boot1f32 into the
# partition's boot record (keeping the FAT32 parameter block, bytes 3-89), boot0 into the
# MBR's code area (first 446 bytes; boot0 finds the EFI partition through the GPT).
if [ -f "$work/legacy/boot1f32" ]; then
    dd if="$loop" of="$work/origbs" count=1 status=none
    cp "$work/legacy/boot1f32" "$work/newbs"
    dd if="$work/origbs" of="$work/newbs" skip=3 seek=3 bs=1 count=87 conv=notrunc status=none
    dd if=/dev/urandom of="$work/newbs" skip=496 seek=496 bs=1 count=14 conv=notrunc status=none
    dd if="$work/newbs" of="$loop" conv=notrunc status=none
fi

mkdir "$work/mnt"
mount -t vfat "$loop" "$work/mnt"
cp -R "$work/esp/." "$work/mnt/"
sync
umount "$work/mnt"
losetup -d "$loop"; loop=
[ -f "$work/legacy/boot0" ] && dd if="$work/legacy/boot0" of="$out" bs=1 count=446 conv=notrunc status=none

echo "$out: $(sha256sum "$out" | cut -c1-16)…"
(cd "$work/esp" && find . -type f | sort)
