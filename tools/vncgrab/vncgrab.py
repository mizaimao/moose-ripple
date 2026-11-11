#!/usr/bin/env python3
# Save what a Proxmox VM's display shows (QEMU's VNC server, so also a virtio-gl scanout that
# QEMU's screendump can't see: "no surface") as a PNG. Python 3 standard library only.
#   VNC_PASSWORD=<pw> .venv/bin/python -I vncgrab.py OUT.png   speaks RFB on stdin/stdout
#   ... vncgrab.py -n 12 -i 0.15 OUT.png    12 frames on one connection, one every 0.15 s (or as
#                                           fast as they come), saved as OUT-00.png..OUT-11.png
# Runs on dev.lan in the repo's .venv; scripts/screen-vnc.sh connects its stdin/stdout to
# `qm vncproxy` on the rig (the web console's path), which sets that one-off password on the
# VM's VNC server for 30 seconds. RFB 3.8, VNC authentication, raw encoding.
import os
import struct
import sys
import time
import zlib

# ---- DES (FIPS 46-3), only what VNC authentication needs: ECB encryption of 8-byte blocks --
PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3,
       60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37,
       29, 21, 13, 5, 28, 20, 12, 4]
PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
       41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
      64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
      61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
FP = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
      37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
      34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25]
E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
     16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1]
P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
     2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
S = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
]


def _permute(v, table, nbits):
    out = 0
    for pos in table:
        out = (out << 1) | ((v >> (nbits - pos)) & 1)
    return out


def des_encrypt(key8, block8):
    k = _permute(int.from_bytes(key8, "big"), PC1, 64)
    c, d = k >> 28, k & 0xFFFFFFF
    subkeys = []
    for s in SHIFTS:
        c = ((c << s) | (c >> (28 - s))) & 0xFFFFFFF
        d = ((d << s) | (d >> (28 - s))) & 0xFFFFFFF
        subkeys.append(_permute((c << 28) | d, PC2, 56))
    v = _permute(int.from_bytes(block8, "big"), IP, 64)
    l, r = v >> 32, v & 0xFFFFFFFF
    for sk in subkeys:
        x = _permute(r, E, 32) ^ sk
        f = 0
        for i in range(8):
            six = (x >> (42 - 6 * i)) & 0x3F
            f = (f << 4) | S[i][((six & 0x20) | ((six & 1) << 4)) | ((six >> 1) & 0xF)]
        l, r = r, l ^ _permute(f, P, 32)
    return _permute((r << 32) | l, FP, 64).to_bytes(8, "big")


def vnc_response(password, challenge):
    """VNC authentication: DES-encrypt the challenge with the bit-reversed password as key."""
    key = bytes(int("{:08b}".format(b)[::-1], 2) for b in password.encode()[:8].ljust(8, b"\0"))
    return des_encrypt(key, challenge[:8]) + des_encrypt(key, challenge[8:16])


# ---- RFB client -------------------------------------------------------------------------
class Conn:
    def __init__(self, r, w):
        self.r, self.w = r, w

    def read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.r.read(n - len(buf))
            if not chunk:
                raise EOFError("VNC connection closed after %d of %d bytes" % (len(buf), n))
            buf += chunk
        return buf

    def write(self, data):
        self.w.write(data)
        self.w.flush()


def handshake(c, password):
    """RFB 3.8 with VNC authentication; returns the screen size."""
    c.read(12)                                   # "RFB 003.008\n"
    c.write(b"RFB 003.008\n")
    n = c.read(1)[0]
    if n == 0:
        raise RuntimeError("server refused: " + c.read(struct.unpack(">I", c.read(4))[0]).decode())
    types = c.read(n)
    if 2 not in types:
        raise RuntimeError("no VNC authentication offered: %r" % list(types))
    c.write(b"\x02")
    c.write(vnc_response(password, c.read(16)))
    if struct.unpack(">I", c.read(4))[0] != 0:
        raise RuntimeError("VNC authentication failed")
    c.write(b"\x01")                              # ClientInit, shared
    w, h = struct.unpack(">HH", c.read(4))
    c.read(16)                                    # server pixel format
    c.read(struct.unpack(">I", c.read(4))[0])     # desktop name
    # SetPixelFormat: 32 bpp, depth 24, little-endian, true colour, R/G/B at 16/8/0.
    c.write(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
    c.write(struct.pack(">BxHii", 2, 2, 0, -223))  # encodings: Raw, DesktopSize
    return w, h


def frame(c, w, h):
    """Ask for the whole screen (non-incremental, so the server answers at once) and return
    (w, h, BGRX bytes). A DesktopSize rectangle means the mode changed: ask again."""
    c.write(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
    fb = bytearray(w * h * 4)
    while True:
        t = c.read(1)[0]
        if t == 0:
            nrect = struct.unpack(">xH", c.read(3))[0]
            resized = False
            for _ in range(nrect):
                x, y, rw, rh, enc = struct.unpack(">HHHHi", c.read(12))
                if enc == -223:
                    w, h, resized = rw, rh, True
                    fb = bytearray(w * h * 4)
                    continue
                if enc != 0:
                    raise RuntimeError("unexpected encoding %d" % enc)
                data = c.read(rw * rh * 4)
                for row in range(rh):
                    o = ((y + row) * w + x) * 4
                    fb[o:o + rw * 4] = data[row * rw * 4:(row + 1) * rw * 4]
            if not resized:
                return w, h, fb
            c.write(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
        elif t == 1:                              # SetColourMapEntries
            ncol = struct.unpack(">xHH", c.read(5))[1]
            c.read(ncol * 6)
        elif t == 2:                              # Bell
            pass
        elif t == 3:                              # ServerCutText
            c.read(struct.unpack(">xxxI", c.read(7))[0])
        else:
            raise RuntimeError("unknown server message %d" % t)


def png(w, h, bgrx):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = bgrx[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        raw += rgb

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        # FIPS 46 example: key 133457799BBCDFF1, plaintext 0123456789ABCDEF -> 85E813540F0AB405
        got = des_encrypt(bytes.fromhex("133457799BBCDFF1"), bytes.fromhex("0123456789ABCDEF")).hex()
        sys.exit(0 if got == "85e813540f0ab405" else "DES self-test failed: " + got)
    args, count, interval = sys.argv[1:], 1, 0.0
    try:
        while len(args) > 1 and args[0] in ("-n", "-i"):
            if args[0] == "-n":
                count = int(args[1])
            else:
                interval = float(args[1])
            args = args[2:]
    except ValueError:
        args = []
    if len(args) != 1 or count < 1 or interval < 0 or "VNC_PASSWORD" not in os.environ:
        sys.exit("usage: VNC_PASSWORD=<pw> vncgrab.py [-n FRAMES] [-i SECONDS] OUT.png  "
                 "(RFB on stdin/stdout)")
    out = args[0]
    conn = Conn(sys.stdin.buffer, sys.stdout.buffer)
    width, height = handshake(conn, os.environ["VNC_PASSWORD"])
    # Grab every frame first and encode afterwards: PNG encoding takes longer than a frame.
    frames, t0 = [], time.monotonic()
    for i in range(count):
        if i:
            time.sleep(max(0.0, t0 + i * interval - time.monotonic()))
        t = time.monotonic() - t0
        width, height, pixels = frame(conn, width, height)
        frames.append((t, width, height, bytes(pixels)))
    sys.stdout.close()
    stem = out[:-4] if out.endswith(".png") else out
    for i, (t, w, h, pixels) in enumerate(frames):
        name = out if count == 1 else "%s-%02d.png" % (stem, i)
        with open(name, "wb") as f:
            f.write(png(w, h, pixels))
        if count > 1:
            print("%s: requested at +%.3f s" % (os.path.basename(name), t), file=sys.stderr)
