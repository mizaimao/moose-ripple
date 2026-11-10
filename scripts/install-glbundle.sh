#!/bin/sh
# Install build/RippleGLDriver.bundle (M4) into the dev VM's /System/Library/Extensions, where
# OpenGL loads it from once the accelerator names it (boot flag ripple_gl=1, kext README).
# Only processes started with RIPPLE_GL=1 then see the renderer (until GL output reaches
# windows). Takes the rolling snapshot "pre-glbundle" first; roll back with
#   scripts/snapshot.sh -b pre-glbundle
#   scripts/install-glbundle.sh      install (replaces an older copy)
#   scripts/install-glbundle.sh -u   remove
set -eu
. "$(dirname "$0")/lib.sh"

dest=/System/Library/Extensions/RippleGLDriver.bundle
if [ "${1:-}" = -u ]; then
    gssh "sudo -n rm -rf $dest && echo removed $dest"
    exit 0
fi
[ -d "$ROOT/build/RippleGLDriver.bundle" ] || die "no build/RippleGLDriver.bundle; run scripts/build.sh glbundle"

ssh "$RETRO" "qm delsnapshot $VMID pre-glbundle >/dev/null 2>&1; qm snapshot $VMID pre-glbundle >/dev/null" &&
    echo "snapshot pre-glbundle taken"
tar --format ustar --no-xattrs --no-mac-metadata -C "$ROOT/build" -cf - RippleGLDriver.bundle |
    gssh "rm -rf /tmp/ripple/RippleGLDriver.bundle && mkdir -p /tmp/ripple && tar -C /tmp/ripple -xf - &&
          sudo -n rm -rf $dest && sudo -n cp -R /tmp/ripple/RippleGLDriver.bundle $dest &&
          sudo -n chown -R root:wheel $dest && file $dest/Contents/MacOS/RippleGLDriver"
