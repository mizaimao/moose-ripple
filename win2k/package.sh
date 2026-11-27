#!/bin/sh
# Build the Windows 2000/XP package on the Linux build host:
#   VERSION=0.4.1 THIRDPTY=/path/to/THIRDPTY.TXT win2k/package.sh
# makes OUT/moose-ripple-win2k-VERSION.zip (OUT defaults to build/win2k-package) and its
# SHA256SUMS.txt. The driver files and the INF come from win2k/build.py, with the INF's
# DriverVer set to VERSION.0 and the build date. ripplegl.dll is built from this checkout's
# win9x/mesa: only a build that has the Windows 2000/XP path in its winsys (it reaches the
# display driver with ExtEscape) draws OpenGL on the host GPU there. Without that path Mesa
# falls back to softpipe, which is what release 0.4.0 shipped (it reused the Windows 9x
# 0.2.0 build); the script refuses such a DLL. THIRDPTY.TXT, the notices for ripplegl.dll,
# comes from the caller and must name this checkout's source. Test the package with
# win2k/tools/pkgtest.cmd before publishing it (docs/win2k/development.md, Release package).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$here/.." && pwd)
: "${VERSION:?set VERSION, for example 0.4.1}"
: "${THIRDPTY:?set THIRDPTY to the notices file for ripplegl.dll}"
case "$VERSION" in *[!0-9.]*|"") echo "package.sh: VERSION is digits and dots" >&2; exit 1 ;; esac
[ -f "$THIRDPTY" ] || { echo "package.sh: no $THIRDPTY" >&2; exit 1; }
T=${TOOLS:-$root/.tools}
PYTHON=${PYTHON:-$root/.venv/bin/python}
[ -x "$PYTHON" ] || PYTHON=$(dirname "$T")/.venv/bin/python
LLVM_MINGW=${LLVM_MINGW:-$(ls -d "$T"/llvm-mingw/llvm-mingw-*-msvcrt-*/ 2>/dev/null | head -1)}
LLVM_MINGW=${LLVM_MINGW%/}
export PYTHON LLVM_MINGW
export WIN9X_TOOLS=${WIN9X_TOOLS:-$T/win9x}
export WDK_ROOT=${WDK_ROOT:-$T/wdk71/kit/WinDDK/7600.16385.win7_wdk.100208-1538}
out=${OUT:-$root/build/win2k-package}
work=$out/work
name=moose-ripple-win2k-$VERSION
rm -rf "$work" "$out/$name.zip" "$out/SHA256SUMS.txt"
mkdir -p "$work/driver" "$work/mesa" "$work/$name"

# A release has no use for the test tools' skip counts query (rbskips.h).
DEVCHECKS=${DEVCHECKS:-0} OUT=$work/driver "$PYTHON" "$here/build.py"
OUT=$work/mesa TOOLS=$T "$root/win9x/mesa/build.sh"
if ! "$LLVM_MINGW/bin/llvm-objdump" -p "$work/mesa/ripplegl.dll" | grep -q 'ExtEscape'; then
    echo "package.sh: this ripplegl.dll has no Windows 2000/XP path (no ExtEscape import)" >&2
    exit 1
fi

pkg=$work/$name
for f in rb2kmp.sys rb2kdisp.dll rbntmode.dll; do cp "$work/driver/$f" "$pkg/"; done
cp "$work/mesa/ripplegl.dll" "$pkg/"
sed "s|^DriverVer=.*|DriverVer=$(date -u +%m/%d/%Y),$VERSION.0\r|" "$work/driver/rb2k.inf" > "$pkg/rb2k.inf"
sed 's/\r$//; s/$/\r/' "$root/LICENSE" > "$pkg/LICENSE.TXT"
sed "s/@VERSION@/$VERSION/; s/\r$//; s/\$/\r/" "$here/package/README.TXT" > "$pkg/README.TXT"
cp "$THIRDPTY" "$pkg/THIRDPTY.TXT"
grep -q "^DriverVer=.*,$VERSION\.0" "$pkg/rb2k.inf" || { echo "package.sh: INF version not set" >&2; exit 1; }

(cd "$pkg" && zip -q -X -9 "$out/$name.zip" *)
(cd "$out" && sha256sum "$name.zip" > SHA256SUMS.txt)
cat "$out/SHA256SUMS.txt"
