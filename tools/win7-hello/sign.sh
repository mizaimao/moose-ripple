#!/bin/sh
# Test-sign Windows drivers and DLLs off Windows with osslsigncode (docs/windows/toolchain.md).
#   tools/win7-hello/sign.sh FILE...        writes FILE in place, signed (SHA-256)
# The test certificate and its key are made once in .tools/win7-testcert/ (never committed).
# Windows 7 x64 loads a kernel driver signed this way only in test mode
# (bcdedit /set testsigning on), and checks SHA-256 only with KB3033929 or KB4474419.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
OSC=${OSSLSIGNCODE:-$root/.tools/osslsigncode/build/osslsigncode}
C=$root/.tools/win7-testcert
[ -x "$OSC" ] || { echo "sign.sh: no osslsigncode at $OSC" >&2; exit 1; }
if [ ! -f "$C/cert.pem" ]; then
    mkdir -p "$C" && chmod 700 "$C"
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
        -subj "/CN=moose-ripple test signing" -addext "extendedKeyUsage=codeSigning" \
        -keyout "$C/key.pem" -out "$C/cert.pem" 2>/dev/null
    openssl x509 -in "$C/cert.pem" -outform der -out "$C/moose-ripple-test.cer"
    chmod 600 "$C/key.pem"
fi
for f in "$@"; do
    "$OSC" sign -certs "$C/cert.pem" -key "$C/key.pem" -h sha256 -n "moose-ripple test" \
        -in "$f" -out "$f.signed" >/dev/null && mv "$f.signed" "$f"
    "$OSC" verify -in "$f" 2>/dev/null | grep -E "^(Signer|Signature verification)" | head -2 || true
    echo "signed $f"
done
