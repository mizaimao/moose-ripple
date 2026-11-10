#!/bin/sh
# Build on dev.lan and copy the results into this repo's build/ (git-ignored).
#   scripts/build.sh [MAKE-TARGETS]      default: all
# Sources go to dev.lan:~/Projects/moose-ripple (no .git, .secrets or .tools); the
# toolchain lives in .tools/ there (docs/decisions/0001-toolchain.md).
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
cd "$ROOT"
rsync -a --delete --exclude .git --exclude .secrets --exclude .tools --exclude build \
    ./ "$DEV:Projects/moose-ripple/"
ssh "$DEV" "cd ~/Projects/moose-ripple && make -j4 $*"
mkdir -p build
rsync -a --delete --include '*.kext/***' --exclude '*' "$DEV:Projects/moose-ripple/build/" build/
ls -d build/*.kext
