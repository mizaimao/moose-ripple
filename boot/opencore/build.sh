#!/bin/sh
# Stage and validate the OpenCore files on dev.lan (repo .venv), build the boot disk on the
# retro rig (shell tools only), and optionally write it to a VM's OpenCore disk.
#   boot/opencore/build.sh                 validate + build /tmp/opencore-build/opencore.img on the rig
#   boot/opencore/build.sh -w VMID         ... and write it to the VM's ide0 OpenCore disk
#                                          (VM stopped; creates the disk on first use)
#   OC_VARIANT=DEBUG boot/opencore/build.sh -w 212    OpenCore's debug build (more logging)
#   OC_DUET=none boot/opencore/build.sh    without OpenDuet (or bootX64: its own disk drivers)
#   boot/opencore/build.sh -p build/moose-ripple-VERSION.zip
#                                          the release disk: 256 MiB, plus a FAT32 partition
#                                          "RIPPLE" with the package unpacked (install.sh etc.)
# Nothing runs on the laptop except ssh/scp. See boot/opencore/README.md.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
RETRO=${RETRO:-retro}
DEV=${DEV:-dev.lan}
variant=${OC_VARIANT:-RELEASE}
die() { echo "build.sh: $*" >&2; exit 1; }

vmid= pkg=
while [ $# -gt 0 ]; do
    case "$1" in
        -w) vmid=${2:?usage: build.sh [-w VMID] [-p PACKAGE.zip]}; shift 2 ;;
        -p) pkg=${2:?usage: build.sh [-w VMID] [-p PACKAGE.zip]}; shift 2 ;;
        *)  die "usage: build.sh [-w VMID] [-p PACKAGE.zip]" ;;
    esac
done
[ -n "$vmid" ] && [ -n "$pkg" ] && die "-p makes the 256 MiB release disk; it is not for -w"
[ -z "$pkg" ] || [ -f "$pkg" ] || die "no such package: $pkg"

duet=${OC_DUET:-bootX64-blockio}

# 1. Stage on dev.lan in the repo's .venv (Python only from a venv in the repo):
#    fetch by SHA-256, ocvalidate, collect the files. 2. Build the image on the rig with shell
#    tools only; the staged files travel as a tar stream through this machine.
# dev.lan scratch lives in /tmp: scripts/build.sh mirrors the repo there with rsync --delete.
work=/tmp/ripple-opencore
ssh -o ConnectTimeout=15 "$DEV" "mkdir -p $work"
scp -q -o ConnectTimeout=15 "$here/stage.py" "$here/config.plist" "$here/sources.txt" "$DEV:$work/"
[ -d "$here/acpi" ] && scp -qr -o ConnectTimeout=15 "$here/acpi" "$DEV:$work/"
ssh -o ConnectTimeout=15 "$RETRO" 'mkdir -p /tmp/opencore-build'
scp -q -o ConnectTimeout=15 "$here/mkimage.sh" "$RETRO:/tmp/opencore-build/mkimage.sh"
stage="~/Projects/moose-ripple/.venv/bin/python -I $work/stage.py $work $variant $duet"
if [ -n "$pkg" ]; then
    # The package goes in as ripple/ after the staged OpenCore files (GNU tar appends).
    scp -q -o ConnectTimeout=15 "$pkg" "$DEV:$work/package.zip"
    stage="$stage > $work/stage.tar && rm -rf $work/pkg && mkdir $work/pkg &&
        unzip -q $work/package.zip -d $work/pkg && mv $work/pkg/* $work/pkg/ripple &&
        tar -C $work/pkg -rf $work/stage.tar ripple && cat $work/stage.tar"
fi
ssh -o ConnectTimeout=15 "$DEV" "$stage" |
    ssh -o ConnectTimeout=15 "$RETRO" "bash /tmp/opencore-build/mkimage.sh /tmp/opencore-build/opencore.img" || die "build failed"

[ -n "$vmid" ] || exit 0

# 3. Write it to the VM's OpenCore disk (ide0). The disk is qcow2 so Proxmox snapshots
#    cover it; writing replaces the current contents and keeps the snapshots.
ssh -o ConnectTimeout=15 "$RETRO" "set -e
    [ \"\$(qm status $vmid)\" = 'status: stopped' ] || { echo 'VM $vmid is running' >&2; exit 1; }
    vol=local:$vmid/vm-$vmid-opencore.qcow2
    [ -f \"\$(pvesm path \$vol)\" ] || pvesm alloc local $vmid vm-$vmid-opencore.qcow2 200M --format qcow2 >/dev/null
    qemu-img convert -n -f raw -O qcow2 /tmp/opencore-build/opencore.img \$(pvesm path \$vol)
    qm config $vmid | grep -q \"^ide0: \$vol\" || qm set $vmid --ide0 \$vol >/dev/null
    qm set $vmid --boot 'order=ide0;sata0' >/dev/null
    echo \"VM $vmid ide0 = \$vol (written)\""
