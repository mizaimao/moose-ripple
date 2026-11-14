#!/bin/sh
# Pack the built guest files into OUT/moose-ripple-VERSION.zip (run by `make package`).
#   package/package.sh OUT VERSION [TOOLS]
# The zip holds the three kexts, the GL bundle, the GA plugin, qe-check, install.sh,
# uninstall.sh, INSTALL, LICENSE, Mesa's license (from the Mesa source under TOOLS) and
# SHA256SUMS (one line per file, `shasum -a 256 -c` format).
set -eu
out=${1:?usage: package.sh OUT VERSION}
version=${2:?usage: package.sh OUT VERSION}
tools=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
name=moose-ripple-$version
dir=$out/$name

for f in RippleVirtioGPU.kext RippleGLDriver.bundle RippleGA.plugin RippleQEMUTablet.kext RippleVirtioSound.kext qe-check; do
    [ -e "$out/$f" ] || { echo "package.sh: $out/$f missing; build it first" >&2; exit 1; }
done
rm -rf "$dir" "$out/$name.zip"
mkdir -p "$dir"
cp -R "$out/RippleVirtioGPU.kext" "$out/RippleGLDriver.bundle" "$out/RippleGA.plugin" \
      "$out/RippleQEMUTablet.kext" "$out/RippleVirtioSound.kext" "$out/qe-check" "$dir/"
cp "$here/install.sh" "$here/uninstall.sh" "$dir/"
cp "$here/../LICENSE" "$dir/LICENSE"
mesalic=$(ls -d "$tools"/mesa/src-*/docs/license.rst 2>/dev/null | tail -1 || true)
[ -n "$mesalic" ] || { echo "package.sh: Mesa's docs/license.rst not found under $tools/mesa" >&2; exit 1; }
cp "$mesalic" "$dir/LICENSE-Mesa.rst"
sed "s/@VERSION@/$version/g" "$here/INSTALL" > "$dir/INSTALL"
chmod -R u=rwX,go=rX "$dir"
chmod 755 "$dir/qe-check" "$dir/install.sh" "$dir/uninstall.sh" "$dir"/*/Contents/MacOS/* 2>/dev/null || true
(cd "$dir" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)
chmod 644 "$dir/SHA256SUMS"
(cd "$out" && zip -q -r -X "$name.zip" "$name")
echo "$out/$name.zip: $(wc -c < "$out/$name.zip") bytes, $(wc -l < "$dir/SHA256SUMS") files"
