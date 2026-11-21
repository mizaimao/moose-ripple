#!/bin/sh
# Cross-compile the NT5 driver and diagnostics on Linux.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "${PYTHON:-$here/../.venv/bin/python}" "$here/build.py"
