"""Print pixel values of PNGs written by vncgrab.py (8-bit RGB, filter 0 rows).

    python pngpix.py X,Y [X,Y ...] -- A.png [B.png ...]

One line per point with the RGB of each image side by side, so captures of the same screen
in two states (desktop, Dashboard, Exposé) can be compared by numbers.
"""
import struct
import sys
import zlib


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if (depth, ctype) != (8, 2):
                raise ValueError("%s: only 8-bit RGB (vncgrab output)" % path)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    if any(raw[y * stride] for y in range(h)):
        raise ValueError("%s: filtered rows (not vncgrab output)" % path)
    return w, h, raw, stride


def main(argv):
    if "--" not in argv:
        sys.exit(__doc__)
    cut = argv.index("--")
    points = [tuple(int(v) for v in p.split(",")) for p in argv[:cut]]
    images = [(p, load(p)) for p in argv[cut + 1:]]
    print("point       " + "  ".join("%-14s" % p.rsplit("/", 1)[-1][:14] for p, _ in images))
    for x, y in points:
        cols = []
        for _, (w, h, raw, stride) in images:
            if 0 <= x < w and 0 <= y < h:
                o = y * stride + 1 + x * 3
                cols.append("%3d %3d %3d   " % tuple(raw[o:o + 3]))
            else:
                cols.append("(outside)     ")
        print("%4d,%-4d   %s" % (x, y, "  ".join(cols)))


if __name__ == "__main__":
    main(sys.argv[1:])
