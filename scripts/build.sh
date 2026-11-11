#!/bin/sh
# Build on dev.lan and copy the results into this repo's build/ (git-ignored).
#   scripts/build.sh [MAKE-TARGETS]      default: all
# Sources go to dev.lan:~/Projects/moose-ripple (no .git, .secrets, .tools or .venv); the
# toolchain lives in .tools/ there (docs/decisions/0001-toolchain.md).
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
cd "$ROOT"
rsync -a --delete --exclude .git --exclude .secrets --exclude .tools --exclude .venv --exclude build --exclude 'tmp-*' \
    ./ "$DEV:Projects/moose-ripple/"
ssh $SSH_OPTS "$DEV" "cd ~/Projects/moose-ripple && make -j4 $*"
mkdir -p build
# No --delete here: build/ keeps what other targets (and other agents) built.
rsync -a --include '*.kext/***' --include '*.bundle/***' --include '*.plugin/***' --exclude '*/' --include '*' "$DEV:Projects/moose-ripple/build/" build/
ls -d build/*
