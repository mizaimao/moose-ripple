#!/bin/sh
# Install build/RippleGLDriver.bundle (M4), and build/RippleGA.plugin (M5) if built, into the dev
# VM's /System/Library/Extensions, where
# OpenGL loads it from once the accelerator names it (boot flag ripple_gl=1, kext README).
# Only processes started with RIPPLE_GL=1 then see the renderer (until GL output reaches
# windows). Takes the rolling snapshot "pre-glbundle" first; roll back with
#   scripts/snapshot.sh -b pre-glbundle
#   scripts/install-glbundle.sh      install (replaces an older copy)
#   scripts/install-glbundle.sh -u   remove
set -eu
. "$(dirname "$0")/lib.sh"

dest=/System/Library/Extensions/RippleGLDriver.bundle
ga=/System/Library/Extensions/RippleGA.plugin
if [ "${1:-}" = -u ]; then
    gssh "sudo -n rm -rf $dest $ga && echo removed $dest $ga"
    exit 0
fi
extra=
[ -d "$ROOT/build/RippleGA.plugin" ] && extra=RippleGA.plugin
[ -d "$ROOT/build/RippleGLDriver.bundle" ] || die "no build/RippleGLDriver.bundle; run scripts/build.sh glbundle"

ssh "$RETRO" "qm delsnapshot $VMID pre-glbundle >/dev/null 2>&1; qm snapshot $VMID pre-glbundle >/dev/null" &&
    echo "snapshot pre-glbundle taken"
tar --format ustar --no-xattrs --no-mac-metadata -C "$ROOT/build" -cf - RippleGLDriver.bundle $extra |
    gssh "rm -rf /tmp/ripple/RippleGLDriver.bundle /tmp/ripple/RippleGA.plugin && mkdir -p /tmp/ripple &&
          tar -C /tmp/ripple -xf - &&
          sudo -n rm -rf $dest && sudo -n cp -R /tmp/ripple/RippleGLDriver.bundle $dest &&
          sudo -n chown -R root:wheel $dest && file $dest/Contents/MacOS/RippleGLDriver &&
          if [ -d /tmp/ripple/RippleGA.plugin ]; then sudo -n rm -rf $ga &&
              sudo -n cp -R /tmp/ripple/RippleGA.plugin $ga && sudo -n chown -R root:wheel $ga &&
              echo installed $ga; fi"
