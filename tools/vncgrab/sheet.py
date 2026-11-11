"""Tile PNG frames (same size, 8-bit RGB or RGBA) into one contact-sheet PNG.

    python sheet.py [-s SCALE] [-c COLUMNS] [-x X,Y,W,H] OUT.png FRAME.png [FRAME.png ...]

SCALE divides each frame (nearest pixel, default 4); -x crops each frame first. Frames go
left to right, top to bottom, with a 4-pixel grey gap. For looking at a burst from
scripts/screen-vnc.sh -n at a glance. Standard library only.
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
    """Rows of RGB bytes. Fast for unfiltered rows (what vncgrab.py writes)."""
    data = open(path, "rb").read()
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
        if f:
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
        prev = line
        if bpp == 4:
            rgb = bytearray(w * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = line[0::4], line[1::4], line[2::4]
            rows.append(rgb)
        else:
            rows.append(line)
    return w, h, rows


def shrink(rows, box, scale):
    x0, y0, w, h = box
    out = []
    for y in range(y0, y0 + h - scale + 1, scale):
        row = rows[y][x0 * 3:(x0 + w) * 3]
        small = bytearray((w // scale) * 3)
        small[0::3] = row[0::3 * scale][:w // scale]
        small[1::3] = row[1::3 * scale][:w // scale]
        small[2::3] = row[2::3 * scale][:w // scale]
        out.append(small)
    return out


def write_png(path, w, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, len(rows), 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main(argv):
    scale, columns, box = 4, 6, None
    while argv and argv[0] in ("-s", "-c", "-x"):
        if argv[0] == "-s":
            scale = int(argv[1])
        elif argv[0] == "-c":
            columns = int(argv[1])
        else:
            box = tuple(int(v) for v in argv[1].split(","))
        argv = argv[2:]
    if len(argv) < 2:
        sys.exit(__doc__)
    out, frames = argv[0], argv[1:]
    gap, tiles = 4, []
    for p in frames:
        w, h, rows = load(p)
        tiles.append(shrink(rows, box or (0, 0, w, h), scale))
    tw, th = len(tiles[0][0]) // 3, len(tiles[0])
    columns = min(columns, len(tiles))
    sheet_w = columns * tw + (columns - 1) * gap
    grey = b"\x60\x60\x60"
    sheet = []
    for r in range(0, len(tiles), columns):
        group = tiles[r:r + columns]
        if sheet:
            sheet += [grey * sheet_w] * gap
        for y in range(th):
            line = (grey * gap).join(bytes(t[y]) for t in group)
            sheet.append(line + grey * (sheet_w - len(line) // 3))
    write_png(out, sheet_w, sheet)


if __name__ == "__main__":
    main(sys.argv[1:])
