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


def summarize(path):
    requests, presents, renders, dirty = [], [], [], []
    starts = {}
    processes = set()
    for _, event in ET.iterparse(path, events=("end",)):
        if not event.tag.endswith("}Event"):
            continue
        system = event.find("{*}System")
        guid = system.find("{*}Provider").get("Guid", "").lower()
        time = datetime.fromisoformat(system.find("{*}TimeCreated").get("SystemTime")[:26])
        if guid == MARKERS and event.findtext("{*}Data") == "restore-start":
            requests.append(time)
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
                    dirty.append(time)
        elif any(int(d.text) for d in event.findall("{*}EventData/{*}Data")
                 if d.get("Name") in ("EventsLost", "BuffersLost")):
            raise ValueError("ETW reports lost events or buffers")
        event.clear()

    if len(processes) != 1 or not requests:
        raise ValueError("expected one compositor process and at least one restore request")
    dirty.sort()
    renders.sort()
    ends = [end for end, _ in renders]
    frames = []
    for p in sorted(presents):
        index = bisect_right(ends, p) - 1
        if index >= 0:
            end, start = renders[index]
            first_dirty = bisect_left(dirty, start)
            if first_dirty < len(dirty) and dirty[first_dirty] <= end and (p-end).total_seconds() < .15:
                frames.append(p)
    stats = defaultdict(list)
    for t in sorted(requests):
        i = bisect_left(frames, t)
        ms = []
        for p in frames[i:]:
            elapsed = (p - t).total_seconds() * 1000
            if elapsed >= 1150:
                break
            ms.append(elapsed)
        if len(ms) < 2:
            continue
        stats["first present (ms)"].append(ms[0])
        stats["longest gap after first present (ms)"].append(max(b - a for a, b in zip(ms, ms[1:])))
    measured = len(stats["first present (ms)"])
    print(f"{measured}/{len(requests)} restores with two or more substantial repaints")
    if not measured:
        raise ValueError("no measurable restore frames")
    for label, values in stats.items():
        values.sort()
        print(f"{label}: median {median(values):.1f}, p95 {values[ceil(.95 * measured)-1]:.1f}, "
              f"max {values[-1]:.1f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xml", help="English tracerpt XML from the transition trace")
    summarize(parser.parse_args().xml)
