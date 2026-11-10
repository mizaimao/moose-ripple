#!/bin/sh
# Copy a kext from build/ to the dev VM, snapshot the VM, load the kext, show its log lines.
#   scripts/deploy.sh RippleHello            load (unloads an older copy first)
#   scripts/deploy.sh -u RippleHello         unload only
#   scripts/deploy.sh -n RippleHello         no snapshot (when nothing new can panic)
# The kext is loaded from /tmp/ripple in the guest with kextutil; nothing is installed in
# /System/Library/Extensions.
set -eu
. "$(dirname "$0")/lib.sh"

snap=1 unload_only=0
while [ $# -gt 1 ]; do
    case "$1" in
        -n) snap=0 ;;
        -u) unload_only=1 ;;
        *) die "unknown option $1" ;;
    esac
    shift
done
name=${1:?usage: deploy.sh [-n] [-u] KEXT-NAME}
kext=$ROOT/build/$name.kext
[ "$unload_only" = 1 ] || [ -d "$kext" ] || die "$kext not found; run scripts/build.sh"
id=$(sed -n '/CFBundleIdentifier/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' "$ROOT/build/$name.kext/Contents/Info.plist" 2>/dev/null ||
     true)
[ -n "$id" ] || die "no CFBundleIdentifier for $name"

# Unload a loaded copy first.
gssh "if kextstat -l -b $id | grep -q $id; then sudo -n kextunload -b $id && echo 'unloaded $id'; fi"
[ "$unload_only" = 0 ] || exit 0

if [ "$snap" = 1 ]; then
    # One rolling snapshot: the state just before the latest load.
    ssh "$RETRO" "qm delsnapshot $VMID pre-load >/dev/null 2>&1; qm snapshot $VMID pre-load >/dev/null" &&
        echo "snapshot pre-load taken"
fi

COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --no-mac-metadata -C "$ROOT/build" -cf - "$name.kext" | gssh "sudo -n rm -rf /tmp/ripple/$name.kext && mkdir -p /tmp/ripple &&
    tar -C /tmp/ripple -xf - && sudo -n chown -R root:wheel /tmp/ripple/$name.kext &&
    sudo -n chmod -R u=rwX,go=rX /tmp/ripple/$name.kext"
gssh "sudo -n kextutil -v 1 /tmp/ripple/$name.kext; echo \"kextutil exit \$?\"; kextstat -l -b $id"
