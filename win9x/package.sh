#!/bin/sh
# Build the Windows 95/98/Me package on the Linux build host:
#   VERSION=0.6.0 THIRDPTY=/path/to/THIRDPTY.TXT win9x/package.sh
# makes OUT/moose-ripple-win9x-VERSION.zip and .iso (OUT defaults to build/win9x-package) and
# their SHA256SUMS.txt. The driver files and the INF come from win9x/build.sh, with the INF's
# DriverVer set to VERSION.0 and the build date; ripplegl.dll is built from this checkout's
# win9x/mesa (TOOLS chooses the folder that holds its source and build trees, .tools by
# default). THIRDPTY.TXT, the notices for ripplegl.dll, comes from the caller and must name
# this release's source. Both downloads hold the same eight files, upper-case 8.3 names as on
# the earlier releases. Test the package with a clean install before publishing it.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$here/.." && pwd)
: "${VERSION:?set VERSION, for example 0.6.0}"
: "${THIRDPTY:?set THIRDPTY to the notices file for ripplegl.dll}"
case "$VERSION" in *[!0-9.]*|"") echo "package.sh: VERSION is digits and dots" >&2; exit 1 ;; esac
[ -f "$THIRDPTY" ] || { echo "package.sh: no $THIRDPTY" >&2; exit 1; }
grep -q "$VERSION" "$THIRDPTY" || { echo "package.sh: $THIRDPTY does not name $VERSION" >&2; exit 1; }
command -v genisoimage >/dev/null || { echo "package.sh: genisoimage is needed for the ISO" >&2; exit 1; }
T=${TOOLS:-$root/.tools}
export WIN9X_TOOLS=${WIN9X_TOOLS:-$root/.tools/win9x}
out=${OUT:-$root/build/win9x-package}
work=$out/work
name=moose-ripple-win9x-$VERSION
rm -rf "$work" "$out/$name.zip" "$out/$name.iso" "$out/SHA256SUMS.txt"
mkdir -p "$work/driver" "$work/mesa" "$work/$name"

OUT=$work/driver sh "$here/build.sh"
OUT=$work/mesa TOOLS=$T sh "$here/mesa/build.sh"

pkg=$work/$name
cp "$work/driver/ripple9x.drv" "$pkg/RIPPLE9X.DRV"
cp "$work/driver/ripplevd.vxd" "$pkg/RIPPLEVD.VXD"
cp "$work/driver/rippledd.dll" "$pkg/RIPPLEDD.DLL"
cp "$work/mesa/ripplegl.dll" "$pkg/RIPPLEGL.DLL"
sed "s|^DriverVer=.*|DriverVer=$(date -u +%m/%d/%Y),$VERSION.0\r|" "$work/driver/ripple9x.inf" > "$pkg/RIPPLE9X.INF"
sed 's/\r$//; s/$/\r/' "$root/LICENSE" > "$pkg/LICENSE.TXT"
sed "s/@VERSION@/$VERSION/g; s/\r$//; s/\$/\r/" "$here/package/README.TXT" > "$pkg/README.TXT"
sed 's/\r$//; s/$/\r/' "$THIRDPTY" > "$pkg/THIRDPTY.TXT"
grep -q "^DriverVer=.*,$VERSION\.0" "$pkg/RIPPLE9X.INF" || { echo "package.sh: INF version not set" >&2; exit 1; }

touch -d "$(date -u +%Y-%m-%d) 00:00:00" "$pkg"/*
(cd "$pkg" && zip -q -X -9 "$out/$name.zip" *)
genisoimage -quiet -J -r -V RIPPLE9X -o "$out/$name.iso" "$pkg"
(cd "$out" && sha256sum "$name.iso" "$name.zip" > SHA256SUMS.txt)
cat "$out/SHA256SUMS.txt"
