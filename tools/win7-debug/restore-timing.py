"""Summarize Windows 7 Explorer restore requests in tracerpt XML."""
import argparse
from bisect import bisect_left, bisect_right
from collections import defaultdict
from datetime import datetime
from math import ceil
from statistics import median
from xml.etree import ElementTree as ET

MARKERS = "{6c38bba1-1768-48c6-9c07-40dadba4b38f}"
DWM = "{8c9dd1ad-e6e5-4b07-b455-684a9d879900}"


def motion_region(marker):
    left, top, right, bottom, x, y, wl, wt, wr, wb = map(int, marker.split()[1:])
    # The window expands away from the button. Look in the empty space
    # between them, leaving 16 pixels for taskbar glow and window shadows.
    if right - left > bottom - top:
        x1, x2 = min(x, wl) - 16, max(x, wr) + 16
        if wb <= top:
            region = (x1, max(wb + 16, top - 200), x2, top - 16)
        else:
            region = (x1, bottom + 16, x2, min(wt - 16, bottom + 200))
    else:
        y1, y2 = min(y, wt) - 16, max(y, wb) + 16
        if wr <= left:
            region = (max(wr + 16, left - 200), y1, left - 16, y2)
        else:
            region = (right + 16, y1, min(wl - 16, right + 200), y2)
    if region[2] - region[0] < 32 or region[3] - region[1] < 32:
        raise ValueError("too little space between the restored window and the taskbar")
    return region


def summarize(path, minimize=False):
    requests, presents, renders, dirty = [], [], [], []
    unminimized = []
    region = None
    starts = {}
    processes = set()
    for _, event in ET.iterparse(path, events=("end",)):
        if not event.tag.endswith("}Event"):
            continue
        system = event.find("{*}System")
        guid = system.find("{*}Provider").get("Guid", "").lower()
        time = datetime.fromisoformat(system.find("{*}TimeCreated").get("SystemTime")[:26])
        if guid == MARKERS:
            marker = event.findtext("{*}Data", "")
            if marker == ("minimize-start" if minimize else "restore-start"):
                requests.append(time)
            elif not minimize and marker == "restore-unminimized":
                unminimized.append(time)
            elif marker.startswith("taskbar "):
                region = motion_region(marker)
        elif guid == DWM:
            task = event.findtext("{*}RenderingInfo/{*}Task")
            op = event.findtext("{*}RenderingInfo/{*}Opcode", "").strip()
            execution = system.find("{*}Execution")
            thread = execution.get("ThreadID")
            if task == "SCHEDULE_RENDER":
                processes.add(execution.get("ProcessID"))
                if op == "Start":
                    starts[thread] = time
                elif op == "Stop" and thread in starts:
                    renders.append((time, starts.pop(thread)))
            elif task == "MILEVENT_MEDIA_UCE_PRESENTEVENT" and op == "Stop":
                presents.append(time)
            elif task == "ETWGUID_DIRTYREGIONEVENT":
                rect = {d.get("Name"): float(d.text) for d in event.findall("{*}EventData/{*}Data")
                        if d.get("Name") in ("left", "right", "top", "bottom")}
                if rect["right"] - rect["left"] >= 32 and rect["bottom"] - rect["top"] >= 32:
                    dirty.append((time, rect))
        elif any(int(d.text) for d in event.findall("{*}EventData/{*}Data")
                 if d.get("Name") in ("EventsLost", "BuffersLost")):
            raise ValueError("ETW reports lost events or buffers")
        event.clear()

    if len(processes) != 1 or not requests:
        raise ValueError("expected one compositor process and at least one requested transition")
    times = sorted(time for time, r in dirty if region is None or
                   (min(r["right"], region[2]) - max(r["left"], region[0]) >= 32 and
                    min(r["bottom"], region[3]) - max(r["top"], region[1]) >= 32))
    renders.sort()
    ends = [end for end, _ in renders]
    frames = []
    for p in sorted(presents):
        index = bisect_right(ends, p) - 1
        if index >= 0:
            end, start = renders[index]
            first_dirty = bisect_left(times, start)
            if first_dirty < len(times) and times[first_dirty] <= end and (p-end).total_seconds() < .15:
                frames.append((p, start))
    stats = defaultdict(list)
    if region and not minimize and len(unminimized) != len(requests):
        raise ValueError("expected one unminimized marker per taskbar click")
    first_label = "minimize to first window-motion present (ms)" if minimize else "click to first window-motion present (ms)"
    for t in sorted(requests):
        i = bisect_left(frames, (t,))
        ms = []
        for p, start in frames[i:]:
            elapsed = (p - t).total_seconds() * 1000
            if elapsed >= 1150:
                break
            if region is None or start >= t:
                ms.append(elapsed)
        if region:
            if ms:
                stats[first_label].append(ms[0])
                stats["motion-region presents"].append(len(ms))
                if len(ms) >= 2:
                    stats["longest gap between motion-region presents (ms)"].append(max(b - a for a, b in zip(ms, ms[1:])))
        elif len(ms) >= 2:
            stats["first present (ms)"].append(ms[0])
            stats["longest gap after first present (ms)"].append(max(b - a for a, b in zip(ms, ms[1:])))
    if region:
        measured = len(stats[first_label])
        print(f"{measured}/{len(requests)} {'minimizes' if minimize else 'clicks'} with a repaint in the motion region")
        for t, u in zip(sorted(requests), sorted(unminimized)):
            delay = (u - t).total_seconds() * 1000
            if not 0 <= delay <= 1500:
                raise ValueError("unminimized marker outside its click interval")
            stats["click to observed unminimized state (ms)"].append(delay)
    else:
        measured = len(stats["first present (ms)"])
        print(f"{measured}/{len(requests)} {'minimizes' if minimize else 'restores'} with two or more substantial repaints")
    if not measured:
        raise ValueError("no measurable transition frames")
    for label, values in stats.items():
        values.sort()
        print(f"{label}: median {median(values):.1f}, p95 {values[ceil(.95 * len(values))-1]:.1f}, "
              f"max {values[-1]:.1f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xml", help="English tracerpt XML from the transition trace")
    parser.add_argument("--minimize", action="store_true", help="measure minimize requests instead of restores")
    args = parser.parse_args()
    summarize(args.xml, args.minimize)
