"""Summarize held-button Explorer movement from the probe and English tracerpt XML."""
import argparse
from bisect import bisect_left
from collections import defaultdict
from datetime import datetime
from math import ceil
from statistics import median
from xml.etree import ElementTree as ET


def clock(text):
    # Host and guest each use their own time origin. Two shared events align them.
    return (datetime.fromisoformat(text.replace(",", ".").rstrip("Z")).replace(tzinfo=None)
            - datetime(1970, 1, 1)).total_seconds()


def read_trace(path):
    marks, presents, processes = defaultdict(list), [], set()
    for _, e in ET.iterparse(path, events=("end",)):
        if not e.tag.endswith("}Event"):
            continue
        system = e.find("{*}System")
        guid = system.find("{*}Provider").get("Guid", "").lower()
        t = clock(system.find("{*}TimeCreated").get("SystemTime"))
        if guid == "{6c38bba1-1768-48c6-9c07-40dadba4b38f}":
            marks[e.findtext("{*}Data", "")].append(t)
        elif guid == "{8c9dd1ad-e6e5-4b07-b455-684a9d879900}":
            if (e.findtext("{*}RenderingInfo/{*}Task") == "MILEVENT_MEDIA_UCE_PRESENTEVENT"
                    and e.findtext("{*}RenderingInfo/{*}Opcode", "").strip() == "Stop"):
                presents.append(t)
                processes.add(system.find("{*}Execution").get("ProcessID"))
        elif any(int(d.text) for d in e.findall("{*}EventData/{*}Data")
                 if d.get("Name") in ("EventsLost", "BuffersLost")):
            raise ValueError("trace lost events or buffers")
        e.clear()
    if len(processes) != 1:
        raise ValueError("expected presents from one DWM process")
    return marks, sorted(presents)


def read_samples(path):
    samples, completed = defaultdict(list), set()
    with open(path) as stream:
        for line in stream:
            if line.startswith("sample,"):
                _, mode, n, _, elapsed, *rect = line.split(",")
                samples[mode, int(n)].append((float(elapsed) / 1000, tuple(map(int, rect))))
            elif line.startswith("motion="):
                fields = dict(item.split("=") for item in line.split())
                mode = fields["motion"]
                expected = (216, 108, 216, 108) if mode == "drag" else (0, 0, 216, 108)
                delta = tuple(map(int, fields["delta"].split(",")))
                if (int(fields["distinct_changes"]) < 3 or len(delta) != 4 or
                        any(abs(a - b) > 2 for a, b in zip(delta, expected))):
                    raise ValueError("window missed its path: " + line.strip())
                completed.add((mode, int(fields["round"])))
            elif "failed" in line.lower():
                raise ValueError(line.strip())
    if not samples or set(samples) != completed:
        raise ValueError("incomplete movement samples")
    return samples


def summarize(args):
    marks, presents = read_trace(args.trace)
    samples = read_samples(args.samples)
    if args.sync and not args.utm_log:
        raise ValueError("clock anchors require a UTM log")
    draws = []
    scale, origin, host_origin = 1, 0, 0
    if args.utm_log:
        if len(args.sync) != 2:
            raise ValueError("supply two guest markers and their corresponding host times")
        (first, h0), (last, h1) = args.sync
        if len(marks[first]) != 1 or len(marks[last]) != 1:
            raise ValueError("each sync marker must occur once")
        origin, end = marks[first][0], marks[last][0]
        host_origin, host_end = clock(h0), clock(h1)
        if end <= origin or host_end <= host_origin:
            raise ValueError("clock anchors must be in time order")
        scale = (host_end - host_origin) / (end - origin)
        with open(args.utm_log) as stream:
            draws = sorted(clock(line[:23]) for line in stream if "display-2:0: gl draw " in line)
        if not draws:
            raise ValueError("no UTM display draws")

    def wall(t):
        return host_origin + (t - origin) * scale

    print("mode stream rounds updates/s median_ms p95_ms max_ms")
    for mode in ("drag", "resize"):
        starts, stops = sorted(marks[mode + "-start"]), sorted(marks[mode + "-stop"])
        rounds = sorted(n for m, n in samples if m == mode)
        if (not rounds or rounds != list(range(1, len(starts) + 1)) or len(starts) != len(stops)
                or any(b <= a for a, b in zip(starts, stops))
                or any(a <= b for a, b in zip(starts[1:], stops))):
            raise ValueError("incomplete " + mode + " trace")
        if args.utm_log and (starts[0] < origin or stops[-1] > end):
            raise ValueError("clock anchors must bracket measured motion")
        gaps, counts, seconds = defaultdict(list), defaultdict(int), 0
        for n, start, stop in zip(rounds, starts, stops):
            a, b = wall(start) + .250, wall(stop) - .100
            if b <= a:
                raise ValueError("movement interval too short")
            seconds += b - a
            data = samples[mode, n]
            streams = {
                "pointer": [wall(start + t) for t, _ in data],
                "bounds": [wall(start + t) for (t, r), (_, prev) in zip(data[1:], data) if r != prev],
                "DWM": [wall(t) for t in presents[bisect_left(presents, start):bisect_left(presents, stop)]],
            }
            if args.utm_log:
                streams["UTM"] = draws[bisect_left(draws, a):bisect_left(draws, b)]
            for name, times in streams.items():
                times = [t for t in times if a <= t < b]
                if len(times) < 2:
                    raise ValueError(f"too few {name} updates in {mode} round {n}")
                counts[name] += len(times)
                gaps[name].extend(1000 * (y - x) for x, y in zip(times, times[1:]))
        for name, values in gaps.items():
            ordered = sorted(values)
            print(mode, name, len(rounds), *(f"{x:.1f}" for x in
                  (counts[name] / seconds, median(ordered), ordered[ceil(.95 * len(ordered)) - 1], ordered[-1])))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples")
    parser.add_argument("trace")
    parser.add_argument("--utm-log", help="UTM debug log containing timestamped display draws")
    parser.add_argument("--sync", nargs=2, action="append", default=[], metavar=("MARKER", "HOST_ISO_TIME"))
    summarize(parser.parse_args())
