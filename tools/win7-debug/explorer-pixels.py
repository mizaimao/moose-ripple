"""Summarize explorerrestore searchphase and animationpixels CSV output."""
import argparse
import csv
from collections import defaultdict
from statistics import median


def read(path):
    pixels = defaultdict(list)
    geometry = {}
    completed = set()
    with open(path) as stream:
        for row in csv.reader(stream):
            if not row:
                continue
            if row[0] == "pixels":
                _, mode, n, layer, *values = row
                start, end, first, last, previous = map(float, values)
                samples = pixels[mode, int(n), layer]
                if end < start or (samples and start < samples[-1][1]):
                    raise ValueError("pixel timestamps out of order")
                samples.append((start, end, first, last, previous))
            elif row[0] == "geometry":
                _, mode, n, low, high = row
                geometry[mode, int(n)] = float(low), float(high)
            elif row[0].startswith(("motion=", "round=")):
                fields = dict(item.split("=") for item in row[0].split() if "=" in item)
                if fields.get("motion") == "resize" or fields.get("restored") == fields.get("composition") == "1":
                    completed.add(int(fields["round"]))
            elif "failed" in row[0].lower():
                raise ValueError(row[0])
    if not pixels or {n for _, n, _ in pixels} != completed:
        raise ValueError("incomplete rounds")
    if completed != set(range(1, max(completed) + 1)):
        raise ValueError("missing round")
    expected = {(mode, layer) for mode, _, layer in pixels}
    if expected not in ({("resize", "window")}, {("resize", "window"), ("resize", "screen")},
                        {("minimize", "screen"), ("restore", "screen")}):
        raise ValueError("unexpected pixel series")
    if any((mode, n, layer) not in pixels for mode, layer in expected for n in completed):
        raise ValueError("incomplete pixel series")
    return pixels, geometry


def settled(samples, tolerance=2):
    """Bracket the last mismatching read and first permanently settled read."""
    if len(samples) < 3 or any(s[3] > tolerance for s in samples[-3:]):
        raise ValueError("endpoint did not settle")
    bad = [i for i, row in enumerate(samples) if row[3] > tolerance]
    if not bad:
        return None, samples[0][1]
    i = bad[-1]
    return samples[i][0], samples[i + 1][1]


def summarize(path):
    pixels, geometry = read(path)
    for (mode, n, layer), samples in sorted(pixels.items()):
        if mode == "resize":
            if (mode, n) not in geometry:
                raise ValueError("missing geometry")
            g0, g1 = geometry[mode, n]
            low, high = settled(samples)
            lower = "unknown" if low is None else f"{low - g1:.1f}"
            print(f"resize {n} {layer} settle after child move (ms) [{lower}, {high - g0:.1f}]")
            continue
        if len(samples) < 6 or samples[0][3] < 10 or samples[-1][2] < 10:
            raise ValueError("transition endpoints too similar")
        if any(r[2] > 2 for r in samples[:2]) or any(r[3] > 2 for r in samples[-2:]):
            raise ValueError("transition endpoints did not settle")
        # Each intermediate differs from both stable endpoints and the previous read.
        distinct = [r for r in samples if min(r[2:]) >= 2]
        steps = [r for r in samples if r[4] >= 2]
        gaps = [b[1] - a[1] for a, b in zip(steps, steps[1:])]
        sampling = median(b[1] - a[1] for a, b in zip(samples, samples[1:]))
        gap = f"{max(gaps):.1f}" if gaps else "unobserved"
        print(f"{mode} {n} intermediate samples {len(distinct)}, longest observed change gap {gap} ms, "
              f"median read interval {sampling:.1f} ms")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples")
    summarize(parser.parse_args().samples)
