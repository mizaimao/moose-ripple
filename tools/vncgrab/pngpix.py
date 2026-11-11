"""Print pixel values of PNGs (8-bit RGB or RGBA, any row filters, not interlaced).

    python pngpix.py X,Y [X,Y ...] -- A.png [B.png ...]

One line per point with the RGB of each image side by side, so captures of the same screen
in two states (desktop, Dashboard, Exposé) or from two renderers can be compared by numbers.
Standard library only.
"""
import struct
import sys
import zlib


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat, w, h, bpp = 8, b"", 0, 0, 3
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body[:13])
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError("%s: only 8-bit RGB or RGBA, not interlaced" % path)
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                line[i] = (line[i] + paeth(a, b, c)) & 255
        rows.append(line)
        prev = line
    return w, h, rows, bpp


def main(argv):
    if "--" not in argv:
        sys.exit(__doc__)
    cut = argv.index("--")
    points = [tuple(int(v) for v in p.split(",")) for p in argv[:cut]]
    images = [(p, load(p)) for p in argv[cut + 1:]]
    print("point       " + "  ".join("%-14s" % p.rsplit("/", 1)[-1][:14] for p, _ in images))
    for x, y in points:
        cols = []
        for _, (w, h, rows, bpp) in images:
            if 0 <= x < w and 0 <= y < h:
                cols.append("%3d %3d %3d   " % tuple(rows[y][x * bpp:x * bpp + 3]))
            else:
                cols.append("(outside)     ")
        print("%4d,%-4d   %s" % (x, y, "  ".join(cols)))


if __name__ == "__main__":
    main(sys.argv[1:])
