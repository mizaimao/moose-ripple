"""framediff.py OURS REFERENCE [OUT.jpg]: compare two pictures of the same benchmark frame, one drawn
by the driver and one by a reference (3DMark03's Image Quality tool renders a chosen frame either way,
see docs/win2k/development.md).

Two numbers. "differ": the share of pixels that are more than TOLERANCE apart in a colour channel;
edges and texture filtering put a few percent here even between two correct pictures. "blocks": the
same after both pictures are averaged over 8x8 blocks, which is what a missing object, a wrong
texture or a wrong colour moves and speckle does not. The run fails when blocks is above LIMIT.
OUT.jpg gets ours, the reference and the differing blocks side by side at half size.
Needs Pillow; run it with the repository's own Python environment on the host."""
import sys
from PIL import Image, ImageChops

TOLERANCE = 16      # of 255, in the worst colour channel
LIMIT = 2.0         # percent of 8x8 blocks

def share(a, b):
    r, g, bl = ImageChops.difference(a, b).split()
    worst = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    hist = worst.histogram()
    over = sum(hist[TOLERANCE + 1:])
    return 100.0 * over / (a.size[0] * a.size[1]), worst.point(lambda v: 255 if v > TOLERANCE else 0)

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a = Image.open(sys.argv[1]).convert("RGB")
    b = Image.open(sys.argv[2]).convert("RGB")
    if a.size != b.size:
        print("FRAMEDIFF ours %dx%d reference %dx%d result=FAIL" % (a.size + b.size))
        return 1
    pixels, _ = share(a, b)
    blocks, mask = share(a.reduce(8), b.reduce(8))
    verdict = "PASS" if blocks <= LIMIT else "FAIL"
    print("FRAMEDIFF %dx%d differ=%.2f%% blocks=%.2f%% tolerance=%d limit=%.1f%% result=%s" %
          (a.size[0], a.size[1], pixels, blocks, TOLERANCE, LIMIT, verdict))
    if len(sys.argv) > 3:
        w, h = a.size[0] // 2, a.size[1] // 2
        sheet = Image.new("RGB", (3 * w, h))
        sheet.paste(a.resize((w, h)), (0, 0))
        sheet.paste(b.resize((w, h)), (w, 0))
        sheet.paste(mask.convert("RGB").resize((w, h), Image.NEAREST), (2 * w, 0))
        sheet.save(sys.argv[3], quality=88)
    return 0 if verdict == "PASS" else 1

sys.exit(main())
