#!/bin/sh
# Validate the OpenCore config on dev.lan, build the boot disk on the retro rig, and
# optionally write it to a VM's OpenCore disk.
#   boot/opencore/build.sh                 validate + build /tmp/opencore-build/opencore.img on the rig
#   boot/opencore/build.sh -w VMID         ... and write it to the VM's ide0 OpenCore disk
#                                          (VM stopped; creates the disk on first use)
#   OC_VARIANT=DEBUG boot/opencore/build.sh -w 212    OpenCore's debug build (more logging)
# Nothing runs on the laptop except ssh/scp. See boot/opencore/README.md.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
RETRO=${RETRO:-retro}
DEV=${DEV:-dev.lan}
variant=${OC_VARIANT:-RELEASE}
die() { echo "build.sh: $*" >&2; exit 1; }

vmid=
[ "${1:-}" = -w ] && { vmid=${2:?usage: build.sh [-w VMID]}; }

# 1. ocvalidate (Linux build from the same pinned OpenCore release) on dev.lan.
line=$(grep "^OpenCore-.*-RELEASE.zip " "$here/sources.txt")
set -- $line
ssh "$DEV" 'mkdir -p ~/Projects/moose-ripple/tmp-opencore'
scp -q "$here/config.plist" "$DEV:Projects/moose-ripple/tmp-opencore/config.plist"
ssh "$DEV" "set -e; cd ~/Projects/moose-ripple/tmp-opencore
    [ -f $1 ] || curl -sSLf -o $1 '$3'
    echo '$2  $1' | sha256sum -c --quiet -
    d=\$PWD; rm -rf x && mkdir x && (cd / && python3 -I -c 'import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); z.extract(\"Utilities/ocvalidate/ocvalidate.linux\", sys.argv[2])' \$d/$1 \$d/x)
    chmod +x x/Utilities/ocvalidate/ocvalidate.linux
    x/Utilities/ocvalidate/ocvalidate.linux config.plist" || die "ocvalidate failed"

# 2. Build the image on the rig.
ssh "$RETRO" 'mkdir -p /tmp/opencore-build/src'
scp -q "$here/mkimage.sh" "$here/sources.txt" "$here/config.plist" "$RETRO:/tmp/opencore-build/src/"
[ -d "$here/acpi" ] && scp -qr "$here/acpi" "$RETRO:/tmp/opencore-build/src/"
ssh "$RETRO" "OC_VARIANT=$variant OC_DUET=${OC_DUET:-bootX64-blockio} bash /tmp/opencore-build/src/mkimage.sh /tmp/opencore-build/src/config.plist /tmp/opencore-build/opencore.img"

[ -n "$vmid" ] || exit 0

# 3. Write it to the VM's OpenCore disk (ide0). The disk is qcow2 so Proxmox snapshots
#    cover it; writing replaces the current contents and keeps the snapshots.
ssh "$RETRO" "set -e
    [ \"\$(qm status $vmid)\" = 'status: stopped' ] || { echo 'VM $vmid is running' >&2; exit 1; }
    vol=local:$vmid/vm-$vmid-opencore.qcow2
    [ -f \"\$(pvesm path \$vol)\" ] || pvesm alloc local $vmid vm-$vmid-opencore.qcow2 200M --format qcow2 >/dev/null
    qemu-img convert -n -f raw -O qcow2 /tmp/opencore-build/opencore.img \$(pvesm path \$vol)
    qm config $vmid | grep -q \"^ide0: \$vol\" || qm set $vmid --ide0 \$vol >/dev/null
    qm set $vmid --boot 'order=ide0;sata0' >/dev/null
    echo \"VM $vmid ide0 = \$vol (written)\""
