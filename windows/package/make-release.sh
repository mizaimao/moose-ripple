#!/bin/sh
# Build, sign and pack the display driver for Windows Vista SP2 and Windows 7, both x64.
#   windows/package/make-release.sh VERSION [OUT]     VERSION like 0.4.0; OUT: build/vista7-VERSION
# Runs on the Linux build host (docs/windows/toolchain.md). From the environment:
#   MAIN            the checkout that holds .tools and .venv (default: this tree's root)
#   WDK_VISTA_LIBS  the WDK's Vista import libraries (lib/wlh), when unpacked apart from the kit
#   VISTA_WINDOWS   a Vista SP2 Windows directory: the Vista binaries' imports are checked
#                   against it (tools/vista/audit-imports.py)
#   REPACK=1        keep the binaries of an earlier run in OUT as built and signed (no build is
#                   the same twice: the kernel driver has its build time inside) and make only
#                   the INFs, the texts and the archive anew
# Result: OUT/moose-ripple-vista7-VERSION.zip and OUT/SHA256SUMS.txt.
#
# Signing. 64-bit Windows loads a kernel driver only when it is signed. This package is signed
# with a test certificate, which Windows accepts in test mode (bcdedit /set testsigning on) and
# not otherwise. Vista SP2, and a Windows 7 without its later code-signing updates (KB3033929,
# KB4474419), check SHA-1 signatures only, so the certificate and the signature are SHA-1. The
# certificate and its key are made once in MAIN/.tools/vista7-testcert/ and are never committed
# or packed. The signature is inside viogpu3d.sys; there is no catalog, which Windows needs only
# to name a publisher it trusts, and it does not trust a test certificate.
#
# The kernel driver is built as in every test so far: with its warnings written to the first
# serial port (DEBUG=1 LEVEL=3 in windows/kmd/build.sh).
set -eu
ver=${1:?usage: make-release.sh VERSION [OUT]}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
export MAIN=${MAIN:-$root}
OUT=${2:-$root/build/vista7-$ver}
T=$MAIN/.tools
OSC=${OSSLSIGNCODE:-$T/osslsigncode/build/osslsigncode}
PY=$MAIN/.venv/bin/python
C=$T/vista7-testcert
LIC=$T/src/vista7-licenses
LLVM_MINGW=${LLVM_MINGW:-$T/llvm-mingw/llvm-mingw-20261104-msvcrt-ubuntu-22.04-x86_64}
# The LLVM commit llvm-mingw 20261104 was built from (clang --version), for its licence files.
LLVM_COMMIT=392dfff97d29b59d77e5cac6d4b6ef2585fe7117
name=moose-ripple-vista7-$ver
vista_libs=${WDK_VISTA_LIBS:-}
unset WDK_TARGET_LIBS RIPPLE_TARGET ARCH VARIANT TRACE_PRESENT TEST_TDR TRACE_TIMING

die() { echo "make-release.sh: $*" >&2; exit 1; }
for inf in "$here/vista/viogpu3d.inf" "$here/viogpu3d.inf"; do
    grep -q "^DriverVer=[0-9/]*,$ver\.0[[:space:]]*\$" "$inf" || die "$inf does not say version $ver.0"
    ! grep -qi "^CatalogFile" "$inf" || die "$inf names a catalog, and the package has none"
done
[ -x "$OSC" ] || die "no osslsigncode at $OSC"
[ -x "$PY" ] || die "no Python at $PY"
mkdir -p "$OUT"

# The test certificate: RSA 2048, SHA-1, for code signing.
if [ ! -f "$C/cert.pem" ]; then
    mkdir -p "$C" && chmod 700 "$C"
    openssl req -x509 -newkey rsa:2048 -nodes -sha1 -days 7300 \
        -subj "/CN=moose-ripple test signing (Vista and 7)" -addext "extendedKeyUsage=codeSigning" \
        -keyout "$C/key.pem" -out "$C/cert.pem" 2>/dev/null || die "could not make the test certificate"
    chmod 600 "$C/key.pem"
fi
openssl x509 -in "$C/cert.pem" -noout -text | grep -q "Signature Algorithm: sha1WithRSAEncryption" \
    || die "the certificate in $C is not SHA-1"

# Licence files that are in neither source tree.
fetch() {  # NAME URL SHA256
    [ -f "$LIC/$1" ] || { mkdir -p "$LIC"; curl -sfL -o "$LIC/$1" "$2" || die "could not fetch $2"; }
    echo "$3  $LIC/$1" | sha256sum -c --quiet || die "$LIC/$1 is not the expected file"
}
llvm=https://raw.githubusercontent.com/llvm/llvm-project/$LLVM_COMMIT
fetch libcxx.txt      $llvm/libcxx/LICENSE.TXT      539dd7aed86e8a4f12cbdd0e6c50c189c7d74847e4fecc64ce2c6ee3a01da38b
fetch libcxxabi.txt   $llvm/libcxxabi/LICENSE.TXT   e2b35be49f7284a45b7baca8fc7b3ab7440e7902392b2528a457816b5bb2a15c
fetch libunwind.txt   $llvm/libunwind/LICENSE.TXT   b5efebcaca80879234098e52d1725e6d9eb8fb96a19fce625d39184b705f7b6d
fetch compiler-rt.txt $llvm/compiler-rt/LICENSE.TXT 1a8f1058753f1ba890de984e48f0242a3a5c29a6a8f2ed9fd813f36985387e8d
fetch blake3.txt https://raw.githubusercontent.com/BLAKE3-team/BLAKE3/1.5.1/LICENSE \
    6a94bedb8b707ed97f6e310d0d015ab14e0683ffa0a612b02958581b9cc9fc0e

stage=$OUT/$name
crlf() { sed -e 's/\r$//' -e 's/$/\r/' "$1" >"$2"; }
if [ -z "${REPACK:-}" ]; then
# 1. Build: the kernel driver and the user-mode driver (64-bit and 32-bit) for each system.
echo "== kernel driver, Vista"
if [ -n "$vista_libs" ]; then
    DEBUG=1 LEVEL=3 RIPPLE_TARGET=vista WDK_TARGET_LIBS=$vista_libs "$root/windows/kmd/build.sh" "$OUT/kmd-vista"
else
    DEBUG=1 LEVEL=3 RIPPLE_TARGET=vista "$root/windows/kmd/build.sh" "$OUT/kmd-vista"
fi
echo "== kernel driver, Windows 7"
DEBUG=1 LEVEL=3 RIPPLE_TARGET=win7 "$root/windows/kmd/build.sh" "$OUT/kmd-win7"
for t in vista win7; do
    for a in x64 x86; do
        echo "== user-mode driver, $t $a"
        RIPPLE_TARGET=$t ARCH=$a VARIANT=virgl "$root/windows/mesa/build-win7.sh" >"$OUT/mesa-$t-$a.log" 2>&1 \
            || { tail -30 "$OUT/mesa-$t-$a.log" >&2; die "the $t $a user-mode driver did not build"; }
    done
done

# 2. The package folder.
rm -rf "$stage"
mkdir -p "$stage/vista" "$stage/win7"
for t in vista win7; do
    cp "$OUT/kmd-$t/amd64/viogpu3d.sys" "$stage/$t/viogpu3d.sys"
    cp "$T/mesa/out-$t-x64-virgl/viogpu_d3d10.dll" "$stage/$t/viogpu_d3d10.dll"
    cp "$T/mesa/out-$t-x86-virgl/viogpu_d3d10.dll" "$stage/$t/viogpu_d3d10.x86.dll"
done
# Windows 7 also gets Mesa's OpenGL driver and the library it shares with opengl32.dll
cp "$T/mesa/out-win7-x64-virgl/viogpu_wgl.dll" "$stage/win7/viogpu_wgl.dll"
cp "$T/mesa/out-win7-x86-virgl/viogpu_wgl.dll" "$stage/win7/viogpu_wgl.x86.dll"
cp "$T/mesa/out-win7-x64-virgl/libglapi.dll" "$stage/win7/libglapi.dll"
cp "$T/mesa/out-win7-x86-virgl/libglapi.dll" "$stage/win7/libglapi.x86.dll"
if [ -n "${VISTA_WINDOWS:-}" ]; then
    echo "== Vista imports"
    "$PY" -I "$root/tools/vista/audit-imports.py" "$stage/vista" "$VISTA_WINDOWS" "$T/mesa/src-24.3.4-vista-virgl"
fi

fi
crlf "$here/vista/viogpu3d.inf" "$stage/vista/viogpu3d.inf"
crlf "$here/viogpu3d.inf" "$stage/win7/viogpu3d.inf"

# 3. Sign the kernel drivers.
for t in vista win7; do
    f=$stage/$t/viogpu3d.sys
    if [ -z "${REPACK:-}" ]; then
    "$OSC" sign -certs "$C/cert.pem" -key "$C/key.pem" -h sha1 -n "moose-ripple display driver" \
        -i https://github.com/mizaimao/moose-ripple -in "$f" -out "$f.signed" >/dev/null
    mv "$f.signed" "$f"
    fi
    "$OSC" verify -in "$f" -CAfile "$C/cert.pem" >"$OUT/verify-$t.txt" 2>&1 || true
    grep -q "^Signature verification: ok" "$OUT/verify-$t.txt" || { cat "$OUT/verify-$t.txt" >&2; die "$f: signature check failed"; }
    grep -q "Message digest algorithm *: SHA1" "$OUT/verify-$t.txt" || die "$f: the signature is not SHA-1"
done

# 4. Texts.
sed "s/@VERSION@/$ver/g" "$here/README.txt" >"$OUT/README.tmp"
crlf "$OUT/README.tmp" "$stage/README.txt" && rm "$OUT/README.tmp"
crlf "$root/LICENSE" "$stage/LICENSE.txt"
PATH="$MAIN/.venv/bin:$PATH" "$PY" -I "$here/notices.py" --kmd "$root/windows/kmd" \
    --mesa "$T/mesa/src-24.3.4-vista-virgl" "$T/mesa/build-vista-x64-virgl" \
    --mesa "$T/mesa/src-24.3.4-vista-virgl" "$T/mesa/build-vista-x86-virgl" \
    --mesa "$T/mesa/src-24.3.4-win-virgl" "$T/mesa/build-win7-x64-virgl" \
    --mesa "$T/mesa/src-24.3.4-win-virgl" "$T/mesa/build-win7-x86-virgl" \
    --toolchain "$LLVM_MINGW" --licenses "$LIC" --out "$stage/THIRD-PARTY-NOTICES.txt"

# 5. The archive: the same bytes for the same input (sorted names, the INF's date on every file).
rm -f "$OUT/$name.zip"
"$PY" -I - "$stage" "$OUT/$name.zip" "$(sed -n 's/^DriverVer=\([0-9/]*\),.*/\1/p' "$here/viogpu3d.inf")" <<'EOF'
import sys, zipfile
from pathlib import Path
stage, out, date = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
month, day, year = (int(x) for x in date.split('/'))
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in sorted(q for q in stage.rglob('*') if q.is_file()):
        info = zipfile.ZipInfo(p.relative_to(stage).as_posix(), (year, month, day, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        z.writestr(info, p.read_bytes())
EOF
(cd "$stage" && find . -type f | LC_ALL=C sort | sed 's#^\./##' | xargs sha256sum) >"$OUT/files.sha256"
(cd "$OUT" && sha256sum "$name.zip" >SHA256SUMS.txt)
cat "$OUT/files.sha256"
cat "$OUT/SHA256SUMS.txt"
ls -l "$OUT/$name.zip"
