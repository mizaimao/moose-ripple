#!/bin/sh
# Install a kext from build/ into the dev VM's /System/Library/Extensions for boot-time
# loading (drivers like the framebuffer can't take over a device at runtime), then reboot.
#   scripts/install-kext.sh RippleVirtioGPU [BOOT-FLAGS]   snapshot, install, check, reboot
#   scripts/install-kext.sh -u RippleVirtioGPU             remove it again, reboot
# Snapshot "pre-install" is taken first (rolling). If the VM doesn't come back, roll back with
#   scripts/snapshot.sh -b pre-install
set -eu
. "$(dirname "$0")/lib.sh"
here=$(dirname "$0")

remove=0
[ "${1:-}" = -u ] && { remove=1; shift; }
name=${1:?usage: install-kext.sh [-u] KEXT-NAME [BOOT-FLAGS]}
flags=${2--v}
dest=/System/Library/Extensions/$name.kext

if [ "$remove" = 1 ]; then
    gssh "sudo -n rm -rf $dest && sudo -n kextcache -system-mkext >/dev/null 2>&1; echo removed $dest"
else
    [ -d "$ROOT/build/$name.kext" ] || die "build/$name.kext not found; run scripts/build.sh"
    ssh "$RETRO" "qm delsnapshot $VMID pre-install >/dev/null 2>&1; qm snapshot $VMID pre-install >/dev/null" &&
        echo "snapshot pre-install taken"
    COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --no-mac-metadata -C "$ROOT/build" -cf - "$name.kext" |
        gssh "set -e; mkdir -p /tmp/ripple; sudo -n rm -rf /tmp/ripple/$name.kext; tar -C /tmp/ripple -xf -
              sudo -n rm -rf $dest; sudo -n cp -R /tmp/ripple/$name.kext $dest
              sudo -n chown -R root:wheel $dest; sudo -n chmod -R u=rwX,go=rX $dest
              sudo -n kextutil -n -t $dest 2>&1 | tail -2
              sudo -n kextcache -system-mkext 2>&1 | tail -1; echo installed $dest"
fi
"$here/stop.sh"
"$here/boot.sh" "$flags"
