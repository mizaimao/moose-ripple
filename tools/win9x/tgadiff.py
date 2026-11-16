#!/usr/bin/env python3
"""Compare two screenshots a game wrote as uncompressed TGA files (Quake III's `screenshot`).

    python -I tgadiff.py A.tga B.tga [TOLERANCE]

Prints how many pixels differ by more than TOLERANCE (default 8, of 255) in any colour
channel, the largest and the mean difference, and where the differing pixels are (a coarse
map of the picture, 16 by 12 cells, with the share of differing pixels in each). Two renderers
never agree to the bit: filtering and rounding differ. A picture that is right shows a small
mean and no solid blocks in the map.

Exit status: 0 when fewer than 2% of the pixels differ, 1 otherwise.
"""
import struct
import sys


def read_tga(path):
    with open(path, "rb") as f:
        d = f.read()
    idlen, cmap, kind = d[0], d[1], d[2]
    w, h, bpp, desc = struct.unpack_from("<HHBB", d, 12)
    if cmap or kind != 2 or bpp not in (24, 32):
        raise ValueError("%s: not an uncompressed true-colour TGA" % path)
    n = bpp // 8
    data = d[18 + idlen:18 + idlen + w * h * n]
    if len(data) != w * h * n:
        raise ValueError("%s: file is cut short" % path)
    return w, h, n, data


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    tol = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    wa, ha, na, a = read_tga(sys.argv[1])
    wb, hb, nb, b = read_tga(sys.argv[2])
    if (wa, ha) != (wb, hb):
        sys.exit("sizes differ: %dx%d and %dx%d" % (wa, ha, wb, hb))
    cols, rows = 16, 12
    cell = [[0] * cols for _ in range(rows)]
    count = [[0] * cols for _ in range(rows)]
    differing = total = biggest = 0
    for y in range(ha):
        cy = (ha - 1 - y) * rows // ha            # TGA rows run from the bottom
        for x in range(wa):
            pa = a[(y * wa + x) * na:(y * wa + x) * na + 3]
            pb = b[(y * wb + x) * nb:(y * wb + x) * nb + 3]
            d = max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2]))
            total += d
            biggest = max(biggest, d)
            cx = x * cols // wa
            count[cy][cx] += 1
            if d > tol:
                differing += 1
                cell[cy][cx] += 1
    pixels = wa * ha
    share = 100.0 * differing / pixels
    print("%dx%d: %d of %d pixels differ by more than %d (%.2f%%), largest difference %d, mean %.2f"
          % (wa, ha, differing, pixels, tol, share, biggest, total / pixels))
    print("share of differing pixels by area (%), top row first:")
    for r in range(rows):
        print(" ".join("%3d" % (100 * cell[r][c] // max(count[r][c], 1)) for c in range(cols)))
    sys.exit(0 if share < 2.0 else 1)


if __name__ == "__main__":
    main()
